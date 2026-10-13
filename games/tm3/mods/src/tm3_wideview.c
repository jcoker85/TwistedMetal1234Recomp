/*
 * Twisted Metal III: terrain for views wider than 16:9, and which packets are HUD.
 *
 * The world renderer does not test terrain cells against the view. It looks
 * the camera's heading up in a table of 64 directions, each a prepared list
 * of the grid cells a 4:3 view can see from there, and draws that list. The
 * lists are generous enough for 16:9, but at 21:9 the cells in the outer
 * edges of the picture are missing and show as holes.
 *
 * While the view is wider than 16:9, the hooks below make the renderer's own
 * loop run again over the lists of the neighbouring directions. A cell the
 * camera's own lists hold is left exactly as the game draws it; any other
 * cell is drawn once, from the nearest of the three detail lists any
 * neighbour has it in. Nothing in guest memory is changed
 * apart from the loop's own stack slots, and at 4:3 and 16:9 the hooks do
 * nothing. The sites are listed in game.toml [recompiler]
 * mod_instruction_sites.
 */
#include "mod_plugins.h"
#include "cpu_state.h"

#include <string.h>

#define REG_T0 8
#define REG_T1 9
#define REG_S0 16
#define REG_S5 21
#define REG_SP 29

#define TABLE_ADDR    0x80067718u
#define TABLE_BUCKETS 64
#define ENTRY_SIZE    1288u     /* 640 (x, z) byte pairs, then three counts */
#define ENTRY_COUNTS  1280u
#define MAX_CELLS     640
/* The stored x byte's low bits are flags the loop shifts out before use. */
#define CELL_X_SHIFT  1

/* The loop's locals: four passes, each a count and a list pointer. Passes 1
 * and 2 share the middle list. */
#define STACK_COUNTS 0x7C
#define STACK_LISTS  0x84
#define LAST_PASS    3

/* Widest reveal (native pixels a side) the prepared lists already cover. */
#define COVERED_MARGIN 56
#define MAX_SPREAD 4

static struct {
    uint32_t sp;            /* the render call the state belongs to; 0 = none */
    int bucket;
    int round;              /* 0 = the game's own lists */
    int rounds;             /* neighbour rounds still wanted, in total */
    uint8_t owner[65536];   /* per (x, z): 1 + the round that draws the cell; 0 = nobody */
    uint8_t detail[65536];  /* the list (0 near, 1 middle, 2 far) it is drawn from */
} s_view;

/* A bucket is 5.6 degrees; 21:9 (120 px a side) takes three more a side. */
static int neighbour_spread(void) {
    int32_t margin = psx_mod_widescreen_view_x_margin();
    if (margin <= COVERED_MARGIN) return 0;
    int spread = (margin - 56 + 21) / 22;
    return spread > MAX_SPREAD ? MAX_SPREAD : spread < 1 ? 1 : spread;
}

static int neighbour_bucket(int round) {
    /* Rounds 1, 2, 3, 4... are the headings +1, -1, +2, -2... from ours. */
    int step = (round + 1) / 2;
    int bucket = s_view.bucket + ((round & 1) ? step : -step);
    return ((bucket % TABLE_BUCKETS) + TABLE_BUCKETS) % TABLE_BUCKETS;
}

/* Read a heading's three list sizes; 0 if they are not a valid entry. */
static int entry_counts(uint32_t entry, int counts[3]) {
    int total = 0;
    for (int i = 0; i < 3; ++i) {
        counts[i] = (int16_t)psx_mod_read_half(entry + ENTRY_COUNTS + 2u * (uint32_t)i);
        if (counts[i] < 0) return 0;
        total += counts[i];
    }
    return total <= MAX_CELLS;
}

/* Decide, for every cell any round offers, which round and list draws it. */
static void assign_cells(void) {
    memset(s_view.owner, 0, sizeof s_view.owner);
    for (int round = 0; round <= s_view.rounds; ++round) {
        uint32_t entry = TABLE_ADDR + (uint32_t)neighbour_bucket(round) * ENTRY_SIZE;
        int counts[3];
        if (!entry_counts(entry, counts)) continue;
        uint32_t cell = entry;
        for (int list = 0; list < 3; ++list) {
            for (int i = 0; i < counts[list]; ++i, cell += 2) {
                int8_t x = (int8_t)psx_mod_read_byte(cell);
                uint32_t key = (((uint32_t)(x >> CELL_X_SHIFT) & 0xFFu) << 8) |
                               psx_mod_read_byte(cell + 1);
                uint8_t owner = s_view.owner[key];
                if (owner == 1) continue;                   /* the game's own lists */
                if (owner && round && s_view.detail[key] <= list) continue;
                s_view.owner[key] = (uint8_t)(round + 1);
                s_view.detail[key] = (uint8_t)list;
            }
        }
    }
}

/* --- HUD packets -----------------------------------------------------------
 * In widescreen the runtime moves HUD primitives out to the screen corners,
 * and knows them by where their packets sit in memory ([widescreen]
 * nw_left_hud_packet*). The game draws into two alternating buffers, world
 * polygons from the start and the HUD a fixed distance in, but the buffers
 * are allocated per level, so fixed addresses only fit some levels: in the
 * rest, part of the HUD stays at its 4:3 position and some world polygons
 * are moved as if they were HUD, on alternate frames. Follow the game's own
 * buffer pointers instead. */
void gpu_ws_set_nw_left_hud_packet_range(uint32_t lo, uint32_t hi);
void gpu_ws_set_nw_left_hud_packet_range2(uint32_t lo, uint32_t hi);

