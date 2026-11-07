#include "ctl.h"
#include <string.h>

int16_t ctl_deci_amps(uint16_t mv, uint16_t apv_x100, uint8_t turns)
{
    if (mv < CT_NOISE_FLOOR_MV) return -1;
    if (turns == 0) turns = 1;
    uint32_t deci = ((uint32_t)mv * apv_x100) / (100UL * 100UL * turns);
    if (deci > 9990) deci = 9990;
    return (int16_t)deci;
}

void ctl_init(ctl_state_t *s, const ctl_persist_t *p, int64_t boot_ms)
{
    memset(s, 0, sizeof *s);
    s->boot_ms = boot_ms;
    s->raw_float = -1;  s->float_db = -1;
    if (p) {
        if (p->user_stop_bore) s->flags |= CTL_F_USER_STOP_BORE;
        if (p->user_stop_sump) s->flags |= CTL_F_USER_STOP_SUMP;
        if (p->single_phase)   s->flags |= CTL_F_SINGLE_PHASE;
    }
}

uint16_t ctl_flags(const ctl_state_t *s)
{
    return s->flags | (s->settled ? 0 : CTL_F_NOT_SETTLED);
}
bool   ctl_bore_running(const ctl_state_t *s) { return s->bore_running; }
int8_t ctl_float_low(const ctl_state_t *s)    { return s->float_db; }

/* The raw float must hold one value for the whole debounce window before it
 * counts: the tank slops and the contact chatters, and a STOP pulse on a
 * ripple would cycle the borewell motor. */
static void debounce(ctl_state_t *s, const ctl_in_t *in)
{
    if (in->float_low < 0) { s->raw_float = -1; s->float_db = -1; return; }
    if (in->float_low != s->raw_float) { s->raw_float = in->float_low; s->raw_since_ms = in->now_ms; }
    if (in->now_ms - s->raw_since_ms >= CTL_FLOAT_DEBOUNCE_MS) s->float_db = s->raw_float;
}

static bool any_at_least(const uint16_t mv[3], uint16_t apv, uint8_t turns, uint16_t da)
{
    for (int i = 0; i < 3; i++) {
        int16_t a = ctl_deci_amps(mv[i], apv, turns);
        if (a >= 0 && a >= (int16_t)da) return true;
    }
    return false;
}

void ctl_step(ctl_state_t *s, const ctl_cfg_t *c, const ctl_in_t *in, ctl_out_t *out)
{
    memset(out, 0, sizeof *out);
    debounce(s, in);
    if (!c->ctrl_node) s->float_db = -1;   /* NEC-49 is in charge: nothing here drives */
    s->bore_running = any_at_least(in->bore_mv, c->bore_apv_x100, c->bore_turns, c->bore_run_da);
    if (!c->ctrl_node) return;

    /* Settle: a fresh boot reads garbage for a while (float not debounced, clamps
     * not yet sampled), and a wrong first decision starts or stops a motor. */
    if (in->float_low >= 0 && s->float_reads < 2) s->float_reads++;
    for (int i = 0; i < 3; i++)
        if (in->bore_mv[i] >= CT_NOISE_FLOOR_MV || in->sump_mv[i] >= CT_NOISE_FLOOR_MV) s->clamp_seen = true;
    if (!s->settled && in->now_ms - s->boot_ms >= CTL_SETTLE_MS && s->float_reads >= 2 && s->clamp_seen)
        s->settled = true;
    if (!s->settled) return;

    /* a person's float going high clears a failed start so the next low gets a fresh try */
    if (s->float_db != 1) { s->flags &= ~CTL_F_START_FAILED; s->start_try = 0; }
    if (s->float_db != 0) s->stop_sent = false;

    if (s->float_db == 0 && s->bore_running && !s->stop_sent) {
        s->stop_sent = true;  s->stop_until_ms = in->now_ms + CTL_STOP_PULSE_MS;
    }
    out->bore_stop = in->now_ms < s->stop_until_ms && s->stop_sent;
    if (out->bore_stop) return;

    if (s->start_try) {
        if (s->bore_running) s->start_try = 0;
        else if (in->now_ms >= s->start_deadline_ms) {
            if (s->start_try == 1) { s->start_try = 2; s->start_deadline_ms = in->now_ms + CTL_START_VERIFY_MS; out->bore_start = true; }
            else { s->start_try = 0; s->flags |= CTL_F_START_FAILED; }
        }
    } else if (s->float_db == 1 && !s->bore_running && !(s->flags & CTL_F_START_FAILED)) {
        s->start_try = 1;  s->start_deadline_ms = in->now_ms + CTL_START_VERIFY_MS;  out->bore_start = true;
    }
}
