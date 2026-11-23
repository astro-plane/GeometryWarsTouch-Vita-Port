/* vita_aac.c - plays the game's own AAC music through the Vita's hardware
 * audio decoder (sceAudiodec, which runs on the multimedia processor, not on
 * the ARM cores).
 *
 * The music in the IPA is ADTS AAC-LC, mono, 48 kHz.  Each ADTS frame (1024
 * samples) is copied into an aligned buffer and handed to sceAudiodecDecode;
 * the 48 kHz output is resampled to the mixer's 44.1 kHz.  If anything here
 * fails, plat_sdl.c falls back to the Ogg Vorbis copy, decoded on the CPU.
 *
 * The structures and type codes are declared here (Sony's SDK layout, the one
 * VitaSDK's psp2/audiodec.h describes) so the file builds whatever field names
 * a given VitaSDK uses; it links against libSceAudiodec_stub. */
#ifdef __vita__
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <stdint.h>
#include "vita_aac.h"

#define AUDIODEC_TYPE_AAC   0x1005u
#define AUDIODEC_WORD_16    16u
#define AUDIODEC_ALIGN      0x100

typedef struct { uint32_t size, totalStreams; } InitStreamParam;
typedef union { uint32_t size; InitStreamParam aac; uint8_t pad[16]; } InitParam;
typedef struct { uint32_t size, isAdts, ch, samplingRate, isSbr; } InfoAac;
typedef union { uint32_t size; InfoAac aac; uint8_t pad[32]; } Info;
typedef struct {
    uint32_t size;
    int32_t handle;
    uint8_t *pEs;
    uint32_t inputEsSize, maxEsSize;
    void *pPcm;
    uint32_t outputPcmSize, maxPcmSize, wordLength;
    Info *pInfo;
} Ctrl;

int sceAudiodecInitLibrary(uint32_t codecType, InitParam *p);
int sceAudiodecTermLibrary(uint32_t codecType);
int sceAudiodecCreateDecoder(Ctrl *c, uint32_t codecType);
int sceAudiodecDeleteDecoder(Ctrl *c);
int sceAudiodecDecode(Ctrl *c);

static int lib_ok, dec_ok;
static Ctrl ctrl;
static Info info;
static uint8_t *es, *file;
static char file_path[512];
static int16_t *pcm;
static size_t file_len, file_pos;
static int ch, pcm_n, pcm_pos;
static double frac;                          /* resampler position between s0 and s1 */
static int16_t s0, s1;
static int primed, ended;
static char status[160] = "music: not started";

const char *aac_status(void) { return status; }

static int adts_frame_len(const uint8_t *p, size_t left)
{
    int len;
    if (left < 7 || p[0] != 0xff || (p[1] & 0xf6) != 0xf0) return -1;
    len = ((p[3] & 3) << 11) | (p[4] << 3) | (p[5] >> 5);
    return len >= 7 && (size_t)len <= left ? len : -1;
}

void aac_close(void)
{
    if (dec_ok) sceAudiodecDeleteDecoder(&ctrl);
    dec_ok = 0;                              /* the file stays loaded: a loop just rewinds it */
    free(es); es = NULL;
    free(pcm); pcm = NULL;
}

