/* The game's additions to the nesrecomp cycle host (cyc_host_extras.h).
 * Widescreen's persistent settings live in the launcher's Mods screen
 * (mods/widescreen_plugin.c); the in-game menu's View mode row switches it
 * live, and the command-line options override both. */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cyc_host_extras.h"
#include "cyc_video.h"
#include "smb1_port.h"
#include "smb3_qol.h"
#include "smb3_widescreen.h"

/* recomp-ui's view modes: 0 native, 1 fixed 16:9, 2 adaptive. */
enum { VIEW_NATIVE = 0, VIEW_16_9 = 1, VIEW_ADAPTIVE = 2 };

static void power_on(void *ctx) {
    (void)ctx;
    smb3_ws_power_on();
    smb3_qol_power_on();
}

static void frame_end(void *ctx) {
    (void)ctx;
    smb3_ws_frame_end();
}

static int get_view_mode(void *ctx) {
    (void)ctx;
    int m = smb3_ws_mode();
    return m == NES_VIDEO_STOCK ? VIEW_NATIVE : m == NES_VIDEO_16_9 ? VIEW_16_9 : VIEW_ADAPTIVE;
}

static bool set_view_mode(void *ctx, int mode) {
    (void)ctx;
    if (mode < VIEW_NATIVE || mode > VIEW_ADAPTIVE) return false;
    smb3_ws_set_mode(mode == VIEW_NATIVE ? NES_VIDEO_STOCK : mode == VIEW_16_9 ? NES_VIDEO_16_9 : NES_VIDEO_FIT);
    return true;
}

static bool option(void *ctx, const char *name, const char *value) {
    (void)ctx;
    if (!strcmp(name, "--widescreen-enemies")) {
        if (!value || (strcmp(value, "wide") && strcmp(value, "classic"))) return false;
        smb3_ws_objects_set_wide(!strcmp(value, "wide"));
        return true;
    }
    if (!strcmp(name, "--widescreen")) {
        int mode;
        if (!value || !nes_video_geometry_parse(value, &mode)) return false;
        smb3_ws_set_mode(mode);
        return true;
    }
    if (!strcmp(name, "--widescreen-hud")) {
        if (!value || (strcmp(value, "classic") && strcmp(value, "modern"))) return false;
        smb3_ws_set_modern_hud(!strcmp(value, "modern"));
        return true;
    }
    if (!strcmp(name, "--smb1-levels")) {
        if (!value) return false;
        smb1_levels_enable_from_path(value);
        return true;
    }
    if (!strcmp(name, "--widescreen-check")) {
        smb3_ws_set_check(true);
        return true;
    }
    return false;
}

const CycHostExtras *cyc_host_extras(void) {
    static const CycHostOption options[] = {
        { "--widescreen", true, "fit|16:9|21:9|32:9|off (overrides the Mods screen)" },
        { "--widescreen-enemies", true, "wide|classic: enemies live across the wide view or the 4:3 one" },
        { "--widescreen-hud", true, "classic|modern: bottom status bar, or taller view with a corner HUD" },
        { "--widescreen-check", false, "report rebuilt-vs-PPU background mismatches" },
        { "--smb1-levels", true, "Super Mario Bros. ROM: play its levels (overrides the Mods screen)" },
    };
    static const CycHostExtras x = {
        .power_on = power_on,
        .frame_end = frame_end,
        .view_modes = (1u << VIEW_NATIVE) | (1u << VIEW_16_9) | (1u << VIEW_ADAPTIVE),
        .get_view_mode = get_view_mode,
        .set_view_mode = set_view_mode,
        .options = options,
        .option_count = sizeof options / sizeof options[0],
        .option = option,
    };
    return &x;
}