/* The game's allocation table: 0x60-byte records, each starting with a block's
 * address. Which records hold the two packet buffers changes from level to
 * level, but the buffers are always 0x23788 apart. */
#define ALLOCATION_TABLE 0x800D6A3Cu
#define ALLOCATION_RECORD 0x60u
#define ALLOCATION_RECORDS 4
#define PACKET_BUFFER_SIZE 0x23788u
#define HUD_OFFSET_LO 0x1BE00u    /* the first HUD packet is 0x1BEE8 past the block address */
#define HUD_OFFSET_HI 0x23700u

static void follow_hud_packets(void) {
    static uint32_t s_first;
    uint32_t block[ALLOCATION_RECORDS];
    for (int i = 0; i < ALLOCATION_RECORDS; ++i)
        block[i] = psx_mod_read_word(ALLOCATION_TABLE + ALLOCATION_RECORD * (uint32_t)i);
    for (int i = 0; i < ALLOCATION_RECORDS; ++i) {
        uint32_t first = block[i];
        if (first < 0x80080000u || first >= 0x801C0000u) continue;
        for (int j = 0; j < ALLOCATION_RECORDS; ++j) {
            if (block[j] != first + PACKET_BUFFER_SIZE) continue;
            if (first == s_first) return;
            s_first = first;
            gpu_ws_set_nw_left_hud_packet_range(first + HUD_OFFSET_LO, first + HUD_OFFSET_HI);
            gpu_ws_set_nw_left_hud_packet_range2(block[j] + HUD_OFFSET_LO, block[j] + HUD_OFFSET_HI);
            return;
        }
    }
}

/* `sw $s5, lists($sp)`: the lists for this heading have just been chosen. */
static void lists_chosen(struct CPUState* cpu, uint32_t address) {
    (void)address;
    follow_hud_packets();
    s_view.sp = 0;
    int spread = neighbour_spread();
    if (!spread) return;
    uint32_t offset = cpu->gpr[REG_S5] - TABLE_ADDR;
    if (offset % ENTRY_SIZE || offset / ENTRY_SIZE >= TABLE_BUCKETS) return;
    s_view.sp = cpu->gpr[REG_SP];
    s_view.bucket = (int)(offset / ENTRY_SIZE);
    s_view.round = 0;
    s_view.rounds = spread * 2;
    assign_cells();
}

/* `add $t0, $t0, $s3`: $t0 and $t1 hold a cell's offset from the camera's. */
static void cell_fetched(struct CPUState* cpu, uint32_t address) {
    (void)address;
    if (!s_view.sp || cpu->gpr[REG_SP] != s_view.sp) return;
    uint32_t key = ((cpu->gpr[REG_T0] & 0xFFu) << 8) | (cpu->gpr[REG_T1] & 0xFFu);
    if (!s_view.round) return;                   /* the game's own lists, as shipped */
    int pass = (int32_t)cpu->gpr[REG_S0];
    int list = pass <= 0 ? 0 : pass >= LAST_PASS ? 2 : 1;
    if (s_view.owner[key] != (uint8_t)(s_view.round + 1) || s_view.detail[key] != list)
        cpu->gpr[REG_T0] = (uint32_t)-0x4000;    /* off the grid: the loop skips it */
}

/* `addiu $t0, $s0, -3`: a pass has ended; after the last one the loop exits. */
static void pass_ended(struct CPUState* cpu, uint32_t address) {
    (void)address;
    if (!s_view.sp || cpu->gpr[REG_SP] != s_view.sp) return;
    if ((int32_t)cpu->gpr[REG_S0] != LAST_PASS) return;
    if (s_view.round >= s_view.rounds) {
        s_view.sp = 0;
        return;
    }
    s_view.round++;
    uint32_t entry = TABLE_ADDR + (uint32_t)neighbour_bucket(s_view.round) * ENTRY_SIZE;
    int counts[3];
    if (!entry_counts(entry, counts)) {
        s_view.sp = 0;
        return;
    }
    int near_cells = counts[0], mid_cells = counts[1], far_cells = counts[2];
    uint32_t sp = s_view.sp;
    uint32_t mid = entry + 2u * (uint32_t)near_cells;
    psx_mod_write_half(sp + STACK_COUNTS + 0, (uint16_t)near_cells);
    psx_mod_write_half(sp + STACK_COUNTS + 2, (uint16_t)mid_cells);
    psx_mod_write_half(sp + STACK_COUNTS + 4, (uint16_t)mid_cells);
    psx_mod_write_half(sp + STACK_COUNTS + 6, (uint16_t)far_cells);
    psx_mod_write_word(sp + STACK_LISTS + 0, entry);
    psx_mod_write_word(sp + STACK_LISTS + 4, mid);
    psx_mod_write_word(sp + STACK_LISTS + 8, mid);
    psx_mod_write_word(sp + STACK_LISTS + 12, mid + 2u * (uint32_t)mid_cells);
    cpu->gpr[REG_S0] = (uint32_t)-1;     /* the loop's own increment restarts at pass 0 */
}

PSX_MOD_CONSTRUCTOR(tm3_register_wideview_plugin) {
    (void)psx_mod_register_instruction_plugin("tm3.wideview", 0x80010C44u, 0xAFB50084u, lists_chosen);
    (void)psx_mod_register_instruction_plugin("tm3.wideview", 0x80010CBCu, 0x01134020u, cell_fetched);
    (void)psx_mod_register_instruction_plugin("tm3.wideview", 0x8001115Cu, 0x2608FFFDu, pass_ended);
}
