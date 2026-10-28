/*
 * ecalld.c - eCall daemon.
 *
 * Single-threaded epoll loop. Registers the GNSS (UDP 10110), SOS (UDP
 * 10111) and CAN (vcan0) sockets. On a crash/manual trigger it runs the
 * eCall sequence: build MSD from the latest live fix, send it to the PSAP
 * with retries, then advance the state machine. Logs to syslog.
 *
 * Baseline v1: location is live GNSS only. No fallback.
 */
#include "ecall_status.h"
#include "ecalld_config.h"
#include "gnss_if.h"
#include "can_if.h"
#include "ecall_core.h"
#include "msd.h"
#include "psap_if.h"

#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <syslog.h>
#include <sys/epoll.h>

#define EPOLL_MAX_EVENTS   (8)
#define EPOLL_TIMEOUT_MS   (1000)

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int signo)
{
    (void)signo;
    g_stop = 1;
}

/* Context bundle passed around the loop. */
typedef struct
{
    ecalld_config_t cfg;
    gnss_if_t       gnss;
    can_if_t        can;
    ecall_core_t    core;
    psap_if_t       psap;
    uint32_t        msg_counter;
} app_t;

/* Build and send one MSD for the current trigger. Advances the FSM. */
static ecall_status_t run_ecall_sequence(app_t         *app,
                                         trigger_kind_t trig)
{
    gnss_fix_t     fix;
    bool           have_fix = false;
    uint64_t       now_ms   = 0u;
    uint64_t       age_ms   = 0u;
    msd_input_t    in;
    uint8_t        buf[MSD_MAX_BYTES];
    size_t         len = 0u;
    ecall_status_t st;

    (void)ecall_monotonic_ms(&now_ms);

    if (gnss_if_get_latest(&app->gnss, &fix) == ECALL_OK)
    {
        have_fix = true;
        age_ms   = (now_ms >= fix.rx_mono_ms)
                       ? (now_ms - fix.rx_mono_ms)
                       : 0u;
    }

    (void)memset(&in, 0, sizeof(in));
    in.version    = (uint8_t)MSD_VERSION;
    in.message_id = ++app->msg_counter;
    in.activation = (trig == TRIGGER_AUTOMATIC)
                        ? MSD_ACT_AUTOMATIC
                        : MSD_ACT_MANUAL;
    in.test_call  = app->cfg.test_call;
    (void)snprintf(in.vin, sizeof(in.vin), "%s", app->cfg.vin);
    in.timestamp_utc = (uint64_t)time(NULL);
    in.passengers    = app->cfg.passengers;

    st = msd_fill_position(&in, have_fix,
                           have_fix ? fix.lat_deg : 0.0,
                           have_fix ? fix.lon_deg : 0.0,
                           age_ms,
                           have_fix ? (uint16_t)fix.course_deg : 0u);
    if (st != ECALL_OK)
    {
        syslog(LOG_ERR, "msd_fill_position failed: %s",
               ecall_status_str(st));
        return st;
    }

    st = msd_encode(&in, buf, sizeof(buf), &len);
    if (st != ECALL_OK)
    {
        syslog(LOG_ERR, "msd_encode failed: %s", ecall_status_str(st));
        return st;
    }

    syslog(LOG_INFO,
           "eCall %s: MSD id=%u bytes=%zu position_trusted=%d",
           (trig == TRIGGER_AUTOMATIC) ? "AUTO" : "MANUAL",
           in.message_id, len, (int)in.position_trusted);

    st = psap_if_send_msd(&app->psap, buf, len);
    if (st != ECALL_OK)
    {
        syslog(LOG_ERR, "MSD send failed after %u attempts: %s",
               (unsigned)app->psap.last_attempts, ecall_status_str(st));
        return st;
    }

    syslog(LOG_INFO, "MSD sent in %u attempt(s)",
           (unsigned)app->psap.last_attempts);

    (void)ecall_core_on_msd_sent(&app->core);
    /* Voice path is out of scope for v1; mark connected then close. */
    (void)ecall_core_on_voice_connected(&app->core);
    (void)ecall_core_on_close(&app->core);
    (void)ecall_core_reset(&app->core);

    return ECALL_OK;
}

static ecall_status_t handle_trigger(app_t *app, trigger_kind_t trig)
{
    ecall_status_t st;

    if (trig == TRIGGER_NONE)
    {
        return ECALL_OK;
    }

    st = ecall_core_on_trigger(&app->core, trig);
    if (st == ECALL_ERR_STATE)
    {
        syslog(LOG_WARNING, "duplicate trigger ignored (active session)");
        return ECALL_OK;
    }
    if (st != ECALL_OK)
    {
        return st;
    }

    return run_ecall_sequence(app, trig);
}

