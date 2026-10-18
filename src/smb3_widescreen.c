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
#include "smb3_ws_internal.h"

#include <stdio.h>
#include <stdlib.h>
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
#define RAM_UPDATE_SELECT    0x0100 /* $20 title/ending, $80 vertical, $C0 normal */
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
static bool s_rom_ok, s_check, s_raw;
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
    /* The map's page entry (11) is its runtime code; its layout, used while
     * the map loads, is Tile_Layout_TS0 at the start of PRG012. */
    if (tileset == 0) bank = 12;
    uint32_t off = (uint32_t)bank * 0x2000u + (ptr & 0x1FFFu);
    if (off + 1024 > hw_cart.prg_len) return NULL;
    return hw_cart.prg + off;
}

/* A pattern byte through the game's own bank selections (PatTable_BankSel:
 * 2K BG banks at $0000/$0800, 1K sprite banks at $1000-$1C00). The status
 * bar's IRQ swaps the PPU's live banks late in the frame; these are the
 * playfield's. */
static uint8_t chr_byte(uint16_t addr) {
    uint32_t off;
    addr &= 0x1FFF;
    if (addr < 0x1000) off = (uint32_t)(ram((uint16_t)(RAM_PATTABLE_BANKSEL + (addr >> 11))) & 0xFE) * 0x400u + (addr & 0x7FFu);
    else off = (uint32_t)ram((uint16_t)(RAM_PATTABLE_BANKSEL + 2 + ((addr - 0x1000) >> 10))) * 0x400u + (addr & 0x3FFu);
    return hw_cart.chr[off % hw_cart.chr_len];
}

static void decode_tiles(void) {
    for (int t = 0; t < 256; t++)
        for (int r = 0; r < 8; r++) {
            uint8_t lo = chr_byte((uint16_t)(t * 16 + r)), hi = chr_byte((uint16_t)(t * 16 + r + 8));
            for (int c = 0; c < 8; c++)
                s_pixels[t][r][c] = (uint8_t)(((lo >> (7 - c)) & 1) | (((hi >> (7 - c)) & 1) << 1));
        }
}

/* An 8-pixel-wide sprite row block of `rows` lines from pattern `pat`. */
static void sprite_tile(uint32_t *out, int width, int height, int x, int y, uint16_t pat, int rows_from, int palette,
                        bool hflip, bool vflip, bool behind, const uint8_t *opaque) {
    for (int r = 0; r < 8; r++) {
        int sy = y + rows_from + r;
        if (sy < 0 || sy >= height) continue;
        int pr = vflip ? 7 - r : r;
        uint8_t lo = chr_byte((uint16_t)(pat + pr)), hi = chr_byte((uint16_t)(pat + pr + 8));
        for (int c = 0; c < 8; c++) {
            int sx = x + c;
            if (sx < 0 || sx >= width) continue;
            int b = hflip ? c : 7 - c;
            int p = ((lo >> b) & 1) | (((hi >> b) & 1) << 1);
            if (!p || (behind && opaque[sy * width + sx])) continue;
            out[sy * width + sx] = cyc_render_color(16 + palette * 4 + p);
        }
    }
}

void smb3_ws_draw_sprite(uint32_t *out, int width, int height, int x, int y, uint8_t tile, uint8_t attr,
                         const uint8_t *opaque) {
    bool hflip = attr & 0x40, vflip = attr & 0x80, behind = attr & 0x20;
    int pal = attr & 3;
    if (cyc_render_line_sprite16(100)) {
        uint16_t base = (uint16_t)(((tile & 1) ? 0x1000 : 0) + (tile & 0xFE) * 16);
        sprite_tile(out, width, height, x, y, vflip ? base + 16 : base, 0, pal, hflip, vflip, behind, opaque);
        sprite_tile(out, width, height, x, y, vflip ? base : base + 16, 8, pal, hflip, vflip, behind, opaque);
    } else {
        sprite_tile(out, width, height, x, y, (uint16_t)(cyc_render_line_sprite_table(100) + tile * 16), 0, pal,
                    hflip, vflip, behind, opaque);
    }
}

/* What one horizontal-level frame needs: shared by the per-frame object pass
 * and the compositor, so both agree on the view. */
typedef struct {
    int tileset, hud, cam, level_px, w0, view, x0;
    const uint8_t *layout, *tile_mem;
} LevelFrame;

static int world_x(int ppu_x, int cam);
static int hud_top(void);

