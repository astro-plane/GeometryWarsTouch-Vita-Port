/* game.c - a twin-stick arena shooter in the manner of GWars:Touch, written
 * from scratch for the PS Vita.  Everything on screen is drawn as glowing
 * lines and dots from one small texture made at start-up; the only things
 * taken from the iPhone game are its sound effects and music, converted from
 * the player's own copy by tools/prep_assets.py.
 *
 * Coordinates: the arena is ARENA_W x ARENA_H world units; the camera follows
 * the ship and shows VIEW_H units top to bottom.  Times are in seconds. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "game.h"

#define GAME_TITLE "NEON GRID"               /* placeholder name, easy to change */
#define PI 3.14159265f

#define ARENA_W 1500.0f
#define ARENA_H 900.0f
#define VIEW_H 680.0f

/* ------------------------------------------------------------ state -- */

enum { SC_TITLE, SC_PLAY, SC_OVER };

static int scr_w, scr_h;
static float zoom, cam_x, cam_y;
static PlatTex *atlas;
static int scene, paused;
static double now_t, scene_t;                /* clock, and time in this scene */
static unsigned rng = 12345;

static long score, best;
static int lives, bombs, mult, kills, best_beaten;
static long next_life, next_bomb;
static double play_t, spawn_t, rush_t, fire_t;
static int shot_n;
static const char *music_cur;

/* the ship */
static float px, py, pvx, pvy, pang;
static int p_alive;
static double respawn_t, invul_t;

static unsigned frnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static float frand(void) { return (frnd() & 0xffffff) / 16777216.0f; }
static float frange(float a, float b) { return a + (b - a) * frand(); }
static float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }

/* ---------------------------------------------------------- drawing -- */

/* The atlas: a soft horizontal line (with soft ends), a soft dot, and a
 * solid patch.  Everything is drawn from these three. */
enum { K_LINE, K_DOT, K_SOLID };
static const int ksrc[3][4] = { { 0, 0, 64, 32 }, { 64, 0, 32, 32 }, { 100, 4, 24, 24 } };

#define MAX_SPR 20000
static PlatSprite spr[MAX_SPR];
static int n_spr, cur_kind = -1, cur_blend = -1;

static void flush(void)
{
    if (n_spr && cur_kind >= 0)
        plat_sprites(atlas, ksrc[cur_kind][0], ksrc[cur_kind][1], ksrc[cur_kind][2], ksrc[cur_kind][3],
                     spr, n_spr, cur_blend);
    n_spr = 0;
}

static PlatSprite *push(int kind, int blend)
{
    if (kind != cur_kind || blend != cur_blend || n_spr == MAX_SPR) {
        flush();
        cur_kind = kind;
        cur_blend = blend;
    }
    return &spr[n_spr++];
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
    sline(x0, y0, x1, y1, 10 * k, with_alpha(c, a * 0.30f));
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
    plat_fill(x, y, w, h, c);
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

/* A sheet of springs: each point is tied to where it belongs and to its four
 * neighbours.  Explosions push it, gravity wells pull it, and it ripples. */
#define GSP 30.0f
#define GX 51                                /* ARENA_W / GSP + 1 */
#define GY 31                                /* ARENA_H / GSP + 1 */
static float gpx[GY][GX], gpy[GY][GX], gvx[GY][GX], gvy[GY][GX];

static void grid_reset(void)
{
    int i, j;
    for (j = 0; j < GY; j++)
        for (i = 0; i < GX; i++) {
            gpx[j][i] = i * GSP; gpy[j][i] = j * GSP;
            gvx[j][i] = gvy[j][i] = 0;
        }
}

/* push (strength > 0) or pull (< 0) the sheet around (x,y) */
static void grid_force(float x, float y, float strength, float radius)
{
    int i0 = (int)((x - radius) / GSP), i1 = (int)((x + radius) / GSP) + 1;
    int j0 = (int)((y - radius) / GSP), j1 = (int)((y + radius) / GSP) + 1, i, j;
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
                float ax = (i * GSP - gpx[j][i]) * k_home +
                           (gpx[j][i - 1] + gpx[j][i + 1] + gpx[j - 1][i] + gpx[j + 1][i] - 4 * gpx[j][i]) * k_link;
                float ay = (j * GSP - gpy[j][i]) * k_home +
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

static void grid_seg(int i0, int j0, int i1, int j1, int major)
{
    float x0 = wx(gpx[j0][i0]), y0 = wy(gpy[j0][i0]), x1 = wx(gpx[j1][i1]), y1 = wy(gpy[j1][i1]);
    float dx = gpx[j0][i0] - i0 * GSP, dy = gpy[j0][i0] - j0 * GSP, d = sqrtf(dx * dx + dy * dy);
    float hot = clampf(d / 25, 0, 1);        /* stretched lines glow brighter */
    float k = scr_h / 544.0f;
    unsigned c = hot > 0.02f ? 0x5aa0ffffu : 0x2a5cd8ffu;
    if ((x0 < -20 && x1 < -20) || (x0 > scr_w + 20 && x1 > scr_w + 20) ||
        (y0 < -20 && y1 < -20) || (y0 > scr_h + 20 && y1 > scr_h + 20))
        return;
    sline(x0, y0, x1, y1, 6 * k, with_alpha(c, (major ? 0.10f : 0.05f) + hot * 0.15f));
    sline(x0, y0, x1, y1, 1.6f * k, with_alpha(c, (major ? 0.42f : 0.22f) + hot * 0.5f));
}

static void grid_draw(void)
{
    int i, j;
    for (j = 0; j < GY; j++)
        for (i = 0; i < GX - 1; i++)
            grid_seg(i, j, i + 1, j, j % 5 == 0);
    for (i = 0; i < GX; i++)
        for (j = 0; j < GY - 1; j++)
            grid_seg(i, j, i, j + 1, i % 5 == 0);
}

/* -------------------------------------------------------- particles -- */

#define MAX_PART 2500
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
        if (p->x < 0) { p->x = 0; p->vx = -p->vx; }
        if (p->x > ARENA_W) { p->x = ARENA_W; p->vx = -p->vx; }
        if (p->y < 0) { p->y = 0; p->vy = -p->vy; }
        if (p->y > ARENA_H) { p->y = ARENA_H; p->vy = -p->vy; }
    }
}

