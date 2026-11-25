/* Win32 platform layer for the recompiled game: shared definitions. */
#ifndef W32_H
#define W32_H
#include "../rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

/* stdcall argument n (0-based) of the import being serviced; the return address is at [esp] */
#define ARG(n)     rt_r32(G_MEM, c->esp + 4 + 4 * (n))
#define RET(v, n)  do { c->eax = (uint32_t)(v); c->esp += 4 + 4 * (n); return; } while (0)   /* stdcall */
#define RETC(v)    do { c->eax = (uint32_t)(v); c->esp += 4; return; } while (0)              /* cdecl */
#define GP(a)      ((void *)(G_MEM + (uint32_t)(a)))
#define GS(a)      ((const char *)(G_MEM + (uint32_t)(a)))
#define GW(a)      ((const uint16_t *)(G_MEM + (uint32_t)(a)))
#define IMPL(dll, name) void imp_##dll##_##name(Ctx *c)

/* guest address-space layout */
#define PEB_ADDR      0x00010000u
#define PARAMS_ADDR   0x00011000u     /* command line, module path, environment block (64 KB) */
#define GDT_ADDR      0x0001f000u
#define TEB_BASE      0x00020000u     /* TEB k: TEB_BASE + k * 0x2000 (TEB page, then its TLS pointer array) */
#define MAX_THREADS   64
#define VM_LO         0x01000000u     /* VirtualAlloc arena */
#define VM_HI         0x7ffe0000u
#define THUNK_BASE    0xfff00000u     /* import thunks: THUNK_BASE + 16 * index (never mapped) */
#define MAX_THUNKS    4096

/* loader / process */
extern uint32_t w32_image_base, w32_image_size, w32_entry, w32_tls_index_addr, w32_tls_template, w32_tls_size, w32_tls_zero;
extern char     w32_game_dir[1024];                /* host path of the game folder ("C:\GOG Games\Dungeon Siege") */
extern int      w32_deterministic;                 /* harness: time and ids are deterministic */
int      w32_load(const char *exe_path);           /* map image, fill IAT, PEB, process heap; 0 on success */
void w32_free_thread_teb(uint32_t teb);
uint32_t w32_new_thread_teb(uint32_t stack_lo, uint32_t stack_hi);   /* TEB guest address */
uint32_t w32_thunk_for(const char *dll, const char *name);           /* dynamic import (GetProcAddress) */
uint32_t w32_thunk_register(const char *name, void (*fn)(Ctx *));   /* internal entry point (COM methods) */
void     w32_exit(Ctx *c, uint32_t code) __attribute__((noreturn));
extern volatile int w32_exited;
extern uint32_t w32_exit_code;

/* Action Mode (runtime/win32/action.c): the controller state the host writes, and what the host's overlay shows */
enum { PAD_A = 1, PAD_B = 2, PAD_X = 4, PAD_Y = 8, PAD_LB = 0x10, PAD_RB = 0x20, PAD_UP = 0x40, PAD_DOWN = 0x80, PAD_LEFT = 0x100,
       PAD_RIGHT = 0x200, PAD_MENU = 0x400, PAD_VIEW = 0x800, PAD_LS = 0x1000, PAD_RS = 0x2000, PAD_LT = 0x4000, PAD_RT = 0x8000 };
typedef struct { float lx, ly, rx, ry, lt, rt; uint32_t buttons; int connected; } ActionPad;   /* sticks -1..1, y up */
#define ACTION_SLOTS 6
typedef struct {
    int in_game, menu_open;                        /* in the world; a game window (inventory, options...) is open */
    char hero[40]; float life, life_max, mana, mana_max;
    struct { char name[28], key[12]; float cooldown, cost; int ready; } slot[ACTION_SLOTS];
    char toast[120]; double toast_until;           /* a short message in the middle of the screen (CLOCK_MONOTONIC s) */
    uint32_t seq;                                  /* bumped on every change */
} ActionHud;
extern volatile ActionPad action_pad;
extern volatile ActionHud action_hud;
int action_on(void);
extern uint8_t w32_keys[256];

/* virtual memory (guest) */
uint32_t vm_alloc(uint32_t addr, uint32_t size, uint32_t type, uint32_t prot);
int      vm_free(uint32_t addr, uint32_t size, uint32_t type);
int      vm_query(uint32_t addr, uint32_t out);                       /* writes MEMORY_BASIC_INFORMATION */
void     vm_register(uint32_t base, uint32_t size, uint32_t type);   /* image, low area */
int      vm_committed(uint32_t addr, uint32_t size);
uint64_t vm_hash(void);

/* heaps */
uint32_t heap_create(uint32_t initial, uint32_t max);
int      heap_destroy(uint32_t h);
uint32_t heap_alloc(uint32_t h, uint32_t flags, uint32_t size);
int      heap_free(uint32_t h, uint32_t p);
uint32_t heap_realloc(uint32_t h, uint32_t flags, uint32_t p, uint32_t size);
uint32_t heap_size(uint32_t h, uint32_t p);
extern uint32_t w32_process_heap;

/* handles */
enum { H_FREE, H_FILE, H_EVENT, H_MUTEX, H_THREAD, H_MAPPING, H_FIND, H_SNAPSHOT, H_PROCESS, H_SEMAPHORE };
typedef struct HObj { int type; int refs; void *p; } HObj;
uint32_t h_new(int type, void *p);
HObj    *h_get(uint32_t h, int type);
int      h_close(uint32_t h);

/* threads */
uint32_t w32_tid(Ctx *c);
void     w32_set_last_error(Ctx *c, uint32_t e);

/* text */
int  w32_mb_to_wide(uint32_t cp, const uint8_t *s, int n, uint16_t *out, int cap);
int  w32_wide_to_mb(uint32_t cp, const uint16_t *s, int n, uint8_t *out, int cap);
int  w32_host_path(const char *win, char *out, size_t cap, int for_create);  /* Windows path -> host path */

/* guest callbacks from inside an import (window procedures, enumerators, thread starts) */
uint32_t w32_callback(Ctx *c, uint32_t fn, int nargs, const uint32_t *args);   /* stdcall; returns eax */
extern void (*w32_callback_hook)(Ctx *c, uint32_t fn);   /* harness: run the guest function elsewhere */

/* tracing (harness) */
void w32_trace_import(Ctx *c, uint32_t index);
#endif
