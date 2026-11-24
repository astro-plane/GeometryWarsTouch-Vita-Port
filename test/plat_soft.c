/* plat_soft.c - headless implementation of plat.h for automated tests:
 * a software renderer into a 960x544 buffer, and a log of sounds played. */
#include <math.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/plat.h"

#define W 960
#define H 544

struct PlatTex { unsigned char *px; int w, h; };
static unsigned char fb[W * H * 4];
int soft_render = 0, soft_live_tex = 0;
double soft_time = 0;
char soft_log[8192];                 /* the most recent sounds (oldest dropped when full) */

int plat_init(int *w, int *h) { *w = W; *h = H; return 0; }
void plat_shutdown(void) {}
void plat_poll(PlatInput *in) { (void)in; }
double plat_time(void) { return soft_time; }
const char *plat_data_dir(void) { const char *e = getenv("GW_DATA"); return e && e[0] ? e : "app_data/"; }
const char *plat_save_file(void) { return "/tmp/gwvita_test_save.txt"; }

PlatTex *plat_tex_create(const unsigned char *rgba, int w, int h)
{
    PlatTex *t = malloc(sizeof *t);
    t->px = malloc((size_t)w * h * 4);
    memcpy(t->px, rgba, (size_t)w * h * 4);
    t->w = w; t->h = h;
    soft_live_tex++;
    return t;
}
void plat_tex_free(PlatTex *t) { if (t) { free(t->px); free(t); soft_live_tex--; } }

void plat_begin(void)
{
    int i;
    if (!soft_render) return;
    for (i = 0; i < W * H; i++) { fb[i*4] = 0; fb[i*4+1] = 0; fb[i*4+2] = 0; fb[i*4+3] = 255; }
}
void plat_end(void) {}

static void put(int x, int y, float r, float g, float b, float a, int add)
{
    unsigned char *d;
    if (x < 0 || y < 0 || x >= W || y >= H || a <= 0) return;
    d = &fb[(y * W + x) * 4];
    if (add) {
        float v;
        v = d[0] + r * a; d[0] = (unsigned char)(v > 255 ? 255 : v);
        v = d[1] + g * a; d[1] = (unsigned char)(v > 255 ? 255 : v);
        v = d[2] + b * a; d[2] = (unsigned char)(v > 255 ? 255 : v);
    } else {
        d[0] = (unsigned char)(d[0] + (r - d[0]) * a);
        d[1] = (unsigned char)(d[1] + (g - d[1]) * a);
        d[2] = (unsigned char)(d[2] + (b - d[2]) * a);
    }
}

static void draw_one(PlatTex *t, int sx, int sy, int sw, int sh, float cx, float cy,
                     float w, float h, float angle_deg, unsigned rgba, int blend)
{
    float rad = angle_deg * 3.14159265f / 180.0f, c = cosf(rad), s = sinf(rad);
    float ex = (fabsf(w * c) + fabsf(h * s)) / 2, ey = (fabsf(w * s) + fabsf(h * c)) / 2;
    float mr = (rgba >> 24) / 255.0f, mg = ((rgba >> 16) & 255) / 255.0f, mb = ((rgba >> 8) & 255) / 255.0f, ma = (rgba & 255) / 255.0f;
    int x0 = (int)floorf(cx - ex), x1 = (int)ceilf(cx + ex), y0 = (int)floorf(cy - ey), y1 = (int)ceilf(cy + ey), x, y;
    if (!soft_render || !t || w <= 0 || h <= 0) return;
    if (sx < 0 || sy < 0 || sx + sw > t->w || sy + sh > t->h) { fprintf(stderr, "SRC RECT OUT OF TEXTURE %d,%d %dx%d in %dx%d\n", sx, sy, sw, sh, t->w, t->h); return; }
    if (y0 < 0) y0 = 0;
    if (y1 > H) y1 = H;
    if (x0 < 0) x0 = 0;
    if (x1 > W) x1 = W;
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            float u = (dx * c + dy * s) / w + 0.5f, v = (-dx * s + dy * c) / h + 0.5f;
            const unsigned char *p;
            if (u < 0 || v < 0 || u >= 1 || v >= 1) continue;
            p = &t->px[((sy + (int)(v * sh)) * t->w + sx + (int)(u * sw)) * 4];
            put(x, y, p[0] * mr, p[1] * mg, p[2] * mb, blend == 2 ? 1.0f : p[3] / 255.0f * ma, blend == 1);
        }
}

