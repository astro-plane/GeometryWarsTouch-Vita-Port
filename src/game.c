/* game.c - GWars:Touch (Evolved mode) on the PS Vita.
 *
 * The rules of play in this file were recovered from the iPhone game's own
 * program (GWTouchPad 1.2.0, armv7 slice) by disassembly and decompilation:
 * the spawn director, every enemy's movement, the black hole's pull, the gun,
 * geoms, scoring, lives and bombs.  Each rule names the function and address
 * it came from; the full write-ups are in the project's notes/ folder.  The
 * code itself is this project's own: none of the game's code, art or text is
 * copied in.  What is drawn (glowing vector lines) is our own rendering; the
 * sound effects and music are converted from the player's copy of the game.
 *
 * World: the arena is centred on (0,0), walls at x = +-300, y = +-200
 * (kMapWidth / kMapHeight).  The game runs at a fixed 60 ticks per second
 * (gTick).  Velocities are in units per second; Entity::Update (0x0002ad1c)
 * moves everything by velocity * dt * 0.75 after each unit's Tick, and the
 * steering and damping inside each Tick are applied once per tick. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "game.h"

#define GAME_TITLE "GWARS TOUCH"
#define PI 3.14159265f

#define AW 300.0f                            /* kMapWidth: half width */
#define AH 200.0f                            /* kMapHeight: half height */
#define TICK (1.0f / 60.0f)                  /* gTick */
#define MOVE 0.75f                           /* Entity::Update position scale */

/* ------------------------------------------------------------ state -- */

enum { SC_TITLE, SC_PLAY, SC_OVER };

static int scr_w, scr_h;
static float zoom, cam_x, cam_y;
static PlatTex *atlas;
static int scene, paused;
static double now_t, scene_t;                /* clock, and time in this scene */
static unsigned rng = 12345;
static const char *music_cur;
static unsigned frnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static float frand(void) { return (frnd() & 0xffffff) / 16777216.0f; }
static float frange(float a, float b) { return a + (b - a) * frand(); }
static float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }

/* ---------------------------------------------------------- drawing -- */

/* The atlas: a soft horizontal line (with soft ends), a soft dot, and a
 * solid patch.  Everything is drawn from these three. */
enum { K_LINE, K_DOT, K_SOLID };
static const int ksrc[3][4] = { { 0, 0, 64, 32 }, { 64, 0, 32, 32 }, { 106, 10, 12, 12 } };   /* well inside the solid patch */

/* A frame is drawn in two lists that are built at the same time on two
 * cores: the grid (by a helper, see grid_job) and everything else. Both are
 * only handed to the platform at the end of the frame, grid first. The main
 * list is a series of runs: sprites of one kind and blend, or a filled
 * rectangle. */
#define MAX_SPR 32768
#define MAX_RUN 1024
static PlatSprite spr[MAX_SPR];
static struct { int kind, blend, start, n; float fx, fy, fw, fh; unsigned fc; } run[MAX_RUN];
static int n_spr, n_run, cur_kind = -1, cur_blend = -1, run_start;
static PlatSprite spare;                     /* where sprites go once the list is full */

static void flush(void)                      /* close the current run */
{
    if (n_spr > run_start && cur_kind >= 0 && n_run < MAX_RUN) {
        run[n_run].kind = cur_kind; run[n_run].blend = cur_blend;
        run[n_run].start = run_start; run[n_run].n = n_spr - run_start;
        n_run++;
    }
    run_start = n_spr;
}

static PlatSprite *push(int kind, int blend)
{
    if (kind != cur_kind || blend != cur_blend) {
        flush();
        cur_kind = kind;
        cur_blend = blend;
    }
    if (n_spr == MAX_SPR) return &spare;
    return &spr[n_spr++];
}

static void draw_reset(void)
{
    n_spr = n_run = run_start = 0;
    cur_kind = cur_blend = -1;
}

static void draw_submit(void)                /* the main list, in order */
{
    int r;
    flush();
    for (r = 0; r < n_run; r++) {
        int kd = run[r].kind;
        if (kd < 0)
            plat_fill(run[r].fx, run[r].fy, run[r].fw, run[r].fh, run[r].fc);
        else
            plat_sprites(atlas, ksrc[kd][0], ksrc[kd][1], ksrc[kd][2], ksrc[kd][3],
                         spr + run[r].start, run[r].n, run[r].blend);
    }
}

static unsigned with_alpha(unsigned c, float a)
{
    return (c & 0xffffff00u) | (unsigned)(clampf(a, 0, 1) * (c & 255));
}

/* a segment in screen pixels, of width w */
static void sline(float x0, float y0, float x1, float y1, float w, unsigned c)
{
    float dx = x1 - x0, dy = y1 - y0;
    PlatSprite *s = push(K_LINE, 1);
    s->cx = (x0 + x1) / 2; s->cy = (y0 + y1) / 2;
    s->w = sqrtf(dx * dx + dy * dy) + w; s->h = w;
    s->ang = atan2f(dy, dx);
    s->rgba = c;
}

/* a glowing segment: a wide faint halo and a thin bright core */
static void gline(float x0, float y0, float x1, float y1, unsigned c, float a)
{
    float k = scr_h / 544.0f;
    sline(x0, y0, x1, y1, 14 * k, with_alpha(c, a * 0.38f));
    sline(x0, y0, x1, y1, 2.4f * k, with_alpha(c, a));
}

static void sdot(float x, float y, float size, unsigned c)
{
    PlatSprite *s = push(K_DOT, 1);
    s->cx = x; s->cy = y; s->w = s->h = size; s->ang = 0; s->rgba = c;
}

static void sfill(float x, float y, float w, float h, unsigned c)
{
    flush();
    cur_kind = cur_blend = -1;               /* the next sprite starts a new run */
    if (n_run < MAX_RUN) {
        run[n_run].kind = -1; run[n_run].start = run[n_run].n = 0;
        run[n_run].fx = x; run[n_run].fy = y; run[n_run].fw = w; run[n_run].fh = h; run[n_run].fc = c;
        n_run++;
    }
}

static float wx(float x) { return (x - cam_x) * zoom + scr_w / 2.0f; }
static float wy(float y) { return (y - cam_y) * zoom + scr_h / 2.0f; }

/* an outline of n points (unit coordinates), centred at screen (x,y), scaled
 * by r pixels, turned by ang radians */
static void sshape(float x, float y, float r, float ang, const float *pts, int n, int closed, unsigned c, float a)
{
    float cs = cosf(ang), sn = sinf(ang), lx = 0, ly = 0, fx = 0, fy = 0;
    int i;
    for (i = 0; i < n; i++) {
        float ux = pts[i * 2], uy = pts[i * 2 + 1];
        float sx = x + (ux * cs - uy * sn) * r, sy = y + (ux * sn + uy * cs) * r;
        if (i) gline(lx, ly, sx, sy, c, a);
        else { fx = sx; fy = sy; }
        lx = sx; ly = sy;
    }
    if (closed) gline(lx, ly, fx, fy, c, a);
}

static void wshape(float x, float y, float r, float ang, const float *pts, int n, int closed, unsigned c, float a)
{
    sshape(wx(x), wy(y), r * zoom, ang, pts, n, closed, c, a);
}

static void wcircle(float x, float y, float r, int segs, unsigned c, float a)
{
    float lx = 0, ly = 0;
    int i;
    for (i = 0; i <= segs; i++) {
        float t = i * 2 * PI / segs, sx = wx(x + cosf(t) * r), sy = wy(y + sinf(t) * r);
        if (i) gline(lx, ly, sx, sy, c, a);
        lx = sx; ly = sy;
    }
}

/* ------------------------------------------------------------- text -- */

/* A stroke font on a 4 x 6 grid: each glyph is segments "x0y0x1y1". */
static const char *glyph(char c)
{
    switch (c) {
    case '0': case 'O': return "0040 4046 4606 0600";
    case '1': return "2026 1020 1636";
    case '2': return "0040 4043 4303 0306 0646";
    case '3': return "0040 4046 4606 1343";
    case '4': return "0003 0343 4046";
    case '5': case 'S': return "4000 0003 0343 4346 4606";
    case '6': return "4000 0006 0646 4643 4303";
    case '7': return "0040 4046";
    case '8': return "0040 4046 4606 0600 0343";
    case '9': return "4303 0300 0040 4046 4606";
    case 'A': return "0620 2046 1333";
    case 'B': return "0006 0030 3041 4142 4233 0333 3344 4445 4536 3606";
    case 'C': return "4000 0006 0646";
    case 'D': return "0030 3041 4145 4536 3606 0600";
    case 'E': return "4000 0006 0646 0333";
    case 'F': return "4000 0006 0333";
    case 'G': return "4000 0006 0646 4643 4323";
    case 'H': return "0006 4046 0343";
    case 'I': return "0040 2026 0646";
    case 'J': return "4046 4606 0604";
    case 'K': return "0006 0340 0346";
    case 'L': return "0006 0646";
    case 'M': return "0600 0023 2340 4046";
    case 'N': return "0600 0046 4640";
    case 'P': return "0600 0040 4043 4303";
    case 'Q': return "0040 4046 4606 0600 2446";
    case 'R': return "0600 0040 4043 4303 2346";
    case 'T': return "0040 2026";
    case 'U': return "0006 0646 4640";
    case 'V': return "0026 2640";
    case 'W': return "0006 0623 2346 4640";
    case 'X': return "0046 4006";
    case 'Y': return "0023 2340 2326";
    case 'Z': return "0040 4006 0646";
    case 'x': return "1236 3216";
    case ':': return "2122 2425";
    case '.': return "2526";
    case ',': return "2527";
    case '-': return "1333";
    case '!': return "2024 2526";
    case '/': return "4006";
    default: return "";
    }
}

static float text_w(const char *s, float size)
{
    float u = size / 6;
    int n = (int)strlen(s);
    return n ? n * 6 * u - 2 * u : 0;
}

/* align: 0 left, 1 centre, 2 right; size = letter height in pixels */
static void text(float x, float y, float size, int align, unsigned c, float a, const char *s)
{
    float u = size / 6;
    if (align) x -= text_w(s, size) / (align == 1 ? 2 : 1);
    for (; *s; s++, x += 6 * u) {
        const char *g = glyph(*s);
        while (g[0] && g[1] && g[2] && g[3]) {
            gline(x + (g[0] - '0') * u, y + (g[1] - '0') * u, x + (g[2] - '0') * u, y + (g[3] - '0') * u, c, a);
            g += 4;
            while (*g == ' ') g++;
        }
    }
}

static void fmt_num(char *buf, size_t n, long v)
{
    char t[32];
    int len, i, o = 0;
    snprintf(t, sizeof t, "%ld", v < 0 ? 0 : v);
    len = (int)strlen(t);
    for (i = 0; i < len && o + 2 < (int)n; i++) {
        if (i && (len - i) % 3 == 0) buf[o++] = ',';
        buf[o++] = t[i];
    }
    buf[o] = 0;
}

/* ------------------------------------------------------------- grid -- */

/* A sheet of springs under the arena (this port's own effect): each point is
 * tied to where it belongs and to its neighbours.  Explosions push it and
 * black holes pull it. */
#define GSP 12.5f
#define GX 49                                /* 2*AW / GSP + 1 */
#define GY 33                                /* 2*AH / GSP + 1 */
static float gpx[GY][GX], gpy[GY][GX], gvx[GY][GX], gvy[GY][GX];

static float g_homex(int i) { return -AW + i * GSP; }
static float g_homey(int j) { return -AH + j * GSP; }

static void grid_reset(void)
{
    int i, j;
    for (j = 0; j < GY; j++)
        for (i = 0; i < GX; i++) {
            gpx[j][i] = g_homex(i); gpy[j][i] = g_homey(j);
            gvx[j][i] = gvy[j][i] = 0;
        }
}

/* push (strength > 0) or pull (< 0) the sheet around (x,y) */
static void grid_force(float x, float y, float strength, float radius)
{
    int i0 = (int)((x - radius + AW) / GSP), i1 = (int)((x + radius + AW) / GSP) + 1;
    int j0 = (int)((y - radius + AH) / GSP), j1 = (int)((y + radius + AH) / GSP) + 1, i, j;
    if (i0 < 1) i0 = 1;
    if (j0 < 1) j0 = 1;
    if (i1 > GX - 2) i1 = GX - 2;
    if (j1 > GY - 2) j1 = GY - 2;
    for (j = j0; j <= j1; j++)
        for (i = i0; i <= i1; i++) {
            float dx = gpx[j][i] - x, dy = gpy[j][i] - y, d = sqrtf(dx * dx + dy * dy), f;
            if (d >= radius || d < 0.001f) continue;
            f = strength * (1 - d / radius) / d;
            gvx[j][i] += dx * f;
            gvy[j][i] += dy * f;
        }
}

static void grid_update(float dt)
{
    const float k_home = 45, k_link = 260, damp = 4.0f;
    int i, j, step;
    float h = dt / 2;
    for (step = 0; step < 2; step++) {
        float keep = expf(-damp * h);
        for (j = 1; j < GY - 1; j++)
            for (i = 1; i < GX - 1; i++) {
                float ax = (g_homex(i) - gpx[j][i]) * k_home +
                           (gpx[j][i - 1] + gpx[j][i + 1] + gpx[j - 1][i] + gpx[j + 1][i] - 4 * gpx[j][i]) * k_link;
                float ay = (g_homey(j) - gpy[j][i]) * k_home +
                           (gpy[j][i - 1] + gpy[j][i + 1] + gpy[j - 1][i] + gpy[j + 1][i] - 4 * gpy[j][i]) * k_link;
                gvx[j][i] = (gvx[j][i] + ax * h) * keep;
                gvy[j][i] = (gvy[j][i] + ay * h) * keep;
            }
        for (j = 1; j < GY - 1; j++)
            for (i = 1; i < GX - 1; i++) {
                gpx[j][i] += gvx[j][i] * h;
                gpy[j][i] += gvy[j][i] * h;
            }
    }
}

/* the grid's own sprite list, filled by grid_job (possibly on another core:
 * it touches nothing but the grid, the camera, and this list) */
#define MAX_GSPR (2 * (GX * GY * 2))
static PlatSprite gspr[MAX_GSPR];
static int n_gspr;

static void gsline(float x0, float y0, float x1, float y1, float w, unsigned c)
{
    float dx = x1 - x0, dy = y1 - y0;
    PlatSprite *s;
    if (n_gspr == MAX_GSPR) return;
    s = &gspr[n_gspr++];
    s->cx = (x0 + x1) / 2; s->cy = (y0 + y1) / 2;
    s->w = sqrtf(dx * dx + dy * dy) + w; s->h = w;
    s->ang = atan2f(dy, dx);
    s->rgba = c;
}

static void grid_seg(int i0, int j0, int i1, int j1, int major)
{
    float x0 = wx(gpx[j0][i0]), y0 = wy(gpy[j0][i0]), x1 = wx(gpx[j1][i1]), y1 = wy(gpy[j1][i1]);
    float dx = gpx[j0][i0] - g_homex(i0), dy = gpy[j0][i0] - g_homey(j0), d = sqrtf(dx * dx + dy * dy);
    float hot = clampf(d / 10, 0, 1);        /* stretched lines glow brighter */
    float k = scr_h / 544.0f;
    unsigned c = hot > 0.02f ? 0x5aa0ffffu : 0x2a5cd8ffu;
    gsline(x0, y0, x1, y1, 5 * k, with_alpha(c, (major ? 0.07f : 0.03f) + hot * 0.16f));
    gsline(x0, y0, x1, y1, 1.4f * k, with_alpha(c, (major ? 0.30f : 0.14f) + hot * 0.55f));
}

static void grid_draw(void)
{
    int i, j;
    n_gspr = 0;
    for (j = 0; j < GY; j++)
        for (i = 0; i < GX - 1; i++)
            grid_seg(i, j, i + 1, j, j % 4 == 0);
    for (i = 0; i < GX; i++)
        for (j = 0; j < GY - 1; j++)
            grid_seg(i, j, i, j + 1, i % 4 == 0);
}

/* The grid's work for one frame: move the springs, then build its sprites.
 * Runs on a helper core while this core draws everything else. Nothing may
 * touch the grid or the camera between plat_job_start and plat_job_wait. */
static float grid_dt;
static void grid_job(void *arg)
{
    (void)arg;
    if (grid_dt > 0) grid_update(grid_dt);
    grid_draw();
}

/* -------------------------------------------------------- particles -- */

#define MAX_PART 3000
typedef struct { float x, y, vx, vy, life, max; unsigned c; } Part;
static Part parts[MAX_PART];
static int n_parts;

static void spark(float x, float y, float vx, float vy, float life, unsigned c)
{
    Part *p;
    if (n_parts == MAX_PART) {               /* full: replace a random one */
        p = &parts[frnd() % MAX_PART];
    } else {
        p = &parts[n_parts++];
    }
    p->x = x; p->y = y; p->vx = vx; p->vy = vy; p->life = p->max = life; p->c = c;
}

static void burst(float x, float y, unsigned c, int n, float speed)
{
    int i;
    for (i = 0; i < n; i++) {
        float a = frand() * 2 * PI, s = speed * (0.25f + 0.75f * frand());
        spark(x, y, cosf(a) * s, sinf(a) * s, frange(0.5f, 1.3f), c);
    }
}

static void parts_update(float dt)
{
    int i;
    float keep = expf(-2.6f * dt);
    for (i = 0; i < n_parts; i++) {
        Part *p = &parts[i];
        p->life -= dt;
        if (p->life <= 0) {
            *p = parts[--n_parts];
            i--;
            continue;
        }
        p->vx *= keep; p->vy *= keep;
        p->x += p->vx * dt; p->y += p->vy * dt;
        if (p->x < -AW) { p->x = -AW; p->vx = -p->vx; }
        if (p->x > AW) { p->x = AW; p->vx = -p->vx; }
        if (p->y < -AH) { p->y = -AH; p->vy = -p->vy; }
        if (p->y > AH) { p->y = AH; p->vy = -p->vy; }
    }
}

static void parts_draw(void)
{
    int i;
    float k = scr_h / 544.0f;
    for (i = 0; i < n_parts; i++) {
        Part *p = &parts[i];
        float a = p->life / p->max, sp = sqrtf(p->vx * p->vx + p->vy * p->vy);
        float len = clampf(sp * 0.02f, 1.0f, 7) * zoom, x = wx(p->x), y = wy(p->y);
        if (sp < 1) sp = 1;
        sline(x - p->vx / sp * len, y - p->vy / sp * len, x, y, 2.2f * k, with_alpha(p->c, a));
    }
}

/* ============================================================= SIMULATION
 * Everything below follows the recovered rules; see the notes folder for the
 * write-ups.  Addresses are in the armv7 slice of GWTouchPad 1.2.0. */

static double sfx_last[64];
static void sfx(const char *name, int slot, double gap)  /* rate-limited per slot */
{
    if (slot >= 0 && slot < 64) {
        if (now_t - sfx_last[slot] < gap) return;
        sfx_last[slot] = now_t;
    }
    plat_sfx(name);
}

static float wrap_pi(float a)                /* the fmodf(a + pi, 2pi) - pi idiom used throughout */
{
    a = fmodf(a + PI, 2 * PI);
    if (a < 0) a += 2 * PI;
    return a - PI;
}

/* ---- units ---- */

/* entity types (EntityUnit::EntityUnit's third argument) that Evolved uses */
enum { U_NONE, U_GRUNT, U_WANDERER, U_WEAVER, U_SPINNER, U_TINY, U_SNAKE, U_BODY, U_WELL,
       U_PROTON, U_ROCKET, U_DUCK, U_GATE, U_REPULSAR,
       /* used by the other modes */
       U_HILL, U_MINE, U_MINELAYER, U_NUFO, U_METEOR, U_BLACKHOLE, U_TITAN, U_AMOEBA, U_CYTO,
       U_TYPES };

/* game modes (GiGameMode at GameInfo+0x794c is in the comment) */
enum { M_EVOLVED, M_DEADLINE, M_PACIFISM, M_WAVES, M_KING, M_SEQUENCE, M_MINELAYER, M_TITANS,
       M_WHIRLPOOL, M_METEOR, M_COUNT };
