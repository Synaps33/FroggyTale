/*
 * sf2000_fw.h - the slice of the HiChip stock firmware ABI that the Butterscotch
 * port needs, declared locally.
 *
 * sf2000_multicore already has include/stockfw.h with the full set, but the core
 * archive is compiled with -I only against the Butterscotch headers; keeping our own
 * small header avoids pulling the frontend's headers into the runner's include path.
 * Every symbol below is resolved either by the core linker script
 * (linker_scripts/bisrv_*_core.ld, which pins them to absolute addresses inside the
 * stock ROM) or by sf2000_multicore/src/libretro_frontend/lib.c, which provides sbrk
 * on top of the firmware's 64 MB scratch buffer at gp_buf_64m.
 */

#ifndef _BS_SF2000_FW_H_
#define _BS_SF2000_FW_H_

#include <stddef.h>
#include <stdint.h>

#define FS_O_RDONLY 0x0000

/* firmware (linker-script pinned) */
extern uint32_t os_get_tick_count(void);   /* milliseconds */
extern int      dly_tsk(unsigned ms);      /* cooperative sleep, ms */
extern int      fs_access(const char *path, int mode);
extern int      fs_open(const char *path, int oflag, int perms);
extern int64_t  fs_lseek(int fd, int64_t offset, int whence);
extern int      fs_close(int fd);

/* Hardware joypad registers updated by stock firmware interrupt */
extern volatile uint32_t g_joy_task_state;
extern volatile uint32_t g_joy_state;

/* multicore frontend (lib.c / debug.c) */
extern void *sbrk(ptrdiff_t incr);
extern void  xlog(const char *fmt, ...);
extern void  lcd_bsod(const char *fmt, ...) __attribute__((noreturn));

/* --- SD card layout, mirrors sf2000_multicore/include/file.h ---------------- */
#define SDCARD_DIRECTORY    "/mnt/sda1"
#define SYSTEM_DIRECTORY    SDCARD_DIRECTORY "/system/bios"
#define ASSETS_DIRECTORY    SDCARD_DIRECTORY "/system/assets"
#define LOGS_DIRECTORY      SDCARD_DIRECTORY "/system/logs"
#define SAVE_DIRECTORY      SDCARD_DIRECTORY "/system/saves"
#define CONFIG_DIRECTORY    SDCARD_DIRECTORY "/system/configs"
#define CORES_DIRECTORY     SDCARD_DIRECTORY "/system/Deimos/cores"
#define SAVESTATE_DIRECTORY SDCARD_DIRECTORY "/system/Deimos/savestates"
#define ROMS_DIRECTORY      SDCARD_DIRECTORY "/ROMS"

/* --- panel ------------------------------------------------------------------ */
#define SF2000_PANEL_W 320
#define SF2000_PANEL_H 240
#define SF2000_SAMPLE_RATE 22050

/* Heap ceiling, from sf2000_multicore's lib.c.  The firmware owns most of the 64 MB
 * scratch buffer; if we cross this the OOM path calls lcd_bsod() and never returns, so
 * the texture cache budget has to be tuned against it rather than discovered. */
#define SF2000_HEAP_LIMIT 0x03A00000u  /* 58 MiB on GB300V2, 60 MiB on SF2000 */

/* Audio output wrapper implemented in main.c */
void sf2000_send_audio(const int16_t *samples, size_t frames);

/* Input polling wrappers implemented in main.c */
void sf2000_poll_input(void);
int16_t sf2000_get_input_state(unsigned id);

#endif /* _BS_SF2000_FW_H_ */
