/*
 * Twisted Metal frame-rate mod: fixes that instruction patches cannot express.
 *
 * As in Twisted Metal 2, the engine keeps a car's speed as distance per frame
 * and rescales it when the frame rate changes, so top speed is the same at
 * any rate. The steps it adds to that speed each frame (throttle, braking,
 * engine drag, the speed a turn or a slope takes or gives) come from handling
 * values it scales by frame rate once, but a change in distance-per-frame per
 * frame needs the factor twice. Left alone, a car reaches top speed in about
 * a third of the time at 60 FPS and loses three times the speed in a turn.
 *
 * The callback below runs just before each of those steps is applied (the
 * sites are listed in game.toml [recompiler] mod_instruction_sites) and
 * scales the step by elapsed refreshes / 3, which is 1 at the original 20 FPS.
 *
 * Steering has the same kind of fault in three places, fixed further down:
 * the turn rate is steering lock times speed, both already per frame, so a
 * car turns at a third of the original rate at 60 FPS; the lock builds by a
 * per-frame step that is only scaled once; and the wheel re-centres by a
 * fixed fraction per frame.
 *
 * Two more things count frames instead of time. The engine's "move a value
 * part of the way to a target" helpers (AI steering, the camera) keep a fixed
 * percentage of the gap each frame, so they settle three times as fast at 60
 * FPS. And every weapon cooldown, for the player and the AI alike, ticks once
 * per frame, so everything fires three times as often.
 */
#include "mod_plugins.h"
#include "cpu_state.h"

#include <math.h>

enum { REG_V0 = 2, REG_V1 = 3, REG_A0 = 4, REG_A1 = 5, REG_S0 = 16, REG_S1 = 17 };

/* Refreshes the current frame spans, as the game measured it (1..5). */
#define FRAME_REFRESHES_ADDR 0x8018E890u
#define ORIGINAL_REFRESHES 3

/* Each site is the addu/subu that feeds a `sw speed, 24(car + 0x380)`. */
typedef struct {
    uint32_t address, instruction;
    uint8_t step_reg;   /* holds the step about to be applied */
    uint8_t car_reg;    /* identifies the car, for the remainder carry */
} SpeedStepSite;

static const SpeedStepSite k_sites[] = {
    { 0x801132CCu, 0x00821021u, REG_A0, REG_S1 },   /* throttle, forward */
    { 0x801132B4u, 0x00441023u, REG_A0, REG_S1 },   /* throttle, reverse */
    { 0x801133B8u, 0x00441023u, REG_A0, REG_S1 },   /* engine drag from forward */
    { 0x801133E0u, 0x00821021u, REG_A0, REG_S1 },   /* engine drag from reverse */
    { 0x801134BCu, 0x00821021u, REG_A0, REG_S1 },   /* braking from reverse */
    { 0x801134E8u, 0x00441023u, REG_A0, REG_S1 },   /* braking from forward */
    { 0x801141DCu, 0x00431023u, REG_V1, REG_A1 },   /* too fast for the turn, forward */
    { 0x801141FCu, 0x00431021u, REG_V1, REG_A1 },   /* too fast for the turn, reverse */
    { 0x801146CCu, 0x00431023u, REG_V1, REG_S0 },   /* slide drag from forward */
    { 0x801146F4u, 0x00621021u, REG_V1, REG_S0 },   /* slide drag from reverse */
    { 0x801179CCu, 0x00501023u, REG_S0, REG_S1 },   /* hard turn: a fraction of the speed */
    { 0x8011E85Cu, 0x00641821u, REG_V1, REG_S1 },   /* steep downhill */
    { 0x8011E8DCu, 0x00832023u, REG_V1, REG_S1 },   /* steep uphill */
    { 0x8011E90Cu, 0x00621823u, REG_V0, REG_S1 },   /* uphill */
    { 0x8011E928u, 0x00431023u, REG_V1, REG_S1 },   /* slight uphill */
    { 0x8011CB6Cu, 0x00621823u, REG_V0, REG_S0 },   /* knocked out of control, from forward */
    { 0x8011CBA8u, 0x00431021u, REG_V0, REG_S0 },   /* knocked out of control, from reverse */
};
enum { SITE_COUNT = sizeof k_sites / sizeof k_sites[0], CARRY_SLOTS = 16 };

/* The steps are small integers, so plain division would lose up to a whole
 * unit per frame. Carry each car's remainder to its next frame instead. */
