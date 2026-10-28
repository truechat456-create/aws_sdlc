/*
 * ecall_core.h - eCall session state machine.
 *
 * States:  IDLE -> TRIGGERED -> MSD_SENT -> VOICE_CONNECTED -> CLOSED
 *
 * While a session is active (any state other than IDLE/CLOSED) duplicate
 * triggers are ignored. From CLOSED the machine can be reset back to IDLE.
 *
 * Baseline v1. Pure logic, no I/O; the service layer drives transitions.
 */
#ifndef ECALL_CORE_H
#define ECALL_CORE_H

#include "ecall_status.h"
#include "can_if.h"   /* trigger_kind_t */

typedef enum
{
    ECALL_STATE_IDLE = 0,
    ECALL_STATE_TRIGGERED,
    ECALL_STATE_MSD_SENT,
    ECALL_STATE_VOICE_CONNECTED,
    ECALL_STATE_CLOSED
} ecall_state_t;

typedef struct
{
    ecall_state_t  state;
    trigger_kind_t active_trigger;  /* trigger that started the session     */
    uint32_t       session_id;      /* increments on each accepted trigger  */
    uint32_t       ignored_dups;    /* count of ignored duplicate triggers  */
} ecall_core_t;

const char *ecall_state_str(ecall_state_t state);

ecall_status_t ecall_core_init(ecall_core_t *core);

/*
 * Feed a trigger. From IDLE (or CLOSED) an AUTOMATIC/MANUAL trigger starts a
 * new session -> TRIGGERED and increments session_id. While already active,
 * any further trigger is ignored (ignored_dups++) and ECALL_ERR_STATE is
 * returned so the caller knows it was a no-op.
 */
ecall_status_t ecall_core_on_trigger(ecall_core_t  *core,
                                     trigger_kind_t trigger);

/* Advance TRIGGERED -> MSD_SENT. Valid only from TRIGGERED. */
ecall_status_t ecall_core_on_msd_sent(ecall_core_t *core);

/* Advance MSD_SENT -> VOICE_CONNECTED. Valid only from MSD_SENT. */
ecall_status_t ecall_core_on_voice_connected(ecall_core_t *core);

/* Advance to CLOSED. Valid from any active state. */
ecall_status_t ecall_core_on_close(ecall_core_t *core);

/* Reset CLOSED -> IDLE so a new call can be placed. */
ecall_status_t ecall_core_reset(ecall_core_t *core);

/* Query helpers. */
ecall_status_t ecall_core_state(const ecall_core_t *core, ecall_state_t *s);
bool           ecall_core_is_active(const ecall_core_t *core);

#endif /* ECALL_CORE_H */