static const struct {
    const char *name;    /* Text-en.loc ID_MODE_* */
    int lives, bombs;    /* GameInfo +0x795c / +0x7960 from the Gr* function */
    int flags;           /* GameInfo +0x7964: 1 no extra ships from score, 2 no extra bombs */
    const char *music;
} modes[M_COUNT] = {
    [M_EVOLVED]   = { "EVOLVED",   3, 3, 0, "retro_evolved" },  /* GrEvolved 0x4e910, mode 1 */
    [M_DEADLINE]  = { "DEADLINE",  0, 3, 1, "deadline" },       /* GrCasual 0x4e58c, mode 0 */
    [M_PACIFISM]  = { "PACIFISM",  0, 0, 3, "pacifism" },       /* GrPacifism 0x4e7dc, mode 4 */
    [M_WAVES]     = { "WAVES",     0, 0, 3, "waves" },          /* GrWaves 0x4e83c, mode 3 */
    [M_KING]      = { "KING",      0, 0, 3, "king" },           /* GrKing 0x4e77c, mode 5 */
    [M_SEQUENCE]  = { "SEQUENCE",  3, 3, 0, "sequence" },       /* GrProgression 0x4e89c, mode 2 */
    [M_MINELAYER] = { "MINELAYER", 0, 0, 3, "minelayer" },      /* GrMinelayer 0x4e71c, mode 8 */
    [M_TITANS]    = { "TITANS",    3, 3, 0, "titans" },         /* GrAsteroids 0x4e6c4, mode 9 */
    [M_WHIRLPOOL] = { "WHIRLPOOL", 0, 0, 3, "blackhole" },      /* GrWhirlpool 0x4e664, mode 10 */
    [M_METEOR]    = { "METEOR",    0, 0, 3, "meteor" },         /* GrMeteor 0x4e604, mode 11 */
};
static int mode;

/* damage types (DamageInfo +8) */
enum { DMG_TOUCH = 0, DMG_BULLET = 1, DMG_EATEN = 2, DMG_WELLBOMB = 3, DMG_BOMB = 4, DMG_GATE = 5 };

static const struct {
    unsigned color;      /* Unit.<name>.Colour tuning, normalised */
    float r;             /* collision radius from the constructor */
    int points;          /* AddScore base points (0x0006046c) */
    int geoms;           /* EmitMotes count (0x0003705c) */
    const char *spawn_sfx;
} ut[U_TYPES] = {
    [U_GRUNT]    = { 0x2a6dffffu, 6.5f,  50, 2, "Enemy_grunt_spawn" },
    [U_WANDERER] = { 0x8040ffffu, 7.0f,  25, 2, "Wanderer_spawn" },
    [U_WEAVER]   = { 0x40ff10ffu, 6.0f, 100, 2, "Enemy_weaver_spawn" },
    [U_SPINNER]  = { 0xff1affffu, 7.0f, 100, 1, "Enemy_splitter_spawn" },
    [U_TINY]     = { 0x5a30ffffu, 4.0f,  50, 1, NULL },
    [U_SNAKE]    = { 0x3a4cffffu, 4.5f, 150, 1, "Snake_spawn" },
    [U_BODY]     = { 0xff5a10ffu, 3.5f,   0, 1, NULL },
    [U_WELL]     = { 0xff2a08ffu, 11.0f,  0, 0, "Enemy_spawn_red" },
    [U_PROTON]   = { 0x3399ffffu, 4.75f, 50, 0, "Enemy_Proton_Spawn" },
    [U_ROCKET]   = { 0xff4a08ffu, 6.0f, 125, 4, "Enemy_rocket_spawn" },
    [U_DUCK]     = { 0xff2a6effu, 7.0f,  25, 2, "Enemy_duck_spawn" },
    [U_GATE]     = { 0xff6a10ffu, 5.0f, 100, 3, "Enemy_gate_spawn" },
    [U_REPULSAR] = { 0xff3a3affu, 9.0f, 350, 8, "Repulsar_spawn" },
    /* radii from the constructors; points from AddScore, geoms from EmitMotes */
    [U_HILL]     = { 0x50ff90ffu, 40.0f,  0, 0, "King_spawned" },   /* Unit.Hill.Radius */
    [U_MINE]     = { 0xffd040ffu, 5.0f,  75, 3, NULL },             /* Mine 0x34dd0 */
    [U_MINELAYER]= { 0xff8a20ffu, 16.0f,  0, 0, NULL },             /* Minelayer 0x3584c */
    [U_NUFO]     = { 0x40e0ffffu, 10.0f, 500, 1, NULL },            /* NUFO 0x37248 */
    [U_METEOR]   = { 0xc09070ffu, 10.0f, 25, 1, NULL },             /* MeteorShower 0x3488c */
    [U_BLACKHOLE]= { 0x3366ffffu, 20.0f,  0, 0, NULL },             /* BlackHole 0x28c2c */
    [U_TITAN]    = { 0xffffffffu, 8.0f,   0, 0, NULL },             /* Asteroid 0x2649c (r = 8 * scale) */
    [U_AMOEBA]   = { 0x30ff70ffu, 8.0f, 350, 8, "Enemy_amoeba_spawn" },
    [U_CYTO]     = { 0x30ff70ffu, 4.0f,   0, 0, NULL },
};

typedef struct {
    int type;
    unsigned id;                 /* changes when the slot is reused */
    float x, y, vx, vy, r, ang, sx, sy;
    float appear;                /* < 1 while spawning in (Entity::Update: += dt * 1.5) */
    float nocollide;             /* Proton: no collisions for its first 0.166 s */
    float flash;
    float a, b, c, d, e, f;      /* per type */
    int n, m;                    /* per type */
    int link;                    /* snake body: segment ahead; gate: partner */
    unsigned link_id;
    int head;                    /* snake body: its head */
    unsigned head_id;
} Unit;

#define MAX_UNITS 800
static Unit un[MAX_UNITS];
static unsigned next_id = 1;

static int unit_alive(int i, unsigned id) { return i >= 0 && i < MAX_UNITS && un[i].type && un[i].id == id; }

static int unit_new(int type, float x, float y)
{
    int i;
    for (i = 0; i < MAX_UNITS; i++)
        if (!un[i].type) {
            Unit *u = &un[i];
            memset(u, 0, sizeof *u);
            u->type = type;
            u->id = next_id++;
            u->x = clampf(x, -AW + 8, AW - 8);
            u->y = clampf(y, -AH + 8, AH - 8);
            u->r = ut[type].r;
            u->sx = u->sy = 1;
            u->link = u->head = -1;
            return i;
        }
    return -1;
}

/* a slot for something that starts off the arena (meteors, NUFOs) */
static int unit_new_free(int type, float x, float y)
{
    int i = unit_new(type, 0, 0);
    if (i >= 0) { un[i].x = x; un[i].y = y; }
    return i;
}

/* what EntityUnit::CountAsEnemy counts (the zones, a minelayer and the
 * amoeba's blobs are not enemies) */
static int counts_as_enemy(int type)
{
    return type != U_NONE && type != U_HILL && type != U_MINELAYER && type != U_CYTO;
}

static int count_enemies(void)
{
    int i, n = 0;
    for (i = 0; i < MAX_UNITS; i++) n += counts_as_enemy(un[i].type);
    return n;
}

/* ---- the player ---- */

static struct {
    int alive;
    float x, y, vx, vy, ang, r;
    float cooldown;              /* +0x80 */
    float bomb_cd;               /* +0x88 */
    float protect;               /* after respawning (approximate, see notes) */
    float respawn_t;
} pl;

/* game info: per-player block at gGameInfo + 0x7770 */
static long long score, best;
static int lives, bombs, mult, next_banner;
static long long next_bomb_at, next_life_at;
static int kills, best_beaten;
static double play_t;
static int deaths;

/* ---- bullets (Projectile) ---- */

#define MAX_BUL 600
typedef struct { int alive; float x, y, vx, vy, px, py, grav; } Bullet;
static Bullet bul[MAX_BUL];

/* ---- geoms (Mote) ---- */

#define MAX_MOTE 900
typedef struct { int alive; float x, y, vx, vy, ang, t, life; int value; } Mote;
static Mote mote[MAX_MOTE];

/* ---- expanding rings: the player's bomb and the black hole's blast (SmartBomb) ---- */

#define MAX_RING 16
typedef struct { int alive, kind; float x, y, r, max, rate; } Ring;   /* kind: DMG_BOMB or DMG_WELLBOMB */
static Ring ring[MAX_RING];

static void add_ring(int kind, float x, float y, float max, float rate)
{
    int i;
    for (i = 0; i < MAX_RING; i++)
        if (!ring[i].alive) {
            ring[i].alive = 1; ring[i].kind = kind; ring[i].x = x; ring[i].y = y;
            ring[i].r = 0; ring[i].max = max; ring[i].rate = rate;
            return;
        }
}

/* ---- score (AddScore 0x0006046c) ---- */

static void add_points(int base, int dmg, float x, float y)
{
    long long pts;
    if (base <= 0 || dmg == DMG_TOUCH || dmg == DMG_EATEN || dmg == DMG_BOMB) return;
    pts = (long long)(mult * 0.25f * base);
    if (mode == M_KING) pts *= 2;            /* AddScore: King doubles every score */
    pts = pts / 5 * 5;
    if (dmg == DMG_WELLBOMB || dmg == DMG_GATE) pts *= 5;   /* killed by a blast (well, gate, mine) */
    if (pts <= 0) return;
    score += pts;
    if (!(modes[mode].flags & 1) && score >= next_life_at) {   /* extra ship, then the next one at 10x */
        if (lives < 9) lives++;
        next_life_at *= 10;
        sfx("pickup_extralife", 30, 0);
    }
    if (!(modes[mode].flags & 2) && score >= next_bomb_at) {   /* extra bomb, same rule */
        if (bombs < 9) bombs++;
        next_bomb_at *= 10;
        sfx("pickup_smartbomb", 31, 0);
    }
    if (score > best && best > 0 && !best_beaten) {
        best_beaten = 1;
        sfx("Hi_Score_achieved", 32, 0);
    }
    (void)x; (void)y;
}

/* ---- geoms (EmitMotes / MakeSingleDebris / Mote) ---- */

static const int mote_values[19] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 5, 5, 10 };

static void emit_motes(float x, float y, int count, int dmg)
{
    if (dmg == DMG_TOUCH || dmg == DMG_EATEN) return;
    while (count > 0) {
        int idx = frnd() % 19, v, i;
        while (idx > 0 && mote_values[idx] > count) idx--;
        v = mote_values[idx] <= count ? mote_values[idx] : 1;
        for (i = 0; i < MAX_MOTE; i++)
            if (!mote[i].alive) {
                Mote *m = &mote[i];
                float dx, dy;
                do { dx = frange(-32, 32); dy = frange(-32, 32); } while (dx * dx + dy * dy >= 1024);
                m->alive = 1;
                m->x = x + frange(-6, 6); m->y = y + frange(-6, 6);
                m->vx = frange(-16, 16) + dx; m->vy = frange(-16, 16) + dy;
                m->ang = frand() * 2 * PI;
                m->t = 0; m->life = 4.225f;  /* Evolved lifetime (Mote::Mote) */
                m->value = v;
                break;
            }
        count -= v;
    }
}

/* ---- death of a unit (DoEnemyDeath 0x0002c92c and the classes' Killed) ---- */

static void kill_unit(int i, int dmg);
static void casual_kill(int type);
static void mine_hit(int k);
static void titan_killed(Unit *u, int dmg, float bvx, float bvy);
static float hit_vx, hit_vy;                 /* the velocity of the shot that is hitting */
static int gate_chain;                       /* GameInfo +0x75f0: gates flown through in a row */
static float gate_last_t;                    /* +0x75ec */

static void spawn_tiny(float x, float y, float bvx, float bvy, float a)
{
    int j = unit_new(U_TINY, x, y);
    Unit *t;
    if (j < 0) return;
    t = &un[j];
    t->appear = 1;                           /* TinySpinner has no spawn-in flag */
    t->vx = bvx + cosf(a) * 300; t->vy = bvy + sinf(a) * 300;
    t->x += 2 * TICK * cosf(a) * 300 * 0.0f;
    t->a = frange(0.09f, 0.10f) * (frnd() & 1 ? 1 : -1);    /* spin rate */
    t->ang = a;
    t->sx = t->sy = 1.25f;
}

static void well_points_and_geoms(Unit *u, int dmg)
{
    static const int pts[13] = { 100, 300, 500, 700, 900, 1100, 1500, 1900, 2400, 2900, 3700, 4500, 6000 };
    int eaten = u->m;
    add_points(pts[eaten < 12 ? eaten : 12], dmg, u->x, u->y);
    emit_motes(u->x, u->y, 5 + 3 * (eaten < 12 ? eaten : 12), dmg);
}

static void kill_unit(int i, int dmg)
{
    Unit *u = &un[i];
    int type = u->type, k;
    float x = u->x, y = u->y;
    if (!type) return;
    if (type == U_HILL || type == U_CYTO || type == U_MINELAYER || type == U_BLACKHOLE) {
        burst(x, y, ut[type].color, 12, 160);
        u->type = U_NONE;
        return;
    }
    if (dmg != DMG_TOUCH && dmg != DMG_EATEN) casual_kill(type);
    if (type == U_TITAN) {
        titan_killed(u, dmg, hit_vx, hit_vy);
        burst(x, y, ut[u->n].color, 70, 300);
    }
    if (type == U_GATE && dmg == DMG_GATE) {  /* AddScore: a gate flown through scores by the chain */
        int c = gate_chain > 5 ? 5 : gate_chain;
        add_points(c > 0 ? c * c * 100 : 100, DMG_BULLET, x, y);
    }
    burst(x, y, ut[type].color, type == U_WELL ? 90 : type == U_BODY ? 8 : 26, type == U_WELL ? 340 : 230);
    burst(x, y, 0xffffffffu, type == U_BODY ? 2 : 6, 160);
    grid_force(x, y, type == U_WELL ? 900 : 260, type == U_WELL ? 90 : 45);
    kills++;
    if (type == U_WELL) {
        well_points_and_geoms(u, dmg);
        /* GravityWell::Killed 0x0002ead4: a blast ring, radius 125, growing 10.4167/tick */
        add_ring(DMG_WELLBOMB, x, y, 125, 10.4167f * 60);
        sfx("Gravity_well_die", 4, 0.05);
    } else {
        if (!(type == U_GATE && dmg == DMG_GATE)) add_points(ut[type].points, dmg, x, y);
        if (type == U_BODY) {
            if ((u->n & 3) == 0) emit_motes(x, y, 1, dmg);    /* every 4th segment */
        } else {
            emit_motes(x, y, ut[type].geoms, dmg);
        }
        sfx(type == U_SNAKE ? "SnakeExplode" : "Enemy_explode", 5, 0.03);
    }
    if (type == U_SPINNER && dmg == DMG_BULLET) {     /* Spinner::Killed 0x00045798 */
        float a = frange(-PI, PI);
        for (k = 0; k < 3; k++)
            spawn_tiny(x, y, u->c, u->d, a + k * 2.0944f);
        sfx("SpinnerSplit", 6, 0.05);
    }
    if (type == U_GATE && unit_alive(u->link, u->link_id)) {  /* the pair goes together */
        Unit *p = &un[u->link];
        u->type = U_NONE;
        burst(p->x, p->y, ut[U_GATE].color, 20, 220);
        p->type = U_NONE;
        sfx("Enemy_gate_broken", 7, 0.05);
        return;
    }
    u->type = U_NONE;
}

/* ---- the gravity-well shockwave and hits (GravityWell::Damage 0x0002e99c) ---- */

static void well_damage(int i, int dmg)
{
    Unit *w = &un[i];
    int j;
    for (j = 0; j < MAX_UNITS; j++) {        /* shove everything within 256 away */
        Unit *e = &un[j];
        float dx, dy, d, f;
        if (j == i || !e->type || e->appear < 1) continue;
        dx = w->x - e->x; dy = w->y - e->y; d = sqrtf(dx * dx + dy * dy);
        if (d > 256 || d < 0.001f) continue;
        f = fminf(128 / d, 1) * 60;
        e->vx -= dx / d * f; e->vy -= dy / d * f;
    }
    w->flash = 1;
    w->n = 1;                                /* +0x84: active from the first hit */
    w->a -= 1;                               /* +0x80 mass */
    if (w->a <= 0) {
        kill_unit(i, dmg);
    } else {
        sfx("Enemy_red_hit", 8, 0.04);
        burst(w->x, w->y, 0xff6a30ffu, 6, 160);
    }
}

/* apply damage of a given type to unit i; returns 1 if a bullet should be used up */
static int damage_unit(int i, int dmg, float from_x, float from_y)
{
    Unit *u = &un[i];
    switch (u->type) {
    case U_BODY:                             /* SnakeBody::Damage 0x00043d10 */
    case U_GATE:                             /* Gate::Damage 0x0002ce80 */
        if (dmg == DMG_BULLET) {
            u->flash = 1;
            if (u->type == U_BODY) sfx("Enemy_pin_ricochet", 9, 0.06);
            return 1;
        }
        kill_unit(i, dmg);
        return 1;
    case U_WELL:
        well_damage(i, dmg);
        return 1;
    case U_REPULSAR:                         /* Repulsar::Damage 0x00040cf4: shielded front */
        if (dmg == DMG_BULLET) {
            float a = atan2f(from_y - u->y, from_x - u->x);
            if (fabsf(wrap_pi(a - u->ang)) < 1.1781f) {
                u->flash = 1;
                return 1;
            }
        }
        kill_unit(i, dmg);
        return 1;
    case U_HILL:                             /* not an enemy: shots pass */
        return 0;
    case U_MINELAYER:                        /* Minelayer::Damage 0x000357fc: nothing */
    case U_BLACKHOLE:                        /* BlackHole::Damage 0x000292fc: nothing */
    case U_CYTO:
        return 1;
    case U_MINE:                             /* Mine::Killed 0x00035180: a 0.3 s fuse */
        mine_hit(i);
        return 1;
    case U_TITAN:                            /* Asteroid::Damage 0x000258ec: hits run out */
        if (dmg == DMG_BULLET && --u->m > 0) {
            u->flash = 1;
            sfx("Enemy_red_hit", 8, 0.05);
            return 1;
        }
        if (dmg != DMG_BULLET && --u->m > 0) return 1;
        kill_unit(i, dmg);
        return 1;
    default:
        kill_unit(i, dmg);
        return 1;
    }
}

/* ---- spawning (SpawnBadGuy 0x00058dec and friends) ---- */

static void get_corner(int k, float *x, float *y)      /* GetCorner 0x00062230 */
{
    *x = (k == 1 || k == 2) ? 292 : -292;
    *y = (k >= 2) ? 192 : -192;
}

