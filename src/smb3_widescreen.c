/*
 * SMB3 widescreen compositor.
 *
 * SMB3 decompresses a whole horizontal level into cartridge WRAM (Tile_Mem,
 * $6000: 15 screens x 27 rows x 16 columns of 16x16 metatiles), so the
 * columns beyond the original 256 already exist in guest memory. This draws
 * them with the game's own metatile layout (TileLayout_ByTileset) and its
 * background CHR bank selections (PatTable_BankSel), at the scroll the PPU
 * actually rendered each line with.
 *
 * The native picture stays authoritative for columns 8-247 of the playfield
 * (sprites, priority, anything the game drew into the nametables). The left
 * 8 columns (masked by the game) and the right 8 (SMB3's attribute-wrap
 * colours) take the rebuilt background plus OAM sprites. The status bar is
 * centred with its edge colours extended. Anything that is not a horizontal
 * level -- title, map, vertical levels -- is left to the centred 4:3 picture.
 *
 * Addresses: Southbird's SMB3 disassembly (smb3-disassembly/), verified
 * against the USA Rev 1 ROM at power-on; a mismatch disables the mode.
 */
#include "smb3_widescreen.h"

#include <stdio.h>
#include <string.h>

#include "cyc_core.h"
#include "cyc_mod.h"
#include "cyc_render.h"
#include "cyc_video.h"
#include "hw_internal.h"

/* CPU RAM */
#define RAM_HORZ_SCROLL_HI   0x0012
#define RAM_LEVEL_WIDTH      0x0022 /* screens - 1 */
#define RAM_HORZ_SCROLL      0x00FD
#define RAM_LEVEL_7VERTICAL  0x03EF
#define RAM_PATTABLE_BANKSEL 0x0719 /* [0] BG $0000, [1] BG $0800 (1K bank numbers) */
#define RAM_LEVEL_TILESET    0x070A /* 0 = world map */

/* Cartridge WRAM ($6000-) */
#define TILE_MEM_SCREEN 0x1B0
#define TILE_MEM_ROWS   27
#define TILE_MEM_SCREENS 15

/* Fixed $8000 bank (PRG bank 30) */
#define ROM_PAGE_C000_BY_TILESET  0x83D6
#define ROM_PAGE_A000_BY_TILESET  0x83E9
#define ROM_TILELAYOUT_BY_TILESET 0x99F7
#define TILESET_COUNT 19

static int  s_mode = NES_VIDEO_STOCK;
static bool s_rom_ok, s_check;
static uint8_t  s_opaque[CYC_VIDEO_MAX_WIDTH * 240];
static uint8_t  s_pixels[256][8][8]; /* decoded BG tiles: 2-bit colour */
static uint64_t s_frames, s_check_px, s_check_bad;

static const uint8_t PAGE_A000_EXPECTED[TILESET_COUNT] = {
    11, 15, 21, 16, 17, 19, 18, 18, 18, 20, 23, 19, 17, 19, 13, 26, 26, 26, 9,
};

static uint8_t ram(uint16_t a) { return cyc_mod_peek(a); }

/* A byte of the fixed $8000 bank, from the image: the mapper only puts bank
 * 30 there once the game has set it up. */
static uint8_t rom30(uint16_t a) { return hw_cart.prg[30 * 0x2000 + (a & 0x1FFF)]; }

static bool verify_rom(void) {
    if (!hw_cart.prg || !hw_cart.chr || hw_cart.prg_len < 0x40000 || !hw_cart.chr_len) return false;
    for (int i = 0; i < TILESET_COUNT; i++)
        if (rom30((uint16_t)(ROM_PAGE_A000_BY_TILESET + i)) != PAGE_A000_EXPECTED[i]) return false;
    /* TS1 (plains) layout pointer is $A000 */
    return rom30(ROM_TILELAYOUT_BY_TILESET + 2) == 0x00 && rom30(ROM_TILELAYOUT_BY_TILESET + 3) == 0xA0;
}