int soft_sprites;                            /* sprites drawn since the last reset */
void plat_sprites(PlatTex *t, int sx, int sy, int sw, int sh, const PlatSprite *s, int n, int blend)
{
    int i;
    soft_sprites += n;
    for (i = 0; i < n; i++)
        draw_one(t, sx, sy, sw, sh, s[i].cx, s[i].cy, s[i].w, s[i].h, s[i].ang * 180.0f / 3.14159265f, s[i].rgba, blend);
}

struct PlatBatch { PlatTex *t; int sx, sy, sw, sh, n, max; PlatLine *l; };
PlatBatch *plat_batch_new(int max_quads)
{
    PlatBatch *b = calloc(1, sizeof *b);
    b->l = malloc((size_t)max_quads * sizeof *b->l);
    b->max = max_quads;
    return b;
}
void plat_batch_free(PlatBatch *b) { if (b) { free(b->l); free(b); } }
void plat_batch_lines(PlatBatch *b, PlatTex *t, int sx, int sy, int sw, int sh, const PlatLine *l, int n)
{
    if (n > b->max) n = b->max;
    b->t = t; b->sx = sx; b->sy = sy; b->sw = sw; b->sh = sh; b->n = n;
    memcpy(b->l, l, (size_t)n * sizeof *l);
}
void plat_batch_draw(PlatBatch *b, int blend)
{
    int i;
    if (!b || !b->t) return;
    soft_sprites += b->n;
    for (i = 0; i < b->n; i++) {
        const PlatLine *p = &b->l[i];
        float dx = p->x1 - p->x0, dy = p->y1 - p->y0;
        draw_one(b->t, b->sx, b->sy, b->sw, b->sh, (p->x0 + p->x1) / 2, (p->y0 + p->y1) / 2,
                 sqrtf(dx * dx + dy * dy) + p->w, p->w, atan2f(dy, dx) * 180.0f / 3.14159265f, p->rgba, blend);
    }
}

void plat_fill(float x, float y, float w, float h, unsigned rgba)
{
    int xi, yi;
    if (!soft_render) return;
    for (yi = (int)y; yi < (int)(y + h); yi++)
        for (xi = (int)x; xi < (int)(x + w); xi++)
            put(xi, yi, (float)(rgba >> 24), (float)((rgba >> 16) & 255), (float)((rgba >> 8) & 255), (rgba & 255) / 255.0f, 0);
}

void plat_sfx(const char *name)
{
    {
        size_t need = strlen(name) + 2, have = strlen(soft_log);
        if (have + need >= sizeof soft_log) {   /* drop the older half */
            size_t cut = have / 2;
            memmove(soft_log, soft_log + cut, have - cut + 1);
        }
        strcat(soft_log, name); strcat(soft_log, " ");
    }
}

int soft_music_on;
char soft_music_path[512];
int plat_music_play(const char *path)
{
    FILE *f = fopen(path, "rb");
    snprintf(soft_music_path, sizeof soft_music_path, "%s", path);
    if (!f) { soft_music_on = 0; return -1; }
    fclose(f);
    soft_music_on = 1;
    return 0;
}
void plat_note(const char *what) { (void)what; }

int plat_loading(void) { return 0; }
void plat_music_stop(void) { soft_music_on = 0; }
void plat_music_pause(int p) { (void)p; }
int plat_music_done(void) { return !soft_music_on; }

void soft_dump(const char *path)
{
    png_image img;
    memset(&img, 0, sizeof img);
    img.version = PNG_IMAGE_VERSION;
    img.width = W; img.height = H; img.format = PNG_FORMAT_RGBA;
    png_image_write_to_file(&img, path, 0, fb, 0, NULL);
}

/* jobs run on the spot here: the tests stay deterministic */
void plat_job_start(void (*fn)(void *), void *arg) { fn(arg); }
void plat_job_wait(void) {}
