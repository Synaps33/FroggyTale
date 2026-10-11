#ifndef BUTTERSCOTCH_HACKER_LOADER_H
#define BUTTERSCOTCH_HACKER_LOADER_H

#include <stdint.h>
#include <stdbool.h>
#include "libretro.h"

#ifdef __cplusplus
extern "C" {
#endif

void hacker_loader_set_video_cb(retro_video_refresh_t cb);
void hacker_loader_init(const char *rom_path);
void hacker_loader_log(const char *tag, const char *msg, int pct);

/* Diagnostics.  hacker_loader_log() is a no-op once the boot screen has been
 * handed over to the game, which is exactly when a hang is hardest to read: the
 * screen keeps the last pre-handover frame and looks frozen.  _force() ignores
 * the handover flag so a stall in the handover itself still advances the log,
 * and _spin() keeps a visible counter alive while a wait loop is stuck. */
void hacker_loader_log_force(const char *tag, const char *msg, int pct);
void hacker_loader_spin(const char *tag, const char *msg);

void hacker_loader_halt(const char *tag, const char *msg);
void hacker_loader_stop(void);
bool hacker_loader_is_active(void);

#ifdef __cplusplus
}
#endif

#endif /* BUTTERSCOTCH_HACKER_LOADER_H */