static int spawn_bad_guy(int type, float x, float y, float ang)
{
    int i = -1, k;
    Unit *u;
    switch (type) {
    case U_GRUNT:
        i = unit_new(U_GRUNT, x, y); if (i < 0) return -1; u = &un[i];
        u->a = 9;                            /* accel, +0.005 per tick */
        u->b = frand() * 2 * PI;             /* wobble phase */
        break;
    case U_WANDERER: {
        float dx, dy, d;
        i = unit_new(U_WANDERER, x, y); if (i < 0) return -1; u = &un[i];
        u->a = 0.1f + frand() * 0.05f;       /* spin rate */
        u->sx = u->sy = 2.5f;
        if (frand() < 0.5f) { u->a = -u->a; u->sx = -u->sx; }
        dx = pl.x - u->x; dy = pl.y - u->y; d = sqrtf(dx * dx + dy * dy) + 0.001f;
        u->vx = dx / d * 10 + frange(-10, 10);
        u->vy = dy / d * 10 + frange(-10, 10);
        break;
    }
    case U_WEAVER:
        i = unit_new(U_WEAVER, x, y); if (i < 0) return -1;
        break;
    case U_SPINNER:
        i = unit_new(U_SPINNER, x, y); if (i < 0) return -1;
        break;
    case U_SNAKE: {
        static const float seg_scale[6] = { 0.047f, 0.135f, 0.254f, 0.388f, 0.59f, 0.898f };
        int prev;
        unsigned prev_id, hid;
        i = unit_new(U_SNAKE, x, y); if (i < 0) return -1; u = &un[i];
        u->ang = frand() * 2 * PI;            /* heading */
        u->b = frand() * 2 * PI;              /* timer */
        u->sx = u->sy = 1.25f;
        prev = i; prev_id = u->id; hid = u->id;
        for (k = 0; k < 23; k++) {
            int j = unit_new(U_BODY, x, y);
            Unit *b;
            if (j < 0) break;
            b = &un[j];
            b->n = k;
            b->sx = b->sy = 1.0f;
            b->e = k < 6 ? seg_scale[k] : 1.0f;   /* per-segment value from the constructor */
            b->r = ut[U_BODY].r;
            b->link = prev; b->link_id = prev_id;
            b->head = i; b->head_id = hid;
            b->ang = u->ang;
            prev = j; prev_id = b->id;
        }
        break;
    }
    case U_WELL:
        i = unit_new(U_WELL, x, y); if (i < 0) return -1; u = &un[i];
        u->a = 6;                            /* mass: Unit.GravityWell.StartHits */
        u->r = u->a + 5;
        break;
    case U_PROTON:
        i = unit_new(U_PROTON, x, y); if (i < 0) return -1; u = &un[i];
        break;
    case U_ROCKET:
        i = unit_new(U_ROCKET, x, y); if (i < 0) return -1; u = &un[i];
        u->a = ang;                          /* heading (+0x84) */
        u->b = 1;                            /* direction sign (+0x80) */
        u->ang = ang;
        break;
    case U_DUCK:
        i = unit_new(U_DUCK, x, y); if (i < 0) return -1; u = &un[i];
        u->n = -1;                           /* last step direction */
        u->a = frand();                      /* step clock */
        break;
    case U_GATE: {                           /* BadGuyType 16: a pair of ends */
        int j;
        float a = frand() * 2 * PI;
        i = unit_new(U_GATE, x + cosf(a) * 32.5f, y + sinf(a) * 32.5f);
        j = unit_new(U_GATE, x - cosf(a) * 32.5f, y - sinf(a) * 32.5f);
        if (i < 0 || j < 0) {
            if (i >= 0) un[i].type = U_NONE;
            if (j >= 0) un[j].type = U_NONE;
            return -1;
        }
        un[i].link = j; un[i].link_id = un[j].id; un[i].n = 1;   /* leader */
        un[j].link = i; un[j].link_id = un[i].id; un[j].n = 0;
        un[i].ang = un[j].ang = a;
        break;
    }
    case U_REPULSAR:
        i = unit_new(U_REPULSAR, x, y); if (i < 0) return -1; u = &un[i];
        u->ang = atan2f(pl.y - y, pl.x - x);
        break;
    case U_MINELAYER: {                      /* Minelayer::Minelayer 0x3584c */
        float a = frand() * 2 * PI;
        i = unit_new(U_MINELAYER, x, y); if (i < 0) return -1; u = &un[i];
        u->c = cosf(a) * 90; u->d = sinf(a) * 90;   /* travel velocity (+0x94/+0x98), speed 90 */
        u->a = 0.3f;                         /* +0x88 state clock, starts at 0.3 */
        u->n = 0;                            /* +0x80 state */
        u->appear = 1;
        break;
    }
    case U_BLACKHOLE:                        /* BlackHole::BlackHole 0x28c2c */
        i = unit_new(U_BLACKHOLE, x, y); if (i < 0) return -1; u = &un[i];
        u->a = 1;                            /* +0x88 direction */
        u->b = 1;                            /* +0x8c strength */
        u->c = 0;                            /* +0x90 clock */
        u->n = 0;                            /* +0x94 reversing */
        u->appear = 1;
        break;
    case U_TITAN: {                          /* Asteroid::Asteroid 0x2649c, a big one */
        float r = frand();
        i = unit_new(U_TITAN, x, y); if (i < 0) return -1; u = &un[i];
        u->n = r < 0.05f ? U_REPULSAR : r < 0.3f ? U_SPINNER : r < 0.5f ? U_WEAVER : U_WANDERER;
        u->m = 15;                           /* hits (+0xa4) */
        u->e = 4;                            /* scale (+0x88), scale type 1 */
        u->f = 1;
        u->r = 8 * u->e;
        u->a = (frand() * 0.0005f + 0.000005f) * (frand() < 0.5f ? -1 : 1);   /* spin */
        break;
    }
    case U_AMOEBA: {                         /* AmoebaNucleus (simplified, see notes) */
        i = unit_new(U_AMOEBA, x, y); if (i < 0) return -1; u = &un[i];
        u->m = 40;                           /* Unit.Amoeba.NoCytoplasmAtStart */
        break;
    }
    }
    if (i >= 0 && ut[type].spawn_sfx) sfx(ut[type].spawn_sfx, 10 + type, 0.05);
    return i;
}

/* RandomPosition(float) 0x00058760: somewhere at least `radius` from where the
 * player will be in a third of a second */
static void random_position(float radius, float *x, float *y)
{
    float tx = pl.x + pl.vx / 3, ty = pl.y + pl.vy / 3;
    int tries;
    for (tries = 0; tries < 24; tries++) {
        *x = frange(-AW + 10, AW - 10);
        *y = frange(-AH + 10, AH - 10);
        if (!pl.alive) return;
        if ((*x - tx) * (*x - tx) + (*y - ty) * (*y - ty) > radius * radius) return;
    }
}

static void spawn_radius(int type)           /* SpawnRadiusBadGuy 0x0005a914 */
{
    float x, y;
    random_position(type == U_DUCK || type == U_WANDERER ? 100 : 165, &x, &y);
    spawn_bad_guy(type, x, y, (frnd() & 3) * (PI / 2));
}

static void spawn_corners(int type)           /* one at each of the four corners */
{
    int k;
    for (k = 0; k < 4; k++) {
        float x, y;
        get_corner(k, &x, &y);
        spawn_bad_guy(type, x, y, (frnd() & 3) * (PI / 2));
    }
}

static void spawn_circle(int type, int n, float radius)   /* SpawnCircleBadGuys 0x000599ec */
{
    int k;
    for (k = 0; k < n; k++) {
        float a = k * 2 * PI / n;
        spawn_bad_guy(type, pl.x + cosf(a) * radius, pl.y + sinf(a) * radius, a + PI);
    }
}

/* SpawnWallBadGuys 0x00059fdc: one every 20 units along a wall, starting 10 in,
 * facing into the arena.  Sides: 0 y=-200, 1 y=+200, 2 x=-300, 3 x=+300. */
static void spawn_wall(int type, int side)
{
    float t;
    switch (side) {
    case 0: for (t = 10 - AW; t < AW; t += 20) spawn_bad_guy(type, t, -AH, PI / 2); break;
    case 1: for (t = 10 - AW; t < AW; t += 20) spawn_bad_guy(type, t, AH, -PI / 2); break;
    case 2: for (t = 10 - AH; t < AH; t += 20) spawn_bad_guy(type, -AW, t, 0); break;
    default: for (t = 10 - AH; t < AH; t += 20) spawn_bad_guy(type, AW, t, PI); break;
    }
}

/* SpawnHalfWallBadGuys 0x00059c38: half of a wall; which half (and, for the
 * flag set, which wall) as the two jump tables in the function give it */
static void spawn_half_wall(int type, int side, int flag)
{
    float t;
    if (flag) {
        switch (side) {
        case 0: for (t = 10 - AW; t < 0; t += 20) spawn_bad_guy(type, t, AH, -PI / 2); break;
        case 1: for (t = 10; t < AW; t += 20) spawn_bad_guy(type, t, -AH, PI / 2); break;
        case 2: for (t = 10 - AH; t < 0; t += 20) spawn_bad_guy(type, AW, t, PI); break;
        default: for (t = 10; t < AH; t += 20) spawn_bad_guy(type, -AW, t, 0); break;
        }
    } else {
        switch (side) {
        case 0: for (t = 10 - AW; t < 0; t += 20) spawn_bad_guy(type, t, -AH, PI / 2); break;
        case 1: for (t = 10; t < AW; t += 20) spawn_bad_guy(type, t, AH, -PI / 2); break;
        case 2: for (t = 10 - AH; t < 0; t += 20) spawn_bad_guy(type, -AW, t, 0); break;
        default: for (t = 10; t < AH; t += 20) spawn_bad_guy(type, AW, t, PI); break;
        }
    }
}

/* ---- the Evolved spawn director (LevelControl 0x0005c97c / 0x0005baf4) ---- */

static struct {
    float level;                 /* +0x1c */
    float timer;                 /* +0x20 */
    int state;                   /* +0x24 */
    int sw_type;                 /* +0x28 */
    int sw_count;                /* +0x2c */
    int walls[4], wall_n;        /* state 4 order */
    int pause;                   /* the death pause flag (*0x13c274): 1 just died, 2 pausing */
    int side[4];                 /* wall director: order of the sides (+0x0..+0xc) */
    int full[4];                 /* and which are full walls (+0x10..+0x13) */
    int wall_i;                  /* +0x14; 5 = never shuffled yet */
    int toggle;                  /* +0x18 */
} lc;

static void lc_tick_normal(void)            /* LevelControl::TickStateNormal */
{
    float old = lc.level, r;
    lc.timer = clampf(2.0f - 0.02f * old, 0.275f, 2.0f);
    lc.level = old + 0.7f;
    if (lc.level < 2) {
        spawn_radius(U_WANDERER); spawn_radius(U_DUCK);
        spawn_radius(frand() < 0.8f ? U_DUCK : U_ROCKET);
        lc.timer += 3;
        return;
    }
    if (lc.level < 5) {
        spawn_radius(U_DUCK); spawn_radius(U_DUCK); spawn_radius(U_DUCK);
        spawn_radius(frand() < 0.5f ? U_DUCK : U_ROCKET);
        spawn_radius(U_GRUNT); spawn_radius(U_WANDERER); spawn_radius(U_WANDERER);
        lc.timer += 1.5f;
        return;
    }
    if (lc.level < 10) {
        r = frand();
        spawn_radius(r < 0.5f ? U_GRUNT : r < 0.75f ? U_WEAVER : U_SPINNER);
        spawn_radius(U_WANDERER); spawn_radius(U_DUCK);
        return;
    }
    /* the group table, used from level 10 on */
    r = frand();
    if (r < 0.24f) { spawn_radius(U_DUCK); spawn_radius(U_WANDERER); spawn_radius(U_GRUNT); }
    else if (r < 0.7f) { spawn_radius(U_WANDERER); spawn_radius(U_WANDERER); spawn_radius(U_ROCKET); }
    else if (r < 0.8f) { spawn_radius(U_GRUNT); spawn_radius(U_GRUNT); spawn_radius(U_GATE); }
    else if (r >= 0.95f) { spawn_radius(U_DUCK); spawn_radius(U_DUCK); spawn_radius(U_WELL); }
    else { spawn_radius(U_DUCK); spawn_radius(U_ROCKET); spawn_radius(U_SPINNER); }
    if (frand() < 0.075f) spawn_radius(frand() < 0.5f ? U_WEAVER : U_ROCKET);
    if (frand() < 0.1f) spawn_radius(frand() < 0.5f ? U_WANDERER : U_DUCK);

    if (lc.level < 40) {
        if (lc.level <= 20) return;
        r = frand();
        if (r < 0.075f) {                    /* corner swarm */
            lc.state = 1;
            if (frand() < 0.5f) { lc.sw_type = U_WEAVER; lc.sw_count = 5; }
            else { lc.sw_type = U_GRUNT; lc.sw_count = 10; }
        } else if (r < 0.15f) {
            lc.state = 4;                    /* rocket walls */
            lc.wall_n = 0;
        }
        return;
    }
    if (frand() < 0.1f) spawn_corners(frand() < 0.8f ? U_WEAVER : U_SNAKE);
    if (lc.level > 90) spawn_radius(frand() < 0.5f ? U_GRUNT : U_WEAVER);
    if (lc.level > 75 && frand() < 0.075f) spawn_corners(U_REPULSAR), (void)0;
    if (lc.level > 250 && frand() < 0.1f) { float x, y; get_corner(frnd() & 3, &x, &y); spawn_bad_guy(U_REPULSAR, x, y, 0); }
    if (lc.level > 500 && frand() < 0.15f) { float x, y; get_corner(frnd() & 3, &x, &y); spawn_bad_guy(U_REPULSAR, x, y, 0); }
    if (lc.level > 140) spawn_radius(U_GRUNT);
    if (lc.level > 250) spawn_radius(U_WEAVER);
    if (lc.level > 325) spawn_radius(U_SPINNER);
    if (lc.level > 400) spawn_radius(U_GRUNT);
    if (lc.level > 500) spawn_radius(U_WEAVER);
    if (frand() < 0.065f) { spawn_corners(U_SNAKE); spawn_corners(U_SNAKE); }
    r = frand();
    if (r < 0.03f) {                         /* corner swarm, scaled with level */
        float s2 = frand();
        lc.timer = 1; lc.state = 1;
        if (s2 < 0.2f) { lc.sw_type = U_WEAVER; lc.sw_count = 10; }
        else if (s2 < 0.4f) { lc.sw_type = U_SPINNER; lc.sw_count = 8; }
        else if (s2 < 0.6f) { lc.sw_type = U_SNAKE; lc.sw_count = 6; }
        else { lc.sw_type = U_GRUNT; lc.sw_count = 20; }
        if (lc.level > 100)
            lc.sw_count = (int)(lc.sw_count * (lc.level < 150 ? 1.25f : lc.level < 250 ? 1.5f :
                                               lc.level < 325 ? 2.0f : lc.level < 425 ? 2.5f : 4.0f));
        sfx("Multiplier", 33, 0);
    } else if (r < 0.06f) {                  /* rings */
        float s2 = frand();
        lc.timer = 1; lc.state = 2;
        if (s2 < 0.2f) { lc.sw_type = U_WEAVER; lc.sw_count = 7; }
        else if (s2 < 0.4f) { lc.sw_type = U_SPINNER; lc.sw_count = 4; }
        else if (s2 < 0.6f) { lc.sw_type = U_GRUNT; lc.sw_count = 12; }
        else if (s2 >= 0.8f) { lc.sw_type = U_SNAKE; lc.sw_count = 8; }
        else { lc.sw_type = U_ROCKET; lc.sw_count = 8; }
    } else if (r < 0.09f) {                  /* mixed corner rush */
        lc.timer = 1; lc.state = 3;
        lc.sw_count = lc.level <= 100 ? 1 : lc.level < 200 ? 2 : lc.level < 275 ? 3 : lc.level < 375 ? 4 : 5;
    } else if (r < 0.12f) {
        lc.state = 4;
        lc.wall_n = 0;
    }
}

static void lc_modes(void);

static void lc_update(void)                  /* LevelControl::Update 0x0005c97c */
{
    float r;
    if (lc.pause == 1) { lc.timer = 3; lc.pause = 2; }   /* a ship was lost */
    if (mode == M_KING) {                    /* King: hurry when no Grunt/Proton/Spinner/Snake is left */
        if (lc.timer > 0) {
            int i, n = 0;
            for (i = 0; i < MAX_UNITS; i++) {
                int t = un[i].type;
                n += t == U_GRUNT || t == U_PROTON || t == U_SPINNER || t == U_SNAKE;
            }
            if (n == 0 && lc.pause == 0) lc.timer = 0;
        }
    } else if (lc.timer > 0.5f && count_enemies() == 0 && lc.pause == 0) {
        lc.timer = lc.level < 10 ? 0.5f : 0;     /* an empty arena: hurry the next spawn */
    }
    lc.timer -= TICK * 0.7f;
    if (lc.timer > 0) return;
    if (lc.pause == 2) { lc.pause = 0; if (lc.timer >= 0) return; }
    if (mode != M_EVOLVED) { lc_modes(); return; }
    switch (lc.state) {
    case 0:
        lc_tick_normal();
        break;
    case 1:                                  /* corner swarm */
        lc.timer = clampf(0.6f - 0.01f * lc.level, 0.075f, 0.6f) * 1.25f;
        if (lc.sw_type == U_SPINNER) lc.timer *= 1.35f;
        if (--lc.sw_count < 1) { lc.state = 0; lc.timer *= 10; }
        spawn_corners(lc.sw_type);
        if (lc.level > 450) spawn_radius(U_WEAVER);
        if (lc.level > 500 && frand() < 0.5f) spawn_radius(U_SPINNER);
        if (lc.level > 600 && frand() < 0.3f) spawn_radius(U_REPULSAR);
        break;
    case 2:                                  /* rings around the player */
        lc.timer = clampf(0.8f - 0.02f * lc.level, 0.3f, 0.8f) * (lc.sw_type == U_SPINNER ? 1.35f : 1);
        spawn_circle(lc.sw_type, lc.sw_count, 120);
        spawn_circle(lc.sw_type, lc.sw_count * 2, 170);
        if (lc.level > 300) spawn_radius(U_GRUNT);
        if (lc.level > 450) { spawn_radius(U_WEAVER); spawn_radius(U_WEAVER); }
        if (lc.level > 600) { spawn_radius(U_SPINNER); spawn_radius(U_SPINNER); spawn_radius(U_SPINNER); }
        if (lc.level > 350) spawn_circle(lc.sw_type, (int)(lc.sw_count * 2.5f), 260);
        lc.state = 0;
        break;
    case 3:                                  /* Grunt, Spinner and Weaver at every corner */
        lc.timer = clampf(1.2f - 0.02f * lc.level, 0.3f, 1.2f);
        spawn_corners(U_GRUNT); spawn_corners(U_SPINNER); spawn_corners(U_WEAVER);
        if (lc.level > 300) { spawn_radius(U_GRUNT); spawn_radius(U_GRUNT); }
        if (lc.level > 450) { spawn_radius(U_WEAVER); spawn_radius(U_WEAVER); }
        if (lc.level > 600) { spawn_radius(U_SPINNER); spawn_radius(U_SPINNER); spawn_radius(U_SPINNER); }
        if (lc.level > 700) { spawn_radius(U_REPULSAR); spawn_radius(U_REPULSAR); spawn_radius(U_REPULSAR); }
        if (--lc.sw_count <= 0) {
            spawn_corners(U_WELL);           /* the last round ends with four black holes */
            lc.state = 0;
        }
        break;
    case 4: {                                /* rocket walls, in shuffled order */
        int k, n;
        if (lc.wall_n == 0) {
            for (k = 0; k < 4; k++) lc.walls[k] = k;
            for (k = 3; k > 0; k--) { int j = frnd() % (k + 1), t = lc.walls[k]; lc.walls[k] = lc.walls[j]; lc.walls[j] = t; }
        }
        n = lc.level >= 100 ? 1 + (int)(lc.level / 150) : 1;
        if (n > 4) n = 4;
        for (k = 0; k < n && lc.wall_n < 4; k++)
            spawn_wall(U_ROCKET, lc.walls[lc.wall_n++]);
        lc.timer = 1.5f;
        if (lc.wall_n >= 4) lc.state = 0;
        break;
    }
    }
    (void)r;
}

/* ===================================================== THE OTHER MODES ====
 * Everything below comes from LevelControl::Update 0x0005c97c (the branches
 * on GameInfo+0x794c), CasualGameUpdate 0x0005b160 (Deadline) and
 * NewGameControlUpdate 0x000549fc (Sequence).  Full write-up: notes/Modes.md */

static void spawn_radius_r(int type, float radius)   /* SpawnRadiusBadGuy(type, radius) 0x0005a914 */
{
    float x, y;
    random_position(radius, &x, &y);
    spawn_bad_guy(type, x, y, (frnd() & 3) * (PI / 2));
}

/* RandomPosition(min, max) 0x000583e4: away from the ship by min and, when
 * max is given, no further than max from it */
static void spawn_radius_minmax(int type, float rmin, float rmax)
{
    float tx = pl.x + pl.vx / 3, ty = pl.y + pl.vy / 3, x = 0, y = 0;
    int tries;
    for (tries = 0; tries < 32; tries++) {
        float d2;
        x = frange(-AW + 10, AW - 10); y = frange(-AH + 10, AH - 10);
        d2 = (x - tx) * (x - tx) + (y - ty) * (y - ty);
        if (d2 >= rmin * rmin && (rmax <= 0 || d2 <= rmax * rmax)) break;
    }
    spawn_bad_guy(type, x, y, (frnd() & 3) * (PI / 2));
}

/* the corner a mode spawns from: GetCorner(rand & 3), re-rolled (the code
 * unrolls ten tries) while it is in the ship's quadrant and level < 30 */
static void corner_pick(float *x, float *y)
{
    int k;
    get_corner(frnd() & 3, x, y);
    if (lc.level >= 30 || !pl.alive) return;
    for (k = 0; k < 10; k++) {
        if ((*x > 0) != (pl.x > 0) || (*y > 0) != (pl.y > 0)) return;
        get_corner(frnd() & 3, x, y);
    }
}

static void nearest_corner(float *x, float *y)        /* GetNearestCorner 0x0005a608 */
{
    int k, best = 0;
    float bd = 1e30f;
    for (k = 0; k < 4; k++) {
        float cx, cy, d;
        get_corner(k, &cx, &cy);
        d = (cx - pl.x) * (cx - pl.x) + (cy - pl.y) * (cy - pl.y);
        if (d < bd) { bd = d; best = k; }
    }
    get_corner(best, x, y);
}

/* SpawnClump 0x0005a9cc: n of them around (x,y), up to `spread` away on each
 * axis (four tries each to land inside the arena) */
static void spawn_clump(int type, float x, float y, float spread, int n)
{
    int i, t;
    for (i = 0; i < n; i++) {
        float px = x, py = y;
        for (t = 0; t < 4; t++) {
            px = x + frange(-1, 1) * spread; py = y + frange(-1, 1) * spread;
            if (px >= -AW && px <= AW && py >= -AH && py <= AH) break;
        }
        spawn_bad_guy(type, px, py, (frnd() & 3) * (PI / 2));
    }
}

