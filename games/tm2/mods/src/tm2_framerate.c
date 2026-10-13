/*
 * Twisted Metal 2 frame-rate mod: fixes that instruction patches cannot express.
 *
 * The engine steps its simulation by the measured frame time, but several of
 * its formulas only hold near the 20 FPS it shipped at. Everything here runs
 * just before one instruction (the sites are listed in game.toml
 * [recompiler] mod_instruction_sites), does nothing at the original three
 * refreshes per frame, and makes that formula give the same result per unit
 * of time at one or two refreshes per frame.
 *
 * Speed. A car's speed is kept as distance per frame and rescaled when the
 * frame rate changes. The steps added to it each frame (throttle, braking,
 * coasting, the drag of a slide) are scaled by frame time once, but a change
 * in distance-per-frame per frame needs the factor twice.
 *
 * Steering. The turn rate is steering lock times speed, both already per
 * frame, so a car turns at a third of the original rate at 60 FPS and never
 * builds into a slide. The lock builds by a step that is scaled once where
 * it needs twice, and it re-centres as lock / refreshes: a third per frame
 * at 20 FPS, a half at 30, and not at all at 60.
 *
 * Smoothing. The car body's position and heading follow the simulated ones
 * through helpers that keep a fixed percentage of the gap each frame.
 */
#include "mod_plugins.h"
#include "cpu_state.h"

#include <math.h>

enum { REG_V0 = 2, REG_V1 = 3, REG_A0 = 4, REG_A1 = 5, REG_A2 = 6,
       REG_S2 = 18, REG_S3 = 19, REG_S4 = 20 };

/* Refreshes the current frame spans, as the game measured it (1..5). */
#define FRAME_REFRESHES_ADDR 0x8017EBF8u
#define ORIGINAL_REFRESHES 3

static int frame_refreshes(void) {
    int16_t refreshes = (int16_t)psx_mod_read_half(FRAME_REFRESHES_ADDR);
    return refreshes < 1 || refreshes > ORIGINAL_REFRESHES ? ORIGINAL_REFRESHES : refreshes;
}

/* --- per-frame steps --------------------------------------------------------
 * Each site is the addu/subu that applies a step to a halfword of the car's
 * motion state (speed at 10, steering lock at 0). */
typedef struct {
    uint32_t address, instruction;
    uint8_t step_reg;   /* holds the step about to be applied */
    uint8_t car_reg;    /* identifies the car, for the remainder carry */
} StepSite;

static const StepSite k_sites[] = {
    { 0x800DE19Cu, 0x00451021u, REG_A1, REG_S2 },   /* throttle, forward */
    { 0x800DE184u, 0x00451023u, REG_A1, REG_S2 },   /* throttle, reverse */
    { 0x800DE278u, 0x00451023u, REG_A1, REG_S2 },   /* coasting from forward */
    { 0x800DE2A4u, 0x00451021u, REG_A1, REG_S2 },   /* coasting from reverse */
    { 0x800DDF74u, 0x00451023u, REG_A1, REG_S2 },   /* braking from forward */
    { 0x800DDFBCu, 0x00451021u, REG_A1, REG_S2 },   /* braking from reverse */
    { 0x800E0578u, 0x00521023u, REG_S2, REG_S3 },   /* slide drag from forward */
    { 0x800E05A4u, 0x00521021u, REG_S2, REG_S3 },   /* slide drag from reverse */
    { 0x800DD6D0u, 0x00451021u, REG_A1, REG_S2 },   /* steering lock, one way */
    { 0x800DD708u, 0x00451023u, REG_A1, REG_S2 },   /* steering lock, the other */
};
enum { SITE_COUNT = sizeof k_sites / sizeof k_sites[0], CARRY_SLOTS = 16 };

/* The steps are small integers, so plain division would lose up to a whole
 * unit per frame. Carry each car's remainder to its next frame instead. */
typedef struct { uint32_t owner; int32_t remainder; } StepCarry;
static StepCarry s_carry[SITE_COUNT][CARRY_SLOTS];

