/* video_stub.c - debug only: no picture, for checking the translated logic on the Amiga (checksum). */
#include "video.h"
const DispCfg video_display = { .planes = 4, .scroll = 0, .fetch = 0, .view_h = 256, .buf_h = 256,
                                .level_w = 0, .buffers = 1, .double_y = 0, .sprite_w = 16, .sprite_bank = 1 };
int video_sprites_dropped;
ULONG vprof[4], vtiles;
int video_load(void) { return 1; }
void video_start(void) { }
void video_frame(void) { }