/* SpawnCircleBadGuys 0x000597e8: n in a ring of radius r round (cx,cy),
 * starting at angle `slip`, facing the middle */
static void spawn_circle_at(int type, int n, float cx, float cy, float r, float slip)
{
    int k;
    for (k = 0; k < n; k++) {
        float a = k * 2 * PI / n + slip;
        spawn_bad_guy(type, cx + cosf(a) * r, cy + sinf(a) * r, a + PI);
    }
}

/* the wall director (the code before each wall in Waves, Meteor and Evolved):
 * four sides in a shuffled order, some of them full walls and the rest half
 * walls, the number of full ones growing with the level */
static void wall_prep(void)
{
    int k, j, t, n, last;
    if (lc.wall_i <= 3) return;
    last = lc.wall_i <= 4 ? lc.side[3] : -1;
    lc.toggle ^= 1;
    for (k = 0; k < 4; k++) { lc.side[k] = k; lc.full[k] = 0; }
    n = (int)(lc.level + 1.5f) / 5;
    if (n > 4) n = 4;
    for (k = 0; k < n; k++) lc.full[k] = 1;
    j = frnd() & 3; t = lc.side[0]; lc.side[0] = lc.side[j]; lc.side[j] = t;
    j = 1 + frnd() % 3; t = lc.side[1]; lc.side[1] = lc.side[j]; lc.side[j] = t;
    j = 2 + (frnd() & 1); t = lc.side[2]; lc.side[2] = lc.side[j]; lc.side[j] = t;
    j = frnd() & 3; t = lc.full[0]; lc.full[0] = lc.full[j]; lc.full[j] = t;
    j = 1 + frnd() % 3; t = lc.full[1]; lc.full[1] = lc.full[j]; lc.full[j] = t;
    j = 2 + (frnd() & 1); t = lc.full[2]; lc.full[2] = lc.full[j]; lc.full[j] = t;
    if (lc.wall_i > 4) {                     /* the very first time: start on the left or right */
        unsigned r1 = frnd(), r2 = frnd();
        int s = r2 % 100 <= 49;
        lc.side[3] = s;
        lc.side[1] = s ^ 1;
        if (r1 % 100 > 49) { lc.side[0] = 3; lc.side[2] = 2; }
        else { lc.side[0] = 2; lc.side[2] = 3; }
    } else if (lc.side[0] == last) {         /* never the same side twice running */
        j = 1 + frnd() % 3; t = lc.side[0]; lc.side[0] = lc.side[j]; lc.side[j] = t;
    }
    lc.wall_i = 0;
}

/* gCasualGame 0x15ddec: the Deadline director's state; +0xc (geoms) is also
 * what Waves and Meteor divide by the level */
static struct {
    float timer, delay;
    int kills, geoms, budget, waves, well_cd, gate_cd, total;
} cg;

static void nufo_shower(float dx, float dy);
static void meteor_shower(float dx, float dy);

static void lc_modes(void)                   /* LevelControl::Update, the per-mode branches */
{
    float L = lc.level, cx, cy;
    int i, n;
    switch (mode) {
    case M_PACIFISM:                         /* 0x0005cb88 */
        corner_pick(&cx, &cy);
        spawn_clump(U_GRUNT, cx, cy, 80, (int)(L * 0.3f) + 3);
        if ((int)L % 3 <= 1) spawn_radius_r(U_GATE, 100);
        lc.timer = clampf(3 - 0.5f * L, 1, 3.5f);
        lc.level += 1;
        break;
    case M_MINELAYER:                        /* 0x0005cdd4 */
        if (L < 1) spawn_circle_at(U_MINELAYER, 3, pl.x, pl.y, 80, 0);   /* SpawnCircleBadGuys(28, 3, 80) */
        corner_pick(&cx, &cy);
        spawn_clump(U_WANDERER, cx, cy, 80, (int)(L * 0.2f) + 3);
        spawn_clump(U_GRUNT, cx, cy, 80, (int)(L * 0.1f) + 4);
        if ((int)L % 5 <= 0) nufo_shower(frange(-1, 1), frange(-1, 1));   /* NUFO::SpawnShower */
        lc.timer = clampf(4.5f - 0.25f * L, 1, 4.5f);
        lc.level += 1;
        break;
    case M_TITANS:                           /* 0x0005d718 */
        corner_pick(&cx, &cy);
        spawn_clump(U_TITAN, cx, cy, 80, (int)(L * 0.1f) + 1);
        lc.timer = clampf(10 - 0.25f * L, 2, 10);
        lc.level += 1;
        break;
    case M_WHIRLPOOL:                        /* 0x0005d7c0 */
        if (L < 1) spawn_bad_guy(U_BLACKHOLE, 0, 0, 0);
        corner_pick(&cx, &cy);
        spawn_clump(U_WANDERER, cx, cy, 80, (int)(L * 0.1f));
        spawn_clump(U_GRUNT, cx, cy, 80, (int)(L * 0.05f) + 1);
        if ((int)L % 10 <= 0) spawn_clump(U_WEAVER, cx, cy, 80, (int)(L * 0.1f) + 4);
        lc.timer = clampf(4.5f - 0.25f * L, 1, 4.5f);
        lc.level += 1;
        break;
    case M_METEOR: {                         /* 0x0005d8d0 */
        if (L > 30 && (int)L % 4 <= 0) {     /* a wall now and then */
            float g;
            int side, full;
            wall_prep();
            g = cg.geoms / (L + 1);
            side = lc.side[lc.wall_i]; full = lc.full[lc.wall_i]; lc.wall_i++;
            if (g > 20 || full) spawn_wall(U_ROCKET, side);
            else spawn_half_wall(U_ROCKET, side, lc.toggle);
        }
        corner_pick(&cx, &cy);
        spawn_clump(U_WANDERER, cx, cy, 80, (int)(L * 0.2f) + 3);
        spawn_clump(U_GRUNT, cx, cy, 80, (int)(L * 0.4f) + 4);
        meteor_shower(frange(-1, 1), frange(-1, 1));
        lc.timer = clampf(8 - 0.25f * L, 1, 8);
        lc.level += 1;
        break;
    }
    case M_KING:                             /* 0x0005e6d0 */
        n = (int)(L * 0.2f) + 2;
        for (i = 0; i < n; i++) {
            spawn_radius_r(U_WANDERER, 165); spawn_radius_r(U_WANDERER, 165);
            spawn_radius_r(U_GRUNT, 165); spawn_radius_r(U_GRUNT, 165);
            if (L > 10) spawn_radius_r(U_SNAKE, 165);
            if (L > 20) spawn_radius_r(U_PROTON, 165);
        }
        lc.timer = clampf(3 - 0.5f * L, 1, 3.5f) * 5;
        lc.level += 1;
        break;
    case M_WAVES: {                          /* 0x0005edb4 */
        float g;
        int side, full;
        wall_prep();
        g = cg.geoms / (L + 1);
        side = lc.side[lc.wall_i]; full = lc.full[lc.wall_i]; lc.wall_i++;
        if (g > 20) {                        /* collecting a lot: full walls and more */
            spawn_wall(U_ROCKET, side);
            nearest_corner(&cx, &cy);
            spawn_clump(U_DUCK, cx, cy, 150, 1);
            if (L > 4) spawn_clump(U_GRUNT, cx, cy, 150, 1);
            if (L > 8) spawn_clump(U_WEAVER, cx, cy, 150, 1);
            if (L > 12) spawn_radius_r(U_GATE, 100);
            lc.timer = clampf(3.5625f - 0.125f * L, 0.25f, 3.5f);
        } else {
            if (full) spawn_wall(U_ROCKET, side);
            else spawn_half_wall(U_ROCKET, side, lc.toggle);
            if (L > 5) spawn_radius_r(U_DUCK, 165);
            if (L > 11) spawn_radius_r(U_GRUNT, 165);
            if (L > 17) spawn_radius_r(U_WEAVER, 165);
            if (L > 25) spawn_radius_r(U_GATE, 100);
            lc.timer = clampf(4.25f - 0.125f * L, 0.25f, 4.25f);
        }
        lc.level += 1;
        sfx("Enemy_rocket_spawn", 40, 0.1);
        break;
    }
    }
}

/* ---- Deadline: CasualGameUpdate 0x0005b160 and friends ---- */

#define DEADLINE_TIME 180.0f                 /* GrCasual: GameInfo+0x7950 */

static void casual_spawn(int type, int *wall_left)   /* SpawnCasual 0x0005b0a4 */
{
    if (type != U_DUCK && type != U_WANDERER && *wall_left > 0 && frand() < 0.5f) {
        /* GetNearestWall 0x0005af20: the wall point nearest the ship, facing in */
        float d[4], x, y, a;
        int k, w = 0;
        d[0] = fabsf(pl.x + AW + 8); d[1] = fabsf(AW - 8 - pl.x);
        d[2] = fabsf(pl.y + AH + 8); d[3] = fabsf(AH - 8 - pl.y);
        for (k = 1; k < 4; k++) if (d[k] < d[w]) w = k;
        x = clampf(pl.x, -292, 292); y = clampf(pl.y, -192, 192);
        if (w == 0) { x = -292; a = 0; } else if (w == 1) { x = 292; a = PI; }
        else if (w == 2) { y = -192; a = PI / 2; } else { y = 192; a = -PI / 2; }
        spawn_bad_guy(type, x, y, a);
        (*wall_left)--;
        return;
    }
    spawn_radius_r(type, 165);
}

static void casual_update(void)
{
    int target, b, i, k1 = 0, k2 = 0, k3 = 0, k4 = 0, wall_left = 1;
    if (play_t >= DEADLINE_TIME) return;
    cg.delay -= TICK;
    if (cg.delay > 0) return;
    if (cg.timer > 0.5f && count_enemies() < 2) cg.timer = 0.5f;
    cg.timer -= TICK;
    if (cg.timer > 0) return;
    target = cg.kills + 2 + (int)(cg.geoms * 0.5f);
    b = cg.budget;
    if (b <= target) { b = (int)(b * 1.5f + 5); if (b > target) b = target; }
    if (cg.well_cd < 1) { if (b > 45) { spawn_radius_r(U_WELL, 165); cg.well_cd = 4; } }
    else cg.well_cd--;
    if (cg.gate_cd < 1) { if (b > 20) { spawn_radius_r(U_GATE, 165); cg.gate_cd = 2; } }
    else cg.gate_cd--;
    cg.total += b;
    for (i = 0; i < b; ) {                   /* spend the budget */
        int r = (b - i) % 80;
        if (r >= 46) {
            if (k4) {
                casual_spawn(k3 ? U_SPINNER : U_WEAVER, &wall_left);
                casual_spawn(k2 ? U_GRUNT : U_ROCKET, &wall_left);
                casual_spawn(k1 ? U_DUCK : U_WANDERER, &wall_left);
                k3 ^= 1; k2 ^= 1; k1 ^= 1;
            } else {
                casual_spawn(U_SNAKE, &wall_left);
            }
            k4 ^= 1;
            i += 8;
        } else if (r >= 16) {
            casual_spawn(k3 ? U_SPINNER : U_WEAVER, &wall_left); k3 ^= 1; i += 4;
        } else if (r >= 6) {
            casual_spawn(k2 ? U_GRUNT : U_ROCKET, &wall_left); k2 ^= 1; i += 2;
        } else {
            casual_spawn(k1 ? U_DUCK : U_WANDERER, &wall_left); k1 ^= 1; i += 1;
        }
    }
    cg.timer = 5;
    cg.kills = cg.geoms = 0;
    cg.budget = b;
    cg.waves++;
    sfx("Bell", 41, 0);
}

static void casual_kill(int type)            /* CasualGameKill 0x000582ac */
{
    switch (type) {
    case U_GRUNT: case U_ROCKET: cg.kills += 2; break;
    case U_WEAVER: case U_SPINNER: cg.kills += 4; break;
    case U_SNAKE: cg.kills += 8; break;
    case U_WANDERER: case U_DUCK: cg.kills += 1; break;
    default: break;
    }
}

static void casual_death(void)               /* CasualGameDeath 0x00058a74 */
{
    float left = DEADLINE_TIME - (float)play_t;
    int cut;
    cg.timer = 0; cg.delay = 3; cg.kills = cg.geoms = 0;
    if (left > 120) cut = (int)(cg.budget * 0.666667f);
    else if (left > 60) cut = (int)(cg.budget * 0.6f);
    else if (left > 30) cut = (int)(cg.budget * 0.5f);
    else cut = (int)(cg.budget * 0.333333f);
    if (cut > 30) cut = 30;
    cg.budget -= cut;
    if (cg.budget < 0) cg.budget = 0;
}

static void casual_bomb(void)                /* CasualGameBomb 0x00058d90 */
{
    int cut = (int)(cg.budget * 0.3f);
    if (cut > 10) cut = 10;
    cg.timer = 0; cg.delay = 1.5f; cg.kills = cg.geoms = 0;
    cg.budget -= cut;
    if (cg.budget < 0) cg.budget = 0;
}

/* ---- Sequence: the level scripts and their runner ---- */

/* the 15 levels played, by script number (ReadLevel 0x00054638, table 0x12b764) */
static const int seq_order[15] = { 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 19, 14, 15, 16, 20 };
#define SEQ_LEVELS 15
#define SEQ_TIME 30.0f                       /* GrProgression: GameInfo+0x7954 */

enum { A_CIRCLE, A_LINE, A_SCATTER, A_CLUMP, A_WAIT };
typedef struct { int kind, type, count, side, half, pos; float radius, rmax, slip, time; } Atom;
#define MAX_ATOMS 64
static Atom seq_atoms[21][MAX_ATOMS];        /* by script number 1..20 */
static int seq_natoms[21];
static int seq_loaded;

static struct {
    int slot, atom, phase, result;           /* phase 0 playing, 1 between levels */
    float wait, left, inter;
    int results[SEQ_LEVELS];
} sq;

static int seq_type(const char *s)           /* GetBadGuy 0x000521b8 */
{
    static const struct { const char *n; int t; } map[] = {
        { "wanderer", U_WANDERER }, { "grunt", U_GRUNT }, { "weaver", U_WEAVER },
        { "spinner", U_SPINNER }, { "gravitywell", U_WELL }, { "repulsar", U_REPULSAR },
        { "snake", U_SNAKE }, { "amoeba", U_AMOEBA }, { "ricochet", U_ROCKET },
        { "rocket", U_ROCKET }, { "proton", U_PROTON }, { "quanta", U_GATE },
        { "gate", U_GATE }, { "duck", U_DUCK },
    };
    size_t k;
    for (k = 0; k < sizeof map / sizeof map[0]; k++) {
        const char *a = map[k].n, *b = s;
        while (*a && *b && (*b | 32) == *a) { a++; b++; }
        if (!*a && (!*b || *b == ')')) return map[k].t;
    }
    return U_NONE;
}

static const char *seq_attr(const char *line, const char *name, char *out, int n)
{
    const char *p = line;
    size_t len = strlen(name);
    while ((p = strstr(p, name)) != NULL) {
        if ((p == line || p[-1] == ' ' || p[-1] == '\t') && p[len] == '(') {
            int k = 0;
            p += len + 1;
            while (*p && *p != ')' && k < n - 1) out[k++] = *p++;
            out[k] = 0;
            return out;
        }
        p += len;
    }
    return NULL;
}

/* app_data/levels.txt is made by tools/prep_assets.py from the game's own
 * Data.bin: "level N" then that script's lines */
static void seq_load(void)
{
    char path[640], line[256], v[64];
    FILE *f;
    int cur = 0;
    seq_loaded = 0;
    memset(seq_natoms, 0, sizeof seq_natoms);
    snprintf(path, sizeof path, "%slevels.txt", plat_data_dir());
    f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        Atom a;
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (!strncmp(s, "level ", 6)) { cur = atoi(s + 6); if (cur < 1 || cur > 20) cur = 0; continue; }
        if (!cur || *s == ';' || *s == '\r' || *s == '\n' || !*s) continue;
        memset(&a, 0, sizeof a);
        if (!strncmp(s, "circle", 6)) a.kind = A_CIRCLE;
        else if (!strncmp(s, "line", 4)) a.kind = A_LINE;
        else if (!strncmp(s, "scatter", 7)) a.kind = A_SCATTER;
        else if (!strncmp(s, "clump", 5)) a.kind = A_CLUMP;
        else if (!strncmp(s, "wait", 4)) a.kind = A_WAIT;
        else continue;
        if (seq_attr(s, "type", v, sizeof v)) a.type = seq_type(v);
        if (seq_attr(s, "count", v, sizeof v)) a.count = atoi(v);
        if (seq_attr(s, "radius", v, sizeof v)) a.radius = (float)atof(v);
        if (seq_attr(s, "maxRadius", v, sizeof v)) a.rmax = (float)atof(v);
        if (seq_attr(s, "slip", v, sizeof v)) a.slip = (float)atof(v) * 0.0174533f;
        if (seq_attr(s, "time", v, sizeof v)) a.time = (float)atof(v);
        if (seq_attr(s, "half", v, sizeof v)) a.half = atoi(v);
        if (seq_attr(s, "wall", v, sizeof v)) {
            int c = v[0] & ~32;
            a.side = c == 'S' ? 1 : c == 'E' ? 2 : c == 'W' ? 3 : 0;
        }
        if (seq_attr(s, "position", v, sizeof v)) {
            static const char *pos[] = { "n", "ne", "e", "se", "s", "sw", "w", "nw", "centre", "nearest", "player" };
            int k;
            a.pos = a.kind == A_CIRCLE ? (strcmp(v, "centre") ? 0 : 1) : 0;
            if (a.kind == A_CLUMP)
                for (k = 0; k < 11; k++) if (!strcmp(v, pos[k])) a.pos = k;
        }
        if (seq_natoms[cur] < MAX_ATOMS) seq_atoms[cur][seq_natoms[cur]++] = a;
    }
    fclose(f);
    seq_loaded = seq_natoms[1] > 0;
}

static void seq_exec(const Atom *a)          /* La*::Exec 0x00057228 .. 0x0005786c */
{
    int k;
    switch (a->kind) {
    case A_WAIT: sq.wait = a->time; break;
    case A_SCATTER:
        for (k = 0; k < a->count; k++) spawn_radius_minmax(a->type, a->radius, a->rmax);
        break;
    case A_LINE:
        if (a->half == 1) spawn_half_wall(a->type, a->side ^ 1, 0);
        else if (a->half == 2) spawn_half_wall(a->type, a->side, 1);
        else spawn_wall(a->type, a->side);
        break;
    case A_CIRCLE:
        if (a->pos == 1) spawn_circle_at(a->type, a->count, 0, 0, a->radius, a->slip);
        else spawn_circle_at(a->type, a->count, pl.x, pl.y, a->radius, a->slip);
        break;
    case A_CLUMP: {
        static const float px[9] = { 0, 1, 1, 1, 0, -1, -1, -1, 0 }, py[9] = { -1, -1, 0, 1, 1, 1, 0, -1, 0 };
        float x, y;
        if (a->pos == 9) nearest_corner(&x, &y);
        else if (a->pos == 10) { x = pl.x; y = pl.y; }
        else { x = px[a->pos] * AW; y = py[a->pos] * AH; }
        spawn_clump(a->type, x, y, a->radius, a->count);
        break;
    }
    }
}

static void seq_start_level(void)            /* SetupLevel 0x00054914 */
{
    sq.atom = 0; sq.wait = 0; sq.left = SEQ_TIME; sq.phase = 0;
}

static void seq_complete(int result)         /* CompleteLevel 0x00051d60: 1 done, 2 died, 3 out of time */
{
    int i;
    if (sq.slot < SEQ_LEVELS) sq.results[sq.slot] = result;
    sq.result = result;
    sq.phase = 1; sq.inter = 2.5f;
    for (i = 0; i < MAX_UNITS; i++)          /* ClearPlayField: the level's enemies go */
        if (un[i].type) kill_unit(i, DMG_TOUCH);
    sfx(result == 1 ? "IntermissionSwoosh" : result == 3 ? "TickClock" : "Short", 42, 0);
}

static void seq_update(void)                 /* NewGameControlUpdate, state 3 */
{
    int script = seq_order[sq.slot < SEQ_LEVELS ? sq.slot : 0];
    if (sq.phase == 1) return;
    if (!pl.alive) return;
    if (sq.wait > 0) sq.wait -= TICK;
    if (sq.wait > 0.5f && count_enemies() < 1) sq.wait = 0.5f;
    while (sq.wait <= 0 && sq.atom < seq_natoms[script])
        seq_exec(&seq_atoms[script][sq.atom++]);
    sq.left -= TICK;
    if (sq.atom >= seq_natoms[script] && sq.wait <= 0 && count_enemies() == 0) seq_complete(1);
    else if (sq.left <= 0) seq_complete(3);
}

/* ---- King: the zones (Hill 0x00030984, Hill::Tick 0x00031348) ---- */