static void parts_draw(void)
{
    int i;
    float k = scr_h / 544.0f;
    for (i = 0; i < n_parts; i++) {
        Part *p = &parts[i];
        float a = p->life / p->max, sp = sqrtf(p->vx * p->vx + p->vy * p->vy);
        float len = clampf(sp * 0.022f, 1.5f, 14) * zoom, x = wx(p->x), y = wy(p->y);
        if (x < -20 || y < -20 || x > scr_w + 20 || y > scr_h + 20) continue;
        if (sp < 1) sp = 1;
        sline(x - p->vx / sp * len, y - p->vy / sp * len, x, y, 2.6f * k, with_alpha(p->c, a));
    }
}

/* ---------------------------------------------------------- enemies -- */

enum { E_SEEKER, E_DODGER, E_DRIFTER, E_SPLITTER, E_SHARD, E_WELL, N_ETYPES };

static const struct {
    unsigned color;
    float r;
    int points, hp;
    const char *spawn_sfx;
} et[N_ETYPES] = {
    { 0x3cc8ffffu, 16, 50, 1, "Enemy_spawn_blue" },      /* seeker: heads for the ship */
    { 0x6cff4cffu, 17, 100, 1, "Enemy_spawn_green" },    /* dodger: steps aside from shots */
    { 0xb478ffffu, 17, 25, 1, "Enemy_spawn_purple" },    /* drifter: wanders about */
    { 0xff58c8ffu, 19, 100, 1, "Enemy_splitter_spawn" }, /* splitter: bursts into shards */
    { 0xff8ad8ffu, 9, 50, 1, NULL },                     /* shard: small and quick */
    { 0xff4a3cffu, 26, 150, 18, "Enemy_spawn_red" },     /* well: pulls everything in */
};

#define MAX_EN 320
#define WARM 0.7                             /* seconds an enemy takes to appear */
typedef struct {
    int type, alive, absorbed;
    float x, y, vx, vy, ang, r, aux, aux2;
    double t, warm, flash;
    int hp;
} Enemy;
static Enemy en[MAX_EN];
static int n_live;

static double sfx_last[64];
static void sfx(const char *name, int slot, double gap)  /* rate-limited per slot */
{
    if (slot >= 0 && slot < 64) {
        if (now_t - sfx_last[slot] < gap) return;
        sfx_last[slot] = now_t;
    }
    plat_sfx(name);
}

static Enemy *spawn(int type, float x, float y)
{
    int i;
    for (i = 0; i < MAX_EN; i++)
        if (!en[i].alive) {
            Enemy *e = &en[i];
            memset(e, 0, sizeof *e);
            e->type = type; e->alive = 1;
            e->x = clampf(x, 30, ARENA_W - 30); e->y = clampf(y, 30, ARENA_H - 30);
            e->r = et[type].r; e->hp = et[type].hp;
            e->warm = type == E_SHARD ? 0.15 : WARM;
            e->aux = type == E_DODGER ? -10 : frand() * 2 * PI;   /* dodger: time of its last dodge */
            e->aux2 = frand() < 0.5f ? -1 : 1;
            if (et[type].spawn_sfx) sfx(et[type].spawn_sfx, 10 + type, 0.12);
            return e;
        }
    return NULL;
}

/* ----------------------------------------------- bullets and geoms -- */

#define MAX_BUL 400
typedef struct { int alive; float x, y, vx, vy; } Bullet;
static Bullet bul[MAX_BUL];

#define MAX_GEOM 400
typedef struct { int alive; float x, y, vx, vy, ang; double t; } Geom;
static Geom geo[MAX_GEOM];

static void drop_geoms(float x, float y, int n)
{
    int i, k = 0;
    for (i = 0; i < MAX_GEOM && k < n; i++)
        if (!geo[i].alive) {
            float a = frand() * 2 * PI, s = frange(40, 160);
            geo[i].alive = 1; geo[i].x = x; geo[i].y = y;
            geo[i].vx = cosf(a) * s; geo[i].vy = sinf(a) * s;
            geo[i].ang = frand() * PI; geo[i].t = 0;
            k++;
        }
}

static void add_score(long pts)
{
    score += pts * mult;
    while (score >= next_life) { lives++; next_life += 75000; sfx("pickup_extralife", 1, 0); }
    while (score >= next_bomb) { bombs++; next_bomb += 100000; sfx("pickup_smartbomb", 2, 0); }
    if (!best_beaten && best > 0 && score > best) { best_beaten = 1; sfx("Hi_Score_achieved", 3, 0); }
}

/* how: 0 shot down (points and geoms), 1 swept away (bomb or the ship's
 * death: no points), 2 swallowed by a gravity well */
