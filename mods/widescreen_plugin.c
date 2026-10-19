/* The launcher's Mods screen: Widescreen on/off with its aspect and enemy
 * options (mods/preloaded/.../manifest.toml). */
#include <stdio.h>
#include <string.h>

#include "mod_runtime.h"
#include "cyc_video.h"
#include "smb3_widescreen.h"

#define PACKAGE "super-mario-bros-3.enhancement.widescreen"

static void reset_widescreen(void) {
    smb3_ws_set_mode(NES_VIDEO_STOCK);
    smb3_ws_objects_set_wide(true);
}

static void activate_widescreen(void) {
    char aspect[16] = "fit", enemies[16] = "wide";
    nes_mod_option_value(PACKAGE, "widescreen", "aspect", aspect, sizeof aspect);
    nes_mod_option_value(PACKAGE, "widescreen", "enemies", enemies, sizeof enemies);
    int mode = !strcmp(aspect, "16-9")   ? NES_VIDEO_16_9
             : !strcmp(aspect, "21-9")   ? NES_VIDEO_21_9
             : !strcmp(aspect, "32-9")   ? NES_VIDEO_32_9
                                         : NES_VIDEO_FIT;
    smb3_ws_objects_set_wide(strcmp(enemies, "classic") != 0);
    smb3_ws_set_mode(mode);
}

NES_MOD_CONSTRUCTOR(register_widescreen_plugin) {
    if (!nes_mod_register_reset_callback(reset_widescreen) ||
        !nes_mod_register_activation_plugin("super-mario-bros-3.widescreen", activate_widescreen))
        fprintf(stderr, "[Mods] Failed to register the SMB3 widescreen plugin\n");
}
