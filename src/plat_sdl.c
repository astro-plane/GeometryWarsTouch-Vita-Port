/* plat_sdl.c - SDL2 implementation of plat.h for the PS Vita (VitaSDK's SDL2
 * port) and for desktop builds.  Music is streamed from Ogg Vorbis with
 * libvorbisfile inside the audio callback; sound effects are WAV files.
 * Sprites are drawn in batches with SDL_RenderGeometry: the game draws
 * thousands of glowing line segments a frame. */
#include <SDL.h>
#include <vorbis/vorbisfile.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <math.h>
#include "plat.h"

#define SCREEN_W 960
#define SCREEN_H 544
#define MIX_FREQ 44100
#define MAX_SOUNDS 160
#define MAX_VOICES 14
#define BATCH 4096                   /* sprites per draw call */

struct PlatTex { SDL_Texture *t; int w, h; };
typedef struct { char name[48]; Sint16 *pcm; Uint32 frames; int failed; } Sound;
typedef struct { const Sound *s; Uint32 pos; } Voice;

static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_AudioDeviceID audio;
static SDL_Joystick *joy;
static int win_w = SCREEN_W, win_h = SCREEN_H;
static char data_dir[512], save_file[512];

static Sound sounds[MAX_SOUNDS];
static int n_sounds;
static Voice voices[MAX_VOICES];

/* music state, shared with the audio callback (guarded by SDL_LockAudioDevice) */
static OggVorbis_File vf;
static int music_open, music_paused, music_eof, music_channels = 2;
static Sint16 dec_buf[4096 * 2];

typedef struct { SDL_FingerID id; float x, y; int on; } Finger;
static Finger fingers[MAX_TOUCH];
static int held_buttons;

static SDL_Vertex verts[BATCH * 4];
static int idx[BATCH * 6];

/* ------------------------------------------------------------- audio -- */

static void mix_cb(void *ud, Uint8 *stream, int len)
{
    Sint16 *out = (Sint16 *)stream;
    int frames = len / 4, done = 0, v, i;
    (void)ud;
    memset(stream, 0, (size_t)len);

    if (music_open && !music_paused && !music_eof) {
        while (done < frames) {
            int want = frames - done, bs, got;
            long n;
            if (want > 4096) want = 4096;
            n = ov_read(&vf, (char *)dec_buf, want * music_channels * 2, 0, 2, 1, &bs);
            if (n <= 0) {
                if (n == 0) { music_eof = 1; break; }
                continue;                    /* a hole in the stream: keep going */
            }
            got = (int)(n / (2 * music_channels));
            for (i = 0; i < got; i++) {
                Sint16 l = dec_buf[i * music_channels];
                Sint16 r = music_channels > 1 ? dec_buf[i * music_channels + 1] : l;
                out[(done + i) * 2] = (Sint16)(l * 3 / 4);   /* leave room for the effects */
                out[(done + i) * 2 + 1] = (Sint16)(r * 3 / 4);
            }
            done += got;
        }
    }

    for (v = 0; v < MAX_VOICES; v++) {
        Voice *vo = &voices[v];
        if (!vo->s)
            continue;
        for (i = 0; i < frames && vo->pos < vo->s->frames; i++, vo->pos++) {
            int s = vo->s->pcm[vo->pos] / 2;
            int l = out[i * 2] + s, r = out[i * 2 + 1] + s;
            out[i * 2] = (Sint16)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
            out[i * 2 + 1] = (Sint16)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
        }
        if (vo->pos >= vo->s->frames)
            vo->s = NULL;
    }
}

int plat_music_play(const char *path)
{
    OggVorbis_File nv;
    vorbis_info *vi;
    plat_music_stop();
    if (!audio)
        return -1;
    if (ov_fopen(path, &nv) != 0) {
        fprintf(stderr, "cannot open %s\n", path);
        return -1;
    }
    vi = ov_info(&nv, -1);
    if (!vi || vi->rate != MIX_FREQ || vi->channels < 1 || vi->channels > 2) {
        fprintf(stderr, "%s: needs 44.1 kHz mono/stereo (rerun prep_assets.py)\n", path);
        ov_clear(&nv);
        return -1;
    }
    SDL_LockAudioDevice(audio);
    vf = nv;
    music_channels = vi->channels;
    music_eof = 0;
    music_paused = 0;
    music_open = 1;
    SDL_UnlockAudioDevice(audio);
    return 0;
}

void plat_music_stop(void)
{
    if (!audio || !music_open)
        return;
    SDL_LockAudioDevice(audio);
    music_open = 0;
    ov_clear(&vf);
    SDL_UnlockAudioDevice(audio);
}