/* The tileset's metatile -> 8x8 table: four 256-byte arrays UL, LL, UR, LR. */
static const uint8_t *tile_layout(int tileset) {
    uint16_t at = (uint16_t)(ROM_TILELAYOUT_BY_TILESET + tileset * 2);
    uint16_t ptr = (uint16_t)(rom30(at) | (rom30((uint16_t)(at + 1)) << 8));
    int bank = ptr >= 0xC000 ? rom30((uint16_t)(ROM_PAGE_C000_BY_TILESET + tileset))
                             : rom30((uint16_t)(ROM_PAGE_A000_BY_TILESET + tileset));
    uint32_t off = (uint32_t)bank * 0x2000u + (ptr & 0x1FFFu);
    if (off + 1024 > hw_cart.prg_len) return NULL;
    return hw_cart.prg + off;
}

/* Background tiles through the playfield's banks. The status bar's IRQ swaps
 * the PPU's BG banks late in the frame, so the live mapping can't be used;
 * the game's own selections can. */
static void decode_tiles(void) {
    uint8_t sel[2] = { ram(RAM_PATTABLE_BANKSEL), ram(RAM_PATTABLE_BANKSEL + 1) };
    for (int t = 0; t < 256; t++) {
        uint32_t base = ((uint32_t)(sel[t >> 7] & 0xFE) * 0x400u + (uint32_t)(t & 127) * 16u) % hw_cart.chr_len;
        for (int r = 0; r < 8; r++) {
            uint8_t lo = hw_cart.chr[(base + r) % hw_cart.chr_len];
            uint8_t hi = hw_cart.chr[(base + r + 8) % hw_cart.chr_len];
            for (int c = 0; c < 8; c++)
                s_pixels[t][r][c] = (uint8_t)(((lo >> (7 - c)) & 1) | (((hi >> (7 - c)) & 1) << 1));
        }
    }
}

/* Unwrap a line's nametable X (mod 256: SMB3's levels are horizontally
 * mirrored) against the level camera, which may already be a frame ahead. */
static int world_x(int ppu_x, int cam) {
    return cam + (((ppu_x - cam) + 128) & 255) - 128;
}

/* First status-bar line: where the scroll stops following the playfield. */
static int hud_top(void) {
    for (int y = 120; y < 240; y++) {
        int sy0 = cyc_render_line_scroll_y(y - 1), sy1 = cyc_render_line_scroll_y(y);
        if (cyc_render_line_scroll_x(y) != cyc_render_line_scroll_x(y - 1) ||
            (sy1 != sy0 + 1 && !(sy0 == 479 && sy1 == 0)) ||
            cyc_render_line_bg_table(y) != cyc_render_line_bg_table(y - 1))
            return y;
    }
    return 240;
}

