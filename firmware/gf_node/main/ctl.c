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

/* Present = raw mv non-zero: 0 mV is an unplugged clamp (ignored), while a few mV
 * is a plugged clamp on a dead conductor = 0 A = a lost phase. */
static void phase_range(const ctl_cfg_t *c, const ctl_in_t *in, int *n, int *lo, int *hi)
{
    *n = 0; *lo = 0; *hi = 0;
    for (int i = 0; i < 3; i++) {
        if (in->bore_mv[i] == 0) continue;
        int a = ctl_deci_amps(in->bore_mv[i], c->bore_apv_x100, c->bore_turns);
        if (a < 0) a = 0;
        if ((*n)++ == 0 || a < *lo) *lo = a;
        if (a > *hi) *hi = a;
    }
}

/* true while `cond` has held for ms; restarts the clock when it breaks */
static bool held_for(bool cond, bool *on, int64_t *since, int64_t now, int64_t ms)
{
    if (!cond) { *on = false; return false; }
    if (!*on) { *on = true; *since = now; }
    return now - *since >= ms;
}

/* Borewell protections. Returns true while a protective stop is held. */
static bool bore_protect(ctl_state_t *s, const ctl_cfg_t *c, const ctl_in_t *in, ctl_out_t *out)
{
    int n, lo, hi;
    phase_range(c, in, &n, &lo, &hi);
    int64_t now = in->now_ms;

    if (held_for(s->bore_running && n >= 2 && lo * 100 < hi * 30, &s->sp_on, &s->sp_since_ms, now, CTL_SINGLE_PHASE_MS))
        s->flags |= CTL_F_SINGLE_PHASE;

    if (held_for(s->bore_running, &s->run_on, &s->run_since_ms, now, (int64_t)c->max_run_min * 60000 + 1) && c->max_run_min)
        s->flags |= CTL_F_MAX_RUN;
    if (!s->bore_running) s->flags &= ~CTL_F_MAX_RUN;

    bool dry = c->bore_dry_da && s->bore_running && hi < (int)c->bore_dry_da;
    if (!c->bore_act) {
        /* Alert only: no rest, no retries -- the flag just follows the debounced condition */
        if (held_for(dry, &s->dry_on, &s->dry_since_ms, now, CTL_DRY_MS)) s->flags |= CTL_F_BORE_DRY;
        else if (!dry) s->flags &= ~CTL_F_BORE_DRY;
    } else if (!(s->flags & (CTL_F_BORE_DRY | CTL_F_BORE_DRY_LATCH))) {
        if (held_for(dry, &s->dry_on, &s->dry_since_ms, now, CTL_DRY_MS)) {
            s->flags |= CTL_F_BORE_DRY;  s->dry_on = false;
            s->rest_until_ms = now + CTL_DRY_REST_MS;
            if (++s->dry_attempts >= CTL_DRY_ATTEMPTS) s->flags |= CTL_F_BORE_DRY_LATCH;
        }
        /* only a long healthy run earns the attempts back, so a bore that recovers
         * briefly and dries again every rest still latches */
        if (held_for(c->bore_dry_da && s->bore_running && hi >= (int)c->bore_dry_da, &s->ok_on, &s->ok_since_ms, now, CTL_DRY_OK_MS))
            s->dry_attempts = 0;
    } else if (!(s->flags & CTL_F_BORE_DRY_LATCH) && now >= s->rest_until_ms
               && (!c->ctrl_node || s->float_db == 1)) {   /* node mode: never restart a full tank */
        s->flags &= ~CTL_F_BORE_DRY;
        if (!(s->flags & CTL_F_SINGLE_PHASE)) {   /* never a START edge while a stop is held */
            out->bore_start = true;
            if (c->ctrl_node) { s->start_try = 1;  s->start_deadline_ms = now + CTL_START_VERIFY_MS; }
        }
    }

    if (!c->bore_act) return false;
    return (s->flags & (CTL_F_SINGLE_PHASE | CTL_F_BORE_DRY | CTL_F_BORE_DRY_LATCH)) != 0;
}

