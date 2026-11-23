/* vita_aac.h - the game's AAC music on the Vita's hardware decoder (see vita_aac.c) */
#ifndef VITA_AAC_H
#define VITA_AAC_H
#include <stdint.h>
int aac_open(const char *path);                       /* 0 = ready; else use the Vorbis copy */
int aac_read(int16_t *out, int frames, int out_rate); /* mono samples; fewer than asked at the end */
void aac_close(void);
const char *aac_status(void);                         /* one line on what is playing and how */
#endif