static void kill_enemy(Enemy *e, int how)
{
    int i;
    e->alive = 0;
    if (how == 2) {
        burst(e->x, e->y, et[e->type].color, 8, 120);
        return;
    }
    burst(e->x, e->y, et[e->type].color, e->type == E_WELL ? 160 : 50, e->type == E_WELL ? 520 : 380);
    grid_force(e->x, e->y, e->type == E_WELL ? 1500.0f : 600.0f, e->type == E_WELL ? 240.0f : 100.0f);
    if (e->type == E_WELL) sfx(how ? "Gravity_well_explode" : "Gravity_well_die", 4, 0.05);
    else sfx("Enemy_explode", 5, 0.05);
    if (how == 0) {
        kills++;
        add_score(et[e->type].points + (e->type == E_WELL ? e->absorbed * 25 : 0));
        drop_geoms(e->x, e->y, e->type == E_WELL ? 8 : e->type == E_SHARD ? 1 : 1 + (frand() < 0.35f));
    }
    if (e->type == E_SPLITTER && how == 0) {
        sfx("Enemy_splitter_split_1", 6, 0.05);
        for (i = 0; i < 3; i++) {
            Enemy *s = spawn(E_SHARD, e->x, e->y);
            if (s) {
                float a = e->aux + i * 2 * PI / 3;
                s->vx = cosf(a) * 260; s->vy = sinf(a) * 260;
            }
        }
    }
}

static void fire(float dx, float dy)
{
    int lanes = score >= 100000 ? 3 : 2, l, i;
    float a = atan2f(dy, dx);
    shot_n++;
    if (shot_n % 3 == 1) sfx("Fire_normal", 7, 0);
    for (l = 0; l < lanes; l++) {
        float off = lanes == 2 ? (l ? 7.0f : -7.0f) : 0, spread = lanes == 3 ? (l - 1) * 0.07f : 0;
        float b = a + spread, c = cosf(b), s = sinf(b);
        for (i = 0; i < MAX_BUL; i++)
            if (!bul[i].alive) {
                bul[i].alive = 1;
                bul[i].x = px + cosf(a) * 18 - sinf(a) * off;
                bul[i].y = py + sinf(a) * 18 + cosf(a) * off;
                bul[i].vx = c * 980; bul[i].vy = s * 980;
                break;
            }
    }
}

static void clear_field(void)
{
    int i;
    for (i = 0; i < MAX_EN; i++)
        if (en[i].alive) kill_enemy(&en[i], 1);
    for (i = 0; i < MAX_GEOM; i++) geo[i].alive = 0;
    for (i = 0; i < MAX_BUL; i++) bul[i].alive = 0;
}

static void bomb(void)
{
    int i;
    if (bombs <= 0 || !p_alive) return;
    bombs--;
    sfx("Fire_smartbomb", 8, 0);
    for (i = 0; i < 90; i++) {               /* a ring rushing outward */
        float a = i * 2 * PI / 90;
        spark(px, py, cosf(a) * 900, sinf(a) * 900, 1.0f, 0xffffffffu);
    }
    grid_force(px, py, 2000, 480);
    for (i = 0; i < MAX_EN; i++)
        if (en[i].alive) kill_enemy(&en[i], 1);
}

/* ---------------------------------------------------------- spawning -- */

/* somewhere not too near the ship */
static void far_point(float *x, float *y)
{
    int tries;
    for (tries = 0; tries < 20; tries++) {
        float dx, dy;
        *x = frange(60, ARENA_W - 60);
        *y = frange(60, ARENA_H - 60);
        dx = *x - px; dy = *y - py;
        if (dx * dx + dy * dy > 330.0f * 330.0f) return;
    }
}

static int count_type(int type)
{
    int i, n = 0;
    for (i = 0; i < MAX_EN; i++)
        if (en[i].alive && en[i].type == type) n++;
    return n;
}

static void spawn_wave(void)
{
    float w[N_ETYPES] = { 0 }, total = 0, pick, x, y;
    int type, n, i;
    w[E_DRIFTER] = 3;
    w[E_SEEKER] = play_t > 6 ? 3.5f : 0;
    w[E_DODGER] = play_t > 22 ? 2.2f : 0;
    w[E_SPLITTER] = play_t > 40 ? 1.4f : 0;
    w[E_WELL] = play_t > 55 && count_type(E_WELL) < 3 ? 0.45f : 0;
    for (i = 0; i < N_ETYPES; i++) total += w[i];
    pick = frand() * total;
    for (type = 0; type < N_ETYPES - 1 && pick >= w[type]; type++) pick -= w[type];
    n = type == E_WELL ? 1 : 1 + (int)(play_t / 35);
    if (n > 6) n = 6;
    far_point(&x, &y);
    for (i = 0; i < n; i++)
        spawn(type, x + frange(-50, 50), y + frange(-50, 50));
}

/* every so often, a swarm from all four corners */
static void spawn_rush(void)
{
    static const float cx[4] = { 50, ARENA_W - 50, 50, ARENA_W - 50 }, cy[4] = { 50, 50, ARENA_H - 50, ARENA_H - 50 };
    int c, i, n = 4 + (int)(play_t / 25);
    if (n > 12) n = 12;
    for (c = 0; c < 4; c++) {
        float dx = cx[c] - px, dy = cy[c] - py;
        if (dx * dx + dy * dy < 300.0f * 300.0f) continue;   /* not on top of the ship */
        for (i = 0; i < n; i++)
            spawn(play_t > 70 && (i & 1) ? E_DODGER : E_SEEKER, cx[c] + frange(-30, 30), cy[c] + frange(-30, 30));
    }
}

