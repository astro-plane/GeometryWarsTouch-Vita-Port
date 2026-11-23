#ifndef GAME_H
#define GAME_H
#include "plat.h"

int game_init(int screen_w, int screen_h);
int game_frame(double dt, const PlatInput *in);   /* 0 = quit */
void game_shutdown(void);
void game_debug(char *buf, int n);                /* state as "key=value ..." for tests */
/* tests only: put an enemy (BadGuyType number; 105 = an awake black hole) on the field */
void game_test_spawn(int type, float x, float y);
/* tests only: hold the spawn director back */
void game_test_quiet(void);
/* tests only: choose the mode of the next game (0 Evolved, 1 Deadline, 2 Pacifism, 3 Waves,
 * 4 King, 5 Sequence, 6 Minelayer, 7 Titans, 8 Whirlpool, 9 Meteor) */
void game_test_mode(int m);

#endif