/* Sump motor. Returns true while a sump stop is held (Act only). */
static bool sump_protect(ctl_state_t *s, const ctl_cfg_t *c, const ctl_in_t *in)
{
    int64_t now = in->now_ms;
    int com = ctl_deci_amps(in->sump_mv[0], c->sump_apv_x100, c->sump_turns);
    int st  = ctl_deci_amps(in->sump_mv[1], c->sump_apv_x100, c->sump_turns);
    if (com < 0) com = 0;
    if (st > 0) s->cap_seen = true;

    /* Hold: the borewell has failed and the tank is low, so the sump motor is held off.
     * It frees only after the borewell runs (or the float is high) and stays that way. */
    if (!(s->flags & CTL_F_SUMP_HOLD)) {
        if (s->float_db == 1 && !s->bore_running
            && (s->flags & (CTL_F_START_FAILED | CTL_F_BORE_DRY | CTL_F_SINGLE_PHASE))) {
            s->flags |= CTL_F_SUMP_HOLD;  s->sc_on = false;
        }
    } else if (held_for(s->bore_running || s->float_db == 0, &s->sc_on, &s->sc_since_ms, now, CTL_SUMP_RELEASE_MS)) {
        s->flags &= ~CTL_F_SUMP_HOLD;
    }

    /* No-load dry. In Act the stop is timed, not condition-based: a stopped pump draws
     * nothing, so it cannot prove itself healthy. */
    bool dry = c->sump_noload_da && in->pump_on && com < (int)c->sump_noload_da;
    if (!c->sump_act) {
        if (held_for(dry, &s->sd_on, &s->sd_since_ms, now, CTL_DRY_MS)) s->flags |= CTL_F_SUMP_DRY;
        else if (!dry) s->flags &= ~CTL_F_SUMP_DRY;
    } else if (!(s->flags & CTL_F_SUMP_DRY)) {
        if (held_for(dry, &s->sd_on, &s->sd_since_ms, now, CTL_DRY_MS)) {
            s->flags |= CTL_F_SUMP_DRY;  s->sd_on = false;  s->sd_until_ms = now + CTL_SUMP_RELEASE_MS;
        }
    } else if (now >= s->sd_until_ms) {
        s->flags &= ~CTL_F_SUMP_DRY;
    }

    /* Capacitor suspect (alert only): Common fine, Start channel dead after having read */
    bool cap = c->sump_noload_da && in->pump_on && com >= (int)c->sump_noload_da && s->cap_seen && st <= 0;
    if (held_for(cap, &s->cap_on, &s->cap_since_ms, now, CTL_DRY_MS)) s->flags |= CTL_F_CAP_SUSPECT;
    else if (!cap) s->flags &= ~CTL_F_CAP_SUSPECT;

    return c->sump_act && (s->flags & (CTL_F_SUMP_HOLD | CTL_F_SUMP_DRY));
}

void ctl_step(ctl_state_t *s, const ctl_cfg_t *c, const ctl_in_t *in, ctl_out_t *out)
{
    memset(out, 0, sizeof *out);
    debounce(s, in);
    if (!c->ctrl_node) s->float_db = -1;   /* NEC-49 is in charge: nothing here drives */
    s->bore_running = any_at_least(in->bore_mv, c->bore_apv_x100, c->bore_turns, c->bore_run_da);

    /* Settle: a fresh boot reads garbage for a while (float not debounced), and a ctl_step is a full clamp sample, so reaching this point is the clamp read
     * (idle clamps read ~0 and must not block settle). A wrong first decision starts or stops a motor. */
    if (in->float_low >= 0 && s->float_reads < 2) s->float_reads++;
    if (!s->settled && in->now_ms - s->boot_ms >= CTL_SETTLE_MS && (s->float_reads >= 2 || !c->ctrl_node))
        s->settled = true;
    if (!s->settled) return;

    out->sump_stop = sump_protect(s, c, in);
    if (bore_protect(s, c, in, out)) { out->bore_stop = true; return; }
    if (!c->ctrl_node) return;     /* NEC mode: the float logic below is node-only */
    if (out->bore_start) return;   /* the dry rest's restart already owns this step */

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