static bool level_frame(LevelFrame *f, int width) {
    if (!s_rom_ok || width <= 256) return false;
    f->tileset = ram(RAM_LEVEL_TILESET);
    if (f->tileset >= TILESET_COUNT || ram(RAM_LEVEL_7VERTICAL)) return false;
    if (!(cyc_render_line_mask(100) & 0x08) || cyc_render_line_bg_table(100) != 0) return false;
    f->layout = tile_layout(f->tileset);
    size_t wram_len = 0;
    f->tile_mem = cyc_cart_ram(&wram_len);
    if (!f->layout || !f->tile_mem || wram_len < TILE_MEM_SCREEN * TILE_MEM_SCREENS) return false;
    /* The world map (Tile_Mem + $110, shown from the lower nametable) draws
     * its status bar in the same nametable, so it ends where the stored rows
     * do rather than at a scroll split. */
    f->hud = hud_top();
    for (int y = 120; y < f->hud; y++)
        if (cyc_render_line_scroll_y(y) >= TILE_MEM_ROWS * 16) { f->hud = y; break; }
    if (f->hud < 160) return false; /* not the usual playfield/status-bar split */
    f->cam = (ram(RAM_HORZ_SCROLL_HI) << 8) | ram(RAM_HORZ_SCROLL);
    f->level_px = (ram(RAM_LEVEL_WIDTH) + 1) * 256;
    if (f->level_px > TILE_MEM_SCREENS * 256) f->level_px = TILE_MEM_SCREENS * 256;
    if (f->tileset == 0 && f->level_px <= 256) return false; /* World 1's framed one-screen map */
    /* Camera: centred on the native view, held at the level's ends. */
    f->w0 = world_x(cyc_render_line_scroll_x(100), f->cam);
    f->view = f->w0 - (width - 256) / 2;
    if (f->level_px <= width) f->view = -(width - f->level_px) / 2;
    else if (f->view < 0) f->view = 0;
    else if (f->view > f->level_px - width) f->view = f->level_px - width;
    f->x0 = f->w0 - f->view;
    return true;
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

/* Status bar centred on black (its own surround colour). The line where the
 * IRQ switches to it renders backdrop only; wide, that reads as a stray band,
 * so a backdrop-only line there is black too. */
static void draw_hud(uint32_t *out, int width, int height, int hud, const uint32_t *native) {
    const uint32_t black = 0xFF000000u, backdrop = cyc_render_color(0);
    int hx0 = (width - 256) / 2;
    for (int y = hud; y < height; y++) {
        uint32_t *line = out + y * width;
        const uint32_t *src = native + y * 256;
        bool blank = true;
        for (int x = 8; x < 256 && blank; x++) blank = src[x] == backdrop;
        for (int x = 0; x < width; x++) line[x] = black;
        if (!blank) memcpy(line + hx0 + 8, src + 8, 248 * sizeof(uint32_t));
    }
}

/* Smallest period (8/16/32) with which the `span` pixels from `start` repeat. */
static int edge_period(const uint32_t *src, int start, int dir) {
    static const int periods[] = { 8, 16, 32 };
    for (int k = 0; k < 3; k++) {
        int p = periods[k];
        bool ok = true;
        for (int i = 0; i < 2 * p && ok; i++) ok = src[start + dir * i] == src[start + dir * (i + p)];
        if (ok) return p;
    }
    return 0;
}

/* Title/ending: a fixed stage with no level behind it. Rows whose edges
 * repeat (curtain, checkered floor) tile outward in phase. Elsewhere the
 * scenery at each edge (the hills, cut by the original frame) is mirrored
 * outward from the sprite-free background, up to TITLE_MIRROR pixels, which
 * stays clear of the logo and menu text; beyond that is backdrop. */
#define TITLE_MIRROR 56
/* Scenery to mirror: anything but the backdrop and the logo's blue shadow
 * fragments that touch the frame edge. */
static uint32_t title_scenery(uint32_t c, uint32_t backdrop) {
    int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
    return (b > r + 40 && b > g + 40) ? backdrop : c;
}
static int compose_title(uint32_t *out, int width, int height, int x0, const uint32_t *native) {
    static uint32_t bg[256 * 240];
    const uint32_t *scenery = cyc_render_background(bg) ? bg : native;
    uint32_t backdrop = cyc_render_color(0);
    for (int y = 0; y < height; y++) {
        const uint32_t *src = native + y * 256, *bgl = scenery + y * 256;
        uint32_t *line = out + y * width;
        int pl = edge_period(src, 8, 1), pr = edge_period(src, 255, -1);
        /* Native columns 0-7 are masked by the game; they continue the edge too. */
        int masked = (cyc_render_line_mask(y) & 0x02) ? 0 : 8;
        for (int x = 0; x < x0 + masked; x++) {
            int d = x0 + 8 - x; /* pixels left of native column 8 (1-based) */
            if (pl) line[x] = src[8 + (pl - d % pl) % pl];
            else line[x] = d <= TITLE_MIRROR ? title_scenery(bgl[8 + d - 1], backdrop) : backdrop;
        }
        for (int x = masked; x < 256; x++) line[x0 + x] = src[x];
        for (int x = x0 + 256; x < width; x++) {
            int d = x - (x0 + 255); /* pixels right of native column 255 (1-based) */
            if (pr) line[x] = src[255 - (pr - d % pr) % pr];
            else line[x] = d <= TITLE_MIRROR ? title_scenery(bgl[255 - d + 1], backdrop) : backdrop;
        }
    }
    return 1;
}

static int compose(uint32_t *out, int width, int height, int native_x0, const uint32_t *native, void *user) {
    (void)user;
    if (s_rom_ok && ram(RAM_UPDATE_SELECT) == 0x20) return compose_title(out, width, height, native_x0, native);
    LevelFrame f;
    if (!level_frame(&f, width)) return 0;
    const uint8_t *layout = f.layout, *tile_mem = f.tile_mem;
    int hud = f.hud, cam = f.cam, level_px = f.level_px, view = f.view, x0 = f.x0;
    decode_tiles();
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

    /* The frame's own sprites in the two native edge strips the rebuilt
     * background replaced, then objects the game kept off its screen. */
    const uint8_t *oam = cyc_render_oam();
    for (int i = 63; i >= 0; i--) {
        const uint8_t *s = oam + i * 4;
        if (s[0] >= 0xEF || (s[3] >= 8 && s[3] < 241)) continue;
        smb3_ws_draw_sprite(out, width, height, x0 + s[3], s[0] + 1, s[1], s[2], s_opaque);
    }
    smb3_ws_objects_draw(out, width, height, view, s_opaque);

    /* Native playfield, minus the masked/garbled edge columns. Where the
     * native pixel is plain background (it equals the sprite-free capture),
     * the exact rebuild stands, which also drops SMB3's right-edge attribute
     * glitch; sprites and anything else the PPU drew stay native. */
    static uint32_t bgcap[256 * 240];
    bool have_bg = cyc_render_background(bgcap);
    for (int y = 0; y < hud && !s_raw; y++)
        for (int x = 8; x < 248; x++) {
            int cx = x0 + x;
            if (cx < 0 || cx >= width) continue;
            uint32_t n = native[y * 256 + x];
            if (!have_bg || n != bgcap[y * 256 + x]) out[y * width + cx] = n;
        }
    draw_hud(out, width, height, hud, native);

    if (s_check && (++s_frames == 1 || s_frames % 60 == 0)) {
        fprintf(stderr, "[smb3-ws] check: %llu/%llu background pixels differ (hud_top %d, view %d, level %dpx)\n",
                (unsigned long long)s_check_bad, (unsigned long long)s_check_px, hud, view, level_px);
        s_check_bad = s_check_px = 0;
    }
    return 1;
}

void smb3_ws_frame_end(void) {
    Smb3WsView v = { 0 };
    LevelFrame f;
    int width = cyc_video_width();
    /* Normal horizontal play only ($C0); maps, bonus games and partitioned
     * screens keep their objects to the native screen. */
    if (s_mode != NES_VIDEO_STOCK && ram(RAM_UPDATE_SELECT) == 0xC0 && level_frame(&f, width) && f.tileset != 0) {
        v.active = true;
        v.width = width;
        v.native_world = f.w0;
        v.view_left = f.view;
    }
    smb3_ws_objects_frame_end(&v);
}

static void apply(void) {
    bool on = s_mode != NES_VIDEO_STOCK && s_rom_ok;
    cyc_render_set_compositor(on ? compose : NULL, NULL);
    cyc_video_set_mode(on ? s_mode : NES_VIDEO_STOCK);
    cyc_render_capture_background(on); /* the title's scenery is mirrored from the BG alone */
    if (!on) smb3_ws_objects_reset();
}

void smb3_ws_power_on(void) {
    smb3_ws_objects_reset();
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
    s_raw = on && getenv("SMB3_WS_RAW"); /* diagnostics: show the rebuilt view alone */
    apply();
}