static void hill_spot(float *x, float *y)    /* RandomHillPosition 0x00030cdc */
{
    float tx = pl.x + pl.vx / 3, ty = pl.y + pl.vy / 3;
    int tries, i;
    for (tries = 0; tries < 40; tries++) {
        int ok = 1;
        *x = frange(-AW + 40, AW - 40); *y = frange(-AH + 40, AH - 40);
        if ((*x - tx) * (*x - tx) + (*y - ty) * (*y - ty) < 60 * 60) continue;   /* 1.5 x Radius */
        for (i = 0; i < MAX_UNITS && ok; i++)
            if (un[i].type == U_HILL) {
                float dx = un[i].x - *x, dy = un[i].y - *y;
                if (dx * dx + dy * dy < 100 * 100) ok = 0;     /* 2.5 x Radius */
            }
        if (ok) return;
    }
}

static void hill_new(float x, float y)
{
    int i = unit_new(U_HILL, x, y);
    if (i < 0) return;
    un[i].r = 1;                             /* grows to Unit.Hill.Radius */
    un[i].n = 0;                             /* +0x8c state */
    un[i].c = 2.5f;                          /* +0x90 Unit.Hill.Time */
    un[i].appear = 1;
    sfx("King_spawned", 43, 0);
}

static int in_hill(float x, float y)         /* IsWithinHill: may the ship fire? */
{
    int i;
    for (i = 0; i < MAX_UNITS; i++)
        if (un[i].type == U_HILL) {
            float dx = un[i].x - x, dy = un[i].y - y;
            if (dx * dx + dy * dy < un[i].r * un[i].r) return 1;
        }
    return 0;
}

static void tick_hill(int i)
{
    Unit *u = &un[i];
    int j;
    if (u->n == 3) {                         /* fading out */
        u->r -= 40 * TICK * 0.666667f;
        if (u->r <= 1) { u->type = U_NONE; return; }
    } else if (u->r < 40) {
        u->r += 40 * TICK * 2;
        if (u->r > 40) u->r = 40;
    }
    switch (u->n) {
    case 0:                                  /* waiting for the ship */
        if (pl.alive) {
            float dx = pl.x - u->x, dy = pl.y - u->y, rr = pl.r + u->r;
            if (dx * dx + dy * dy < rr * rr) { u->n = 1; sfx("FirstEnterHill", 44, 0); }
        }
        break;
    case 1:                                  /* claimed: counting down */
        u->c -= TICK;
        if (u->c <= 1) u->n = 2;
        break;
    case 2:
        u->c -= TICK;
        if (u->c <= 0) {                     /* the next zone appears, this one fades */
            float x, y;
            hill_spot(&x, &y);
            hill_new(x, y);
            sfx("King_fading", 45, 0);
            u->n = 3;
        }
        break;
    }
    /* Unit.Hill.SafeZone: enemies are pushed out to the edge (+16) */
    for (j = 0; j < MAX_UNITS; j++) {
        Unit *e = &un[j];
        float dx, dy, d2, lim;
        if (!e->type || j == i || e->type == U_HILL || e->appear < 1) continue;
        dx = e->x - u->x; dy = e->y - u->y; d2 = dx * dx + dy * dy;
        lim = e->r + u->r + 16;
        if (d2 >= 1 && d2 <= lim * lim) {
            float d = sqrtf(d2);
            e->x = u->x + dx / d * lim; e->y = u->y + dy / d * lim;
        }
    }
}

/* ---- Minelayer, Mine, NUFO ---- */

static void tick_minelayer(int i)            /* Minelayer::Tick 0x00035ab0 */
{
    Unit *u = &un[i];
    float k;
    switch (u->n) {
    case 0:                                  /* travelling */
        u->a += TICK;
        u->vx = u->c; u->vy = u->d;
        if (u->a >= 0.6f) { u->n = 1; u->b = 0; }
        break;
    case 1:                                  /* braking for 0.1 s, then a mine */
        k = 1 - u->b * 10;
        u->b += TICK;
        u->vx = u->c * k; u->vy = u->d * k;
        if (u->b >= 0.1f) {
            int j = unit_new(U_MINE, u->x, u->y);
            if (j >= 0) { un[j].appear = 1; un[j].f = 0.5f; }   /* inert while the layer leaves */
            u->n = 2; u->b = 0.1f; u->e = 0.5f;
        }
        break;
    case 2:                                  /* sitting for 0.5 s */
        u->vx = u->vy = 0;
        u->e -= TICK;
        if (u->e <= 0) u->n = 3;
        break;
    default:                                 /* speeding up again, then a new heading */
        k = 1 - u->b * 10;
        u->b -= TICK;
        u->vx = u->c * k; u->vy = u->d * k;
        if (u->b <= 0) {
            float a = frand() * 2 * PI, s = sqrtf(u->c * u->c + u->d * u->d);
            u->c = cosf(a) * s; u->d = sinf(a) * s;
            u->n = 0; u->a = 0;
        }
        break;
    }
    u->ang += 0.05f;
    /* bounce off walls (CollideWithWall 0x00035804 reflects the heading) */
    if ((u->x <= -AW + u->r && u->c < 0) || (u->x >= AW - u->r && u->c > 0)) u->c = -u->c;
    if ((u->y <= -AH + u->r && u->d < 0) || (u->y >= AH - u->r && u->d > 0)) u->d = -u->d;
}

static void mine_trigger(int i, float fuse)  /* Mine::Killed 0x00035180 (not by the ship) */
{
    Unit *u = &un[i];
    if (u->n != 0 || u->f > 0) return;
    u->n = 1; u->a = 0; u->b = fuse;
}

static void mine_hit(int k) { mine_trigger(k, 0.3f); }

static void tick_mine(int i)                 /* Mine::Tick 0x000355cc, Explode 0x00035354 */
{
    Unit *u = &un[i];
    int j;
    u->vx *= 0.9f; u->vy *= 0.9f;
    if (u->f > 0) { u->f -= TICK; return; }
    if (u->n == 0) {                         /* armed: the ship coming near starts a 1 s fuse */
        float dx = pl.x - u->x, dy = pl.y - u->y, rr = (u->r + pl.r) * 2;
        if (pl.alive && dx * dx + dy * dy <= rr * rr) { u->n = 1; u->a = 0; u->b = 1.0f; }
    } else if (u->n == 1) {
        u->a += TICK;
        if (u->a >= u->b) {                  /* goes off */
            u->n = 2; u->a = 0; u->b = 0.5f; u->c = 7; u->d = 0;
            burst(u->x, u->y, 0xffd040ffu, 60, 300);
            grid_force(u->x, u->y, 900, 120);
            sfx("Enemy_well_explode", 46, 0.03);
        }
    } else {
        u->a += TICK;
        if (u->a >= u->b || u->c < 0.1f) { u->type = U_NONE; return; }
        u->c = 0.975f * fmaxf(u->c - 0.025f, 0.001f);   /* the shockwave grows, slowing */
        u->d += u->c;
        for (j = 0; j < MAX_UNITS; j++) {
            Unit *e = &un[j];
            float dx, dy, d2;
            if (j == i || !e->type || e->appear < 1) continue;
            dx = e->x - u->x; dy = e->y - u->y; d2 = dx * dx + dy * dy;
            if (e->type == U_MINE) {         /* sets off other mines a little further out */
                if (e->n == 0 && d2 <= (u->d * 1.5f) * (u->d * 1.5f))
                    mine_trigger(j, fminf(d2 / 3600.0f, 1) * 0.09f + 0.01f);
            } else if (e->type != U_MINELAYER && e->type != U_HILL && d2 <= u->d * u->d) {
                damage_unit(j, DMG_WELLBOMB, u->x, u->y);
            }
        }
    }
}

/* NUFO::SpawnShower 0x00037ccc: a formation, 4 rows of 6, 40 apart, crossing
 * the arena at 50 units/s */
static void nufo_shower(float dx, float dy)
{
    float l = sqrtf(dx * dx + dy * dy), D = 2 * sqrtf(AW * AW + AH * AH), px, py;
    int r, c;
    if (l < 0.01f) { dx = 1; dy = 0; l = 1; }
    dx /= l; dy /= l;
    px = dy; py = -dx;
    for (r = 0; r < 4; r++)
        for (c = 0; c < 6; c++) {
            float along = -D / 2 - r * 40, side = (c - 2.5f) * 40;
            int i = unit_new_free(U_NUFO, dx * along + px * side, dy * along + py * side);
            if (i < 0) return;
            un[i].vx = dx * 50; un[i].vy = dy * 50;
            un[i].c = dx; un[i].d = dy;
            un[i].e = D / 2 + 60;            /* gone once this far past the middle */
            un[i].f = frand() * 2 * PI;
            un[i].appear = 1;
        }
}

static void tick_nufo(int i)                 /* NUFO::Tick 0x000378b8 (the wobble is approximate) */
{
    Unit *u = &un[i];
    u->f += TICK * PI;
    u->vx = u->c * 50 - u->d * sinf(u->f) * 20;
    u->vy = u->d * 50 + u->c * sinf(u->f) * 20;
    u->ang += TICK;
    if (u->x * u->c + u->y * u->d > u->e) u->type = U_NONE;
}

/* ---- Meteor: MeteorShower::SpawnShower 0x000349ac ---- */

static void meteor_shower(float dx, float dy)
{
    float l = sqrtf(dx * dx + dy * dy), D = 2 * sqrtf(AW * AW + AH * AH), px, py;
    int r, c;
    static const float col[4] = { 0, 0.25f, 0.5f, 0.75f };
    if (l < 0.01f) { dx = 1; dy = 0; l = 1; }
    dx /= l; dy /= l;
    px = dy; py = -dx;                       /* the formation's sideways axis */
    for (r = 0; r < 8; r++)                  /* 8 rows of 4, D/8 apart, jittered by D/16 */
        for (c = 0; c < 4; c++) {
            float bx = -dx * D / 2 - px * D / 2 - dx * r * D / 8 + px * col[c] * D;
            float by = -dy * D / 2 - py * D / 2 - dy * r * D / 8 + py * col[c] * D;
            int i = unit_new_free(U_METEOR, bx + frange(-1, 1) * D / 16, by + frange(-1, 1) * D / 16);
            if (i < 0) return;
            un[i].vx = dx * 150; un[i].vy = dy * 150;          /* 150 units/s */
            un[i].c = dx * D; un[i].d = dy * D;                /* target: across the arena */
            un[i].a = 0.1f + frand() * 0.05f;   /* spin: 0.1 + rand * 1.16e-11 */
            un[i].appear = 1;
        }
}

static void tick_meteor(int i)               /* MeteorShower::Tick 0x0003481c */
{
    Unit *u = &un[i];
    u->ang += u->a;
    if ((u->c - u->x) * u->vx + (u->d - u->y) * u->vy < 0) u->type = U_NONE;   /* past the target */
}

/* ---- Whirlpool: BlackHole::Tick 0x00028f6c ---- */

static void swirl(float *vx, float *vy, float ex, float ey, const Unit *h)
{
    float dx = h->x - ex, dy = h->y - ey, d = sqrtf(dx * dx + dy * dy), a, b, s, k;
    if (d < 1) return;
    dx /= d; dy /= d;
    s = h->b * h->a;                         /* strength x direction */
    a = clampf(d - h->r * 0.00285714f, 0, 1);
    k = (0.6f * a) * (0.6f * a) * s / TICK;  /* round and round */
    *vx += dy * k; *vy -= dx * k;
    k = h->b * 0.2f / TICK;                  /* a gentle pull inwards */
    *vx += dx * k; *vy += dy * k;
    b = clampf((d - h->r) / (2 * h->r), 0, 1);
    k = (1 - b) * (1 - b) * s / TICK;        /* faster near the middle */
    *vx += dy * k; *vy -= dx * k;
}

static void tick_blackhole(int i)
{
    Unit *u = &un[i];
    int j;
    u->c -= TICK;
    if (!u->n) {
        if (u->c < 0) { u->n = 1; u->c = 2; }            /* start turning round */
    } else if (u->c >= 0) {
        float t = u->c * 0.5f;
        u->b = 2 * t * t * (3 - 2 * t) - 1;              /* strength eases from 1 to -1 */
    } else {
        u->b = 1; u->a = -u->a; u->n = 0;
        u->c = 6 + frand() * 4;                          /* the next turn in 6-10 s */
    }
    for (j = 0; j < MAX_UNITS; j++) {
        Unit *e = &un[j];
        if (j == i || !e->type || e->type == U_PROTON || e->appear < 1) continue;
        swirl(&e->vx, &e->vy, e->x, e->y, u);
    }
    if (pl.alive) swirl(&pl.vx, &pl.vy, pl.x, pl.y, u);
    u->ang -= u->a * u->b * TICK * 2.5f;
    u->vx = u->vy = 0;
}

/* ---- Titans: Asteroid 0x0002649c (Tick 0x00026c64, Killed 0x00026b24) ---- */

static void tick_titan(int i)
{
    Unit *u = &un[i];
    float s;
    u->ang += u->a;
    u->vx += frange(-1, 1) * 5; u->vy += frange(-1, 1) * 5;  /* wander */
    if (pl.alive) {                          /* and drift towards the ship */
        float dx = pl.x - u->x, dy = pl.y - u->y, d = sqrtf(dx * dx + dy * dy) + 0.01f;
        u->vx += dx / d * 0.1f * 50 * 0.1f; u->vy += dy / d * 0.1f * 50 * 0.1f;
    }
    s = sqrtf(u->vx * u->vx + u->vy * u->vy) + 0.01f;
    s = s + (50 - s) * 0.1f;                 /* speed eases toward 50 */
    {
        float l = sqrtf(u->vx * u->vx + u->vy * u->vy) + 0.01f;
        u->vx = u->vx / l * s; u->vy = u->vy / l * s;
    }
    if (u->flash > 0) u->flash -= 0.1f;
}

static void titan_killed(Unit *u, int dmg, float bvx, float bvy)
{
    int k, n, child = u->n;
    if (dmg != DMG_BULLET) return;           /* only shot ones break up */
    if (u->f) {                              /* a big one: two medium ones fly off */
        for (k = 0; k < 2; k++) {
            int j = unit_new(U_TITAN, u->x, u->y);
            Unit *c;
            if (j < 0) return;
            c = &un[j];
            c->n = child; c->m = 8; c->e = frange(2, 3); c->f = 0; c->r = 8 * c->e;
            c->appear = 1;
            c->vx = (bvx + 20) * 0.2f * (k ? 1 : -1); c->vy = (bvy + 20) * 0.2f * (k ? -1 : 1);
            c->a = frand() * 0.0005f * (frand() < 0.5f ? -1 : 1);
        }
        return;
    }
    n = child == U_WANDERER ? 8 : child == U_WEAVER ? 6 : child == U_SPINNER || child == U_REPULSAR ? 4 :
        child == U_PROTON ? 10 : 5;          /* a medium one: a swarm of its kind */
    for (k = 0; k < n; k++) {
        int j = spawn_bad_guy(child, u->x + frange(-12, 12), u->y + frange(-12, 12), frand() * 2 * PI);
        if (j >= 0) un[j].appear = 1;
    }
}

/* ---- Amoeba (only in two Sequence levels; simplified: see notes) ---- */

static void tick_amoeba(int i)
{
    Unit *u = &un[i];
    int j, have = 0;
    if (pl.alive) {
        float dx = pl.x - u->x, dy = pl.y - u->y, d = sqrtf(dx * dx + dy * dy) + 0.01f;
        u->vx = (u->vx + dx / d * 4) * 0.9f; u->vy = (u->vy + dy / d * 4) * 0.9f;
    }
    for (j = 0; j < MAX_UNITS; j++) have += un[j].type == U_CYTO && un[j].link == i && un[j].link_id == u->id;
    u->a -= TICK;
    if (have < u->m && u->a <= 0) {          /* Unit.Amoeba.CytoplasmSpawnTime 0.25 */
        int k = unit_new(U_CYTO, u->x, u->y);
        if (k >= 0) {
            un[k].link = i; un[k].link_id = u->id; un[k].appear = 1;
            un[k].f = frand() * 2 * PI; un[k].e = frange(12, 26);
        }
        u->a = have < 10 ? 0 : 0.25f;
    }
    if (u->flash > 0) u->flash -= 0.1f;
}

static void tick_cyto(int i)
{
    Unit *u = &un[i];
    Unit *n;
    float tx, ty;
    if (!unit_alive(u->link, u->link_id)) { u->type = U_NONE; return; }
    n = &un[u->link];
    u->f += TICK * 1.5f;
    tx = n->x + cosf(u->f) * u->e; ty = n->y + sinf(u->f) * u->e;
    u->vx = (u->vx + (tx - u->x) * 0.5f * 60 * 0.025f) * 0.9f;   /* TendToPosition / Damp */
    u->vy = (u->vy + (ty - u->y) * 0.5f * 60 * 0.025f) * 0.9f;
    u->r = 3 + 3 * (0.5f + 0.5f * sinf(u->f * 20 * 0.1f));       /* RadiusLow..High */
}

/* ---- per-unit behaviour (each class's Tick) ---- */

static void separate_same(int i, float strength)       /* ApplySimilarAvoidance 0x0002b71c */
{
    Unit *a = &un[i];
    int j;
    for (j = 0; j < MAX_UNITS; j++) {
        Unit *b = &un[j];
        float dx, dy, d2, rr, d, push;
        if (j == i || b->type != a->type || b->appear < 1) continue;
        dx = b->x - a->x; dy = b->y - a->y; d2 = dx * dx + dy * dy;
        rr = a->r + b->r;
        if (d2 >= rr * rr) continue;
        if (d2 < 0.1f) { a->vx += frange(-1, 1); a->vy += frange(-1, 1); continue; }
        d = sqrtf(d2);
        push = strength * (rr - d) * 0.5f;
        a->vx -= dx / d * push; a->vy -= dy / d * push;
        b->vx += dx / d * push; b->vy += dy / d * push;
    }
}

static int dangerous_bullet(float x, float y, float range)   /* FindMostDangerousProjectile 0x0003eda8 */
{
    int i, best_i = -1;
    float best_s = 0;
    for (i = 0; i < MAX_BUL; i++) {
        Bullet *b = &bul[i];
        float dx, dy, d, sp, s;
        if (!b->alive) continue;
        dx = x - b->x; dy = y - b->y; d = sqrtf(dx * dx + dy * dy);
        if (d > range || d < 0.001f) continue;
        sp = sqrtf(b->vx * b->vx + b->vy * b->vy) + 0.001f;
        s = (dx * b->vx + dy * b->vy) / (d * sp) / (d + 1);   /* heading at us, and close */
        if (s > best_s) { best_s = s; best_i = i; }
    }
    return best_i;
}

static void tick_grunt(int i)                /* Grunt::Tick 0x0002fe60 */
{
    Unit *u = &un[i];
    float dx, dy, d, w;
    u->b += TICK * 3.75f;
    w = cosf(u->b);
    if (pl.alive) {
        dx = pl.x - u->x; dy = pl.y - u->y; d = sqrtf(dx * dx + dy * dy);
        if (d > 0.001f) { u->vx += dx / d * u->a; u->vy += dy / d * u->a; }
        u->vx *= 0.9f; u->vy *= 0.9f;
        u->a += 0.005f;
    }
    separate_same(i, 0.9f);
    u->sx = 1 + 0.2f * w; u->sy = 1 - 0.2f * w;
}

static void tick_wanderer(int i)             /* Wanderer::Tick 0x0004699c */
{
    Unit *u = &un[i];
    float s, ns;
    u->ang += u->a;
    if (pl.alive) {
        float dx = pl.x - u->x, dy = pl.y - u->y, d = sqrtf(dx * dx + dy * dy) + 0.001f;
        u->vx += dx / d * 0.1f; u->vy += dy / d * 0.1f;
    }
    u->vx += frange(-5, 5); u->vy += frange(-5, 5);
    s = sqrtf(u->vx * u->vx + u->vy * u->vy) + 0.001f;
    ns = s + (50 - s) * 0.05f;
    u->vx = u->vx / s * ns; u->vy = u->vy / s * ns;
}