/* ------------------------------------------------------------ update -- */

static void ship_die(void)
{
    p_alive = 0;
    respawn_t = 2.2;
    mult = 1;
    sfx("Player_explode", 9, 0);
    burst(px, py, 0xffffffffu, 220, 650);
    burst(px, py, 0xffd060ffu, 160, 450);
    grid_force(px, py, 1600, 320);
    clear_field();
}

static void update_enemies(float dt)
{
    int i, j;
    n_live = 0;
    for (i = 0; i < MAX_EN; i++) {
        Enemy *e = &en[i];
        float dx, dy, d;
        if (!e->alive) continue;
        n_live++;
        e->t += dt;
        if (e->warm > 0) { e->warm -= dt; continue; }
        if (e->flash > 0) e->flash -= dt;
        dx = px - e->x; dy = py - e->y;
        d = sqrtf(dx * dx + dy * dy);
        if (d < 0.01f) d = 0.01f;
        if (!p_alive) { dx = dy = 0; }       /* nothing to chase while the ship is gone */
        switch (e->type) {
        case E_SEEKER:
            e->vx += ((dx / d) * 190 - e->vx) * 2.5f * dt;
            e->vy += ((dy / d) * 190 - e->vy) * 2.5f * dt;
            break;
        case E_DODGER:
            if ((float)e->t - e->aux > 0.25f) {     /* not mid-dodge: look for shots */
                for (j = 0; j < MAX_BUL; j++) {
                    float rx, ry, rd, bs, dot;
                    if (!bul[j].alive) continue;
                    rx = e->x - bul[j].x; ry = e->y - bul[j].y;
                    rd = rx * rx + ry * ry;
                    if (rd > 150.0f * 150.0f) continue;
                    rd = sqrtf(rd); bs = sqrtf(bul[j].vx * bul[j].vx + bul[j].vy * bul[j].vy);
                    dot = (rx * bul[j].vx + ry * bul[j].vy) / (rd * bs + 0.001f);
                    if (dot > 0.9f) {        /* coming straight at it: step aside */
                        float side = (rx * bul[j].vy - ry * bul[j].vx) > 0 ? 1.0f : -1.0f;
                        e->vx = -bul[j].vy / bs * 430 * side;
                        e->vy = bul[j].vx / bs * 430 * side;
                        e->aux = (float)e->t;
                        break;
                    }
                }
            }
            if ((float)e->t - e->aux > 0.25f) {
                e->vx += ((dx / d) * 215 - e->vx) * 3.0f * dt;
                e->vy += ((dy / d) * 215 - e->vy) * 3.0f * dt;
            }
            e->ang += 2.0f * dt;
            break;
        case E_DRIFTER:
            if (frand() < dt * 0.7f) e->aux += frange(-1.6f, 1.6f);
            e->vx = cosf(e->aux) * 85; e->vy = sinf(e->aux) * 85;
            e->ang += 3.2f * dt;
            break;
        case E_SPLITTER:
            e->vx += ((dx / d) * 150 - e->vx) * 2.0f * dt;
            e->vy += ((dy / d) * 150 - e->vy) * 2.0f * dt;
            e->ang += 1.5f * dt;
            break;
        case E_SHARD: {
            float tx = (dx / d) * 220 - (dy / d) * 140 * e->aux2, ty = (dy / d) * 220 + (dx / d) * 140 * e->aux2;
            e->vx += (tx - e->vx) * 2.2f * dt;
            e->vy += (ty - e->vy) * 2.2f * dt;
            e->ang += 9.0f * dt;
            break;
        }
        case E_WELL:
            e->vx = (dx / d) * 22; e->vy = (dy / d) * 22;
            grid_force(e->x, e->y, -2400.0f * dt, 200 + e->r * 2);   /* a dent in the grid */
            if (p_alive && d < 380) {        /* pull the ship */
                float f = 520 * (1 - d / 380);
                pvx -= dx / d * f * dt; pvy -= dy / d * f * dt;
            }
            for (j = 0; j < MAX_EN; j++) {   /* swallow others that come too close */
                Enemy *o = &en[j];
                float ox, oy, od;
                if (!o->alive || o == e || o->type == E_WELL || o->warm > 0) continue;
                ox = o->x - e->x; oy = o->y - e->y; od = ox * ox + oy * oy;
                if (od < 260.0f * 260.0f) {  /* draw them in */
                    float dd = sqrtf(od) + 0.01f, f = 260 * (1 - dd / 260);
                    o->vx -= ox / dd * f * dt * 3; o->vy -= oy / dd * f * dt * 3;
                }
                if (od < (e->r + o->r) * (e->r + o->r)) {
                    kill_enemy(o, 2);
                    e->absorbed++; e->hp += 2; e->r += 1.2f;
                    sfx("Enemy_red_suck", 12, 0.1);
                }
            }
            if (e->absorbed >= 12) {         /* too full: it bursts into shards */
                kill_enemy(e, 1);
                for (j = 0; j < 8; j++) {
                    Enemy *s = spawn(E_SHARD, e->x, e->y);
                    if (s) { float a = j * PI / 4; s->vx = cosf(a) * 380; s->vy = sinf(a) * 380; }
                }
                continue;
            }
            break;
        }
        e->x += e->vx * dt; e->y += e->vy * dt;
        if (e->x < e->r) { e->x = e->r; e->vx = fabsf(e->vx); if (e->type == E_DRIFTER) e->aux = PI - e->aux; }
        if (e->x > ARENA_W - e->r) { e->x = ARENA_W - e->r; e->vx = -fabsf(e->vx); if (e->type == E_DRIFTER) e->aux = PI - e->aux; }
        if (e->y < e->r) { e->y = e->r; e->vy = fabsf(e->vy); if (e->type == E_DRIFTER) e->aux = -e->aux; }
        if (e->y > ARENA_H - e->r) { e->y = ARENA_H - e->r; e->vy = -fabsf(e->vy); if (e->type == E_DRIFTER) e->aux = -e->aux; }
        if (p_alive && invul_t <= 0) {       /* touching the ship */
            float hx = px - e->x, hy = py - e->y, rr = e->r * 0.8f + 11;
            if (hx * hx + hy * hy < rr * rr) { ship_die(); return; }
        }
    }
    /* keep chasers from piling into one spot */
    for (i = 0; i < MAX_EN; i++) {
        Enemy *a = &en[i];
        if (!a->alive || a->warm > 0 || a->type == E_WELL || a->type == E_DRIFTER) continue;
        for (j = i + 1; j < MAX_EN; j++) {
            Enemy *b = &en[j];
            float dx, dy, d2, rr;
            if (!b->alive || b->warm > 0 || b->type == E_WELL || b->type == E_DRIFTER) continue;
            dx = b->x - a->x; dy = b->y - a->y; d2 = dx * dx + dy * dy; rr = a->r + b->r;
            if (d2 < rr * rr && d2 > 0.01f) {
                float d = sqrtf(d2), push = (rr - d) * 0.5f;
                a->x -= dx / d * push; a->y -= dy / d * push;
                b->x += dx / d * push; b->y += dy / d * push;
            }
        }
    }
}

