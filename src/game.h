#ifndef GAME_H
#define GAME_H
#include "plat.h"

int game_init(int screen_w, int screen_h);
int game_frame(double dt, const PlatInput *in);   /* 0 = quit */
void game_shutdown(void);
void game_debug(char *buf, int n);                /* state as "key=value ..." for tests */

#endif