static void tick_weaver(int i)               /* Weaver::Tick 0x0004717c */
{
    Unit *u = &un[i];
    int b;
    if (!pl.alive) return;
    b = dangerous_bullet(u->x, u->y, 64);
    if (b >= 0) {
        float dx = u->x - bul[b].x, dy = u->y - bul[b].y, d = sqrtf(dx * dx + dy * dy) + 0.001f;
        float bs = sqrtf(bul[b].vx * bul[b].vx + bul[b].vy * bul[b].vy) + 0.001f;
        float a = ((dx / d) * (bul[b].vx / bs) + (dy / d) * (bul[b].vy / bs) + 1) * 0.5f, push;
        push = a > 0.95f ? (1 - (a - 0.95f) * 20) * 90 : a > 0.375f ? sqrtf(a) * 90 : 0;
        u->vx += dx / d * push; u->vy += dy / d * push;
    }
    u->a -= TICK;                            /* sidestep timer */
    if (u->a <= 0) {
        u->b = frange(-1, 1) * 9;
        u->a = (frand() + 1) * 0.2f;
    }
    {
        float dx = pl.x - u->x, dy = pl.y - u->y, d = sqrtf(dx * dx + dy * dy);
        if (d > 0.001f) {
            dx /= d; dy /= d;
            u->vx += dx * 18 + dy * u->b;
            u->vy += dy * 18 - dx * u->b;
        }
    }
    u->vx *= 0.9f; u->vy *= 0.9f;
    {
        float sp = sqrtf(u->vx * u->vx + u->vy * u->vy) * TICK;
        u->ang += sp * sp * 0.01f;
    }
    separate_same(i, 0.5f);
}

static void tick_spinner(int i)              /* Spinner::Tick 0x0004599c */
{
    Unit *u = &un[i];
    if (pl.alive) {
        float dx = pl.x - u->x, dy = pl.y - u->y, d = sqrtf(dx * dx + dy * dy);
        if (d > 0.001f) { u->vx += dx / d * 7.5f; u->vy += dy / d * 7.5f; }
        u->vx *= 0.965f; u->vy *= 0.965f;
    }
    u->a += 0.1f;
    u->ang = sinf(u->a);
}

static void tick_tiny(int i)                 /* TinySpinner::Tick 0x00046450 */
{
    Unit *u = &un[i];
    u->vx += sinf(u->ang) * 24; u->vy += cosf(u->ang) * 24;
    u->vx *= 0.9f; u->vy *= 0.9f;
    u->ang += u->a;
}

static void tick_snake(int i)                /* SnakeHead::Tick 0x00044c18 */
{
    Unit *u = &un[i];
    u->b += TICK;
    u->vx += cosf(u->ang) * 36; u->vy += sinf(u->ang) * 36;
    u->vx *= 0.8f; u->vy *= 0.8f;
    if (pl.alive) {
        float target = atan2f(pl.y - u->y, pl.x - u->x) - cosf(u->b) - 1.75f * cosf(4 * u->b);
        u->ang += wrap_pi(target - u->ang) * 0.1f;
    }
}

static void tick_body(int i)                 /* SnakeBody::Tick 0x00043e7c */
{
    Unit *u = &un[i];
    if (!unit_alive(u->head, u->head_id) || un[u->head].type != U_SNAKE) {
        if (u->c == 0) u->c = 0.05f + 0.035f * u->n;     /* the tail dies off in a ripple */
        u->c -= TICK;
        u->vx *= 0.8f; u->vy *= 0.8f;
        if (u->c <= 0) kill_unit(i, DMG_TOUCH);
        return;
    }
    if (unit_alive(u->link, u->link_id)) {
        Unit *l = &un[u->link];
        float dx = l->x - u->x, dy = l->y - u->y, d = sqrtf(dx * dx + dy * dy + 1e-5f);
        if (d <= 1) {
            u->ang = l->ang;
        } else {
            float pull = clampf(d * 0.125f, 0.5f, 2) * 36;
            u->vx += dx / d * pull; u->vy += dy / d * pull;
            u->vx *= 0.8f; u->vy *= 0.8f;
            u->vx += (l->vx - u->vx) * 0.2f; u->vy += (l->vy - u->vy) * 0.2f;
            u->ang = atan2f(dy, dx);
        }
    }
    if (u->flash > 0) u->flash -= 0.2f;
}

static void well_apply_force(int i)          /* GravityWell::ApplyForce 0x0002ed00 */
{
    Unit *w = &un[i];
    int j;
    for (j = 0; j < MAX_UNITS; j++) {
        Unit *e = &un[j];
        float dx, dy, d, k, ex, ey;
        if (j == i || !e->type || e->appear < 1) continue;
        dx = w->x - e->x; dy = w->y - e->y; d = sqrtf(dx * dx + dy * dy);
        if (d > 256) continue;
        if (d < w->r) {                      /* inside: enemies are eaten */
            if (w->a <= 12 && e->type != U_WELL) {
                kill_unit(j, DMG_EATEN);
                w->m++; w->a += 1;
                w->e = 1;                    /* +0x91 */
                sfx("Enemy_red_suck", 12, 0.05);
            }
            continue;
        }
        dx /= d; dy /= d;
        if (e->type == U_BODY) {
            k = fminf(16 / d, 0.175f);
            ex = dx; ey = dy;
        } else {
            float swirl;
            if (e->type == U_SNAKE) k = fminf(500 / (d * d), 0.5f);
            else k = fminf(1500 / (d * d), 0.75f);
            swirl = 0.4f * k;
            ex = dx + (dy - dx) * swirl;     /* bend toward the tangent (dy, -dx) */
            ey = dy + (-dx - dy) * swirl;
            {
                float l = sqrtf(ex * ex + ey * ey) + 1e-6f;
                ex /= l; ey /= l;
            }
        }
        e->vx += ex * k * 60; e->vy += ey * k * 60;
    }
    if (pl.alive) {                          /* the player (flag 2): straight in */
        float dx = w->x - pl.x, dy = w->y - pl.y, d = sqrtf(dx * dx + dy * dy);
        if (d < 256 && d > 0.5f) {
            float k = fminf(16 / d, 0.5f);
            pl.vx += dx / d * k * 60; pl.vy += dy / d * k * 60;
        }
    }
    for (j = 0; j < MAX_BUL; j++) {          /* bullets curve round it */
        Bullet *b = &bul[j];
        float dx, dy, d, sp, px, py, k;
        if (!b->alive) continue;
        dx = w->x - b->x; dy = w->y - b->y; d = sqrtf(dx * dx + dy * dy);
        if (d < 1 || d > 256) continue;
        dx /= d; dy /= d;
        sp = sqrtf(b->vx * b->vx + b->vy * b->vy) + 0.001f;
        px = dy; py = -dx;                   /* pick the perpendicular the bullet is turning toward */
        if ((b->vx / sp) * dy - (b->vy / sp) * dx < 0) { px = -px; py = -py; }
        k = fminf(64 / d, 0.5f) * 15 * b->grav;
        b->vx += px * k; b->vy += py * k;
    }
}

static void tick_well(int i)                 /* GravityWell::Tick 0x0002f448 */
{
    Unit *w = &un[i];
    int k;
    w->e = 0;
    if (w->n) {                              /* active: pull, and spin faster as it fills */
        well_apply_force(i);
        if (!w->type) return;
        w->f += 0.02f + w->m * 0.01f;
        grid_force(w->x, w->y, -55, 70);
    }
    w->d += 0.05f + w->a * 0.075f;           /* pulse phase */
    if ((int)w->a == 12 && w->n) {           /* full: warn every 30 ticks */
        if ((w->c += 1) >= 30) { w->c = 0; sfx("GravityWellWarning", 13, 0.4); }
    }
    if (w->a > 12) {                         /* overfull: burst into 15 Protons */
        static const struct { int n; float sp; } rings[3] = { { 7, 150 }, { 5, 120 }, { 3, 90 } };
        float x = w->x, y = w->y;
        int r2;
        w->type = U_NONE;
        burst(x, y, 0xff3a20ffu, 110, 380);
        grid_force(x, y, 1100, 110);
        sfx("Gravity_well_explode", 4, 0);
        for (r2 = 0; r2 < 3; r2++)
            for (k = 0; k < rings[r2].n; k++) {
                float a = k * 2 * PI / rings[r2].n;
                int j = unit_new(U_PROTON, x, y);
                if (j < 0) break;
                un[j].appear = 1;
                un[j].nocollide = 0.166f;
                un[j].vx = cosf(a) * rings[r2].sp * 1.3f;
                un[j].vy = sinf(a) * rings[r2].sp * 1.3f;
                un[j].c = frand() * 2 * PI;
            }
        return;
    }
    if (pl.alive) {                          /* drift toward the player */
        float dx = pl.x - w->x, dy = pl.y - w->y, d = sqrtf(dx * dx + dy * dy);
        if (d > 0.001f) { w->vx += dx / d * 0.6f; w->vy += dy / d * 0.6f; }
    }
    w->vx *= 0.95f; w->vy *= 0.95f;
    w->r = w->a + 5 + (w->n ? 2 * cosf(w->d) : 0);
    separate_same(i, 1.0f);
    if (w->flash > 0) w->flash -= 0.1f;
}

static void tick_proton(int i)               /* Proton::Tick */
{
    Unit *u = &un[i];
    if (u->nocollide > 0) { u->nocollide -= TICK; return; }
    if (pl.alive) {
        float dx = pl.x - u->x, dy = pl.y - u->y, d = sqrtf(dx * dx + dy * dy);
        if (d > 0.001f) { u->vx += dx / d * 8; u->vy += dy / d * 8; }
    }
    u->a -= TICK;
    if (u->a <= 0) {
        u->b = frange(-4, 4); u->d = frange(-4, 4);
        u->a = (frand() + 1) * 0.2f;
    }
    u->vx = (u->vx + u->b) * 0.97f; u->vy = (u->vy + u->d) * 0.97f;
    u->ang += 0.25f;
}

static int wall_probe(float x, float y)
{
    return x < -AW || x > AW || y < -AH || y > AH;
}

static void tick_rocket(int i)               /* Rocket::Tick 0x0004193c */
{
    Unit *u = &un[i];
    if (!u->n) {                             /* flying straight */
        float hx = cosf(u->a) * u->b, hy = sinf(u->a) * u->b;
        u->vx += hx * 12.5f; u->vy += hy * 12.5f;      /* Unit.Rocket.Speed */
        u->vx *= 0.95f; u->vy *= 0.95f;                 /* Unit.Rocket.Drag */
        if (wall_probe(u->x + hx * 35, u->y + hy * 35)) {   /* Unit.Rocket.LookAhead */
            u->n = 1;
            u->b = -u->b;
            u->c = frand() < 0.5f ? PI : -PI;            /* turn left to do */
            sfx("Enemy_rocket_turn", 14, 0.08);
        }
    } else {                                 /* U-turn */
        float step = clampf(u->c * 0.25f, -0.15f, 0.15f);   /* DampRotation, MaxRotation */
        u->ang += step; u->c -= step;
        u->vx *= 0.95f; u->vy *= 0.95f;                     /* Unit.Rocket.Brake */
        if (fabsf(u->c) < 0.01f) { u->n = 0; u->c = 0; }
    }
}

static void tick_duck(int i)                 /* Duck::Tick 0x000298bc */
{
    Unit *u = &un[i];
    float s;
    u->a += TICK;
    if (u->a >= 1.0f) {                      /* Unit.Duck.Tick: a new step */
        int dir, tries = 0;
        u->a -= 1.0f;
        do {
            if (frnd() % 6 < 4 || !pl.alive) {
                dir = frnd() & 3;
            } else {
                float dx = pl.x - u->x, dy = pl.y - u->y;
                dir = fabsf(dx) > fabsf(dy) ? (dx < 0 ? 0 : 1) : (dy < 0 ? 2 : 3);
            }
        } while (dir == u->n && ++tries < 8);
        u->n = dir;
        u->c = dir == 0 ? -1 : dir == 1 ? 1 : 0;           /* step direction */
        u->d = dir == 2 ? -1 : dir == 3 ? 1 : 0;
        u->ang = atan2f(u->d, u->c);
        sfx("Enemy_duck_move", 15, 0.15);
    }
    s = fabsf(sinf(PI * u->a)) * 40;
    u->vx = (u->vx + u->c * s) * 0.9f;
    u->vy = (u->vy + u->d * s) * 0.9f;
    u->r = 7 - 2 * fabsf(sinf(u->a * 2 * PI));
    separate_same(i, 0.5f);
}

static void tick_gate(int i)                 /* Gate::Tick 0x0002d458 (simplified, see notes) */
{
    Unit *u = &un[i];
    Unit *p;
    if (!unit_alive(u->link, u->link_id)) { kill_unit(i, DMG_TOUCH); return; }
    p = &un[u->link];
    if (u->n) {                              /* the leader steers the pair */
        float mx = (u->x + p->x) / 2, my = (u->y + p->y) / 2, a;
        u->ang += 2 * TICK * 0.35f;          /* Unit.Gate.RotationSpeed */
        u->b = (u->b + frange(-1, 1) * 1.0f) * 0.975f;     /* Wobble / WobbleDamp */
        u->c = (u->c + frange(-1, 1) * 1.0f) * 0.975f;
        a = u->ang;
        {
            float tx = mx + u->b * 0.6f, ty = my + u->c * 0.6f;
            float ux = tx + cosf(a) * 32.5f, uy = ty + sinf(a) * 32.5f;
            float qx = tx - cosf(a) * 32.5f, qy = ty - sinf(a) * 32.5f;
            u->vx = (u->vx + (ux - u->x) * 4) * 0.9f; u->vy = (u->vy + (uy - u->y) * 4) * 0.9f;  /* Damp */
            p->vx = (p->vx + (qx - p->x) * 4) * 0.9f; p->vy = (p->vy + (qy - p->y) * 4) * 0.9f;
        }
    }
    if (u->flash > 0) u->flash -= 0.1f;
}

static void tick_repulsar(int i)             /* Repulsar::Tick 0x000408c4 (simplified) */
{
    Unit *u = &un[i];
    float diff, step;
    if (!pl.alive) { u->vx *= 0.9f; u->vy *= 0.9f; return; }
    diff = wrap_pi(atan2f(pl.y - u->y, pl.x - u->x) - u->ang);
    step = clampf(diff * 0.5f, -0.15f, 0.15f);
    u->ang += step;
    if (fabsf(diff) < 0.2f) {
        u->a += TICK;                        /* charge */
        if (u->a > 0.6f) {
            u->vx += cosf(u->ang) * 36 * 6; u->vy += sinf(u->ang) * 36 * 6;   /* dash (Unit.Repulsar.Speed) */
            u->a = 0;
            sfx("Repulsar_activate", 16, 0.2);
        }
    } else if (u->a > 0) {
        u->a -= TICK;
    }
    u->vx *= 0.94f; u->vy *= 0.94f;
    if (u->flash > 0) u->flash -= 0.1f;
}

/* ---- the player (Player::Tick 0x0003b528, FireProjectile 0x0003b11c, FireSecondary 0x000398dc) ---- */

static void fire_volley(float dx, float dy)
{
    static const struct { float off, speed; } shot[3] = { { -0.03f, 520 }, { 0, 600 }, { 0.03f, 520 } };
    float a = atan2f(dy, dx);
    int s, i;
    for (s = 0; s < 3; s++)
        for (i = 0; i < MAX_BUL; i++)
            if (!bul[i].alive) {
                Bullet *b = &bul[i];
                float sa = a + shot[s].off;
                b->alive = 1;
                b->x = b->px = pl.x + cosf(a) * 8;
                b->y = b->py = pl.y + sinf(a) * 8;
                b->vx = cosf(sa) * shot[s].speed + pl.vx;
                b->vy = sinf(sa) * shot[s].speed + pl.vy;
                b->grav = 1;
                break;
            }
    pl.cooldown += 0.1f;
    sfx("Fire_normal", 17, 0.09);
}

static long long bests[M_COUNT];

static void save_bests(void)
{
    FILE *f = fopen(plat_save_file(), "w");
    int m;
    if (!f) return;
    for (m = 0; m < M_COUNT; m++) fprintf(f, "best %d %lld\n", m, bests[m]);
    fclose(f);
}

static void game_over(void)
{
    scene = SC_OVER; scene_t = 0;
    if (score > best) {
        best = bests[mode] = score;
        save_bests();
    }
    sfx(mode == M_DEADLINE ? "DeadlineEnd" : "Game_over", 19, 0);
}

static void player_die(void)
{
    pl.alive = 0;
    pl.respawn_t = 1.0f;                     /* respawn timer (gGameInfo + 0x7968) */
    deaths++;
    burst(pl.x, pl.y, 0xffffffffu, 140, 420);
    burst(pl.x, pl.y, 0x9ad0ffffu, 80, 260);
    grid_force(pl.x, pl.y, 1300, 130);
    sfx("Ship_explode", 18, 0);
    lc.pause = 1;                            /* Player::Killed sets *0x13c274 = 1: spawning pauses */
    if (mode == M_DEADLINE) casual_death();  /* Player::Damage 0x0003a02c */
    if (mode == M_SEQUENCE && sq.phase == 0) seq_complete(2);   /* losing a ship ends the level */
}

static void update_player(const PlatInput *in)
{
    float lx = in->lx, ly = in->ly, rx = in->rx, ry = in->ry, m2;
    if (in->held & BTN_LEFT) lx = -1;
    if (in->held & BTN_RIGHT) lx = 1;
    if (in->held & BTN_UP) ly = -1;
    if (in->held & BTN_DOWN) ly = 1;
    if (in->held & BTN_SQUARE) rx = -1;
    if (in->held & BTN_CIRCLE) rx = 1;
    if (in->held & BTN_TRIANGLE) ry = -1;
    if (in->held & BTN_CROSS) ry = 1;
    if (!pl.alive) {
        pl.respawn_t -= TICK;
        if (pl.respawn_t <= 0) {
            if (lives <= 0 && mode != M_DEADLINE) {   /* Deadline (GameInfo+0x7944 = 2) always respawns */
                game_over();
                return;
            }
            if (mode != M_DEADLINE) lives--; /* respawn where it died */
            pl.alive = 1; pl.vx = pl.vy = 0;
            pl.protect = 2.0f;
            pl.cooldown = 0;
            sfx("Player_Spawn", 20, 0);
        }
        return;
    }
    m2 = lx * lx + ly * ly;
    if (m2 > 1) { float m = sqrtf(m2); lx /= m; ly /= m; m2 = 1; }
    if (m2 <= 0.02f) {
        pl.vx *= 0.8f; pl.vy *= 0.8f;
    } else {
        pl.vx = (pl.vx + lx * 50) * 0.8f;
        pl.vy = (pl.vy + ly * 50) * 0.8f;
        pl.ang += wrap_pi(atan2f(ly, lx) - pl.ang) * 0.35f;
    }
    if (pl.protect > 0) pl.protect -= TICK;
    if (pl.bomb_cd > 0) pl.bomb_cd -= TICK;
    if ((in->pressed & (BTN_L | BTN_R)) && pl.bomb_cd <= 0) {
        pl.bomb_cd = 0.2f;
        if (bombs > 0) {
            bombs--;
            add_ring(DMG_BOMB, pl.x, pl.y, 400, 533.2f);
            if (mode == M_DEADLINE) casual_bomb();
            grid_force(pl.x, pl.y, 1600, 260);
            sfx("Fire_smartbomb", 21, 0);
        } else {
            sfx("Bomb_dryfire", 21, 0);
        }
    }
    if (pl.cooldown <= 0.001f) {
        /* Player::FireProjectile 0x0003b11c: no gun in Pacifism or Minelayer,
         * and in King only from inside a zone */
        int can = mode != M_PACIFISM && mode != M_MINELAYER && (mode != M_KING || in_hill(pl.x, pl.y));
        if (can && rx * rx + ry * ry > 0.2f) fire_volley(rx, ry);
    } else {
        pl.cooldown -= TICK;
    }
}

/* ---- one fixed tick of the whole game ---- */

static void move_and_walls(Unit *u)
{
    u->x += u->vx * TICK * MOVE;
    u->y += u->vy * TICK * MOVE;
    if (u->x < -AW + u->r) { u->x = -AW + u->r; if (u->vx < 0) u->vx = -u->vx; }
    if (u->x > AW - u->r) { u->x = AW - u->r; if (u->vx > 0) u->vx = -u->vx; }
    if (u->y < -AH + u->r) { u->y = -AH + u->r; if (u->vy < 0) u->vy = -u->vy; }
    if (u->y > AH - u->r) { u->y = AH - u->r; if (u->vy > 0) u->vy = -u->vy; }
}

static float seg_dist2(float px, float py, float ax, float ay, float bx, float by)
{
    float vx = bx - ax, vy = by - ay, wx2 = px - ax, wy2 = py - ay;
    float l2 = vx * vx + vy * vy, t = l2 > 0 ? (wx2 * vx + wy2 * vy) / l2 : 0, dx, dy;
    t = clampf(t, 0, 1);
    dx = ax + vx * t - px; dy = ay + vy * t - py;
    return dx * dx + dy * dy;
}

static int segs_cross(float ax, float ay, float bx, float by, float cx, float cy, float dx, float dy)
{
    float d1 = (dx - cx) * (ay - cy) - (dy - cy) * (ax - cx);
    float d2 = (dx - cx) * (by - cy) - (dy - cy) * (bx - cx);
    float d3 = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    float d4 = (bx - ax) * (dy - ay) - (by - ay) * (dx - ax);
    return ((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0));
}