static void update_bullets(float dt)
{
    int i, j;
    for (i = 0; i < MAX_BUL; i++) {
        Bullet *b = &bul[i];
        if (!b->alive) continue;
        for (j = 0; j < MAX_EN; j++) {       /* gravity wells bend shots */
            Enemy *w = &en[j];
            float dx, dy, d;
            if (!w->alive || w->type != E_WELL || w->warm > 0) continue;
            dx = w->x - b->x; dy = w->y - b->y; d = sqrtf(dx * dx + dy * dy);
            if (d < 280 && d > 1) {
                float sp = sqrtf(b->vx * b->vx + b->vy * b->vy), f = 2200 * (1 - d / 280);
                b->vx += dx / d * f * dt; b->vy += dy / d * f * dt;
                d = sqrtf(b->vx * b->vx + b->vy * b->vy);
                b->vx *= sp / d; b->vy *= sp / d;
            }
        }
        b->x += b->vx * dt; b->y += b->vy * dt;
        if (b->x < 0 || b->y < 0 || b->x > ARENA_W || b->y > ARENA_H) {
            b->alive = 0;
            b->x = clampf(b->x, 0, ARENA_W); b->y = clampf(b->y, 0, ARENA_H);
            burst(b->x, b->y, 0xffe070ffu, 3, 160);
            grid_force(b->x, b->y, 120, 50);
            continue;
        }
        grid_force(b->x, b->y, 18, 40);
        for (j = 0; j < MAX_EN; j++) {
            Enemy *e = &en[j];
            float dx, dy;
            if (!e->alive || e->warm > 0) continue;
            dx = e->x - b->x; dy = e->y - b->y;
            if (dx * dx + dy * dy < (e->r + 4) * (e->r + 4)) {
                b->alive = 0;
                if (--e->hp <= 0) {
                    kill_enemy(e, 0);
                } else {
                    e->flash = 0.08;
                    if (e->type == E_WELL) sfx("Enemy_red_hit", 13, 0.08);
                    burst(b->x, b->y, et[e->type].color, 4, 200);
                    e->vx += b->vx * 0.02f; e->vy += b->vy * 0.02f;
                }
                break;
            }
        }
    }
}

static void update_geoms(float dt)
{
    int i, j;
    float keep = expf(-2.0f * dt);
    for (i = 0; i < MAX_GEOM; i++) {
        Geom *g = &geo[i];
        float dx, dy, d;
        if (!g->alive) continue;
        g->t += dt;
        if (g->t > 7) { g->alive = 0; continue; }
        g->vx *= keep; g->vy *= keep;
        for (j = 0; j < MAX_EN; j++)         /* gravity wells eat geoms too */
            if (en[j].alive && en[j].type == E_WELL && en[j].warm <= 0) {
                float wx2 = en[j].x - g->x, wy2 = en[j].y - g->y, wd = sqrtf(wx2 * wx2 + wy2 * wy2) + 0.01f;
                if (wd < 240) { g->vx += wx2 / wd * 500 * dt; g->vy += wy2 / wd * 500 * dt; }
                if (wd < en[j].r) g->alive = 0;
            }
        if (p_alive) {                       /* the ship draws them in when close */
            dx = px - g->x; dy = py - g->y; d = sqrtf(dx * dx + dy * dy) + 0.01f;
            if (d < 150) { g->vx += dx / d * 1400 * dt; g->vy += dy / d * 1400 * dt; }
            if (d < 26) {
                g->alive = 0;
                mult++;
                sfx("Pickup_geom", 14, 0.05);
                if (mult % 10 == 0) sfx("Multiplier", 15, 0);
                continue;
            }
        }
        g->x = clampf(g->x + g->vx * dt, 5, ARENA_W - 5);
        g->y = clampf(g->y + g->vy * dt, 5, ARENA_H - 5);
        g->ang += 3 * dt;
    }
}