void plat_music_pause(int p)
{
    if (!audio)
        return;
    SDL_LockAudioDevice(audio);
    music_paused = p;
    SDL_UnlockAudioDevice(audio);
}

int plat_music_done(void)
{
    int d;
    if (!audio)
        return 1;
    SDL_LockAudioDevice(audio);
    d = !music_open || music_eof;
    SDL_UnlockAudioDevice(audio);
    return d;
}

static Sound *sound_get(const char *name)
{
    char path[640];
    SDL_AudioSpec spec;
    SDL_AudioCVT cvt;
    Uint8 *buf = NULL;
    Uint32 len = 0;
    Sound *s;
    int i;
    for (i = 0; i < n_sounds; i++)
        if (!strcmp(sounds[i].name, name))
            return sounds[i].failed ? NULL : &sounds[i];
    if (n_sounds == MAX_SOUNDS)
        return NULL;
    s = &sounds[n_sounds++];
    SDL_snprintf(s->name, sizeof s->name, "%s", name);
    s->failed = 1;
    SDL_snprintf(path, sizeof path, "%ssfx/%s.wav", data_dir, name);
    if (!SDL_LoadWAV(path, &spec, &buf, &len))
        return NULL;
    /* the game's effects are 24 kHz mono: one conversion to the mixer's format */
    if (SDL_BuildAudioCVT(&cvt, spec.format, spec.channels, spec.freq, AUDIO_S16SYS, 1, MIX_FREQ) < 0) {
        SDL_FreeWAV(buf);
        return NULL;
    }
    cvt.len = (int)len;
    cvt.buf = SDL_malloc((size_t)len * (size_t)cvt.len_mult + 16);
    if (!cvt.buf) {
        SDL_FreeWAV(buf);
        return NULL;
    }
    memcpy(cvt.buf, buf, len);
    SDL_FreeWAV(buf);
    if (cvt.needed && SDL_ConvertAudio(&cvt) < 0) {
        SDL_free(cvt.buf);
        return NULL;
    }
    s->pcm = (Sint16 *)cvt.buf;
    s->frames = (Uint32)(cvt.needed ? cvt.len_cvt : (int)len) / 2;
    s->failed = 0;
    return s;
}

void plat_sfx(const char *name)
{
    Sound *s;
    int v, pick = -1;
    Uint32 oldest = 0;
    if (!audio || !name || !(s = sound_get(name)))
        return;
    SDL_LockAudioDevice(audio);
    for (v = 0; v < MAX_VOICES; v++)
        if (!voices[v].s) { pick = v; break; }
    if (pick < 0)                            /* all busy: replace the one furthest along */
        for (v = 0; v < MAX_VOICES; v++)
            if (voices[v].pos >= oldest) { oldest = voices[v].pos; pick = v; }
    voices[pick].s = s;
    voices[pick].pos = 0;
    SDL_UnlockAudioDevice(audio);
}

/* -------------------------------------------------------------- video -- */

int plat_init(int *screen_w, int *screen_h)
{
    SDL_AudioSpec want, have;
    int i;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return -1;
    }
#ifdef __vita__
    /* The game's sound lives on the memory card (ux0:data/gwvita/app_data/),
     * copied there by hand; app0: is used when it was built into the VPK. */
    mkdir("ux0:data", 0777);
    mkdir("ux0:data/gwvita", 0777);
    SDL_snprintf(save_file, sizeof save_file, "ux0:data/gwvita/save.txt");
    {
        static const char *where[2] = { "ux0:data/gwvita/app_data/", "app0:app_data/" };
        char probe[640];
        SDL_snprintf(data_dir, sizeof data_dir, "%s", where[0]);
        for (i = 0; i < 2; i++) {
            FILE *f;
            SDL_snprintf(probe, sizeof probe, "%smanifest.txt", where[i]);
            f = fopen(probe, "r");
            if (f) {
                fclose(f);
                SDL_snprintf(data_dir, sizeof data_dir, "%s", where[i]);
                break;
            }
        }
        if (i == 2)
            fprintf(stderr, "no manifest.txt - copy app_data to ux0:data/gwvita/app_data/ (the game runs silent)\n");
    }
#else
    {
        const char *env = getenv("GW_DATA");
        SDL_snprintf(data_dir, sizeof data_dir, "%s", env && env[0] ? env : "app_data/");
        if (data_dir[0] && data_dir[strlen(data_dir) - 1] != '/')
            SDL_strlcat(data_dir, "/", sizeof data_dir);
        SDL_snprintf(save_file, sizeof save_file, "gwvita_save.txt");
    }