static void sim_tick(const PlatInput *in)
{
    int i, j;
    float opx = pl.x, opy = pl.y;
    play_t += TICK;
    if (mode == M_DEADLINE) {                /* CasualGameUpdate; the game ends at 3 minutes */
        casual_update();
        if (play_t >= DEADLINE_TIME) { game_over(); return; }
    } else if (mode == M_SEQUENCE) {
        if (sq.phase == 1) {                 /* between levels */
            sq.inter -= TICK;
            if (sq.inter <= 0) {
                if (++sq.slot >= SEQ_LEVELS) { game_over(); return; }
                seq_start_level();
            }
        } else {
            seq_update();
        }
    } else if (pl.alive || lives > 0) {
        lc_update();
    }
    update_player(in);
    if (scene != SC_PLAY) return;

    /* units */
    for (i = 0; i < MAX_UNITS; i++) {
        Unit *u = &un[i];
        if (!u->type) continue;
        if (u->appear < 1) {                 /* spawning in: no tick, no movement */
            u->appear += TICK * 1.5f;
            if (u->appear > 1) u->appear = 1;
            continue;
        }
        switch (u->type) {
        case U_GRUNT: tick_grunt(i); break;
        case U_WANDERER: tick_wanderer(i); break;
        case U_WEAVER: tick_weaver(i); break;
        case U_SPINNER: tick_spinner(i); break;
        case U_TINY: tick_tiny(i); break;
        case U_SNAKE: tick_snake(i); break;
        case U_BODY: tick_body(i); break;
        case U_WELL: tick_well(i); break;
        case U_PROTON: tick_proton(i); break;
        case U_ROCKET: tick_rocket(i); break;
        case U_DUCK: tick_duck(i); break;
        case U_GATE: tick_gate(i); break;
        case U_REPULSAR: tick_repulsar(i); break;
        case U_HILL: tick_hill(i); break;
        case U_MINELAYER: tick_minelayer(i); break;
        case U_MINE: tick_mine(i); break;
        case U_NUFO: tick_nufo(i); break;
        case U_METEOR: tick_meteor(i); break;
        case U_BLACKHOLE: tick_blackhole(i); break;
        case U_TITAN: tick_titan(i); break;
        case U_AMOEBA: tick_amoeba(i); break;
        case U_CYTO: tick_cyto(i); break;
        }
        if (u->type == U_METEOR || u->type == U_NUFO) {   /* flags 0x800000: they cross the walls */
            u->x += u->vx * TICK * MOVE; u->y += u->vy * TICK * MOVE;
        } else if (u->type && u->type != U_HILL && u->type != U_BLACKHOLE) {
            move_and_walls(u);
        }
    }

    /* the player moves */
    if (pl.alive) {
        pl.x += pl.vx * TICK * MOVE;
        pl.y += pl.vy * TICK * MOVE;
        if (pl.x < -AW + pl.r) { pl.x = -AW + pl.r; pl.vx = 0; }
        if (pl.x > AW - pl.r) { pl.x = AW - pl.r; pl.vx = 0; }
        if (pl.y < -AH + pl.r) { pl.y = -AH + pl.r; pl.vy = 0; }
        if (pl.y > AH - pl.r) { pl.y = AH - pl.r; pl.vy = 0; }
    }

    /* bullets */
    for (i = 0; i < MAX_BUL; i++) {
        Bullet *b = &bul[i];
        if (!b->alive) continue;
        b->px = b->x; b->py = b->y;
        b->x += b->vx * TICK * MOVE;
        b->y += b->vy * TICK * MOVE;
        if (b->x < -AW || b->x > AW || b->y < -AH || b->y > AH) {   /* walls end them */
            b->alive = 0;
            burst(clampf(b->x, -AW, AW), clampf(b->y, -AH, AH), 0xffc080ffu, 3, 90);
            continue;
        }
        for (j = 0; j < MAX_UNITS; j++) {
            Unit *u = &un[j];
            float rr;
            if (!u->type || u->appear < 1 || u->nocollide > 0) continue;
            rr = u->r + 2;
            if (u->type == U_HILL) continue;
            if (seg_dist2(u->x, u->y, b->px, b->py, b->x, b->y) < rr * rr) {
                if (u->type == U_SPINNER) { u->c = b->vx; u->d = b->vy; }
                hit_vx = b->vx; hit_vy = b->vy;
                if (damage_unit(j, DMG_BULLET, b->px, b->py)) { b->alive = 0; break; }
            }
        }
    }

    /* rings: bombs and well blasts */
    for (i = 0; i < MAX_RING; i++) {
        Ring *r = &ring[i];
        if (!r->alive) continue;
        if (r->r >= r->max) { r->alive = 0; continue; }
        r->r = fminf(r->r + r->rate * TICK, r->max);
        for (j = 0; j < MAX_UNITS; j++) {
            Unit *u = &un[j];
            float dx, dy;
            if (!u->type || u->appear < 1) continue;
            if (r->kind == DMG_GATE && (u->type == U_GATE || u->type == U_MINELAYER)) continue;  /* ApplyGateBomb */
            dx = u->x - r->x; dy = u->y - r->y;
            if (dx * dx + dy * dy <= r->r * r->r)
                damage_unit(j, r->kind == DMG_GATE ? DMG_WELLBOMB : r->kind, r->x, r->y);
        }
    }

    /* gates: flying through the bar between the ends sets them off */
    if (pl.alive)
        for (i = 0; i < MAX_UNITS; i++) {
            Unit *u = &un[i];
            if (u->type != U_GATE || !u->n || u->appear < 1 || !unit_alive(u->link, u->link_id)) continue;
            if (segs_cross(opx, opy, pl.x, pl.y, u->x, u->y, un[u->link].x, un[u->link].y)) {
                /* UpdateGate 0x0002d7ac: a blast at each end (SmartBomb type 1:
                 * radius 70, 700 units/s), the chain counter, and both ends go */
                static const char *chain_sfx[5] = { "GateScore1", "GateScore2", "GateScore3", "GateScore4", "GateScore5" };
                float gx = (u->x + un[u->link].x) / 2, gy = (u->y + un[u->link].y) / 2;
                add_ring(DMG_GATE, u->x, u->y, 70, 700);
                add_ring(DMG_GATE, un[u->link].x, un[u->link].y, 70, 700);
                if ((float)play_t - gate_last_t > 1.0f || gate_chain > 4) gate_chain = 0;   /* Unit.Gate.ChainTime */
                gate_chain++;
                gate_last_t = (float)play_t;
                sfx(chain_sfx[gate_chain < 5 ? gate_chain - 1 : 4], 22, 0.1);
                kill_unit(i, DMG_GATE);
                grid_force(gx, gy, 900, 100);
            }
        }

    /* the player against enemies (Player::CollideWithEnemy 0x00038b90) */
    if (pl.alive && pl.protect <= 0)
        for (i = 0; i < MAX_UNITS; i++) {
            Unit *u = &un[i];
            float dx, dy, rr;
            /* not the zones, and not what carries flag 0x1000 (BlackHole 0x21054, Mine
             * 0x21454, Asteroid 0x41014): the black hole is created on top of the
             * ship at the start of Whirlpool, so touching those cannot be fatal */
            if (!u->type || u->appear < 1 || u->nocollide > 0 || u->type == U_HILL ||
                u->type == U_BLACKHOLE || u->type == U_MINE || u->type == U_TITAN) continue;
            dx = u->x - pl.x; dy = u->y - pl.y; rr = u->r + pl.r;
            if (dx * dx + dy * dy < rr * rr) {
                if (u->type == U_MINELAYER) {    /* flag 0x2000000: it blocks (Minelayer::Blocked) */
                    float d = sqrtf(dx * dx + dy * dy) + 0.001f, vn = (pl.vx * dx + pl.vy * dy) / d;
                    pl.x = u->x - dx / d * rr; pl.y = u->y - dy / d * rr;
                    if (vn > 0) { pl.vx -= 2 * vn * dx / d; pl.vy -= 2 * vn * dy / d; }
                    continue;
                }
                player_die();
                break;
            }
        }

    /* geoms (Mote::Tick 0x00036858, Mote::CollideWithPlayer 0x000361fc) */
    for (i = 0; i < MAX_MOTE; i++) {
        Mote *m = &mote[i];
        float dx, dy, d2;
        if (!m->alive) continue;
        m->t += TICK;
        if (m->t >= m->life) { m->alive = 0; continue; }
        if (m->vx * m->vx + m->vy * m->vy < 1024) { m->vx *= 0.985f; m->vy *= 0.985f; }
        m->ang += TICK * 5;
        if (pl.alive) {
            dx = pl.x - m->x; dy = pl.y - m->y; d2 = dx * dx + dy * dy;
            if (d2 < 45 * 45 && d2 > 0.01f) {           /* Unit.Mote.MagnetRange */
                float d = sqrtf(d2);
                m->vx += dx / d * 40; m->vy += dy / d * 40;
            }
            if (d2 < (pl.r + 5) * (pl.r + 5)) {          /* collected */
                m->alive = 0;
                mult += m->value;
                cg.geoms += m->value;            /* CasualGamePickup 0x00058268 */
                sfx("Pickup_geom", 23, 0.03);
                if (mult >= next_banner) {
                    next_banner += next_banner < 100 ? 10 : next_banner < 1000 ? 50 : 100;
                    sfx("Multiplier", 24, 0.2);
                }
                continue;
            }
        }
        m->x += m->vx * TICK * MOVE;
        m->y += m->vy * TICK * MOVE;
        if (m->x < -AW || m->x > AW) { m->vx = -m->vx; m->x = clampf(m->x, -AW, AW); }
        if (m->y < -AH || m->y > AH) { m->vy = -m->vy; m->y = clampf(m->y, -AH, AH); }
    }
    for (i = 0; i < MAX_UNITS; i++)
        if (un[i].flash > 0 && un[i].type != U_BODY && un[i].type != U_WELL && un[i].type != U_GATE &&
            un[i].type != U_REPULSAR)
            un[i].flash -= 0.1f;
}

/* ============================================================== DRAWING
 * This port's own look: every unit is a few glowing lines in the colour the
 * game's tuning gives it. */

static const float SH_SHIP[] = { 1.0f, 0, -0.75f, 0.9f, -0.35f, 0, -0.75f, -0.9f };
static const float SH_DIAMOND[] = { 0, -1, 0.7f, 0, 0, 1, -0.7f, 0 };
static const float SH_SQUARE[] = { -0.75f, -0.75f, 0.75f, -0.75f, 0.75f, 0.75f, -0.75f, 0.75f };
static const float SH_INNER[] = { 0, -0.6f, 0.6f, 0, 0, 0.6f, -0.6f, 0 };
static const float SH_BLADE[] = { 0, 0, 1, 0.15f, 0.55f, 0.6f };
static const float SH_CHEVRON[] = { 1.0f, 0, -0.8f, 0.8f, -0.35f, 0, -0.8f, -0.8f };
static const float SH_HEAD[] = { 1.2f, 0, 0.1f, 0.8f, -0.8f, 0.45f, -0.8f, -0.45f, 0.1f, -0.8f };
static const float SH_PENT[] = { 1, 0, 0.31f, 0.95f, -0.81f, 0.59f, -0.81f, -0.59f, 0.31f, -0.95f };
static const float SH_SLIVER[] = { 0, -1, 0.35f, 0, 0, 1, -0.35f, 0 };

static float glow_k(void) { return scr_h / 544.0f; }

static void draw_unit(const Unit *u);
static void draw_unit(const Unit *u)
{
    float x = u->x, y = u->y, r = u->r, a = u->appear < 1 ? 0.25f + 0.75f * u->appear : 1;
    unsigned c = u->flash > 0 ? 0xffffffffu : ut[u->type].color;
    int k;
    if (u->appear < 1) r *= 1 + (1 - u->appear) * 1.6f;      /* grows in while spawning */
    switch (u->type) {
    case U_GRUNT: {
        float pts[8];
        for (k = 0; k < 4; k++) { pts[k * 2] = SH_DIAMOND[k * 2] * u->sx; pts[k * 2 + 1] = SH_DIAMOND[k * 2 + 1] * u->sy; }
        wshape(x, y, r * 1.25f, 0, pts, 4, 1, c, a);
        break;
    }
    case U_WANDERER:
        for (k = 0; k < 4; k++) {
            float ang = u->ang + k * PI / 2, pts[6];
            int q;
            for (q = 0; q < 3; q++) {
                pts[q * 2] = SH_BLADE[q * 2];
                pts[q * 2 + 1] = SH_BLADE[q * 2 + 1] * (u->sx < 0 ? -1 : 1);
            }
            wshape(x, y, r * 1.3f, ang, pts, 3, 1, c, a);
        }
        break;
    case U_WEAVER:
        wshape(x, y, r * 1.3f, u->ang, SH_SQUARE, 4, 1, c, a);
        wshape(x, y, r * 1.3f, u->ang, SH_INNER, 4, 1, c, a * 0.85f);
        break;
    case U_SPINNER:
        wshape(x - 1.5f, y - 1.5f, r * 1.15f, u->ang * 0.6f, SH_SQUARE, 4, 1, c, a);
        wshape(x + 1.5f, y + 1.5f, r * 1.15f, u->ang * 0.6f + 0.2f, SH_SQUARE, 4, 1, c, a * 0.85f);
        break;
    case U_TINY:
        wshape(x, y, r * 1.3f, u->ang, SH_SQUARE, 4, 1, c, a);
        break;
    case U_SNAKE:
        wshape(x, y, r * 1.6f, u->ang, SH_HEAD, 5, 1, c, a);
        break;
    case U_BODY:
        if (unit_alive(u->link, u->link_id)) {
            const Unit *l = &un[u->link];
            gline(wx(x), wy(y), wx(l->x), wy(l->y), c, a * 0.55f);
        }
        wshape(x, y, r * 1.1f, u->ang, SH_DIAMOND, 4, 1, c, a * (u->n & 1 ? 0.6f : 0.9f));
        break;
    case U_WELL: {
        unsigned wc = u->n ? c : 0x3a6cffffu;            /* blue ring until it is woken */
        if (u->flash > 0) wc = 0xffffffffu;
        wcircle(x, y, r, 22, wc, a);
        wcircle(x, y, r * (0.55f + 0.1f * sinf(u->d * 3)), 16, wc, a * 0.7f);
        if (u->n) {
            for (k = 0; k < 3; k++) {             /* swirl arms while it pulls */
                float t0 = u->f + k * 2 * PI / 3, lx = x, ly = y, s;
                for (s = 1; s <= 6; s++) {
                    float t = t0 + s * 0.35f, rr = r * (0.4f + s * 0.3f);
                    float sx2 = x + cosf(t) * rr, sy2 = y + sinf(t) * rr;
                    gline(wx(lx), wy(ly), wx(sx2), wy(sy2), wc, a * 0.35f * (1 - s / 7));
                    lx = sx2; ly = sy2;
                }
            }
        }
        break;
    }
    case U_PROTON:
        wshape(x, y, r * 1.1f, u->ang, SH_DIAMOND, 4, 1, c, a);
        break;
    case U_ROCKET:
        wshape(x, y, r * 1.4f, u->ang + (u->b < 0 ? PI : 0) * 0, SH_CHEVRON, 4, 1, c, a);
        break;
    case U_DUCK:
        wshape(x, y, r * 1.2f, u->ang, SH_PENT, 5, 1, c, a);
        wshape(x + cosf(u->ang) * r * 0.45f, y + sinf(u->ang) * r * 0.45f, r * 0.35f, u->ang, SH_DIAMOND, 4, 1, c, a);
        break;
    case U_GATE:
        if (u->n && unit_alive(u->link, u->link_id)) {
            const Unit *p = &un[u->link];
            gline(wx(x), wy(y), wx(p->x), wy(p->y), 0x5080ffffu, a * 0.9f);   /* Unit.Gate.BarColour */
        }
        wcircle(x, y, r, 10, c, a);
        break;
    case U_HILL: {                               /* a zone: a ring, brighter while claimed */
        float al = u->n == 0 ? 0.55f : u->n == 3 ? 0.3f : 0.9f;
        wcircle(x, y, u->r, 40, u->n ? 0xffe060ffu : c, al);
        wcircle(x, y, u->r * 0.92f, 40, u->n ? 0xffe060ffu : c, al * 0.35f);
        if (u->n == 1 || u->n == 2) {            /* the time left, as an arc */
            float f = clampf(u->c / 2.5f, 0, 1), lx = 0, ly = 0;
            int s2;
            for (s2 = 0; s2 <= 24; s2++) {
                float t = -PI / 2 + s2 * 2 * PI * f / 24, sx = wx(x + cosf(t) * (u->r + 4)), sy = wy(y + sinf(t) * (u->r + 4));
                if (s2) gline(lx, ly, sx, sy, 0xffffffffu, 0.7f);
                lx = sx; ly = sy;
            }
        }
        break;
    }
    case U_MINE: {
        float al = a;
        if (u->n == 1 && fmodf(u->a * 12, 1) < 0.5f) c = 0xffffffffu;   /* the fuse blinks */
        if (u->n == 2) {                         /* the shockwave */
            wcircle(x, y, u->d, 36, 0xffd040ffu, clampf(1 - u->a / u->b, 0, 1));
            break;
        }
        if (u->f > 0) al *= 0.4f;
        wcircle(x, y, r, 8, c, al);
        wshape(x, y, r * 0.8f, u->ang, SH_INNER, 4, 1, c, al);
        break;
    }
    case U_MINELAYER:
        wshape(x, y, r, u->ang, SH_SQUARE, 4, 1, c, a);
        wshape(x, y, r * 0.6f, -u->ang, SH_SQUARE, 4, 1, c, a * 0.7f);
        break;
    case U_NUFO:
        wshape(x, y, r, 0, SH_DIAMOND, 4, 1, c, a);
        gline(wx(x - r * 1.3f), wy(y), wx(x + r * 1.3f), wy(y), c, a * 0.8f);
        break;
    case U_METEOR:
        wshape(x, y, r, u->ang, SH_PENT, 5, 1, c, a);
        break;
    case U_BLACKHOLE:
        wcircle(x, y, r, 30, c, a);
        for (k = 0; k < 4; k++) {                /* arms turning with it */
            float t0 = u->ang + k * PI / 2, lx = x, ly = y, s2;
            for (s2 = 1; s2 <= 8; s2++) {
                float t = t0 + s2 * 0.3f * (u->a > 0 ? 1 : -1), rr = r * (0.3f + s2 * 0.35f);
                float sx2 = x + cosf(t) * rr, sy2 = y + sinf(t) * rr;
                gline(wx(lx), wy(ly), wx(sx2), wy(sy2), c, a * 0.5f * (1 - s2 / 9.0f));
                lx = sx2; ly = sy2;
            }
        }
        break;
    case U_TITAN: {                              /* drawn as a giant one of what it breaks into */
        Unit big = *u;
        big.type = u->n; big.r = r; big.appear = 1;
        if (u->flash > 0) big.flash = u->flash;
        big.sx = big.sy = 1;
        draw_unit(&big);
        wcircle(x, y, r * 1.05f, 24, ut[u->n].color, a * 0.25f);
        break;
    }
    case U_AMOEBA:
        wcircle(x, y, r, 12, c, a);
        wcircle(x, y, r * 0.5f, 8, c, a * 0.8f);
        break;
    case U_CYTO:
        wcircle(x, y, r, 6, c, a * 0.8f);
        break;
    case U_REPULSAR:
        wcircle(x, y, r, 14, c, a);
        for (k = -2; k <= 2; k++) {              /* the shield on its front */
            float t0 = u->ang + k * 0.28f, t1 = t0 + 0.28f;
            gline(wx(x + cosf(t0) * r * 1.5f), wy(y + sinf(t0) * r * 1.5f),
                  wx(x + cosf(t1) * r * 1.5f), wy(y + sinf(t1) * r * 1.5f), 0xffd0d0ffu, a);
        }
        break;
    }
}

static int banner_mult;
static double banner_t;

