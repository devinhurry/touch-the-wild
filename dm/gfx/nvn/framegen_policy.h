// Experimental frame generation is available only to a profile with validated scene/camera inputs.
#pragma once
#include <stdlib.h>
#include <string.h>
#include "game_profile.h"
static inline int dm_framegen_requested(void) {
    const char *value=getenv("DM_FRAMEGEN");
    const char *mode=getenv("DM_OPMODE");
    // Only the docked upscale boundary has been validated for this experiment.
    int docked=mode ? atoi(mode)!=0 : DM_GAME_OPMODE;
    return *DM_GAME_FRAMEGEN_SCENE && docked && value && !strcmp(value,"30");
}
extern int dm_fg_enabled(void);
static inline int dm_framegen_active(void) {
    return dm_framegen_requested() && dm_fg_enabled();
}
