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
static int have_levels;

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

static void to_title(void)
{
    in.lx = in.ly = in.rx = in.ry = 0;
    if (field("scene=") == 1) { press(BTN_START); press(BTN_CIRCLE); }
    if (field("scene=") == 2) { run(1.7); press(BTN_CROSS); }
    run(0.4);
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
    {
        char lp[600];
        FILE *lf;
        snprintf(lp, sizeof lp, "%slevels.txt", getenv("GW_DATA") ? getenv("GW_DATA") : "");
        lf = fopen(lp, "r");
        have_levels = lf != NULL;
        if (lf) fclose(lf);
    }
    printf("app_data %s\n", have_data ? "found: sound checks on" : "missing: sound checks skipped");

    run(1); snap("01_title"); say("title");
    check("starts on the title", field("scene=") == 0);
    if (have_data) check("title music playing", soft_music_on);

    press(BTN_CROSS); run(0.5);
    say("started");
    check("X starts a game", field("scene=") == 1 && field("lives=") == 3 && field("bombs=") == 3);
    if (have_data) check("start sound", strstr(soft_log, "Game_start") != NULL);

    /* pause stops the clock; O quits to the title */
    {
        long t0;
        play_auto(1);
        press(BTN_START);
        t0 = field("playt=");
        run(2);
        check("pause holds the game", field("paused=") == 1 && field("playt=") == t0);
        snap("05_paused");
        press(BTN_START); run(0.2);
        check("START resumes", field("paused=") == 0);
    }

    play_auto(8); snap("02_early"); say("8 s");
    check("enemies appear", field("enemies=") > 0 || field("kills=") > 0);
    check("the gun fires", strstr(soft_log, "Fire_normal") != NULL || !have_data);

    play_auto(40); snap("03_busy"); say("48 s");
    check("enemies are shot down for points", field("kills=") > 0 && field("score=") > 0);
    check("geoms raise the multiplier", field("mult=") > 1 || field("lives=") < 3);
    if (have_data) check("explosion sound", strstr(soft_log, "Enemy_explode") != NULL);

    /* a bomb: an expanding ring (radius 400, 0.75 s) that kills but scores nothing */
    {
        long b0, s0, e0;
        play_auto(2);
        b0 = field("bombs="); s0 = field("score="); e0 = field("enemies=");
        if (field("alive=") == 1 && b0 > 0) {
            press(BTN_L); run(0.8);
            say("bomb");
            check("bomb kills enemies", field("enemies=") < e0 || e0 == 0);
            check("bomb used", field("bombs=") == b0 - 1);
            check("bomb scores nothing", field("score=") == s0);
            snap("04_bomb");
        }
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
        if (have_data) check("death sound", strstr(soft_log, "Ship_explode") != NULL);
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

    /* the recovered rules, one at a time, with the spawn director held back */
    {
        long x0, e0;
        press(BTN_CROSS); run(0.1);
        game_test_quiet();
        run(1.2);                                /* ship's spawn protection runs out */
        say("fresh game, quiet");

        /* gun: 3 bullets a volley, a volley every 0.1 s (FireProjectile 0x0003b11c) */
        in.rx = 1; run(0.5); in.rx = 0;
        say("0.5 s of fire");
        check("3 bullets per volley, 10 volleys a second", field("bullets=") >= 12 && field("bullets=") <= 18);
        run(1.0);                                /* they fly off into the wall */
        check("bullets end at the wall", field("bullets=") == 0);

        /* first kill at x1: floor(1 * 0.25 * 50) rounded down to 5 = 10 points */
        game_test_spawn(2, (float)field("px=") + 90, (float)field("py="));
        in.rx = 1; run(0.3); in.rx = 0; run(0.2);
        say("grunt shot");
        check("a grunt at x1 scores 10", field("score=") == 10);
        /* fly over the geoms it left to raise the multiplier */
        in.lx = 1; run(0.5); in.lx = 0; run(0.6);
        say("geoms collected");
        check("geoms raise the multiplier", field("mult=") > 1);
        if (have_data) check("geom pickup sound", strstr(soft_log, "Pickup_geom") != NULL);

        /* a sleeping black hole does not pull; one shot wakes it */
        x0 = field("px=");
        game_test_spawn(5, (float)x0 + 80, (float)field("py="));
        run(0.6);
        say("sleeping well beside the ship");
        check("a sleeping black hole does not pull", labs(field("px=") - x0) <= 2);
        check("the black hole is asleep", field("active=") == 0);
        in.rx = 1; run(0.04); in.rx = 0;   /* one volley: StartHits is 6, so more would kill it */
        run(0.4);
        say("awake well");
        check("one volley wakes it", field("active=") == 1);
        check("and it survives three hits (StartHits 6)", field("wells=") == 1);
        check("an awake black hole drags the ship in", field("px=") > x0 + 6);
        snap("09_well");
        press(BTN_L); run(1.0);                  /* bomb it away (it shrinks to nothing) */
        check("the bomb destroys the black hole", field("wells=") == 0);

        /* an awake black hole eats what comes near and bursts into Protons */
        game_test_spawn(105, -200, -100);
        {
            int k;
            for (k = 0; k < 14; k++) game_test_spawn(2, -200, -100);
        }
        run(0.5);
        say("overfed well");
        check("an overfull black hole bursts", field("wells=") == 0);
        check("it releases a swarm of Protons", field("enemies=") >= 15);
        snap("10_burst");
        press(BTN_L); run(1.0);

        /* a Spinner splits in three when a bullet kills it */
        run(2.5);                                /* the bomb's cooldown and the field settle */
        e0 = field("kills=");
        game_test_spawn(4, (float)field("px=") + 70, (float)field("py="));
        in.rx = 1; run(0.04); in.rx = 0; run(0.1);
        say("spinner shot");
        check("the spinner died", field("kills=") > e0);
        if (have_data) check("it splits (sound)", strstr(soft_log, "SpinnerSplit") != NULL);

        /* the snake's body shrugs off bullets */
        game_test_spawn(8, 150, 120);
        run(0.2);
        snap("11_snake");
        if (have_data) check("snake spawn sound", strstr(soft_log, "Snake_spawn") != NULL);
    }

    /* ---- the other modes, each from the title ---- */
    {
        static const char *names[10] = { "evolved", "deadline", "pacifism", "waves", "king",
                                         "sequence", "minelayer", "titans", "whirlpool", "meteor" };
        int m;
        for (m = 1; m < 10; m++) {
            char nm[64];
            long k0;
            to_title();
            game_test_mode(m);
            run(0.4);
            press(BTN_CROSS);
            run(0.1);
            snprintf(nm, sizeof nm, "mode %s", names[m]);
            say(nm);
            if (m == 5 && !have_levels) { check("sequence needs levels.txt (skipped)", 1); continue; }
            check("the mode starts", field("scene=") == 1 && field("mode=") == m);
            switch (m) {
            case 1:                              /* Deadline: endless ships, a clock, the budget grows */
                check("deadline: no spare ships, 3 bombs", field("lives=") == 0 && field("bombs=") == 3);
                play_auto(12);
                say("deadline 12 s");
                check("deadline: waves come (budget > 0)", field("budget=") > 0);
                break;
            case 2:                              /* Pacifism: no gun, grunts and gates */
                play_auto(6);
                say("pacifism 6 s");
                check("pacifism: the gun does not fire", field("bullets=") == 0);
                check("pacifism: grunts come", field("enemies=") > 0);
                break;
            case 3:                              /* Waves: walls of rockets */
                run(2.5);
                say("waves 2.5 s");
                check("waves: a wall of rockets", field("rockets=") >= 10);
                break;
            case 4:                              /* King: three zones; firing only inside one */
                check("king: three zones", field("hills=") == 3);
                check("king: one life", field("lives=") == 0);
                in.rx = 1; run(0.3); in.rx = 0;
                check("king: firing works in the middle zone", field("bullets=") > 0 || field("kills=") > 0);
                in.lx = 1; run(1.2); in.lx = 0; run(0.6);
                k0 = field("bullets=");
                in.rx = -1; run(0.3); in.rx = 0;
                say("king outside");
                check("king: no firing outside a zone", field("bullets=") <= k0);
                break;
            case 5:                              /* Sequence: level 1 starts from its script */
                run(1.5);
                say("sequence 1.5 s");
                check("sequence: level 1's opening ring spawned", field("enemies=") >= 4);
                break;
            case 6:                              /* Minelayer: three layers, mines, no gun */
                run(4);
                say("minelayer 4 s");
                check("minelayer: three minelayers laying mines", field("mines=") > 0);
                check("minelayer: the gun does not fire", field("bullets=") == 0);
                break;
            case 7:                              /* Titans: big ones that break up */
                run(1.3);
                say("titans 1.3 s");
                check("titans: a titan arrived", field("titans=") >= 1);
                break;
            case 8:                              /* Whirlpool: the black hole in the middle */
                run(1.3);
                say("whirlpool 1.3 s");
                check("whirlpool: the black hole is there", field("holes=") == 1);
                break;
            case 9:                              /* Meteor: a shower of 32 */
                run(1.3);
                say("meteor 1.3 s");
                check("meteor: a meteor shower", field("meteors=") >= 20);
                break;
            }
            run(0.5);
            snprintf(nm, sizeof nm, "20_mode_%s", names[m]);
            snap(nm);
        }
    }

    game_shutdown();
    printf("textures alive after shutdown: %d\n", soft_live_tex);
    check("no textures left", soft_live_tex == 0);
    printf(failures ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", failures);
    return failures != 0;
}
