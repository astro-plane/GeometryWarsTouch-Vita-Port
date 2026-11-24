/* plat.h - the platform layer the game is written against.
 * plat_sdl.c: SDL2 + libvorbis (PS Vita and desktop).
 * test/plat_soft.c: headless software renderer for automated tests. */
#ifndef PLAT_H
#define PLAT_H

typedef struct PlatTex PlatTex;

enum {
    BTN_UP = 1, BTN_DOWN = 2, BTN_LEFT = 4, BTN_RIGHT = 8,
    BTN_TRIANGLE = 16, BTN_CIRCLE = 32, BTN_CROSS = 64, BTN_SQUARE = 128,
    BTN_L = 256, BTN_R = 512, BTN_SELECT = 1024, BTN_START = 2048
};

#define MAX_TOUCH 6

typedef struct {
    int n;                                   /* fingers on the screen right now */
    struct { long long id; float x, y; } t[MAX_TOUCH];   /* screen pixels   */
    int held;                                /* BTN_* currently down            */
    int pressed;                             /* BTN_* that went down this frame */
    float lx, ly, rx, ry;                    /* sticks, -1..1, y down; no dead zone */
    int quit;
} PlatInput;

/* One textured quad: centre, size and rotation (radians, clockwise) in screen
 * pixels; rgba is 0xRRGGBBAA. */
typedef struct { float cx, cy, w, h, ang; unsigned rgba; } PlatSprite;

int plat_init(int *screen_w, int *screen_h);
void plat_shutdown(void);
void plat_poll(PlatInput *in);
double plat_time(void);
const char *plat_data_dir(void);             /* ends with '/' */
const char *plat_save_file(void);

PlatTex *plat_tex_create(const unsigned char *rgba, int w, int h);
void plat_tex_free(PlatTex *t);
void plat_begin(void);
void plat_end(void);
/* Many quads, all showing the sub-rectangle (sx,sy,sw,sh) of one texture, in
 * as few draw calls as the platform can.  blend: 0 normal alpha, 1 additive. */
void plat_sprites(PlatTex *t, int sx, int sy, int sw, int sh, const PlatSprite *s, int n, int blend);
void plat_fill(float x, float y, float w, float h, unsigned rgba);

/* A bar from (x0,y0) to (x1,y1), w pixels thick, its ends carried w/2 past
 * the two points (as a PlatSprite of length len + w would be). */
typedef struct { float x0, y0, x1, y1, w; unsigned rgba; } PlatLine;

/* A block of quads built away from the game's core: plat_batch_lines may be
 * called from inside a job (it only does arithmetic into the batch's own
 * memory, sized once by plat_batch_new), and plat_batch_draw, on the game's
 * thread, adds the finished block to the frame as one copy. */
typedef struct PlatBatch PlatBatch;
PlatBatch *plat_batch_new(int max_quads);
void plat_batch_free(PlatBatch *b);
void plat_batch_lines(PlatBatch *b, PlatTex *t, int sx, int sy, int sw, int sh, const PlatLine *l, int n);
void plat_batch_draw(PlatBatch *b, int blend);

/* One background job at a time on a helper core (plat_sdl.c), or run on the
 * spot (test/plat_soft.c, or when threads are off).  plat_job_wait returns
 * once it is done; a job must not call the drawing functions. */
void plat_job_start(void (*fn)(void *), void *arg);
void plat_job_wait(void);

/* The frame loop.  The game (frame(), including its drawing calls, which are
 * recorded) runs on its own thread, while this thread hands the previous
 * frame to the GPU and waits for the display; frame() returns 0 to quit.
 * GW_THREADS=0 in the environment runs everything on one thread. */
void plat_run(int (*frame)(double dt, const PlatInput *in));

void plat_sfx(const char *name);             /* app_data/sfx/<name>.wav */
void plat_sfx_vol(const char *name, float vol);   /* the same at vol (0..1) */
int plat_loading(void);                      /* nonzero while the effects are still being loaded */
void plat_note(const char *what);            /* a word on what the game is doing, for log.txt */
int plat_music_play(const char *path);       /* Ogg Vorbis, 44.1 kHz; 0 on success */
void plat_music_stop(void);
void plat_music_pause(int paused);
int plat_music_done(void);                   /* nothing playing, or the end reached */

#endif