#endif
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    win = SDL_CreateWindow("GWars Vita (reimplementation)", SDL_WINDOWPOS_CENTERED,
                           SDL_WINDOWPOS_CENTERED, SCREEN_W, SCREEN_H, SDL_WINDOW_SHOWN);
    if (!win) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return -1;
    }
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren)
        ren = SDL_CreateRenderer(win, -1, 0);
    if (!ren) {
        fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        return -1;
    }
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_GetRendererOutputSize(ren, &win_w, &win_h);
    *screen_w = win_w;
    *screen_h = win_h;
    if (SDL_NumJoysticks() > 0)
        joy = SDL_JoystickOpen(0);
    for (i = 0; i < BATCH; i++) {            /* two triangles per quad, fixed for good */
        idx[i * 6 + 0] = i * 4 + 0; idx[i * 6 + 1] = i * 4 + 1; idx[i * 6 + 2] = i * 4 + 2;
        idx[i * 6 + 3] = i * 4 + 0; idx[i * 6 + 4] = i * 4 + 2; idx[i * 6 + 5] = i * 4 + 3;
    }

    SDL_zero(want);
    want.freq = MIX_FREQ;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = mix_cb;
    audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (audio)
        SDL_PauseAudioDevice(audio, 0);
    else
        fprintf(stderr, "no audio device (%s): playing silent\n", SDL_GetError());
    return 0;
}

void plat_shutdown(void)
{
    int i;
    plat_music_stop();
    if (audio)
        SDL_CloseAudioDevice(audio);
    for (i = 0; i < n_sounds; i++)
        SDL_free(sounds[i].pcm);
    if (joy)
        SDL_JoystickClose(joy);
    if (ren)
        SDL_DestroyRenderer(ren);
    if (win)
        SDL_DestroyWindow(win);
    SDL_Quit();
}

static int finger_slot(SDL_FingerID id, int make)
{
    int i;
    for (i = 0; i < MAX_TOUCH; i++)
        if (fingers[i].on && fingers[i].id == id)
            return i;
    if (make)
        for (i = 0; i < MAX_TOUCH; i++)
            if (!fingers[i].on) {
                fingers[i].on = 1;
                fingers[i].id = id;
                return i;
            }
    return -1;
}

/* Vita buttons as numbered by SDL's Vita joystick driver */
static int joy_bit(int b)
{
    static const int map[12] = { BTN_TRIANGLE, BTN_CIRCLE, BTN_CROSS, BTN_SQUARE, BTN_L, BTN_R,
                                 BTN_DOWN, BTN_LEFT, BTN_UP, BTN_RIGHT, BTN_SELECT, BTN_START };
    return b >= 0 && b < 12 ? map[b] : 0;
}

/* Desktop keys.  WASD and the arrow keys are read as sticks in plat_poll. */
static int key_bit(SDL_Keycode k)
{
    switch (k) {
    case SDLK_RETURN: return BTN_CROSS;
    case SDLK_ESCAPE: case SDLK_BACKSPACE: return BTN_CIRCLE;
    case SDLK_SPACE: return BTN_L;           /* bomb */
    case SDLK_p: return BTN_START;
    case SDLK_w: return BTN_UP;
    case SDLK_s: return BTN_DOWN;
    default: return 0;
    }
}

static float axis(int a)
{
    float v;
    if (!joy || a >= SDL_JoystickNumAxes(joy))
        return 0;
    v = SDL_JoystickGetAxis(joy, a) / 32767.0f;
    return v < -1 ? -1 : v > 1 ? 1 : v;
}

void plat_poll(PlatInput *in)
{
    SDL_Event e;
    int i, before = held_buttons, slot;
    const Uint8 *k;
    float kx, ky;
    in->quit = 0;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT:
            in->quit = 1;
            break;
        case SDL_FINGERDOWN:
        case SDL_FINGERMOTION:
            if (SDL_GetTouchDeviceType(e.tfinger.touchId) != SDL_TOUCH_DEVICE_DIRECT)
                break;                       /* front screen only */
            slot = finger_slot(e.tfinger.fingerId, e.type == SDL_FINGERDOWN);
            if (slot >= 0) {
                fingers[slot].x = e.tfinger.x * win_w;
                fingers[slot].y = e.tfinger.y * win_h;
            }
            break;
        case SDL_FINGERUP:
            slot = finger_slot(e.tfinger.fingerId, 0);
            if (slot >= 0)
                fingers[slot].on = 0;
            break;
        case SDL_KEYDOWN:
            if (!e.key.repeat)
                held_buttons |= key_bit(e.key.keysym.sym);
            break;
        case SDL_KEYUP:
            held_buttons &= ~key_bit(e.key.keysym.sym);
            break;
        case SDL_JOYBUTTONDOWN:
            held_buttons |= joy_bit(e.jbutton.button);
            break;
        case SDL_JOYBUTTONUP:
            held_buttons &= ~joy_bit(e.jbutton.button);
            break;
        default:
            break;
        }
    }
    in->n = 0;
    for (i = 0; i < MAX_TOUCH; i++)
        if (fingers[i].on) {
            in->t[in->n].id = (long long)fingers[i].id;
            in->t[in->n].x = fingers[i].x;
            in->t[in->n].y = fingers[i].y;
            in->n++;
        }
    in->held = held_buttons;
    in->pressed = held_buttons & ~before;

    /* sticks: the Vita's (axes 0-1 left, 2-3 right), or WASD + arrow keys */
    in->lx = axis(0); in->ly = axis(1); in->rx = axis(2); in->ry = axis(3);
    k = SDL_GetKeyboardState(NULL);
    kx = (float)(k[SDL_SCANCODE_D] - k[SDL_SCANCODE_A]);
    ky = (float)(k[SDL_SCANCODE_S] - k[SDL_SCANCODE_W]);
    if (kx || ky) { in->lx = kx; in->ly = ky; }
    kx = (float)(k[SDL_SCANCODE_RIGHT] - k[SDL_SCANCODE_LEFT]);
    ky = (float)(k[SDL_SCANCODE_DOWN] - k[SDL_SCANCODE_UP]);
    if (kx || ky) { in->rx = kx; in->ry = ky; }
}