typedef struct { uint32_t owner; int32_t remainder; } StepCarry;
static StepCarry s_carry[SITE_COUNT + 2][CARRY_SLOTS];   /* + the two steering steps */

static int32_t scale_step(int site, uint32_t owner, int32_t step) {
    int16_t refreshes = (int16_t)psx_mod_read_half(FRAME_REFRESHES_ADDR);
    if (refreshes < 1 || refreshes >= ORIGINAL_REFRESHES)
        return step;        /* at or below the original rate the engine is as shipped */
    StepCarry* carry = &s_carry[site][(owner >> 2) % CARRY_SLOTS];
    if (carry->owner != owner) {
        carry->owner = owner;
        carry->remainder = 0;
    }
    int32_t scaled = step * refreshes + carry->remainder;
    int32_t whole = scaled / ORIGINAL_REFRESHES;
    carry->remainder = scaled - whole * ORIGINAL_REFRESHES;
    return whole;
}

static void scale_speed_step(struct CPUState* cpu, uint32_t address) {
    for (int i = 0; i < SITE_COUNT; ++i) {
        if (k_sites[i].address != address) continue;
        const SpeedStepSite* site = &k_sites[i];
        cpu->gpr[site->step_reg] = (uint32_t)scale_step(
            i, cpu->gpr[site->car_reg], (int32_t)cpu->gpr[site->step_reg]);
        return;
    }
}

static int frame_refreshes(void) {
    int16_t refreshes = (int16_t)psx_mod_read_half(FRAME_REFRESHES_ADDR);
    return refreshes < 1 || refreshes > ORIGINAL_REFRESHES ? ORIGINAL_REFRESHES : refreshes;
}

/* `sra $v1, $v1, 5` in turn rate = (speed / 32) * lock / handling value: use
 * the speed the car would have per original frame. */
static void turn_rate_speed(struct CPUState* cpu, uint32_t address) {
    (void)address;
    int32_t speed = (int32_t)cpu->gpr[REG_V1];
    cpu->gpr[REG_V1] = (uint32_t)(speed * ORIGINAL_REFRESHES / frame_refreshes());
}

/* `addu/subu $v0, $v0, $a0` on the steering lock at 0($s1): $a0 is the step. */
static void steering_step(struct CPUState* cpu, uint32_t address) {
    int site = address == 0x80112CB4u ? SITE_COUNT : SITE_COUNT + 1;
    cpu->gpr[REG_A0] = (uint32_t)scale_step(site, cpu->gpr[REG_S1], (int32_t)cpu->gpr[REG_A0]);
}

/* Re-centring halves the lock each frame (`sra $v1, $v1, 1`), or on some
 * surfaces takes a sixteenth off it (`sra $v1, $v1, 4`, then subtracted).
 * Hand each shift the value that leaves the same lock after the same time:
 * 0.5 ^ (refreshes / 3) of it, or 1 - (15/16) ^ (refreshes / 3) taken off. */
static void recentre_half(struct CPUState* cpu, uint32_t address) {
    (void)address;
    static const int32_t k_keep_q12[ORIGINAL_REFRESHES + 1] = { 0, 3251, 2580, 2048 };
    int32_t lock = (int32_t)cpu->gpr[REG_V1];
    cpu->gpr[REG_V1] = (uint32_t)((lock * k_keep_q12[frame_refreshes()] * 2) / 4096);
}
static void recentre_sixteenth(struct CPUState* cpu, uint32_t address) {
    (void)address;
    static const int32_t k_take_q12[ORIGINAL_REFRESHES + 1] = { 0, 1397, 2752, 4096 };
    int32_t lock = (int32_t)cpu->gpr[REG_V1];
    cpu->gpr[REG_V1] = (uint32_t)((lock * k_take_q12[frame_refreshes()]) / 4096);
}

/* --- "keep pct% of the gap" helpers -------------------------------------
 * lerp(cur, target, pct) = (cur * pct + target * (100 - pct)) / 100 at
 * 0x8015395C, and an angle version at 0x80153884 that first moves the target
 * a turn closer. Both end in `subu $v0, $v0, $v1`; the entry hook remembers
 * cur (the body overwrites $a0), and the hook on that subu substitutes the
 * value that keeps pct ^ (refreshes / 3) of the gap instead. */
static int32_t s_lerp_current;

static void lerp_entered(struct CPUState* cpu, uint32_t address) {
    (void)address;
    s_lerp_current = (int32_t)cpu->gpr[REG_A0];
}