static int compose(uint32_t *out, int width, int height, int native_x0, const uint32_t *native, void *user) {
    (void)user;
    (void)native_x0;
    int tileset = ram(RAM_LEVEL_TILESET);
    if (!s_rom_ok || tileset == 0 || tileset >= TILESET_COUNT || ram(RAM_LEVEL_7VERTICAL)) return 0;
    if (!(cyc_render_line_mask(100) & 0x08) || cyc_render_line_bg_table(100) != 0) return 0;
    const uint8_t *layout = tile_layout(tileset);
    size_t wram_len = 0;
    const uint8_t *tile_mem = cyc_cart_ram(&wram_len);
    if (!layout || !tile_mem || wram_len < TILE_MEM_SCREEN * TILE_MEM_SCREENS) return 0;

    int hud = hud_top();
    if (hud < 160) return 0; /* not the usual playfield/status-bar split */
    decode_tiles();

    int cam = (ram(RAM_HORZ_SCROLL_HI) << 8) | ram(RAM_HORZ_SCROLL);
    int level_px = (ram(RAM_LEVEL_WIDTH) + 1) * 256;
    if (level_px > TILE_MEM_SCREENS * 256) level_px = TILE_MEM_SCREENS * 256;

    /* Camera: centred on the native view, held at the level's ends. */
    int w0 = world_x(cyc_render_line_scroll_x(100), cam);
    int view = w0 - (width - 256) / 2;
    if (level_px <= width) view = -(width - level_px) / 2;
    else if (view < 0) view = 0;
    else if (view > level_px - width) view = level_px - width;
    int x0 = w0 - view;
    cyc_render_set_native_origin(x0);

    uint32_t backdrop = cyc_render_color(0);
    for (int y = 0; y < hud; y++) {
        int wx0 = world_x(cyc_render_line_scroll_x(y), cam) - x0;
        int wy = cyc_render_line_scroll_y(y);
        int row = wy >> 4, py = wy & 15;
        uint32_t *line = out + y * width;
        uint8_t *cov = s_opaque + y * width;
        for (int x = 0; x < width; x++) {
            int wx = wx0 + x;
            uint32_t color = backdrop;
            uint8_t opaque = 0;
            if (wx >= 0 && wx < level_px && row < TILE_MEM_ROWS) {
                int scr = wx >> 8, col = (wx >> 4) & 15, px = wx & 15;
                uint8_t m = tile_mem[scr * TILE_MEM_SCREEN + row * 16 + col];
                int quad = (px >> 3) * 2 + (py >> 3); /* UL, LL, UR, LR */
                uint8_t t = layout[quad * 256 + m];
                uint8_t p = s_pixels[t][py & 7][px & 7];
                if (p) { color = cyc_render_color((m >> 6) * 4 + p); opaque = 1; }
            }
            line[x] = color;
            cov[x] = opaque;
        }
    }
    for (int y = hud; y < height; y++) memset(s_opaque + y * width, 1, (size_t)width);

    if (s_check) {
        static uint32_t bg[256 * 240];
        if (cyc_render_background(bg)) {
            for (int y = 8; y < hud; y++)
                for (int x = 8; x < 248; x++) {
                    int cx = x0 + x;
                    if (cx < 0 || cx >= width) continue;
                    s_check_px++;
                    s_check_bad += out[y * width + cx] != bg[y * 256 + x];
                }
        }
    }

    cyc_render_sprites(out, width, height, x0, s_opaque, NULL, NULL);

    /* Native playfield, minus the masked/garbled edge columns. */
    for (int y = 0; y < hud; y++)
        for (int x = 8; x < 248; x++) {
            int cx = x0 + x;
            if (cx >= 0 && cx < width) out[y * width + cx] = native[y * 256 + x];
        }
    /* Status bar centred, its edge colours carried out to the sides (from
     * inside the game's masked left 8 columns, which show only backdrop). */
    int hx0 = (width - 256) / 2;
    for (int y = hud; y < height; y++) {
        uint32_t *line = out + y * width;
        const uint32_t *src = native + y * 256;
        for (int x = 0; x < hx0 + 8; x++) line[x] = src[8];
        memcpy(line + hx0 + 8, src + 8, 248 * sizeof(uint32_t));
        for (int x = hx0 + 256; x < width; x++) line[x] = src[255];
    }

    if (s_check && (++s_frames == 1 || s_frames % 60 == 0)) {
        fprintf(stderr, "[smb3-ws] check: %llu/%llu background pixels differ (hud_top %d, view %d, level %dpx)\n",
                (unsigned long long)s_check_bad, (unsigned long long)s_check_px, hud, view, level_px);
        s_check_bad = s_check_px = 0;
    }
    return 1;
}

static void apply(void) {
    if (s_mode != NES_VIDEO_STOCK && s_rom_ok) {
        cyc_render_set_compositor(compose, NULL);
        cyc_video_set_mode(s_mode);
    } else {
        cyc_render_set_compositor(NULL, NULL);
        cyc_video_set_mode(NES_VIDEO_STOCK);
    }
    cyc_render_capture_background(s_check);
}

void smb3_ws_power_on(void) {
    s_rom_ok = verify_rom();
    if (!s_rom_ok && s_mode != NES_VIDEO_STOCK)
        fprintf(stderr, "[smb3-ws] ROM tables not recognised (needs SMB3 USA Rev 1); widescreen off\n");
    apply();
}

void smb3_ws_set_mode(int mode) {
    s_mode = mode;
    apply();
}

int smb3_ws_mode(void) { return s_mode; }

void smb3_ws_set_check(bool on) {
    s_check = on;
    apply();
}