double plat_time(void)
{
    return (double)SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency();
}

const char *plat_data_dir(void) { return data_dir; }
const char *plat_save_file(void) { return save_file; }

PlatTex *plat_tex_create(const unsigned char *rgba, int w, int h)
{
    PlatTex *t = malloc(sizeof *t);
    if (!t)
        return NULL;
    t->w = w;
    t->h = h;
    t->t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h);
    if (!t->t || SDL_UpdateTexture(t->t, NULL, rgba, w * 4) != 0) {
        if (t->t)
            SDL_DestroyTexture(t->t);
        free(t);
        return NULL;
    }
    return t;
}

void plat_tex_free(PlatTex *t)
{
    if (t) {
        SDL_DestroyTexture(t->t);
        free(t);
    }
}

void plat_begin(void)
{
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
}

void plat_end(void)
{
    SDL_RenderPresent(ren);
}

void plat_sprites(PlatTex *t, int sx, int sy, int sw, int sh, const PlatSprite *s, int n, int blend)
{
    float u0, v0, u1, v1;
    int i, k, m;
    if (!t || n <= 0)
        return;
    u0 = (float)sx / t->w; u1 = (float)(sx + sw) / t->w;
    v0 = (float)sy / t->h; v1 = (float)(sy + sh) / t->h;
    SDL_SetTextureBlendMode(t->t, blend == 1 ? SDL_BLENDMODE_ADD : SDL_BLENDMODE_BLEND);
    for (k = 0; k < n; k += BATCH) {
        m = n - k < BATCH ? n - k : BATCH;
        for (i = 0; i < m; i++) {
            const PlatSprite *p = &s[k + i];
            float c = cosf(p->ang), sn = sinf(p->ang), hx = p->w / 2, hy = p->h / 2;
            SDL_Color col;
            SDL_Vertex *v = &verts[i * 4];
            col.r = (Uint8)(p->rgba >> 24); col.g = (Uint8)(p->rgba >> 16);
            col.b = (Uint8)(p->rgba >> 8); col.a = (Uint8)p->rgba;
            v[0].position.x = p->cx - hx * c + hy * sn; v[0].position.y = p->cy - hx * sn - hy * c;
            v[1].position.x = p->cx + hx * c + hy * sn; v[1].position.y = p->cy + hx * sn - hy * c;
            v[2].position.x = p->cx + hx * c - hy * sn; v[2].position.y = p->cy + hx * sn + hy * c;
            v[3].position.x = p->cx - hx * c - hy * sn; v[3].position.y = p->cy - hx * sn + hy * c;
            v[0].tex_coord.x = u0; v[0].tex_coord.y = v0;
            v[1].tex_coord.x = u1; v[1].tex_coord.y = v0;
            v[2].tex_coord.x = u1; v[2].tex_coord.y = v1;
            v[3].tex_coord.x = u0; v[3].tex_coord.y = v1;
            v[0].color = v[1].color = v[2].color = v[3].color = col;
        }
        SDL_RenderGeometry(ren, t->t, verts, m * 4, idx, m * 6);
    }
}

void plat_fill(float x, float y, float w, float h, unsigned rgba)
{
    SDL_FRect rc;
    rc.x = x; rc.y = y; rc.w = w; rc.h = h;
    SDL_SetRenderDrawColor(ren, (Uint8)(rgba >> 24), (Uint8)(rgba >> 16), (Uint8)(rgba >> 8), (Uint8)rgba);
    SDL_RenderFillRectF(ren, &rc);
}