static void scale_step(struct CPUState* cpu, uint32_t address) {
    int refreshes = frame_refreshes();
    if (refreshes >= ORIGINAL_REFRESHES) return;
    for (int i = 0; i < SITE_COUNT; ++i) {
        if (k_sites[i].address != address) continue;
        uint32_t owner = cpu->gpr[k_sites[i].car_reg];
        StepCarry* carry = &s_carry[i][(owner >> 2) % CARRY_SLOTS];
        if (carry->owner != owner) {
            carry->owner = owner;
            carry->remainder = 0;
        }
        int32_t scaled = (int32_t)cpu->gpr[k_sites[i].step_reg] * refreshes + carry->remainder;
        int32_t whole = scaled / ORIGINAL_REFRESHES;
        carry->remainder = scaled - whole * ORIGINAL_REFRESHES;
        cpu->gpr[k_sites[i].step_reg] = (uint32_t)whole;
        return;
    }
}

/* --- steering ---------------------------------------------------------------
 * `sra $v1, $v1, 5` in turn rate = (speed / 32) * lock / handling value, and
 * in the speed test that picks that formula: use the speed the car would
 * have per original frame. */
static void turn_rate_speed(struct CPUState* cpu, uint32_t address) {
    (void)address;
    int32_t speed = (int32_t)cpu->gpr[REG_V1];
    cpu->gpr[REG_V1] = (uint32_t)(speed * ORIGINAL_REFRESHES / frame_refreshes());
}

/* `div $v1, $v0` computing lock / refreshes: at one refresh per frame that is
 * the lock unchanged. Keep (1/3) ^ (1/3) of it instead. Two refreshes halve
 * it, close enough to the (1/3) ^ (2/3) it should keep. */
static void recentre(struct CPUState* cpu, uint32_t address) {
    (void)address;
    if ((int32_t)cpu->gpr[REG_V0] != 1) return;
    cpu->gpr[REG_V1] = (uint32_t)((int32_t)cpu->gpr[REG_V1] * 2840);
    cpu->gpr[REG_V0] = 4096;
}

/* On some surfaces re-centring takes a sixteenth off the lock per frame
 * (`sra $v1, $v1, 4`, then subtracted): hand the shift the value that takes
 * 1 - (15/16) ^ (refreshes / 3) off instead. */
static void recentre_sixteenth(struct CPUState* cpu, uint32_t address) {
    (void)address;
    static const int32_t k_take_q12[ORIGINAL_REFRESHES + 1] = { 0, 1397, 2752, 4096 };
    int32_t lock = (int32_t)cpu->gpr[REG_V1];
    cpu->gpr[REG_V1] = (uint32_t)((lock * k_take_q12[frame_refreshes()]) / 4096);
}

/* --- slide drag -------------------------------------------------------------
 * `blez $v0` after v0 = ((value * refreshes) / 32) * 32: with one refresh a
 * value under 32 rounds to nothing and the whole slide term is skipped. Give
 * it the original frame's amount, scaled to this frame. */
static void slide_term(struct CPUState* cpu, uint32_t address) {
    (void)address;
    int refreshes = frame_refreshes();
    if (refreshes >= ORIGINAL_REFRESHES) return;
    int32_t value = (int16_t)psx_mod_read_half(cpu->gpr[REG_S4] + 44u);
    int32_t original = value * ORIGINAL_REFRESHES;
    if (original < 0) original += 31;
    original = (original >> 5) << 5;
    cpu->gpr[REG_V0] = (uint32_t)(original * refreshes / ORIGINAL_REFRESHES);
}

/* --- "keep pct% of the gap" helpers -----------------------------------------
 * lerp(cur, target, pct) = (cur * pct + target * (100 - pct)) / 100 at
 * 0x80103840, and an angle version at 0x801037A8 that first moves the target
 * a turn closer. The entry hook remembers cur; the hook on the final subu
 * substitutes the value that keeps pct ^ (refreshes / 3) of the gap. */
static int32_t s_lerp_current;

static void lerp_entered(struct CPUState* cpu, uint32_t address) {
    (void)address;
    s_lerp_current = (int32_t)cpu->gpr[REG_A0];
}