int aac_open(const char *path)
{
    FILE *f;
    long n;
    int r, rates[16] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000 };
    aac_close();
    if (!file || strcmp(path, file_path) != 0) {
        free(file); file = NULL; file_path[0] = 0;
        f = fopen(path, "rb");
        if (!f) { snprintf(status, sizeof status, "music: no %s", path); return -1; }
        fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
        file = malloc((size_t)n);
        if (!file || fread(file, 1, (size_t)n, f) != (size_t)n) {
            fclose(f); free(file); file = NULL;
            snprintf(status, sizeof status, "music: cannot read %s", path);
            return -1;
        }
        fclose(f);
        file_len = (size_t)n;
        snprintf(file_path, sizeof file_path, "%s", path);
    }
    file_pos = 0;
    if (adts_frame_len(file, file_len) < 0) {
        snprintf(status, sizeof status, "music: %s is not ADTS AAC", path);
        aac_close();
        return -1;
    }
    ch = ((file[2] & 1) << 2) | (file[3] >> 6);
    if (ch < 1 || ch > 2) { snprintf(status, sizeof status, "music: %d channels", ch); aac_close(); return -1; }

    if (!lib_ok) {
        InitParam ip;
        memset(&ip, 0, sizeof ip);
        ip.aac.size = sizeof ip.aac;
        ip.aac.totalStreams = 1;
        r = sceAudiodecInitLibrary(AUDIODEC_TYPE_AAC, &ip);
        if (r < 0) {
            snprintf(status, sizeof status, "music: sceAudiodecInitLibrary failed 0x%08x, using Vorbis", (unsigned)r);
            aac_close();
            return -1;
        }
        lib_ok = 1;
    }
    memset(&info, 0, sizeof info);
    info.aac.size = sizeof info.aac;
    info.aac.isAdts = 1;
    info.aac.ch = (uint32_t)ch;
    info.aac.samplingRate = (uint32_t)rates[(file[2] >> 2) & 15];
    info.aac.isSbr = 0;
    memset(&ctrl, 0, sizeof ctrl);
    ctrl.size = sizeof ctrl;
    ctrl.wordLength = AUDIODEC_WORD_16;
    ctrl.pInfo = &info;
    r = sceAudiodecCreateDecoder(&ctrl, AUDIODEC_TYPE_AAC);
    if (r < 0) {
        snprintf(status, sizeof status, "music: sceAudiodecCreateDecoder failed 0x%08x, using Vorbis", (unsigned)r);
        aac_close();
        return -1;
    }
    dec_ok = 1;
    if (!ctrl.maxEsSize) ctrl.maxEsSize = 6144;
    if (!ctrl.maxPcmSize) ctrl.maxPcmSize = 2048 * 2 * 2;
    es = memalign(AUDIODEC_ALIGN, (ctrl.maxEsSize + AUDIODEC_ALIGN - 1) & ~(AUDIODEC_ALIGN - 1));
    pcm = memalign(AUDIODEC_ALIGN, (ctrl.maxPcmSize + AUDIODEC_ALIGN - 1) & ~(AUDIODEC_ALIGN - 1));
    if (!es || !pcm) { aac_close(); return -1; }
    pcm_n = pcm_pos = 0; frac = 0; primed = ended = 0;
    snprintf(status, sizeof status, "music: hardware AAC (sceAudiodec), %d Hz, %d ch, %s",
             (int)info.aac.samplingRate, ch, path);
    return 0;
}

/* decode the next ADTS frame into pcm[] (mono, down-mixed if stereo) */
static int next_frame(void)
{
    int len = adts_frame_len(file + file_pos, file_len - file_pos), r, i, n;
    if (len < 0) return 0;
    if ((uint32_t)len > ctrl.maxEsSize) { file_pos += (size_t)len; return next_frame(); }
    memcpy(es, file + file_pos, (size_t)len);
    file_pos += (size_t)len;
    ctrl.pEs = es;
    ctrl.inputEsSize = 0;
    ctrl.pPcm = pcm;
    ctrl.outputPcmSize = 0;
    r = sceAudiodecDecode(&ctrl);
    if (r < 0) {                             /* a bad frame: skip it, keep going */
        snprintf(status, sizeof status, "music: sceAudiodecDecode 0x%08x (frame skipped)", (unsigned)r);
        pcm_n = pcm_pos = 0;
        return 1;
    }
    n = (int)(ctrl.outputPcmSize / 2 / (uint32_t)ch);
    if (ch == 2)
        for (i = 0; i < n; i++) pcm[i] = (int16_t)((pcm[2 * i] + pcm[2 * i + 1]) / 2);
    pcm_n = n;
    pcm_pos = 0;
    return 1;
}

/* the next 48 kHz sample, decoding frames as needed */
static int get_sample(int16_t *v)
{
    while (pcm_pos >= pcm_n)
        if (!next_frame()) return 0;
    *v = pcm[pcm_pos++];
    return 1;
}

/* fill `frames` mono samples at out_rate (linear resampling); returns how
 * many, fewer at the end of the track */
int aac_read(int16_t *out, int frames, int out_rate)
{
    double step = (double)info.aac.samplingRate / out_rate;
    int done = 0;
    if (!dec_ok || ended) return 0;
    if (!primed) {
        if (!get_sample(&s0) || !get_sample(&s1)) { ended = 1; return 0; }
        primed = 1; frac = 0;
    }
    while (done < frames) {
        out[done++] = (int16_t)(s0 + (s1 - s0) * frac);
        frac += step;
        while (frac >= 1) {
            frac -= 1;
            s0 = s1;
            if (!get_sample(&s1)) { ended = 1; return done; }
        }
    }
    return done;
}
#endif