static void hud(void)
{
    char buf[64], num[32];
    float k = glow_k(), cx = scr_w / 2.0f;
    int i;
    fmt_num(num, sizeof num, (long)score);
    text(16 * k, 12 * k, 22 * k, 0, 0xffffffffu, 1, num);
    snprintf(buf, sizeof buf, "x%d", mult);
    text(16 * k, 42 * k, 16 * k, 0, 0x82f532ffu, 1, buf);
    fmt_num(num, sizeof num, (long)(best > score ? best : score));
    snprintf(buf, sizeof buf, "HI %s", num);
    text(scr_w - 16 * k, 12 * k, 16 * k, 2, 0x9ad0ffffu, 0.9f, buf);
    text(scr_w - 16 * k, 34 * k, 12 * k, 2, 0x82f532ffu, 0.8f, modes[mode].name);   /* which mode this is */
    for (i = 0; i < lives && i < 9; i++)     /* ships left, to the left of centre */
        sshape(cx - 30 * k - i * 24 * k, 22 * k, 9 * k, -PI / 2, SH_SHIP, 4, 1, 0xffffffffu, 0.9f);
    for (i = 0; i < bombs && i < 9; i++) {   /* bombs, to the right */
        float bx = cx + 30 * k + i * 24 * k, by = 22 * k, rr = 7 * k, lx = 0, ly = 0;
        int s;
        for (s = 0; s <= 10; s++) {
            float t = s * 2 * PI / 10, sx = bx + cosf(t) * rr, sy = by + sinf(t) * rr;
            if (s) gline(lx, ly, sx, sy, 0x3a8cffffu, 0.9f);
            lx = sx; ly = sy;
        }
    }
    if (mode == M_DEADLINE) {                /* the clock */
        float left = DEADLINE_TIME - (float)play_t;
        if (left < 0) left = 0;
        snprintf(buf, sizeof buf, "%d:%02d", (int)left / 60, (int)left % 60);
        text(cx, 46 * k, 20 * k, 1, left < 10 ? 0xff6a5affu : 0xffffffffu, 1, buf);
    } else if (mode == M_SEQUENCE) {
        snprintf(buf, sizeof buf, "LEVEL %d  %d", sq.slot + 1, (int)ceilf(sq.left > 0 ? sq.left : 0));
        text(cx, 46 * k, 18 * k, 1, sq.left < 5 ? 0xff6a5affu : 0xffffffffu, 1, buf);
        if (sq.phase == 1)
            text(cx, scr_h * 0.4f, 30 * k, 1, sq.result == 1 ? 0x82f532ffu : 0xff6a5affu, 1,
                 sq.result == 1 ? "LEVEL COMPLETE" : sq.result == 3 ? "OUT OF TIME" : "SHIP LOST");
    } else if (mode == M_KING && pl.alive && !in_hill(pl.x, pl.y) && fmodf((float)now_t, 1) < 0.5f) {
        text(cx, 46 * k, 14 * k, 1, 0xffe060ffu, 0.8f, "FIRE FROM INSIDE A ZONE");
    }
    if (now_t - banner_t < 1.5) {            /* "Multiplier xN" */
        snprintf(buf, sizeof buf, "MULTIPLIER X%d", banner_mult);
        text(cx, scr_h * 0.82f, 18 * k, 1, 0x82f532ffu, (float)(1.5 - (now_t - banner_t)) / 1.5f, buf);
    }
}

static void draw_world(void)
{
    int i;
    float k = glow_k();
    unsigned wall = 0x9ad0ffffu;
    gline(wx(-AW), wy(-AH), wx(AW), wy(-AH), wall, 1);      /* the arena's wall */
    gline(wx(AW), wy(-AH), wx(AW), wy(AH), wall, 1);
    gline(wx(AW), wy(AH), wx(-AW), wy(AH), wall, 1);
    gline(wx(-AW), wy(AH), wx(-AW), wy(-AH), wall, 1);
    /* a soft glow behind every unit, geom and the ship (all in one batch) */
    for (i = 0; i < MAX_UNITS; i++)
        if (un[i].type) {
            const Unit *u = &un[i];
            float a = u->appear < 1 ? 0.25f : 1, r = u->r * zoom * (u->type == U_WELL ? 5 : 3.6f);
            unsigned c = u->type == U_WELL && !u->n ? 0x3a6cffffu : u->type == U_TITAN ? ut[u->n].color : ut[u->type].color;
            if (u->type == U_BODY && (u->n % 3)) continue;
            if (u->type == U_HILL || u->type == U_CYTO || (u->type == U_MINE && u->n == 2)) continue;
            if (u->type == U_TITAN || u->type == U_BLACKHOLE) r = u->r * zoom * 1.8f;
            sdot(wx(u->x), wy(u->y), r, with_alpha(u->flash > 0 ? 0xffffffffu : c, 0.42f * a));
        }
    for (i = 0; i < MAX_MOTE; i++)
        if (mote[i].alive)
            sdot(wx(mote[i].x), wy(mote[i].y), 18 * zoom, with_alpha(0x82f532ffu, 0.5f));
    if (pl.alive)
        sdot(wx(pl.x), wy(pl.y), 46 * zoom, 0xffffff70u);
    for (i = 0; i < MAX_BUL; i++)
        if (bul[i].alive)
            sdot(wx(bul[i].x), wy(bul[i].y), 12 * zoom, 0xffd06040u);
    /* rings */
    for (i = 0; i < MAX_RING; i++)
        if (ring[i].alive) {
            float a = 1 - ring[i].r / ring[i].max;
            wcircle(ring[i].x, ring[i].y, ring[i].r, 48,
                    ring[i].kind == DMG_BOMB ? 0x3a8cffffu : 0xff4a20ffu, 0.35f + 0.65f * a);
        }
    for (i = 0; i < MAX_MOTE; i++)
        if (mote[i].alive) {
            const Mote *m = &mote[i];
            float a = 1;
            if (m->t > m->life - 1 && fmodf(m->t * 15, 1) < 0.5f) a = 0.25f;   /* blinks in its last second */
            wshape(m->x, m->y, 3.2f + (m->value > 1 ? 1.2f : 0) + (m->value >= 5 ? 1.2f : 0),
                   m->ang, SH_SLIVER, 4, 1, 0x82f532ffu, a);
        }
    for (i = 0; i < MAX_BUL; i++)
        if (bul[i].alive) {
            float sp = sqrtf(bul[i].vx * bul[i].vx + bul[i].vy * bul[i].vy) + 0.01f;
            float x = wx(bul[i].x), y = wy(bul[i].y), tx = bul[i].vx / sp * 6 * zoom, ty = bul[i].vy / sp * 6 * zoom;
            sline(x - tx, y - ty, x, y, 7 * k, 0xffd06055u);
            sline(x - tx, y - ty, x, y, 2.4f * k, 0xfff0a0ffu);
        }
    for (i = 0; i < MAX_UNITS; i++)
        if (un[i].type) draw_unit(&un[i]);
    if (pl.alive && !(pl.protect > 0 && ((int)(pl.protect * 12) & 1)))
        wshape(pl.x, pl.y, 8.5f, pl.ang, SH_SHIP, 4, 1, 0xffffffffu, 1);
    parts_draw();
}

/* ============================================================== SCENES */

static void music(const char *name)
{
    char path[640];
    if (music_cur && !strcmp(music_cur, name)) return;
    music_cur = name;
    snprintf(path, sizeof path, "%smusic/%s.ogg", plat_data_dir(), name);
    plat_music_play(path);
}

static void clear_field(void)
{
    memset(un, 0, sizeof un);
    memset(bul, 0, sizeof bul);
    memset(mote, 0, sizeof mote);
    memset(ring, 0, sizeof ring);
}

static void start_game(void)                 /* the mode's Gr* function, ResetGameInfo 0x0004e57c */
{
    clear_field();
    n_parts = 0;
    grid_reset();
    score = 0; mult = 1; next_banner = 10;
    lives = modes[mode].lives; bombs = modes[mode].bombs;   /* +0x795c / +0x7960 */
    best = bests[mode];
    memset(&cg, 0, sizeof cg);
    cg.delay = 1;                            /* ResetGameControl 0x0005b4a4 */
    memset(&sq, 0, sizeof sq);
    gate_chain = 0; gate_last_t = -10;
    next_bomb_at = 100000; next_life_at = 100000;
    kills = 0; best_beaten = 0; deaths = 0;
    play_t = 0;
    memset(&lc, 0, sizeof lc);
    lc.timer = 1.0f;
    lc.wall_i = 5;                           /* ResetGameControl: the walls have not been shuffled yet */
    memset(&pl, 0, sizeof pl);
    pl.alive = 1; pl.r = 7; pl.ang = -PI / 2; pl.protect = 1.0f;
    if (mode == M_KING) {                    /* UpdateGame: three zones to start, one in the middle */
        float x, y;
        hill_new(0, 0);
        hill_spot(&x, &y); hill_new(x, y);
        hill_spot(&x, &y); hill_new(x, y);
    }
    if (mode == M_SEQUENCE) seq_start_level();
    paused = 0;
    scene = SC_PLAY;
    scene_t = 0;
    rng = (unsigned)(now_t * 1000.0) * 2654435761u + 1;
    sfx("Game_start", 25, 0);
    music_cur = NULL;
    music(modes[mode].music);
}

int game_init(int screen_w, int screen_h)
{
    unsigned char *px8 = calloc(128 * 32 * 4, 1);
    int x, y;
    FILE *f;
    if (!px8) return -1;
    scr_w = screen_w;
    scr_h = screen_h;
    zoom = fminf(scr_h / (2 * AH + 34), scr_w / (2 * AW + 34));   /* the whole arena on screen */
    for (y = 0; y < 32; y++)
        for (x = 0; x < 128; x++) {
            unsigned char *p = &px8[(y * 128 + x) * 4];
            float a = 0;
            if (x < 64) {                    /* soft line */
                float dy = y + 0.5f - 16, ends = clampf(fminf(x + 0.5f, 64 - x - 0.5f) / 10, 0, 1);
                a = expf(-(dy * dy) / (6.5f * 6.5f)) * ends;
            } else if (x < 96) {             /* soft dot */
                float dx = x + 0.5f - 80, dy = y + 0.5f - 16;
                a = expf(-(dx * dx + dy * dy) / (6.0f * 6.0f));
            } else if (x >= 100 && x < 124 && y >= 4 && y < 28) {
                a = 1;                       /* solid, with an empty gutter round it:
                                              * touching the dot, bilinear filtering
                                              * drew a faint line down the right of
                                              * every glow (most visible on the ship) */
            }
            p[0] = p[1] = p[2] = 255;
            p[3] = (unsigned char)(a * 255 + 0.5f);
        }
    atlas = plat_tex_create(px8, 128, 32);
    free(px8);
    if (!atlas) return -1;
    f = fopen(plat_save_file(), "r");
    if (f) {                                 /* "best <mode> <score>" lines (an old file has one bare score) */
        char line[96];
        while (fgets(line, sizeof line, f)) {
            int m; long long v;
            if (sscanf(line, "best %d %lld", &m, &v) == 2 && m >= 0 && m < M_COUNT) bests[m] = v;
            else if (sscanf(line, "best %lld", &v) == 1) bests[M_EVOLVED] = v;
        }
        fclose(f);
    }
    best = bests[mode];
    seq_load();
    grid_reset();
    cam_x = 0; cam_y = 0;
    scene = SC_TITLE;
    music("main_menu");
    return 0;
}

void game_shutdown(void)
{
    plat_music_stop();
    plat_tex_free(atlas);
    atlas = NULL;
}

static double tick_acc;

int game_frame(double dt, const PlatInput *in)
{
    char num[32];
    float k = glow_k(), fdt;
    static int prev_touch, last_mult = 1, last_banner = 10;
    int tap = in->n > 0 && !prev_touch;
    prev_touch = in->n > 0;
    if (in->quit) return 0;
    if (dt > 0.1) dt = 0.1;                  /* after a stall, don't run away */
    if (dt < 0) dt = 0;
    now_t += dt;
    scene_t += dt;
    fdt = (float)dt;
    if (music_cur && plat_music_done()) {    /* loop the music */
        const char *m = music_cur;
        music_cur = NULL;
        music(m);
    }

    grid_dt = 0;                             /* set below by scenes that move the grid */
    switch (scene) {
    case SC_TITLE:
        if (frand() < fdt * 1.5f)            /* the grid ripples on its own */
            grid_force(frange(-AW + 40, AW - 40), frange(-AH + 40, AH - 40), frange(300, 900), 70);
        grid_dt = fdt;
        parts_update(fdt);
        {                                    /* choose a mode: up/down (or the stick), X to play */
            /* the stick only counts once it has been let go since this screen
             * appeared: a thumb still on it from the last game must not quietly
             * change the mode */
            static int stick_held = 1;
            int d = 0;
            if (scene_t < 0.05) stick_held = 1;
            if (scene_t > 0.3 && (in->pressed & BTN_UP)) d = -1;
            if (scene_t > 0.3 && (in->pressed & BTN_DOWN)) d = 1;
            if (!stick_held && in->ly < -0.6f) d = -1;
            if (!stick_held && in->ly > 0.6f) d = 1;
            if (fabsf(in->ly) > 0.4f) { if (scene_t < 0.3) stick_held = 1; }
            else stick_held = 0;
            if (d) stick_held = 1;
            if (d) {
                mode = (mode + d + M_COUNT) % M_COUNT;
                best = bests[mode];
                sfx("UI_Change", 47, 0);
            }
            if (scene_t > 0.3 && ((in->pressed & (BTN_CROSS | BTN_START)) || tap) &&
                (mode != M_SEQUENCE || seq_loaded))
                start_game();
        }
        break;
    case SC_PLAY: {
        PlatInput tin = *in;
        int steps = 0;
        if (in->pressed & BTN_START) {
            paused = !paused;
            plat_music_pause(paused);
        }
        if (paused) {
            if (in->pressed & BTN_CIRCLE) {
                paused = 0;
                scene = SC_TITLE; scene_t = 0;
                clear_field();
                music_cur = NULL;
                music("main_menu");
            }
            break;
        }
        tick_acc += dt;                      /* fixed 60 Hz ticks */
        while (tick_acc >= TICK && steps < 6 && scene == SC_PLAY) {
            sim_tick(&tin);
            tin.pressed = 0;                 /* a press counts once */
            tick_acc -= TICK;
            steps++;
        }
        if (tick_acc > TICK) tick_acc = 0;
        if (next_banner != last_banner && mult >= last_banner) {   /* banner when the multiplier steps */
            banner_mult = last_banner;
            banner_t = now_t;
        }
        last_banner = next_banner;
        last_mult = mult;
        grid_dt = fdt;
        parts_update(fdt);
        break;
    }
    case SC_OVER:
        grid_dt = fdt;
        parts_update(fdt);
        if (scene_t > 1.5 && ((in->pressed & (BTN_CROSS | BTN_START)) || tap)) {
            scene = SC_TITLE; scene_t = 0;
            music("main_menu");
        }
        break;
    }
    (void)last_mult;

    /* ---- draw ---- */
    plat_job_start(grid_job, NULL);          /* the grid moves and draws itself on another core */
    draw_reset();
    if (scene == SC_TITLE) {
        parts_draw();
        int m;
        text(scr_w / 2.0f, 80 * k, 48 * k, 1, 0x7cd8ffffu, 1, GAME_TITLE);
        for (m = 0; m < M_COUNT; m++) {      /* the modes, the chosen one bright */
            float y = 140 * k + m * 27 * k;
            int sel = m == mode;
            text(scr_w * 0.40f, y, (sel ? 19 : 15) * k, 0, sel ? 0x82f532ffu : 0x5a8ab0ffu, sel ? 1 : 0.8f,
                 modes[m].name);
            if (bests[m] > 0) {
                fmt_num(num, sizeof num, (long)bests[m]);
                text(scr_w * 0.62f, y + 2 * k, 13 * k, 0, sel ? 0xffe070ffu : 0x8a7a50ffu, 1, num);
            }
            if (sel) sshape(scr_w * 0.40f - 18 * k, y + 9 * k, 8 * k, 0, SH_SHIP, 4, 1, 0xffffffffu, 1);
        }
        if (mode == M_SEQUENCE && !seq_loaded)
            text(scr_w / 2.0f, 425 * k, 13 * k, 1, 0xff6a5affu, 1, "SEQUENCE NEEDS LEVELS.TXT IN APP_DATA (RUN PREP_ASSETS)");
        else if (((int)(scene_t * 2)) & 1 || scene_t < 0.5)
            text(scr_w / 2.0f, 425 * k, 18 * k, 1, 0xffffffffu, 1, "UP/DOWN CHOOSE   X START");
        text(scr_w / 2.0f, 480 * k, 12 * k, 1, 0x9ad0ffffu, 0.8f, "LEFT STICK MOVE   RIGHT STICK FIRE   L/R BOMB   START PAUSE");
    } else {
        draw_world();
        hud();
        if (scene == SC_PLAY && paused) {
            sfill(0, 0, (float)scr_w, (float)scr_h, 0x00000090u);
            text(scr_w / 2.0f, 220 * k, 40 * k, 1, 0xffffffffu, 1, "PAUSED");
            text(scr_w / 2.0f, 300 * k, 16 * k, 1, 0x9ad0ffffu, 1, "START RESUME   O QUIT");
        }
        if (scene == SC_OVER) {
            sfill(0, 0, (float)scr_w, (float)scr_h, 0x00000070u);
            text(scr_w / 2.0f, 180 * k, 44 * k, 1, 0xff6a5affu, 1, "GAME OVER");
            fmt_num(num, sizeof num, (long)score);
            text(scr_w / 2.0f, 260 * k, 28 * k, 1, 0xffffffffu, 1, num);
            if (score >= best && score > 0)
                text(scr_w / 2.0f, 310 * k, 18 * k, 1, 0xffe070ffu, 1, "NEW HIGH SCORE!");
            if (scene_t > 1.5)
                text(scr_w / 2.0f, 380 * k, 16 * k, 1, 0x9ad0ffffu, 1, "PRESS X");
        }
    }
    plat_job_wait();
    plat_begin();
    if (n_gspr)
        plat_sprites(atlas, ksrc[K_LINE][0], ksrc[K_LINE][1], ksrc[K_LINE][2], ksrc[K_LINE][3], gspr, n_gspr, 1);
    draw_submit();
    plat_end();
    return 1;
}

void game_debug(char *buf, int n)
{
    int i, ne = 0, nb = 0, ng = 0, wells = 0, active = 0, cnt[U_TYPES] = { 0 };
    for (i = 0; i < MAX_UNITS; i++) {
        ne += counts_as_enemy(un[i].type);
        cnt[un[i].type]++;
        if (un[i].type == U_WELL) { wells++; active += un[i].n; }
    }
    for (i = 0; i < MAX_BUL; i++) nb += bul[i].alive;
    for (i = 0; i < MAX_MOTE; i++) ng += mote[i].alive;
    snprintf(buf, (size_t)n, "scene=%d paused=%d score=%lld mult=%d lives=%d bombs=%d enemies=%d kills=%d "
             "bullets=%d geoms=%d particles=%d alive=%d playt=%.1f best=%lld level=%.1f state=%d "
             "wells=%d active=%d px=%.0f py=%.0f deaths=%d mode=%d budget=%d slot=%d phase=%d hills=%d "
             "mines=%d meteors=%d nufos=%d titans=%d holes=%d rockets=%d",
             scene, paused, score, mult, lives, bombs, ne, kills, nb, ng, n_parts, pl.alive, play_t, best,
             lc.level, lc.state, wells, active, pl.x, pl.y, deaths, mode, cg.budget, sq.slot, sq.phase,
             cnt[U_HILL], cnt[U_MINE], cnt[U_METEOR], cnt[U_NUFO], cnt[U_TITAN], cnt[U_BLACKHOLE], cnt[U_ROCKET]);
}

/* tests only: put an enemy (BadGuyType number) on the field, ready to go.
 * 105 = a black hole that is already awake. */
void game_test_spawn(int type, float x, float y)
{
    static const int map[32] = { [1] = U_WANDERER, [2] = U_GRUNT, [3] = U_WEAVER, [4] = U_SPINNER,
                                 [5] = U_WELL, [6] = U_REPULSAR, [8] = U_SNAKE, [12] = U_ROCKET,
                                 [14] = U_PROTON, [16] = U_GATE, [22] = U_DUCK,
                                 [11] = U_AMOEBA, [26] = U_TITAN, [27] = U_MINE, [28] = U_MINELAYER,
                                 [29] = U_BLACKHOLE };
    int awake = type == 105, i, t;
    if (awake) type = 5;
    if (type < 0 || type >= 32 || !map[type]) return;
    t = map[type];
    i = t == U_MINE ? unit_new(U_MINE, x, y) : spawn_bad_guy(t, x, y, 0);
    if (i < 0) return;
    for (int j = 0; j < MAX_UNITS; j++)      /* everything just made, including a snake's body */
        if (un[j].type && un[j].appear < 1 && (j == i || un[j].head == i || un[j].link == i)) un[j].appear = 1;
    if (awake) un[i].n = 1;
}

/* tests only: hold the spawn director back */
void game_test_quiet(void)
{
    lc.timer = 1e9f;
    cg.delay = 1e9f;
    sq.wait = 1e9f; sq.left = 1e9f;
}

/* tests only: choose the mode the next game starts in (M_* order) */
void game_test_mode(int m)
{
    if (m >= 0 && m < M_COUNT) { mode = m; best = bests[mode]; }
}
