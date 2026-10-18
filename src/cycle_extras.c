/* The game's additions to the nesrecomp cycle host (cyc_host_extras.h). */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cyc_host_extras.h"
#include "cyc_video.h"
#include "smb3_widescreen.h"

static void power_on(void *ctx) {
    (void)ctx;
    smb3_ws_power_on();
}

static void frame_end(void *ctx) {
    (void)ctx;
    smb3_ws_frame_end();
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
    if (!strcmp(name, "--widescreen-check")) {
        smb3_ws_set_check(true);
        return true;
    }
    return false;
}

static void load_setting(void *ctx, const char *key, const char *value) {
    (void)ctx;
    int mode;
    if (!strcmp(key, "Widescreen") && nes_video_geometry_parse(value, &mode)) smb3_ws_set_mode(mode);
    if (!strcmp(key, "WidescreenEnemies")) smb3_ws_objects_set_wide(strcmp(value, "classic") != 0);
}

static void save_settings(void *ctx, FILE *f) {
    (void)ctx;
    fprintf(f, "Widescreen = %s\n", nes_video_geometry_name(smb3_ws_mode()));
    fprintf(f, "WidescreenEnemies = %s\n", smb3_ws_objects_wide() ? "wide" : "classic");
}

const CycHostExtras *cyc_host_extras(void) {
    static const CycHostOption options[] = {
        { "--widescreen", true, "fit|16:9|21:9|32:9|off (default fit)" },
        { "--widescreen-enemies", true, "wide|classic: enemies live across the wide view (default) or the 4:3 one" },
        { "--widescreen-check", false, "report rebuilt-vs-PPU background mismatches" },
    };
    static const CycHostExtras x = {
        .power_on = power_on,
        .frame_end = frame_end,
        .options = options,
        .option_count = sizeof options / sizeof options[0],
        .option = option,
        .load_setting = load_setting,
        .save_settings = save_settings,
    };
    static bool defaulted;
    if (!defaulted) { defaulted = true; smb3_ws_set_mode(NES_VIDEO_FIT); }
    return &x;
}