static void lerp_result(struct CPUState* cpu, uint32_t address) {
    (void)address;
    int refreshes = frame_refreshes();
    if (refreshes >= ORIGINAL_REFRESHES) return;
    int32_t target = (int32_t)cpu->gpr[REG_A1];
    int32_t pct = (int32_t)cpu->gpr[6];
    if (pct <= 0 || pct >= 100) return;        /* snap or hold: nothing to rescale */
    double keep = pow(pct / 100.0, (double)refreshes / ORIGINAL_REFRESHES);
    int32_t gap = s_lerp_current - target;
    cpu->gpr[REG_V0] = (uint32_t)(target + (int32_t)(gap * keep));
    cpu->gpr[REG_V1] = 0;
}

/* --- logic that counts frames ---------------------------------------------
 * Outside the car physics the game does not use frame time at all: these
 * functions count cooldowns, lifetimes and delays in frames. Let each run
 * once per three refreshes, as at the original rate, and skip the calls in
 * between. None of them draws; drawing is separate and stays every frame.
 *
 *   0x80132658(car, which)  a car's weapon logic: cooldowns, heat, firing
 *   0x801374F4()            shot and impact effects: lifetimes and delays
 *   0x80136AF8()            the timer that spaces out pickup respawns
 */
enum { FRAME_LOGIC_SLOTS = 32 };
static struct { uint32_t key; int32_t refreshes; } s_frame_logic[FRAME_LOGIC_SLOTS];

static int frame_logic_filter(struct CPUState* cpu, uint32_t address) {
    int refreshes = frame_refreshes();
    if (refreshes >= ORIGINAL_REFRESHES) return 0;
    uint32_t key = address;
    if (address == 0x80132658u) key ^= cpu->gpr[REG_A0] ^ ((cpu->gpr[REG_A1] & 0xFFu) << 28);
    unsigned slot = (key >> 2) % FRAME_LOGIC_SLOTS;
    if (s_frame_logic[slot].key != key) {
        s_frame_logic[slot].key = key;
        s_frame_logic[slot].refreshes = ORIGINAL_REFRESHES;    /* run the first call */
    }
    s_frame_logic[slot].refreshes += refreshes;
    if (s_frame_logic[slot].refreshes < ORIGINAL_REFRESHES) return 1;   /* skip this call */
    s_frame_logic[slot].refreshes -= ORIGINAL_REFRESHES;
    if (s_frame_logic[slot].refreshes > ORIGINAL_REFRESHES) s_frame_logic[slot].refreshes = ORIGINAL_REFRESHES;
    return 0;
}

PSX_MOD_CONSTRUCTOR(tm1_register_framerate_plugin) {
    (void)psx_mod_register_function_filter_plugin("tm1.framerate", 0x80132658u, frame_logic_filter);
    (void)psx_mod_register_function_filter_plugin("tm1.framerate", 0x801374F4u, frame_logic_filter);
    (void)psx_mod_register_function_filter_plugin("tm1.framerate", 0x80136AF8u, frame_logic_filter);
    (void)psx_mod_register_instruction_plugin("tm1.framerate", 0x8015395Cu, 0x00860018u, lerp_entered);
    (void)psx_mod_register_instruction_plugin("tm1.framerate", 0x80153994u, 0x00431023u, lerp_result);
    (void)psx_mod_register_instruction_plugin("tm1.framerate", 0x80153884u, 0x27BDFFE0u, lerp_entered);
    (void)psx_mod_register_instruction_plugin("tm1.framerate", 0x801538FCu, 0x00431023u, lerp_result);
    (void)psx_mod_register_instruction_plugin("tm1.framerate", 0x80153940u, 0x00431023u, lerp_result);
    (void)psx_mod_register_instruction_plugin("tm1.framerate", 0x80112DD8u, 0x00031943u, turn_rate_speed);
    (void)psx_mod_register_instruction_plugin("tm1.framerate", 0x80112CB4u, 0x00441021u, steering_step);
    (void)psx_mod_register_instruction_plugin("tm1.framerate", 0x80112CECu, 0x00441023u, steering_step);
    (void)psx_mod_register_instruction_plugin("tm1.framerate", 0x80112DB8u, 0x00031843u, recentre_half);
    (void)psx_mod_register_instruction_plugin("tm1.framerate", 0x80112D8Cu, 0x00031903u, recentre_sixteenth);
    for (int i = 0; i < SITE_COUNT; ++i)
        (void)psx_mod_register_instruction_plugin("tm1.framerate", k_sites[i].address,
                                                  k_sites[i].instruction, scale_speed_step);
}
