/* main.c - program entry: open the platform, run the frame loop */
#include "game.h"

#ifdef __vita__
int _newlib_heap_size_user = 64 * 1024 * 1024;
#endif

int main(int argc, char **argv)
{
    int w, h;
    (void)argc; (void)argv;

    if (plat_init(&w, &h) != 0)
        return 1;
    if (game_init(w, h) != 0) {
        plat_shutdown();
        return 1;
    }
    plat_run(game_frame);                /* the game on one core, drawing on another */
    game_shutdown();
    plat_shutdown();
    return 0;
}