static ecall_status_t setup_sockets(app_t *app, int epfd)
{
    struct epoll_event ev;
    int                fd;

    /* GNSS */
    if (gnss_if_open(&app->gnss, app->cfg.gnss_port) != ECALL_OK)
    {
        syslog(LOG_ERR, "cannot open GNSS UDP %u", app->cfg.gnss_port);
        return ECALL_ERR_IO;
    }
    (void)gnss_if_fd(&app->gnss, &fd);
    (void)memset(&ev, 0, sizeof(ev));
    ev.events  = EPOLLIN;
    ev.data.fd = fd;
    (void)epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);

    /* SOS */
    if (can_if_open_sos(&app->can, app->cfg.sos_port) != ECALL_OK)
    {
        syslog(LOG_ERR, "cannot open SOS UDP %u", app->cfg.sos_port);
        return ECALL_ERR_IO;
    }
    (void)can_if_sos_fd(&app->can, &fd);
    ev.data.fd = fd;
    (void)epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);

    /* CAN (best-effort: vcan0 may not exist in all environments) */
    if (can_if_open_can(&app->can, app->cfg.can_ifname) == ECALL_OK)
    {
        (void)can_if_can_fd(&app->can, &fd);
        ev.data.fd = fd;
        (void)epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
    }
    else
    {
        syslog(LOG_WARNING, "CAN iface %s unavailable; SOS-only mode",
               app->cfg.can_ifname);
    }

    return ECALL_OK;
}

int main(int argc, char **argv)
{
    static app_t       app;   /* static: no heap, fixed lifetime           */
    int                epfd;
    const char        *cfg_path = (argc > 1) ? argv[1] : NULL;
    struct epoll_event events[EPOLL_MAX_EVENTS];

    openlog("ecalld", LOG_PID | LOG_CONS, LOG_DAEMON);
    syslog(LOG_INFO, "ecalld starting (baseline v1)");

    (void)memset(&app, 0, sizeof(app));
    (void)ecalld_config_load(&app.cfg, cfg_path);
    (void)gnss_if_init(&app.gnss);
    (void)can_if_init(&app.can);
    (void)ecall_core_init(&app.core);
    (void)psap_if_init(&app.psap);

    if (psap_if_open(&app.psap, app.cfg.psap_host, app.cfg.psap_port)
        != ECALL_OK)
    {
        syslog(LOG_ERR, "cannot open PSAP socket");
        closelog();
        return 1;
    }

    epfd = epoll_create1(0);
    if (epfd < 0)
    {
        syslog(LOG_ERR, "epoll_create1 failed");
        closelog();
        return 1;
    }

    if (setup_sockets(&app, epfd) != ECALL_OK)
    {
        (void)close(epfd);
        closelog();
        return 1;
    }

    (void)signal(SIGINT, on_signal);
    (void)signal(SIGTERM, on_signal);

    syslog(LOG_INFO, "ecalld ready");

    while (g_stop == 0)
    {
        int n = epoll_wait(epfd, events, EPOLL_MAX_EVENTS, EPOLL_TIMEOUT_MS);
        int i;

        if (n < 0)
        {
            if (g_stop != 0)
            {
                break;
            }
            continue;
        }

        for (i = 0; i < n; ++i)
        {
            int            efd = events[i].data.fd;
            int            gfd = -1;
            int            sfd = -1;
            int            cfd = -1;
            trigger_kind_t trig = TRIGGER_NONE;

            (void)gnss_if_fd(&app.gnss, &gfd);
            (void)can_if_sos_fd(&app.can, &sfd);
            (void)can_if_can_fd(&app.can, &cfd);

            if (efd == gfd)
            {
                (void)gnss_if_poll(&app.gnss);
            }
            else if (efd == sfd)
            {
                if (can_if_poll_sos(&app.can, &trig) == ECALL_OK)
                {
                    (void)handle_trigger(&app, trig);
                }
            }
            else if (efd == cfd)
            {
                if (can_if_poll_can(&app.can, &trig) == ECALL_OK)
                {
                    (void)handle_trigger(&app, trig);
                }
            }
            else
            {
                /* unknown fd: ignore */
            }
        }
    }

    syslog(LOG_INFO, "ecalld stopping");
    (void)gnss_if_close(&app.gnss);
    (void)can_if_close(&app.can);
    (void)psap_if_close(&app.psap);
    (void)close(epfd);
    closelog();
    return 0;
}