static void update_ship(const PlatInput *in, float dt)
{
    float mx = in->lx, my = in->ly, fx = in->rx, fy = in->ry, m;
    if (in->held & BTN_UP) my = -1;          /* the d-pad moves too */
    if (in->held & BTN_DOWN) my = 1;
    if (in->held & BTN_LEFT) mx = -1;
    if (in->held & BTN_RIGHT) mx = 1;
    if (in->held & BTN_TRIANGLE) fy = -1;    /* and the face buttons fire, like a second stick */
    if (in->held & BTN_CROSS) fy = 1;
    if (in->held & BTN_SQUARE) fx = -1;
    if (in->held & BTN_CIRCLE) fx = 1;

    if (!p_alive) {
        respawn_t -= dt;
        if (respawn_t <= 0) {
            if (lives > 0) {
                lives--;
                p_alive = 1; invul_t = 2.0;
                px = ARENA_W / 2; py = ARENA_H / 2; pvx = pvy = 0;
                sfx("Player_Spawn", 9, 0);
                spawn_t = 1.5;
            } else {
                scene = SC_OVER;
                scene_t = 0;
                plat_music_stop();
                music_cur = NULL;
                sfx("Game_over", 9, 0);
                if (score > best) {
                    FILE *f = fopen(plat_save_file(), "w");
                    best = score;
                    if (f) { fprintf(f, "best %ld\n", best); fclose(f); }
                }
            }
        }
        return;
    }
    if (invul_t > 0) invul_t -= dt;
    m = sqrtf(mx * mx + my * my);
    if (m > 0.18f) {                         /* dead zone */
        float s = (m > 1 ? 1 : (m - 0.18f) / 0.82f) * 400;
        pvx += (mx / m * s - pvx) * clampf(14 * dt, 0, 1);
        pvy += (my / m * s - pvy) * clampf(14 * dt, 0, 1);
        pang = atan2f(my, mx);
        if (frand() < dt * 40)               /* a trail of exhaust */
            spark(px - cosf(pang) * 14, py - sinf(pang) * 14, -cosf(pang) * 200 + frange(-40, 40),
                  -sinf(pang) * 200 + frange(-40, 40), 0.35f, 0xffb050ffu);
    } else {
        pvx *= expf(-10 * dt); pvy *= expf(-10 * dt);
    }
    px = clampf(px + pvx * dt, 12, ARENA_W - 12);
    py = clampf(py + pvy * dt, 12, ARENA_H - 12);
    fire_t -= dt;
    m = sqrtf(fx * fx + fy * fy);
    if (m > 0.35f && fire_t <= 0) {
        fire(fx, fy);
        fire_t = 0.085;
    }
    if (in->pressed & (BTN_L | BTN_R)) bomb();
}

/* ------------------------------------------------------------ shapes -- */

static const float SH_SHIP[] = { 1.0f, 0, -0.7f, 0.85f, -0.3f, 0, -0.7f, -0.85f };
static const float SH_DIAMOND[] = { 0, -1, 0.62f, 0, 0, 1, -0.62f, 0 };
static const float SH_SQUARE[] = { -0.8f, -0.8f, 0.8f, -0.8f, 0.8f, 0.8f, -0.8f, 0.8f };
static const float SH_INNER[] = { 0, -0.55f, 0.55f, 0, 0, 0.55f, -0.55f, 0 };
static const float SH_TRI[] = { 1, 0, -0.6f, 0.8f, -0.6f, -0.8f };
static const float SH_BLADE[] = { 0, 0, 1, 0, 0.35f, 0.55f };
static const float SH_GEOM[] = { 0, -1, 0.5f, 0, 0, 1, -0.5f, 0 };
static const float SH_X1[] = { -0.8f, -0.8f, 0.8f, 0.8f };
static const float SH_X2[] = { 0.8f, -0.8f, -0.8f, 0.8f };

static void draw_enemy(const Enemy *e)
{
    unsigned c = e->flash > 0 ? 0xffffffffu : et[e->type].color;
    float a = 1, r = e->r, x = e->x, y = e->y;
    int k;
    if (e->warm > 0) {                       /* appearing: grows in, blinking */
        float f = (float)(e->warm / WARM);
        r *= 1 + f * 1.4f;
        a = ((int)(e->warm * 20) & 1) ? 0.25f : 0.65f;
    }
    switch (e->type) {
    case E_SEEKER:
        r *= 1 + 0.08f * sinf((float)e->t * 8);
        wshape(x, y, r, 0, SH_DIAMOND, 4, 1, c, a);
        break;
    case E_DODGER:
        wshape(x, y, r, e->ang, SH_SQUARE, 4, 1, c, a);
        wshape(x, y, r, e->ang, SH_INNER, 4, 1, c, a * 0.8f);
        break;
    case E_DRIFTER:
        for (k = 0; k < 4; k++) wshape(x, y, r, e->ang + k * PI / 2, SH_BLADE, 3, 1, c, a);
        break;
    case E_SPLITTER:
        wshape(x, y, r, e->ang, SH_SQUARE, 4, 1, c, a);
        wshape(x, y, r, e->ang, SH_X1, 2, 0, c, a * 0.7f);
        wshape(x, y, r, e->ang, SH_X2, 2, 0, c, a * 0.7f);
        break;
    case E_SHARD:
        wshape(x, y, r, e->ang, SH_TRI, 3, 1, c, a);
        break;
    case E_WELL:
        wcircle(x, y, r, 20, c, a);
        wcircle(x, y, r * (0.55f + 0.12f * sinf((float)e->t * 6)), 14, c, a * 0.8f);
        sdot(wx(x), wy(y), r * 2.4f * zoom, with_alpha(c, 0.35f * a));
        break;
    }
}