static int lerp_value(struct CPUState* cpu, int32_t* out) {
    int refreshes = frame_refreshes();
    if (refreshes >= ORIGINAL_REFRESHES) return 0;
    int32_t target = (int32_t)cpu->gpr[REG_A1];
    int32_t pct = (int32_t)cpu->gpr[REG_A2];
    if (pct <= 0 || pct >= 100) return 0;      /* snap or hold: nothing to rescale */
    double keep = pow(pct / 100.0, (double)refreshes / ORIGINAL_REFRESHES);
    *out = target + (int32_t)((s_lerp_current - target) * keep);
    return 1;
}

/* `subu $v0, $v0, $v1` */
static void angle_lerp_result(struct CPUState* cpu, uint32_t address) {
    (void)address;
    int32_t value;
    if (!lerp_value(cpu, &value)) return;
    cpu->gpr[REG_V0] = (uint32_t)value;
    cpu->gpr[REG_V1] = 0;
}

/* `subu $v0, $v1, $v0` */
static void lerp_result(struct CPUState* cpu, uint32_t address) {
    (void)address;
    int32_t value;
    if (!lerp_value(cpu, &value)) return;
    cpu->gpr[REG_V1] = (uint32_t)value;
    cpu->gpr[REG_V0] = 0;
}

/* --- counters that count frames ---------------------------------------------
 * Nearly all of the game uses frame time, but two counters tick once per
 * frame: a 100-frame window at 0x800FBC44 that publishes running totals, and
 * a 40-frame delay in the car body code (`addiu $v0, $v0, 1` at 0x80102DE8).
 * Let each advance once per three refreshes. */
static int32_t s_window_refreshes = ORIGINAL_REFRESHES, s_delay_refreshes;

static int window_filter(struct CPUState* cpu, uint32_t address) {
    (void)cpu; (void)address;
    int refreshes = frame_refreshes();
    if (refreshes >= ORIGINAL_REFRESHES) return 0;
    s_window_refreshes += refreshes;
    if (s_window_refreshes < ORIGINAL_REFRESHES) return 1;     /* skip this call */
    s_window_refreshes -= ORIGINAL_REFRESHES;
    return 0;
}

static void delay_count(struct CPUState* cpu, uint32_t address) {
    (void)address;
    int refreshes = frame_refreshes();
    if (refreshes >= ORIGINAL_REFRESHES) return;
    s_delay_refreshes += refreshes;
    if (s_delay_refreshes >= ORIGINAL_REFRESHES) {
        s_delay_refreshes -= ORIGINAL_REFRESHES;
        return;                         /* let this frame's +1 through */
    }
    cpu->gpr[REG_V0] -= 1;              /* the +1 that follows leaves the count where it was */
}

PSX_MOD_CONSTRUCTOR(tm2_register_framerate_plugin) {
    (void)psx_mod_register_function_filter_plugin("tm2.framerate", 0x800FBC44u, window_filter);
    (void)psx_mod_register_instruction_plugin("tm2.framerate", 0x80102DE8u, 0x24420001u, delay_count);
    for (int i = 0; i < SITE_COUNT; ++i)
        (void)psx_mod_register_instruction_plugin("tm2.framerate", k_sites[i].address,
                                                  k_sites[i].instruction, scale_step);
    (void)psx_mod_register_instruction_plugin("tm2.framerate", 0x800DD878u, 0x00031943u, turn_rate_speed);
    (void)psx_mod_register_instruction_plugin("tm2.framerate", 0x800DD840u, 0x00031943u, turn_rate_speed);
    (void)psx_mod_register_instruction_plugin("tm2.framerate", 0x800DD7E0u, 0x0062001Au, recentre);
    (void)psx_mod_register_instruction_plugin("tm2.framerate", 0x800DD7B0u, 0x00031903u, recentre_sixteenth);
    (void)psx_mod_register_instruction_plugin("tm2.framerate", 0x800E03ACu, 0x1840006Bu, slide_term);
    (void)psx_mod_register_instruction_plugin("tm2.framerate", 0x801037A8u, 0x27BDFFE0u, lerp_entered);
    (void)psx_mod_register_instruction_plugin("tm2.framerate", 0x80103820u, 0x00431023u, angle_lerp_result);
    (void)psx_mod_register_instruction_plugin("tm2.framerate", 0x80103840u, 0x00860018u, lerp_entered);
    (void)psx_mod_register_instruction_plugin("tm2.framerate", 0x80103874u, 0x00621023u, lerp_result);
}
