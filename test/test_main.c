/* test_main.c - plays the game headless with simulated sticks and checks it.
 *   test/run_tests.sh <app_data dir> [snapshot dir]
 * Works without app_data too (silent), but then the sound checks are skipped. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/game.h"

extern int soft_render, soft_sprites, soft_live_tex, soft_music_on;
extern double soft_time;
extern char soft_log[];
void soft_dump(const char *path);

static PlatInput in;
static int prev_held, failures;
static const char *out_dir;

static void frame(void)
{
    soft_time += 1.0 / 60;
    in.pressed = in.held & ~prev_held;
    prev_held = in.held;
    game_frame(1.0 / 60, &in);
}

static void run(double secs) { double t; for (t = 0; t < secs; t += 1.0 / 60) frame(); }
static void press(int b) { in.held |= b; frame(); in.held &= ~b; frame(); }

static long field(const char *key)
{
    char buf[512], *p;
    game_debug(buf, sizeof buf);
    p = strstr(buf, key);
    return p ? atol(p + strlen(key)) : -999;
}

static void say(const char *label)
{
    char buf[512];
    game_debug(buf, sizeof buf);
    printf("%-26s %s\n", label, buf);
}

static void check(const char *what, int ok)
{
    printf("%s %s\n", ok ? "  ok  " : "  FAIL", what);
    if (!ok) failures++;
}

static void snap(const char *name)
{
    char path[512];
    soft_render = 1;
    soft_sprites = 0;
    frame();
    soft_render = 0;
    snprintf(path, sizeof path, "%s/%s.png", out_dir, name);
    soft_dump(path);
    printf("  snapshot %s (%d sprites)\n", name, soft_sprites);
}

/* circle the arena and sweep the gun round: plays well enough to score */
static void play_auto(double secs)
{
    double t;
    for (t = 0; t < secs; t += 1.0 / 60) {
        in.lx = cosf((float)soft_time * 0.7f) * 0.8f;
        in.ly = sinf((float)soft_time * 0.9f) * 0.8f;
        in.rx = cosf((float)soft_time * 2.3f);
        in.ry = sinf((float)soft_time * 2.3f);
        frame();
    }
    in.lx = in.ly = in.rx = in.ry = 0;
}

int main(int argc, char **argv)
{
    int have_data;
    long kills0, score0;
    FILE *f;
    out_dir = argc > 1 ? argv[1] : "/tmp";
    remove("/tmp/gwvita_test_save.txt");
    {
        int w, h;
        plat_init(&w, &h);
        if (game_init(w, h) != 0) { printf("game_init failed\n"); return 1; }
    }
    have_data = soft_music_on;
    printf("app_data %s\n", have_data ? "found: sound checks on" : "missing: sound checks skipped");

    run(1); snap("01_title"); say("title");
    check("starts on the title", field("scene=") == 0);
    if (have_data) check("title music playing", soft_music_on);

    press(BTN_CROSS); run(0.5);
    say("started");
    check("X starts a game", field("scene=") == 1 && field("lives=") == 3 && field("bombs=") == 3);
    if (have_data) check("start sound", strstr(soft_log, "Game_start") != NULL);

    play_auto(8); snap("02_early"); say("8 s");
    check("enemies appear", field("enemies=") > 0 || field("kills=") > 0);
    check("the gun fires", strstr(soft_log, "Fire_normal") != NULL || !have_data);

    play_auto(40); snap("03_busy"); say("48 s");
    check("enemies are shot down for points", field("kills=") > 0 && field("score=") > 0);
    check("geoms raise the multiplier", field("mult=") > 1 || field("lives=") < 3);
    if (have_data) check("explosion sound", strstr(soft_log, "Enemy_explode") != NULL);

    /* a bomb sweeps the field and costs a bomb, but scores nothing */
    {
        long b0, s0;
        play_auto(2);
        b0 = field("bombs="); s0 = field("score=");
        if (field("alive=") == 1 && b0 > 0) {
            press(BTN_L); run(0.05);
            say("bomb");
            check("bomb clears the field", field("enemies=") == 0);
            check("bomb used", field("bombs=") == b0 - 1);
            check("bomb scores nothing", field("score=") == s0);
            snap("04_bomb");
        }
    }

    /* pause stops the clock; O quits to the title */
    {
        long t0;
        play_auto(3);
        press(BTN_START);
        t0 = field("playt=");
        run(2);
        check("pause holds the game", field("paused=") == 1 && field("playt=") == t0);
        snap("05_paused");
        press(BTN_START); run(0.2);
        check("START resumes", field("paused=") == 0);
    }

    /* the gravity wells and the rest arrive later on */
    kills0 = field("kills="); score0 = field("score=");
    play_auto(60); snap("06_later"); say("~2 min");
    check("still scoring later", field("kills=") > kills0 || field("score=") > score0 || field("lives=") < 3);

    /* stand still and let the enemies win */
    {
        int i;
        for (i = 0; i < 60 * 600 && field("scene=") == 1; i++) frame();
        say("game over");
        check("the game ends when the ships run out", field("scene=") == 2);
        if (have_data) check("death sound", strstr(soft_log, "Player_explode") != NULL);
        run(2); snap("07_game_over");
        check("high score kept", field("best=") > 0 && field("best=") == field("score="));
        f = fopen("/tmp/gwvita_test_save.txt", "r");
        check("high score saved", f != NULL);
        if (f) fclose(f);
        press(BTN_CROSS); run(0.5);
        check("X returns to the title", field("scene=") == 0);
    }

    /* a fresh start remembers the high score */
    {
        long b = field("best=");
        int w, h;
        game_shutdown();
        plat_init(&w, &h);
        game_init(w, h);
        check("high score loaded at start", field("best=") == b);
        run(1); snap("08_title_with_best");
    }

    game_shutdown();
    printf("textures alive after shutdown: %d\n", soft_live_tex);
    check("no textures left", soft_live_tex == 0);
    printf(failures ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", failures);
    return failures != 0;
}