/* -------------------------------------------------------------- HUD -- */

static void hud(void)
{
    char buf[64], num[32];
    float k = scr_h / 544.0f, cx = scr_w / 2.0f;
    int i;
    fmt_num(num, sizeof num, score);
    text(16 * k, 12 * k, 22 * k, 0, 0xffffffffu, 1, num);
    snprintf(buf, sizeof buf, "x%d", mult);
    text(16 * k, 42 * k, 16 * k, 0, 0xffe070ffu, 1, buf);
    fmt_num(num, sizeof num, best > score ? best : score);
    snprintf(buf, sizeof buf, "HI %s", num);
    text(scr_w - 16 * k, 12 * k, 16 * k, 2, 0x9ad0ffffu, 0.9f, buf);
    for (i = 0; i < lives && i < 8; i++)     /* ships left, to the left of centre */
        sshape(cx - 30 * k - i * 26 * k, 24 * k, 10 * k, -PI / 2, SH_SHIP, 4, 1, 0xffffffffu, 0.9f);
    for (i = 0; i < bombs && i < 8; i++) {   /* bombs, to the right */
        float bx = cx + 30 * k + i * 26 * k, by = 24 * k, rr = 8 * k, lx = 0, ly = 0;
        int s;
        for (s = 0; s <= 10; s++) {
            float t = s * 2 * PI / 10, sx = bx + cosf(t) * rr, sy = by + sinf(t) * rr;
            if (s) gline(lx, ly, sx, sy, 0x7cff7cffu, 0.9f);
            lx = sx; ly = sy;
        }
    }
}

/* ------------------------------------------------------------ scenes -- */

static void music(const char *name)
{
    char path[640];
    if (music_cur && !strcmp(music_cur, name)) return;
    music_cur = name;
    snprintf(path, sizeof path, "%smusic/%s.ogg", plat_data_dir(), name);
    plat_music_play(path);
}

static void start_game(void)
{
    memset(en, 0, sizeof en);
    memset(bul, 0, sizeof bul);
    memset(geo, 0, sizeof geo);
    n_parts = 0;
    grid_reset();
    score = 0; mult = 1; kills = 0; lives = 3; bombs = 3; best_beaten = 0;
    next_life = 75000; next_bomb = 100000;
    play_t = 0; spawn_t = 1.2; rush_t = 35; fire_t = 0; shot_n = 0;
    px = ARENA_W / 2; py = ARENA_H / 2; pvx = pvy = 0; pang = -PI / 2;
    p_alive = 1; invul_t = 1.5;
    cam_x = px; cam_y = py;
    paused = 0;
    scene = SC_PLAY;
    scene_t = 0;
    rng = (unsigned)(now_t * 1000.0) * 2654435761u + 1;
    sfx("Game_start", 9, 0);
    music_cur = NULL;
    music("retro_evolved");
}

static void update_camera(float dt)
{
    float hw = scr_w / zoom / 2, hh = scr_h / zoom / 2;
    cam_x += (px - cam_x) * clampf(dt * 5, 0, 1);
    cam_y += (py - cam_y) * clampf(dt * 5, 0, 1);
    cam_x = clampf(cam_x, hw - 50, ARENA_W - hw + 50);
    cam_y = clampf(cam_y, hh - 50, ARENA_H - hh + 50);
}

static void draw_world(void)
{
    int i;
    float k = scr_h / 544.0f;
    grid_draw();
    gline(wx(0), wy(0), wx(ARENA_W), wy(0), 0x9ad0ffffu, 1);      /* the arena's wall */
    gline(wx(ARENA_W), wy(0), wx(ARENA_W), wy(ARENA_H), 0x9ad0ffffu, 1);
    gline(wx(ARENA_W), wy(ARENA_H), wx(0), wy(ARENA_H), 0x9ad0ffffu, 1);
    gline(wx(0), wy(ARENA_H), wx(0), wy(0), 0x9ad0ffffu, 1);
    for (i = 0; i < MAX_GEOM; i++)
        if (geo[i].alive) {
            float a = geo[i].t > 5 ? (((int)(geo[i].t * 10) & 1) ? 0.3f : 1) : 1;
            wshape(geo[i].x, geo[i].y, 7, geo[i].ang, SH_GEOM, 4, 1, 0xe8ff60ffu, a);
        }
    for (i = 0; i < MAX_BUL; i++)
        if (bul[i].alive) {
            float sp = sqrtf(bul[i].vx * bul[i].vx + bul[i].vy * bul[i].vy) + 0.01f;
            float x = wx(bul[i].x), y = wy(bul[i].y), tx = bul[i].vx / sp * 12 * zoom, ty = bul[i].vy / sp * 12 * zoom;
            sline(x - tx, y - ty, x, y, 9 * k, 0xffd06055u);
            sline(x - tx, y - ty, x, y, 3 * k, 0xfff0a0ffu);
        }
    for (i = 0; i < MAX_EN; i++)
        if (en[i].alive) draw_enemy(&en[i]);
    if (p_alive && !(invul_t > 0 && ((int)(invul_t * 12) & 1)))
        wshape(px, py, 15, pang, SH_SHIP, 4, 1, 0xffffffffu, 1);
    parts_draw();
}

