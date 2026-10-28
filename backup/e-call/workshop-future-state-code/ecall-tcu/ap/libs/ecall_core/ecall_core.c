/*
 * ecall_core.c - eCall session state machine implementation.
 */
#include "ecall_core.h"

#include <string.h>

const char *ecall_state_str(ecall_state_t state)
{
    const char *s;

    switch (state)
    {
        case ECALL_STATE_IDLE:            s = "IDLE";            break;
        case ECALL_STATE_TRIGGERED:       s = "TRIGGERED";       break;
        case ECALL_STATE_MSD_SENT:        s = "MSD_SENT";        break;
        case ECALL_STATE_VOICE_CONNECTED: s = "VOICE_CONNECTED"; break;
        case ECALL_STATE_CLOSED:          s = "CLOSED";          break;
        default:                          s = "UNKNOWN";         break;
    }

    return s;
}

ecall_status_t ecall_core_init(ecall_core_t *core)
{
    if (core == NULL)
    {
        return ECALL_ERR_PARAM;
    }
    (void)memset(core, 0, sizeof(*core));
    core->state          = ECALL_STATE_IDLE;
    core->active_trigger = TRIGGER_NONE;
    return ECALL_OK;
}

bool ecall_core_is_active(const ecall_core_t *core)
{
    if (core == NULL)
    {
        return false;
    }
    return (core->state != ECALL_STATE_IDLE) &&
           (core->state != ECALL_STATE_CLOSED);
}

ecall_status_t ecall_core_on_trigger(ecall_core_t  *core,
                                     trigger_kind_t trigger)
{
    if (core == NULL)
    {
        return ECALL_ERR_PARAM;
    }
    if ((trigger != TRIGGER_AUTOMATIC) && (trigger != TRIGGER_MANUAL))
    {
        return ECALL_ERR_PARAM;
    }

    /* Ignore duplicate triggers while a session is already active. */
    if (ecall_core_is_active(core))
    {
        core->ignored_dups++;
        return ECALL_ERR_STATE;
    }

    /* Accept from IDLE or CLOSED. */
    core->state          = ECALL_STATE_TRIGGERED;
    core->active_trigger = trigger;
    core->session_id++;
    return ECALL_OK;
}

ecall_status_t ecall_core_on_msd_sent(ecall_core_t *core)
{
    if (core == NULL)
    {
        return ECALL_ERR_PARAM;
    }
    if (core->state != ECALL_STATE_TRIGGERED)
    {
        return ECALL_ERR_STATE;
    }
    core->state = ECALL_STATE_MSD_SENT;
    return ECALL_OK;
}

ecall_status_t ecall_core_on_voice_connected(ecall_core_t *core)
{
    if (core == NULL)
    {
        return ECALL_ERR_PARAM;
    }
    if (core->state != ECALL_STATE_MSD_SENT)
    {
        return ECALL_ERR_STATE;
    }
    core->state = ECALL_STATE_VOICE_CONNECTED;
    return ECALL_OK;
}

ecall_status_t ecall_core_on_close(ecall_core_t *core)
{
    if (core == NULL)
    {
        return ECALL_ERR_PARAM;
    }
    if (!ecall_core_is_active(core))
    {
        return ECALL_ERR_STATE;
    }
    core->state = ECALL_STATE_CLOSED;
    return ECALL_OK;
}

ecall_status_t ecall_core_reset(ecall_core_t *core)
{
    if (core == NULL)
    {
        return ECALL_ERR_PARAM;
    }
    if (core->state != ECALL_STATE_CLOSED)
    {
        return ECALL_ERR_STATE;
    }
    core->state          = ECALL_STATE_IDLE;
    core->active_trigger = TRIGGER_NONE;
    return ECALL_OK;
}

ecall_status_t ecall_core_state(const ecall_core_t *core, ecall_state_t *s)
{
    if ((core == NULL) || (s == NULL))
    {
        return ECALL_ERR_PARAM;
    }
    *s = core->state;
    return ECALL_OK;
}