int game_init(int screen_w, int screen_h)
{
    unsigned char *px8 = calloc(128 * 32 * 4, 1);
    int x, y;
    FILE *f;
    if (!px8) return -1;
    scr_w = screen_w;
    scr_h = screen_h;
    zoom = scr_h / VIEW_H;
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
            } else {
                a = 1;                       /* solid */
            }
            p[0] = p[1] = p[2] = 255;
            p[3] = (unsigned char)(a * 255 + 0.5f);
        }
    atlas = plat_tex_create(px8, 128, 32);
    free(px8);
    if (!atlas) return -1;
    f = fopen(plat_save_file(), "r");
    if (f) {
        if (fscanf(f, "best %ld", &best) != 1) best = 0;
        fclose(f);
    }
    grid_reset();
    cam_x = ARENA_W / 2; cam_y = ARENA_H / 2;
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

int game_frame(double dt, const PlatInput *in)
{
    char buf[96], num[32];
    float k = scr_h / 544.0f, fdt;
    static int prev_touch;
    int tap = in->n > 0 && !prev_touch;
    prev_touch = in->n > 0;
    if (in->quit) return 0;
    if (dt > 0.05) dt = 0.05;                /* after a stall, don't jump */
    if (dt < 0) dt = 0;
    now_t += dt;
    scene_t += dt;
    fdt = (float)dt;
    if (music_cur && plat_music_done()) {    /* loop the music */
        const char *m = music_cur;
        music_cur = NULL;
        music(m);
    }

    switch (scene) {
    case SC_TITLE:
        if (frand() < fdt * 1.5f)            /* the grid ripples on its own */
            grid_force(frange(100, ARENA_W - 100), frange(100, ARENA_H - 100), frange(800, 2200), 180);
        grid_update(fdt);
        parts_update(fdt);
        if (scene_t > 0.3 && ((in->pressed & (BTN_CROSS | BTN_START)) || tap))
            start_game();
        break;
    case SC_PLAY:
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
        play_t += dt;
        if (p_alive) {
            spawn_t -= dt;
            if (spawn_t <= 0) {
                spawn_wave();
                spawn_t = fmax(0.35, 1.7 - play_t * 0.011) * frange(0.7f, 1.3f);
            }
            rush_t -= dt;
            if (rush_t <= 0) {
                spawn_rush();
                rush_t = 30;
            }
        }
        update_ship(in, fdt);
        if (scene != SC_PLAY) break;
        update_bullets(fdt);
        update_enemies(fdt);
        update_geoms(fdt);
        grid_update(fdt);
        parts_update(fdt);
        update_camera(fdt);
        break;
    case SC_OVER:
        grid_update(fdt);
        parts_update(fdt);
        if (scene_t > 1.5 && ((in->pressed & (BTN_CROSS | BTN_START)) || tap)) {
            scene = SC_TITLE; scene_t = 0;
            music("main_menu");
        }
        break;
    }

    /* ---- draw ---- */
    plat_begin();
    if (scene == SC_TITLE) {
        cam_x += (ARENA_W / 2 - cam_x) * clampf(fdt * 2, 0, 1);
        cam_y += (ARENA_H / 2 - cam_y) * clampf(fdt * 2, 0, 1);
        grid_draw();
        parts_draw();
        text(scr_w / 2.0f, 150 * k, 60 * k, 1, 0x7cd8ffffu, 1, GAME_TITLE);
        if (((int)(scene_t * 2)) & 1 || scene_t < 0.5)
            text(scr_w / 2.0f, 300 * k, 20 * k, 1, 0xffffffffu, 1, "PRESS X TO START");
        if (best > 0) {
            fmt_num(num, sizeof num, best);
            snprintf(buf, sizeof buf, "HIGH SCORE %s", num);
            text(scr_w / 2.0f, 350 * k, 16 * k, 1, 0xffe070ffu, 1, buf);
        }
        text(scr_w / 2.0f, 470 * k, 12 * k, 1, 0x9ad0ffffu, 0.8f, "LEFT STICK MOVE   RIGHT STICK FIRE   L/R BOMB   START PAUSE");
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
            fmt_num(num, sizeof num, score);
            text(scr_w / 2.0f, 260 * k, 28 * k, 1, 0xffffffffu, 1, num);
            if (score >= best && score > 0)
                text(scr_w / 2.0f, 310 * k, 18 * k, 1, 0xffe070ffu, 1, "NEW HIGH SCORE!");
            if (scene_t > 1.5)
                text(scr_w / 2.0f, 380 * k, 16 * k, 1, 0x9ad0ffffu, 1, "PRESS X");
        }
    }
    flush();
    plat_end();
    return 1;
}

void game_debug(char *buf, int n)
{
    int i, ne = 0, nb = 0, ng = 0;
    for (i = 0; i < MAX_EN; i++) ne += en[i].alive;
    for (i = 0; i < MAX_BUL; i++) nb += bul[i].alive;
    for (i = 0; i < MAX_GEOM; i++) ng += geo[i].alive;
    snprintf(buf, (size_t)n, "scene=%d paused=%d score=%ld mult=%d lives=%d bombs=%d enemies=%d kills=%d "
             "bullets=%d geoms=%d particles=%d alive=%d playt=%.1f best=%ld",
             scene, paused, score, mult, lives, bombs, ne, kills, nb, ng, n_parts, p_alive, play_t, best);
}
