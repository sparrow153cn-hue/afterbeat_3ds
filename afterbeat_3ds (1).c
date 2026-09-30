/*
 * AFTERBEAT v2 (Nintendo 3DS) - devkitPro / libctru / citro2d
 *
 * Top screen (400x240): the arena.   Bottom screen (320x240): touch level select / status.
 *
 * Play:   Circle Pad / D-Pad = move      A / B / L / R = dash (i-frames; dash on the beat = PERFECT)
 *         Y or tap the BLUE BALL (last boss only) = slow time + every attack heals you    START = pause
 * Finale (after HOLLOW's song ends): R = shoot beams upward, hold R to charge a bigger beam.
 * Editor: SELECT on the level-select screen. Import a video (see tools/vid2abv.py), pick the colours
 *         that hurt with the eyedropper / HSV palette, then play the video as a level.
 * Menu:   D-Pad or touch = choose, A / tap again / PLAY = start
 *         Y = toggle HARDCORE (original difficulty)     X = toggle CASUAL (2x HP)      START = exit
 *
 * Normal mode is the easier version of every level; Hardcore is the original difficulty.
 * HP: 3 (boss levels 6), casual doubles it.
 * Checkpoints (grey expanding circle) appear in standard levels.
 * Audio is synthesized at runtime (16-step sequencer); if ndsp/DSP firmware is missing it runs silently.
 */
#include <3ds.h>
#include <citro2d.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

#define SW 400
#define SH 240
#define MAXH 700
#define MAXP 450
#define NL 16
#define PI2 6.2831853f
#define PIF 3.1415927f
#define PR 3.5f
#define DASH_TIME 0.17f
#define SR 22050
#define VOICES 23           /* channel 23 is reserved for streaming video audio */
#define PADCH 3             /* channels 0-2 hold the sustained pad chord; the rest are shared */
#define ACH 23
#define SAVE_DIR "sdmc:/3ds/afterbeat"
#define SAVE_FILE "sdmc:/3ds/afterbeat/afterbeat2.sav"
#define ULT (NL - 1)
#define ULT_PHASE 128           /* beats per boss phase in the ultimate level */

enum { ST_MENU, ST_PLAY, ST_PAUSE, ST_OVER, ST_WIN, ST_CUT, ST_EDFILES, ST_EDIT, ST_VID, ST_VPAUSE, ST_VOVER, ST_VWIN };
enum { P_RAIN, P_RING, P_AIM, P_FAN, P_HBEAM, P_VBEAM, P_WALLH, P_WALLV, P_BOMB, P_SPIRAL, P_PULSE,
       P_SIDE, P_CROSS, P_HELIX, P_TRI, P_TRIGRID, P_SAWROW, P_SAWRAIL, P_SAWTHROW, P_SPINBEAM, P_CANNON,
       P_BOUNCE, P_HOMING, P_CHASE, P_WAVE, P_LANES, P_BOMBLINE, P_STAR, P_PINWHEEL };
enum { H_NONE, H_BULLET, H_RECT, H_RING, H_BOMB, H_LINE, H_SAW, H_SPIN, H_CANNON, H_EMIT };
enum { S_KICK, S_HAT, S_SNARE, S_HIT, S_DASH, S_CLEAR, S_OHAT, S_CLAP, S_COUNT, K_BASS = 20, K_LEAD };
enum { INS_BASSW, INS_BASSQ, INS_SUB, INS_PLUCK, INS_PAD, INS_BELL, INS_LEAD, INS_STAB, INS_COUNT };   /* synth instruments */
#define GIM_ICE 1        /* ice physics: you slide */
#define GIM_WIND 2       /* gentle crosswind */
#define GIM_WINDS 4      /* strong crosswind */
#define GIM_DARK 8       /* bullets are hidden unless close (or on the beat) */
#define GIM_SQUEEZE 16   /* the side walls squeeze in and out with the beat */

/* ------------------------------------------------------------------ types */
typedef struct { unsigned char r, g, b, a; } Color;
typedef struct { s16 *d; int n; } Snd;
typedef struct { float x, y, vx, vy, life, max, size, grow, rot, spin; Color c; int kind; } Particle;
typedef struct {
    int type, flag; float x, y, vx, vy, r, w, h, warn, life, thick, vr; int split, dmg; Color c;
    float x2, y2, rot, amp, ph, y0;
} Hazard;
typedef struct { int from, to, every, pat, arg; } Ev;
typedef struct {
    const char *name, *sub;
    int bpm, beats, boss, shape, root, style;      /* boss: 0 standard, 1 boss, 2 ultimate */
    float scroll, nspd;                            /* factory scroll speed; Normal-mode bullet speed multiplier */
    int song, gim, bgs;                            /* song index (-1 = old style engine), gimmick flags, background style */
    Color c1, c2;
    int nev;
    Ev ev[96];
} Level;
typedef struct {
    float x, y, fx, fy, ang, inv, dashT, dashCD, dx, dy, trail, healCD, mvx, mvy;
    int hp, maxhp;
} Player;
typedef struct { const char *kick, *snare, *hat, *ohat, *clap, *bass, *lead; } Style;

static const Color WHITE = {255, 255, 255, 255};
static const Color BLACK = {0, 0, 0, 255};

/* ------------------------------------------------------------ level data */
/* Hardcore = these scripts as written. Normal derives an easier version (see adjEv). */
static Level LV[NL] = {
 /* 1  */ {"FIRST STEPS", "warm up", 100, 48, 0, 0, 3, 0, 0, 0.8f, 0, 0, 1, {0,229,255,255}, {255,0,170,255}, 6,
   {{2,48,4,P_RAIN,2},{8,48,8,P_RING,8},{16,48,8,P_WAVE,1},{20,48,8,P_HBEAM,1},{28,48,8,P_AIM,1},{36,48,8,P_STAR,1}}},
 /* 2  */ {"NEON RAIN", "wind & rain", 110, 64, 0, 0, 5, 0, 0, 0.8f, 1, GIM_WIND, 4, {80,255,120,255}, {0,180,255,255}, 8,
   {{0,64,2,P_RAIN,3},{8,64,8,P_FAN,5},{12,64,16,P_BOUNCE,2},{16,64,8,P_VBEAM,2},{24,64,16,P_RING,10},{32,64,16,P_HOMING,1},{40,64,8,P_PULSE,1},{48,64,8,P_WAVE,2}}},
 /* 3  */ {"CROSSWIRE", "lanes & wires", 120, 72, 0, 0, 7, 0, 0, 0.8f, 2, 0, 0, {255,220,0,255}, {255,90,0,255}, 8,
   {{0,72,2,P_SIDE,2},{8,72,8,P_CROSS,1},{16,72,8,P_WALLH,0},{20,72,8,P_LANES,2},{32,72,8,P_BOMB,2},{40,72,16,P_PULSE,1},{48,72,8,P_BOMBLINE,4},{56,72,8,P_HBEAM,2}}},
 /* 4  */ {"METRONOME", "BOSS 1", 124, 96, 1, 6, 0, 5, 0, 0.8f, -1, 0, 0, {255,60,90,255}, {255,200,60,255}, 12,
   {{2,96,4,P_AIM,1},{4,40,8,P_CANNON,2},{8,96,8,P_RING,12},{16,64,16,P_SPINBEAM,1},{24,96,8,P_BOMB,2},{32,64,2,P_SPIRAL,3},{40,96,16,P_SPINBEAM,2},{48,96,16,P_PULSE,2},{56,96,8,P_CANNON,3},{64,96,8,P_WALLH,0},{72,96,8,P_BOMB,3},{80,96,16,P_HBEAM,2}}},
 /* 5  */ {"BASSLINE", "the walls squeeze", 128, 80, 0, 0, 10, 0, 0, 0.8f, 3, GIM_SQUEEZE, 5, {170,90,255,255}, {0,255,220,255}, 9,
   {{0,80,2,P_SIDE,2},{8,80,8,P_WALLH,0},{12,80,8,P_WALLV,0},{16,80,8,P_BOMB,2},{24,80,8,P_HBEAM,2},{32,80,16,P_PULSE,1},{40,80,4,P_FAN,3},{48,80,16,P_PINWHEEL,2},{56,80,8,P_LANES,2}}},
 /* 6  */ {"SPIRAL STATIC", "dizzy", 132, 88, 0, 0, 2, 0, 0, 0.65f, 4, 0, 2, {0,200,255,255}, {255,255,255,255}, 8,
   {{0,88,2,P_SPIRAL,2},{8,88,8,P_VBEAM,3},{16,88,8,P_RING,12},{24,88,16,P_PULSE,1},{32,88,8,P_BOMB,3},{48,88,8,P_WALLV,0},{56,88,8,P_STAR,2},{64,88,8,P_HOMING,1}}},
 /* 7  */ {"OVERCLOCK", "hold on", 140, 96, 0, 0, 8, 0, 0, 0.8f, 5, 0, 3, {255,120,0,255}, {255,0,100,255}, 10,
   {{0,96,2,P_RAIN,3},{4,96,4,P_FAN,5},{8,96,8,P_CROSS,1},{16,96,8,P_WALLH,0},{24,96,8,P_BOMB,3},{32,96,16,P_PULSE,1},{40,96,4,P_SPIRAL,3},{48,96,8,P_BOUNCE,2},{56,96,8,P_HBEAM,3},{64,96,8,P_WALLV,0}}},
 /* 8  */ {"AFTERBEAT", "BOSS 2", 150, 128, 1, 8, 0, 3, 0, 0.65f, -1, 0, 0, {255,0,120,255}, {130,90,255,255}, 16,
   {{2,128,4,P_AIM,1},{4,32,4,P_FAN,7},{8,128,8,P_RING,16},{16,64,16,P_SPINBEAM,2},{20,128,16,P_VBEAM,3},{24,128,8,P_BOMB,3},{32,64,2,P_SPIRAL,4},{40,128,16,P_PULSE,2},{48,96,8,P_CANNON,3},{56,128,16,P_SPINBEAM,3},{64,128,8,P_WALLH,0},{72,128,8,P_WALLV,0},{88,120,4,P_SPIRAL,3},{96,128,8,P_BOMB,4},{96,128,4,P_FAN,5},{104,128,8,P_CANNON,4}}},
 /* 9  */ {"GLITCH CITY", "static & neon", 136, 88, 0, 0, 0, 0, 0, 0.8f, 6, 0, 6, {0,255,180,255}, {255,0,255,255}, 9,
   {{0,88,2,P_SIDE,3},{8,88,8,P_HBEAM,2},{16,88,8,P_VBEAM,2},{24,88,16,P_PULSE,1},{32,88,8,P_FAN,5},{40,88,8,P_LANES,3},{48,88,8,P_CROSS,1},{56,88,4,P_AIM,1},{64,88,8,P_BOUNCE,2}}},
 /* 10 */ {"BLACKOUT", "lights out - bullets hide", 124, 80, 0, 0, 4, 0, 0, 0.8f, 7, GIM_DARK, 1, {200,200,255,255}, {90,90,220,255}, 8,
   {{0,80,4,P_BOMB,1},{8,80,8,P_WALLH,0},{12,80,8,P_WALLV,0},{16,80,8,P_BOMB,3},{24,80,8,P_HOMING,2},{32,80,16,P_PULSE,2},{40,80,8,P_HELIX,2},{48,80,8,P_CHASE,1}}},
 /* 11 */ {"TRIGRID", "BOSS 3 - triangle grid", 138, 112, 1, 3, 4, 6, 0, 0.8f, -1, 0, 0, {60,255,200,255}, {255,255,120,255}, 13,
   {{2,112,4,P_AIM,1},{4,48,8,P_TRIGRID,1},{8,48,16,P_SPINBEAM,3},{16,48,16,P_TRIGRID,2},{24,112,8,P_FAN,5},{32,112,8,P_BOMB,3},{40,112,16,P_PULSE,2},{48,112,16,P_TRIGRID,3},{52,112,16,P_SPINBEAM,3},{56,112,8,P_TRI,4},{64,112,8,P_CANNON,3},{80,112,8,P_RING,12},{88,112,8,P_BOMB,4}}},
 /* 12 */ {"SHATTER", "break it", 144, 96, 0, 0, 6, 0, 0, 0.8f, 8, 0, 2, {255,255,120,255}, {255,120,60,255}, 9,
   {{0,96,2,P_RAIN,4},{8,96,8,P_RING,14},{16,96,8,P_FAN,7},{24,96,8,P_HBEAM,3},{32,96,16,P_PULSE,2},{40,96,8,P_STAR,2},{48,96,8,P_SPIRAL,3},{56,96,8,P_BOMBLINE,5},{64,96,8,P_WALLH,0}}},
 /* 13 */ {"HYPERDRIVE", "crosswinds", 150, 104, 0, 0, 1, 0, 0, 0.8f, 9, GIM_WINDS, 3, {255,80,80,255}, {255,200,80,255}, 11,
   {{0,104,2,P_SIDE,3},{4,104,4,P_FAN,5},{8,104,8,P_CROSS,1},{16,104,8,P_WALLV,0},{24,104,8,P_BOMB,3},{32,104,8,P_HELIX,2},{40,104,8,P_WAVE,2},{48,104,16,P_PULSE,2},{56,104,8,P_HBEAM,3},{72,104,8,P_WALLH,0},{80,104,8,P_HOMING,2}}},
 /* 14 */ {"SAWMILL", "BOSS 4 - the factory", 142, 112, 1, 4, 1, 5, 105, 0.8f, -1, 0, 0, {200,205,215,255}, {255,150,40,255}, 13,
   {{2,112,4,P_SAWTHROW,1},{4,112,8,P_SAWROW,1},{8,112,8,P_SAWRAIL,2},{16,112,16,P_SPINBEAM,2},{24,112,8,P_FAN,3},{32,112,16,P_BOMB,3},{44,112,8,P_SAWROW,1},{48,112,8,P_SAWTHROW,2},{56,112,8,P_CANNON,3},{64,112,16,P_PULSE,1},{72,112,8,P_SAWRAIL,3},{88,112,8,P_BOMB,4},{96,112,16,P_SPINBEAM,3}}},
 /* 15 */ {"ZERO GRAVITY", "ice physics", 128, 96, 0, 0, 9, 0, 0, 0.8f, 10, GIM_ICE, 1, {140,120,255,255}, {80,220,255,255}, 10,
   {{0,96,2,P_RAIN,3},{8,96,8,P_SPIRAL,3},{16,96,8,P_VBEAM,3},{24,96,8,P_BOMB,3},{32,96,16,P_PULSE,2},{40,96,8,P_CHASE,1},{48,96,8,P_HELIX,3},{64,96,8,P_WALLH,0},{72,96,8,P_WALLV,0},{80,96,8,P_STAR,2}}},
 /* 16 - ULTIMATE: 4 minutes. Events are generated from the four bosses (see buildUltimate). */
 /* 16 */ {"HOLLOW", "ULTIMATE BOSS", 160, 640, 2, 3, 0, 7, 0, 0.8f, -1, 0, 0, {255,255,255,255}, {255,40,80,255}, 0, {{0}}},
};
static const int BOSSIDX[4] = {3, 7, 10, 13};   /* the four bosses that fuse into HOLLOW */

static void buildUltimate(void) {
    Level *U = &LV[ULT];
    U->nev = 0;
    for (int p = 0; p < 4; p++) {                       /* phases 1-4: each boss's own attack script, stretched to 128 beats */
        const Level *S = &LV[BOSSIDX[p]];
        int base = p * ULT_PHASE;
        for (int i = 0; i < S->nev && U->nev < 96; i++) {
            Ev n = S->ev[i];
            n.from = base + S->ev[i].from;
            n.to = (S->ev[i].to >= S->beats) ? base + ULT_PHASE : base + S->ev[i].to;
            if (n.to > base + ULT_PHASE) n.to = base + ULT_PHASE;
            U->ev[U->nev++] = n;
        }
    }
    static const Ev fin[] = {                           /* phase 5: everybody's signature moves together */
        {512,640,8,P_RING,14},{514,640,4,P_AIM,1},{516,640,4,P_SPIRAL,3},{520,640,16,P_PULSE,2},
        {524,640,16,P_TRIGRID,2},{528,640,8,P_SAWROW,1},{536,640,16,P_SAWTHROW,2},{540,640,16,P_TRI,4},
        {544,640,16,P_BOMB,4},{552,640,16,P_CROSS,1},{560,640,16,P_SPINBEAM,3},{568,640,8,P_CANNON,3}};
    for (unsigned i = 0; i < sizeof fin / sizeof fin[0] && U->nev < 96; i++) U->ev[U->nev++] = fin[i];
}

/* 16-step drum/bass/lead patterns ('x' = hit, digit = note index, '.' = rest) - one per style */
static const Style STY[8] = {
 {"x...x...x...x...", "....x.......x...", "x.x.x.x.x.x.x.x.", "..x...x...x...x.", "....x.......x...", "0..0..0.2..2..3.", "..4...5...4...3."},
 {"x.....x...x.....", "....x.......x..x", "x.x.x.x.x.x.x.xx", "......x.......x.", "................", "0..0.2..0..3.2..", "4.4.5.4.3.3.5.4."},
 {"x...x...x...x...", "....x.......x...", "x.xxx.xxx.xxx.xx", "..x...x...x...x.", "................", "0.0.0.0.2.2.3.3.", "4.5.4.3.4.5.6.7."},
 {"x.........x.....", "....x.......x...", "x.x.x.x.x.x.x.x.", "..x.......x.....", "....x.......x...", "0..0..2..3..0..5", "7.5.6.4.7.5.6.3."},
 {"x.....x...x.....", "........x.......", "x.x.xxx.x.x.xxx.", "......x.......x.", "........x.......", "0.....0.2...3...", "....4.......5..."},
 {"x..x..x...x..x..", "....x.......x...", ".x.x.x.x.x.x.x.x", "x.......x.......", "....x.......x..x", "0.0.0.2.0.0.3.5.", "5.4.3.4.5.4.3.2."},
 {"x..x....x..x....", "....x.......x...", "x.x.x.x.x.x.x.x.", ".......x.......x", "....x.......x...", "0..0..0..2..3.5.", ".4...4.5...3...."},
 {"x...x..xx...x...", "....x.......x..x", "xxxxxxxxxxxxxxxx", "..x...x...x...x.", "....x.......x...", "0.0.3.0.5.0.3.7.", "7.6.5.6.7.6.5.4."},
};
static const int SEMI[8] = {0, 3, 5, 7, 10, 12, 15, 17};


/* ---------------------------------------------------------------- SONGS --------------------------------------------------------------
 * Each standard level has its own song: a mode, a 4-bar chord progression, two drum grooves (verse / drop), a bass line, an arpeggio,
 * a 2-bar lead melody, a pad chord, and an 8-bar form:  i = intro (soft), A = verse, B = drop (full), b = breakdown (no kick/bass).
 * Bass chars: 0 root, 1 octave, 3 third, 5 fifth.  Arp chars 0-5 = chord tones.  Lead chars 0-9,a-e = scale degree.  '.' = rest. */
typedef struct { const char *kick, *snare, *hat, *ohat, *clap; } Groove;
typedef struct {
    const char *name; int mode; int prog[4]; int gA, gB;
    const char *bass, *arp, *lead, *form;
    int iBass, iArp, iLead, iPad;
} Song;
static const int SCALE[7][7] = {
    {0,2,3,5,7,8,10}, {0,2,4,5,7,9,11}, {0,2,3,5,7,9,10}, {0,1,3,5,7,8,10}, {0,2,4,6,7,9,11}, {0,2,4,5,7,9,10}, {0,2,3,5,7,8,11}};
static const Groove DRM[12] = {
 /* 0 house   */ {"x...x...x...x...", "....x.......x...", "x.x.x.x.x.x.x.x.", "..x...x...x...x.", "....x.......x..."},
 /* 1 break   */ {"x.....x...x.....", "....x.......x..x", "x.x.x.x.x.x.x.xx", "......x.......x.", "................"},
 /* 2 techno  */ {"x...x...x...x...", "....x.......x...", "x.xxx.xxx.xxx.xx", "..x...x...x...x.", "................"},
 /* 3 dnb     */ {"x.........x.....", "....x.......x...", "x.x.x.x.x.x.x.x.", "..x.......x.....", "....x.......x..."},
 /* 4 trap    */ {"x.....x...x.....", "........x.......", "x.x.xxx.x.x.xxx.", "......x.......x.", "........x......."},
 /* 5 electro */ {"x..x..x...x..x..", "....x.......x...", ".x.x.x.x.x.x.x.x", "x.......x.......", "....x.......x..x"},
 /* 6 sync    */ {"x..x....x..x....", "....x.......x...", "x.x.x.x.x.x.x.x.", ".......x.......x", "....x.......x..."},
 /* 7 stomp   */ {"x...x...x...x...", "....x...x...x..x", "x.x.x.x.x.x.x.x.", "..x...x...x...x.", "....x.......x..."},
 /* 8 shuffle */ {"x.....x..x.x....", "....x.......x...", "x.xxx.xxx.xxx.xx", "......x.......x.", "................"},
 /* 9 minimal */ {"x.......x.......", "........x.......", "..x...x...x...x.", "......x.......x.", "................"},
 /* 10 rolling*/ {"x..x..x.x..x..x.", "....x.......x...", "x.x.x.x.x.x.x.x.", "..x...x...x...x.", "....x.......x..."},
 /* 11 finale */ {"x...x..xx...x...", "....x.......x..x", "xxxxxxxxxxxxxxxx", "..x...x...x...x.", "....x.......x..."},
};
static const Song SONGS[11] = {
 /* 0  */ {"Sunrise Steps", 1, {0,4,5,3}, 9, 0, "0...0...0...0.3.", "0.2.4.2.0.2.4.2.", "4...5...7...5...4...2...4...0...", "iAAABBAb", INS_SUB,   INS_PLUCK, INS_BELL, INS_PAD},
 /* 1  */ {"Neon Rain",     2, {0,3,0,6}, 5, 2, "0.0.0.0.0.0.3.5.", "0.1.2.3.2.1.0.1.", "7.5.6.4.7.5.6.3.7.5.4.2.4.5.6...", "iAAAABBb", INS_BASSQ, INS_PLUCK, INS_LEAD, INS_PAD},
 /* 2  */ {"Crosswire",     0, {0,5,2,6}, 6, 5, "0..0..1..0..1.3.", "0.1.2.3.4.3.2.1.", "4...4.5.7...5...4...4.5.3...2...", "iAABAABb", INS_BASSW, INS_STAB,  INS_LEAD, INS_PAD},
 /* 3  */ {"Bassline",      3, {0,1,0,6}, 4, 3, "0..0....1..0.5..", "0...2...0...4...", "7...6...4...3...4...3...1...0...", "iAABBBAb", INS_BASSQ, INS_PLUCK, INS_LEAD, INS_PAD},
 /* 4  */ {"Static Spiral", 4, {0,1,4,1}, 0, 7, "0.1.0.1.0.1.0.1.", "0123210123210123", "7.......9.......8.......6.......", "iiAABBAb", INS_BASSW, INS_PLUCK, INS_LEAD, INS_PAD},
 /* 5  */ {"Overclock",     6, {0,5,3,4}, 2, 11, "0.0.0.0.0.0.0.1.", "0.1.2.1.0.1.2.1.", "7.6.7.9.7.6.4.6.7.6.4.3.4.6.7...", "iAABBBAb", INS_BASSQ, INS_PLUCK, INS_LEAD, INS_PAD},
 /* 6  */ {"Glitch City",   5, {0,6,3,4}, 1, 6, "0..0..0.1..0.3..", "0.2.0.4.0.2.0.5.", "4.4.6...4.4.3...4.4.6...9...7...", "iAABAABb", INS_BASSW, INS_BELL,  INS_LEAD, INS_PAD},
 /* 7  */ {"Blackout",      0, {0,0,5,4}, 9, 4, "0.......0.....5.", "0...1...2...1...", "4.......5.......3.......2.......", "iiAABBAb", INS_SUB,   INS_BELL,  INS_BELL, INS_PAD},
 /* 8  */ {"Shatter",       6, {0,5,6,4}, 10, 3, "0.00.0.0.0.00.1.", "0.2.4.5.4.2.0.2.", "7.9.b.9.7.6.7...9.b.d.b.9.7.6...", "iAABBBBb", INS_BASSQ, INS_STAB, INS_LEAD, INS_PAD},
 /* 9  */ {"Hyperdrive",    5, {0,6,3,4}, 0, 2, "0.0.1.0.0.0.1.0.", "0.2.4.2.0.2.4.2.", "4.6.7.6.4.6.9.7.6.7.9.7.6.4.6...", "iAABBAAB", INS_BASSW, INS_PLUCK, INS_LEAD, INS_PAD},
 /* 10 */ {"Zero Gravity",  4, {0,4,5,1}, 9, 8, "0.......1.......", "0.1.2.3.4.5.4.3.", "9...7...9...b...a...9...7.......", "iiAABBAb", INS_SUB,   INS_BELL,  INS_BELL, INS_PAD},
};

/* --------------------------------------------------------------- globals */
static int state = ST_MENU, sel = 0, cur = 0, casual = 0, hard = 0, quit = 0, audio = 0;
static unsigned char clr[2][NL];              /* [hardcore][level]: bit0 = cleared in casual, bit1 = cleared without casual */
static Player P;
static Hazard HZ[MAXH];
static Particle PT[MAXP];
static float T, bl, shake, flash, deadT, menuT, bossX, bossY, winT, bgScroll;
static int lastStep, curBeat, lastMenuBeat = -1;
static unsigned rs = 1;
static Snd drum[S_COUNT], bassS[8], leadS[8];
static int levelSounds = 0;
static float OX, OY;                          /* draw offset (screen shake / camera sway) */
static C2D_TextBuf tbuf;
static u32 kDown, kHeld;
static int cpx, cpy, touchDown, tx, ty;
/* cutscene */
static float cutT, whiteF, cutBeam[4];
static int cutFired[4], cutFused, cutTick;
/* blue ball (ultimate boss) */
static float healT, ballCD, tsc = 1.0f;
/* checkpoints */
static struct { int on; float x, y, r; } CP;
static int cpReached, cpHp; static float cpT, cpMsg; static unsigned cpRs;
static float winX0, winY0;
/* Hollow finale: eats the blue ball -> saw curtain -> crusher -> shatter -> rise -> you shoot back */
static int finPhase, finHp, finMax, eaten, reformed;
static float phT, curtainY, crushL, finFlash, hitFlash, charge, fireCD, finBX0, finBY0, px0, py0, hintT, finAng;
static struct { float x, y, w, life, max; int lvl; } BM[8];
static u32 kUp;
static int vidMode;                    /* 1 while a video level (level editor) is being played */

/* --------------------------------------------------------------- helpers */
static float grand(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return (rs & 0xFFFFFF) / 16777216.0f; }
static float gr(float a, float b) { return a + (b - a) * grand(); }                 /* gameplay RNG (deterministic) */
static float rr(float a, float b) { return a + (b - a) * ((float)rand() / (float)RAND_MAX); }   /* visual RNG */
static float dist(float a, float b, float c, float d) { return hypotf(a - c, b - d); }
static float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static Color Fade(Color c, float a) { c.a = (unsigned char)(clampf(a, 0, 1) * 255.0f); return c; }
static Color mul(Color c, float k) { Color o = {(unsigned char)(c.r * k), (unsigned char)(c.g * k), (unsigned char)(c.b * k), 255}; return o; }
static int startHP(int lvl) { int hp = 3; if (LV[lvl].boss) hp *= 2; if (casual) hp *= 2; return hp; }
static float spdMul(void) { return hard ? 1.0f : LV[cur].nspd; }
static float pScale(void) { return LV[cur].boss ? 1.8f : 1.0f; }      /* bigger particles on boss levels */
static float dashCd(void) { return hard ? 0.60f : 0.50f; }
static float beamW(void) { return hard ? 44.0f * 0.4f : 36.0f * 0.4f; }
static float beamWarn(void) { return bl * (hard ? 2.0f : 2.6f); }
static int phaseOf(int b) { int p = b / ULT_PHASE; return p < 0 ? 0 : (p > 4 ? 4 : p); }
static int inSawPhase(void) { return LV[cur].scroll > 0 || (cur == ULT && phaseOf(curBeat) == 3); }
static int inGridPhase(void) { return cur == 10 || (cur == ULT && phaseOf(curBeat) == 2); }
static int inRect(float px, float py, float x, float y, float w, float h) { return px >= x && px < x + w && py >= y && py < y + h; }
static float segDist(float px, float py, float x1, float y1, float x2, float y2) {
    float dx = x2 - x1, dy = y2 - y1, l2 = dx * dx + dy * dy;
    float t = l2 > 0 ? ((px - x1) * dx + (py - y1) * dy) / l2 : 0;
    t = clampf(t, 0, 1);
    return dist(px, py, x1 + dx * t, y1 + dy * t);
}

/* Normal mode = an easier version of each Hardcore script: sparser, smaller volleys, bigger gaps.
   SPIRAL STATIC and AFTERBEAT (nspd < 0.7) get an extra layer of easing. */
static Ev adjEv(const Ev *e) {
    Ev o = *e;
    if (hard) return o;
    int soft = LV[cur].nspd < 0.7f;
    switch (o.pat) {
    case P_RAIN: case P_SIDE:
        if (o.arg > 1) o.arg--;
        if (soft && o.arg > 1) o.arg--;
        break;
    case P_AIM: o.every *= 2; break;
    case P_FAN: o.every *= 2; if (o.arg > 3) o.arg -= 2; if (soft && o.arg > 3) o.arg--; break;
    case P_SPIRAL: case P_HELIX: case P_TRI:
        if (o.every <= 4) o.every *= 2;
        if (soft && o.every <= 8) o.every *= 2;
        if (o.arg > 1) o.arg--;
        break;
    case P_RING: o.arg = o.arg * 3 / 4; if (o.arg < 6) o.arg = 6; if (soft) { o.arg = o.arg * 3 / 4; if (o.arg < 6) o.arg = 6; } break;
    case P_BOMB: case P_HBEAM: case P_VBEAM: case P_PULSE: case P_TRIGRID:
        if (o.arg > 1) o.arg--;
        if (soft && o.arg > 1) o.arg--;
        break;
    case P_SPINBEAM: if (o.arg > 1) o.arg--; if (soft) o.every = o.every * 3 / 2; break;
    case P_CANNON: if (o.arg > 1) o.arg--; if (soft) o.every = o.every * 3 / 2; break;
    case P_SAWROW: o.arg++; o.every = o.every * 3 / 2; break;
    case P_SAWRAIL: if (o.arg > 1) o.arg--; break;
    case P_SAWTHROW: if (o.every <= 4) o.every *= 2; break;
    case P_BOUNCE: case P_HOMING: case P_CHASE: case P_WAVE: case P_STAR:
        if (o.arg > 1) o.arg--;
        if (soft && o.arg > 1) o.arg--;
        break;
    case P_LANES: if (o.arg > 1) o.arg--; o.every = o.every * 3 / 2; break;
    case P_BOMBLINE: if (o.arg > 3) o.arg--; break;
    case P_PINWHEEL: if (o.arg > 2) o.arg--; break;
    default: break;
    }
    return o;
}

/* ------------------------------------------------------------ 2D drawing */
static u32 U(Color c) { return C2D_Color32(c.r, c.g, c.b, c.a); }
static void dRect(float x, float y, float w, float h, Color c) {
    if (c.a == 0 || w <= 0 || h <= 0) return;
    C2D_DrawRectSolid(x + OX, y + OY, 0, w, h, U(c));
}
static void dRectLines(float x, float y, float w, float h, float t, Color c) {
    dRect(x, y, w, t, c); dRect(x, y + h - t, w, t, c);
    dRect(x, y + t, t, h - 2 * t, c); dRect(x + w - t, y + t, t, h - 2 * t, c);
}
static void dCircle(float x, float y, float r, Color c) {
    if (c.a == 0 || r <= 0) return;
    C2D_DrawCircleSolid(x + OX, y + OY, 0, r, U(c));
}
static void dLine(float x0, float y0, float x1, float y1, float t, Color c) {
    u32 k = U(c);
    C2D_DrawLine(x0 + OX, y0 + OY, k, x1 + OX, y1 + OY, k, t, 0);
}
static void dCircleLines(float x, float y, float r, float t, Color c) {
    if (r <= 0.5f) return;
    int n = 10 + (int)(r * 0.15f); if (n > 56) n = 56;
    float px = x + r, py = y;
    for (int i = 1; i <= n; i++) {
        float a = i * PI2 / n, nx = x + cosf(a) * r, ny = y + sinf(a) * r;
        dLine(px, py, nx, ny, t, c);
        px = nx; py = ny;
    }
}
static void dPolyLines(float x, float y, int sides, float r, float rotDeg, float t, Color c) {
    float rot = rotDeg * PIF / 180.0f;
    float px = x + cosf(rot) * r, py = y + sinf(rot) * r;
    for (int i = 1; i <= sides; i++) {
        float a = rot + i * PI2 / sides, nx = x + cosf(a) * r, ny = y + sinf(a) * r;
        dLine(px, py, nx, ny, t, c);
        px = nx; py = ny;
    }
}
static void dPolyFill(float x, float y, int sides, float r, float rotDeg, Color c) {
    float rot = rotDeg * PIF / 180.0f;
    u32 k = U(c);
    for (int i = 0; i < sides; i++) {
        float a0 = rot + i * PI2 / sides, a1 = rot + (i + 1) * PI2 / sides;
        C2D_DrawTriangle(x + OX, y + OY, k, x + OX + cosf(a0) * r, y + OY + sinf(a0) * r, k,
                         x + OX + cosf(a1) * r, y + OY + sinf(a1) * r, k, 0);
    }
}
static void dSaw(float x, float y, float r, float rot, Color c) {          /* a spinning saw blade */
    dCircle(x, y, r * 0.78f, c);
    u32 k = U(c);
    for (int i = 0; i < 8; i++) {
        float a = rot + i * PI2 / 8;
        C2D_DrawTriangle(x + OX + cosf(a) * r * 0.7f, y + OY + sinf(a) * r * 0.7f, k,
                         x + OX + cosf(a + 0.42f) * r * 0.7f, y + OY + sinf(a + 0.42f) * r * 0.7f, k,
                         x + OX + cosf(a + 0.1f) * r * 1.12f, y + OY + sinf(a + 0.1f) * r * 1.12f, k, 0);
    }
    dCircle(x, y, r * 0.32f, (Color){20, 20, 26, 255});
    dRect(x - 1, y - 1, 2, 2, WHITE);
}
static void dTri(float x0, float y0, float x1, float y1, float x2, float y2, Color c) {
    u32 k = U(c);
    C2D_DrawTriangle(x0 + OX, y0 + OY, k, x1 + OX, y1 + OY, k, x2 + OX, y2 + OY, k, 0);
}
static void dGradV(float x, float y, float w, float h, Color top, Color bot) {
    C2D_DrawRectangle(x, y, 0, w, h, U(top), U(top), U(bot), U(bot));
}
static void text(float x, float y, float sc, Color c, const char *s) {
    C2D_Text t;
    C2D_TextParse(&t, tbuf, s);
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor, x + OX, y + OY, 0, sc, sc, U(c));
}
static float textW(const char *s, float sc) {
    C2D_Text t; float w, h;
    C2D_TextParse(&t, tbuf, s);
    C2D_TextGetDimensions(&t, sc, sc, &w, &h);
    return w;
}
static void textC(float cx, float y, float sc, Color c, const char *s) { text(cx - textW(s, sc) * 0.5f, y, sc, c, s); }

/* ------------------------------------------------------- audio synthesis */
static Snd synth(int kind, float f, float dur) {
    Snd out = {NULL, 0};
    int n = (int)(SR * dur);
    s16 *d = (s16 *)linearAlloc(n * sizeof(s16));
    if (!d) return out;
    float ph = 0, prev = 0;
    for (int i = 0; i < n; i++) {
        float t = (float)i / SR, s = 0, nz = rr(-1, 1);
        switch (kind) {
        case S_KICK:  ph += PI2 * (40 + 130 * expf(-t * 26)) / SR; s = sinf(ph) * expf(-t * 7.5f) * 1.25f; break;
        case S_HAT:   s = (nz - prev) * expf(-t * 80) * 0.7f; prev = nz; break;
        case S_OHAT:  s = (nz - prev) * expf(-t * 12) * 0.5f; prev = nz; break;
        case S_SNARE: s = nz * expf(-t * 20) * 0.6f + sinf(PI2 * 185 * t) * expf(-t * 25) * 0.5f; break;
        case S_CLAP:  { float g = (t < 0.03f ? expf(-fmodf(t, 0.01f) * 300) : 0) + expf(-t * 26) * 0.5f; s = nz * g * 0.6f; } break;
        case S_HIT:   ph += PI2 * (30 + 160 * expf(-t * 10)) / SR; s = nz * expf(-t * 12) * 0.7f + sinf(ph) * expf(-t * 6); break;
        case S_DASH:  s = (nz - prev) * expf(-t * 14) * 0.9f * (1.0f - t / dur); prev = nz; break;
        case S_CLEAR: { float nf = 523.25f * (1.0f + 0.25f * (int)(t * 8)); float lt = fmodf(t, 0.125f);
                        s = sinf(PI2 * nf * t) * expf(-lt * 9) * 0.6f; } break;
        case K_BASS:  ph += PI2 * f / SR; s = (sinf(ph) + 0.4f * sinf(2 * ph) + 0.25f * sinf(3 * ph)) * expf(-t * 4.5f) * 0.6f; break;
        case K_LEAD:  ph += PI2 * f / SR; s = (sinf(ph) > 0 ? 0.32f : -0.32f) * expf(-t * 9); break;
        }
        d[i] = (s16)(clampf(s, -1, 1) * 30000);
    }
    DSP_FlushDataCache(d, n * sizeof(s16));
    out.d = d; out.n = n;
    return out;
}
static ndspWaveBuf wbuf[VOICES];
static int nextVoice = 0;
static void playCh(int ch, Snd s, float v) {
    if (!audio || !s.d) return;
    float mix[12]; memset(mix, 0, sizeof mix);
    mix[0] = mix[1] = v;
    ndspChnWaveBufClear(ch);
    ndspChnSetMix(ch, mix);
    memset(&wbuf[ch], 0, sizeof(ndspWaveBuf));
    wbuf[ch].data_vaddr = s.d;
    wbuf[ch].nsamples = s.n;
    ndspChnWaveBufAdd(ch, &wbuf[ch]);
}
static void play(Snd s, float v) {                 /* shared voices: PADCH .. VOICES-1 */
    int ch = PADCH + nextVoice; nextVoice = (nextVoice + 1) % (VOICES - PADCH);
    playCh(ch, s, v);
}
static int padVoice = 0;
static void playPad(Snd s, float v) {              /* the sustained chord gets its own channels so it is never cut off */
    int ch = padVoice; padVoice = (padVoice + 1) % PADCH;
    playCh(ch, s, v);
}
static void makeDrums(void) {
    drum[S_KICK] = synth(S_KICK, 0, 0.3f);   drum[S_HAT] = synth(S_HAT, 0, 0.07f);
    drum[S_SNARE] = synth(S_SNARE, 0, 0.2f); drum[S_HIT] = synth(S_HIT, 0, 0.45f);
    drum[S_DASH] = synth(S_DASH, 0, 0.18f);  drum[S_CLEAR] = synth(S_CLEAR, 0, 0.6f);
    drum[S_OHAT] = synth(S_OHAT, 0, 0.22f);  drum[S_CLAP] = synth(S_CLAP, 0, 0.2f);
}
static void stopVoices(void) { for (int i = 0; i < VOICES; i++) ndspChnWaveBufClear(i); }

/* ---- melodic instruments (fast table-lookup synth) ---- */
static float SINT[4096];
static void initTables(void) { for (int i = 0; i < 4096; i++) SINT[i] = sinf(PI2 * (float)i / 4096.0f); }
static float sinT(float cyc) { return SINT[(int)(cyc * 4096.0f) & 4095]; }
static Snd NOTE[INS_COUNT][72];
static int lazyNotes = 0, builtLvl = -1;
static Snd synthNote(int ins, float f) {
    static const float DUR[INS_COUNT] = {0.40f, 0.30f, 0.50f, 0.35f, 1.20f, 0.60f, 0.30f, 0.18f};
    Snd out = {NULL, 0};
    float dur = DUR[ins];
    int n = (int)(SR * dur);
    s16 *d = (s16 *)linearAlloc(n * sizeof(s16));
    if (!d) return out;
    float ph = 0, ph2 = 0, ph3 = 0, dp = f / SR, e = 1, e2 = 1, e3 = 1, e4 = 1, dec = 1, dec2 = 1, dec3 = 1, dec4 = 1;
    switch (ins) {
    case INS_BASSW: dec = expf(-4.5f / SR); break;
    case INS_BASSQ: dec = expf(-6.0f / SR); break;
    case INS_SUB:   dec = expf(-3.0f / SR); break;
    case INS_PLUCK: dec = expf(-7.0f / SR); dec2 = expf(-10.0f / SR); dec3 = expf(-14.0f / SR); dec4 = expf(-18.0f / SR); break;
    case INS_BELL:  dec = expf(-5.0f / SR); break;
    case INS_LEAD:  dec = expf(-6.0f / SR); break;
    case INS_STAB:  dec = expf(-14.0f / SR); break;
    default: break;
    }
    for (int i = 0; i < n; i++) {
        float t = (float)i / SR, s = 0, a = t < 0.004f ? t / 0.004f : 1.0f;
        switch (ins) {
        case INS_BASSW: s = (sinT(ph) + 0.5f * sinT(2 * ph) + 0.25f * sinT(3 * ph)) * 0.6f * e; break;
        case INS_BASSQ: s = ((ph < 0.5f ? 0.3f : -0.3f) + 0.4f * sinT(ph)) * e; break;
        case INS_SUB:   s = (sinT(ph) * 0.9f + 0.15f * sinT(2 * ph) * e * e) * e; break;
        case INS_PLUCK: s = 0.55f * (sinT(ph) * e + 0.5f * sinT(2 * ph) * e2 + 0.33f * sinT(3 * ph) * e3 + 0.25f * sinT(4 * ph) * e4); break;
        case INS_PAD: {
            float att = t / 0.14f; if (att > 1) att = 1;
            float rel = (dur - t) / 0.3f; if (rel > 1) rel = 1;
            s = (sinT(ph) + sinT(ph2) + sinT(ph3) + 0.3f * sinT(2 * ph)) * 0.22f * att * rel;
        } break;
        case INS_BELL:  s = sinT(ph + 2.2f * sinT(ph * 3.5f)) * e * 0.55f; break;
        case INS_LEAD:  s = ((ph < 0.5f ? 0.22f : -0.22f) + (2 * ph - 1) * 0.22f) * e; break;
        case INS_STAB:  s = (2 * ph - 1) * 0.5f * e; break;
        default: break;
        }
        d[i] = (s16)(clampf(s * a, -1, 1) * 28000);
        ph += dp; if (ph >= 1) ph -= 1;
        ph2 += dp * 1.006f; if (ph2 >= 1) ph2 -= 1;
        ph3 += dp * 0.994f; if (ph3 >= 1) ph3 -= 1;
        e *= dec; e2 *= dec2; e3 *= dec3; e4 *= dec4;
    }
    DSP_FlushDataCache(d, n * sizeof(s16));
    out.d = d; out.n = n;
    return out;
}
static Snd getNote(int ins, int n) {
    if (n < 0) n = 0;
    if (n > 71) n = 71;
    if (!NOTE[ins][n].d) { NOTE[ins][n] = synthNote(ins, 55.0f * powf(2.0f, n / 12.0f)); lazyNotes++; }
    return NOTE[ins][n];
}
static void freeNotes(void) {
    for (int i = 0; i < INS_COUNT; i++) for (int n = 0; n < 72; n++) if (NOTE[i][n].d) { linearFree(NOTE[i][n].d); NOTE[i][n].d = NULL; NOTE[i][n].n = 0; }
}

/* ---- note selection (shared by the scanner that pre-renders notes and by playback) ---- */
static int degSemi(int mode, int deg, int oct, int root) { return root + SCALE[mode][deg % 7] + 12 * (oct + deg / 7); }
static int hexv(char c) { if (c >= '0' && c <= '9') return c - '0'; if (c >= 'a' && c <= 'e') return c - 'a' + 10; return -1; }
static int songNote(const Song *S, int role, int bar, int s, int root, int *ins) {       /* role 0 bass, 1 arp, 2 lead; -1 = rest */
    static const int ARPD[6] = {0, 2, 4, 7, 9, 11};
    int chord = S->prog[bar & 3];
    if (role == 0) {
        char c = S->bass[s]; int d;
        switch (c) { case '0': d = chord; break; case '1': d = chord + 7; break; case '3': d = chord + 2; break; case '5': d = chord + 4; break; default: return -1; }
        *ins = S->iBass; return degSemi(S->mode, d, 0, root);
    }
    if (role == 1) {
        char c = S->arp[s];
        if (c < '0' || c > '5') return -1;
        *ins = S->iArp; return degSemi(S->mode, chord + ARPD[c - '0'], 2, root);
    }
    int d = hexv(S->lead[((bar & 1) << 4) + s]);
    if (d < 0) return -1;
    *ins = S->iLead; return degSemi(S->mode, d, 2, root);
}
static int padNote(const Song *S, int bar, int k, int root) { return degSemi(S->mode, S->prog[bar & 3] + 2 * k, 1, root); }
static void prepareSong(int lvl) {                       /* render every note the song will use, so playback never hitches */
    const Level *L = &LV[lvl];
    if (L->song < 0) return;
    const Song *S = &SONGS[L->song];
    static unsigned char need[INS_COUNT][72];
    memset(need, 0, sizeof need);
    for (int bar = 0; bar < 4; bar++) {
        for (int st = 0; st < 16; st++)
            for (int role = 0; role < 3; role++) { int ins = 0, n = songNote(S, role, bar, st, L->root, &ins); if (n >= 0) need[ins][n] = 1; }
        for (int k = 0; k < 3; k++) need[S->iPad][padNote(S, bar, k, L->root)] = 1;
    }
    for (int i = 0; i < INS_COUNT; i++) for (int n = 0; n < 72; n++) if (need[i][n]) getNote(i, n);
}

static void freeLevelSounds(void) {
    if (!audio || !levelSounds) return;
    stopVoices();
    for (int i = 0; i < 8; i++) { linearFree(bassS[i].d); linearFree(leadS[i].d); bassS[i].d = leadS[i].d = NULL; }
    freeNotes();
    levelSounds = 0; builtLvl = -1;
}
static void buildLevelSounds(int lvl) {
    if (!audio) return;
    if (levelSounds && builtLvl == lvl) return;           /* retrying the same level: sounds are already rendered */
    freeLevelSounds();
    if (LV[lvl].song >= 0) prepareSong(lvl);
    else {
        float root = 55.0f * powf(2.0f, LV[lvl].root / 12.0f);
        for (int i = 0; i < 8; i++) {
            float f = root * powf(2.0f, SEMI[i] / 12.0f);
            bassS[i] = synth(K_BASS, f, 0.4f);
            leadS[i] = synth(K_LEAD, f * 4.0f, 0.25f);
        }
    }
    levelSounds = 1; builtLvl = lvl;
}

/* ------------------------------------------------------------- particles */
static void emit(int kind, float x, float y, float vx, float vy, float life, float size, Color c, float grow, float spin) {
    static int idx = 0;
    for (int k = 0; k < MAXP; k++) {
        int i = (idx + k) % MAXP;
        if (PT[i].life <= 0) {
            Particle p = {x, y, vx, vy, life, life, size, grow, 0, spin, c, kind};
            PT[i] = p;
            idx = i + 1;
            return;
        }
    }
}
/* kind: 0 dot, 1 spark, 2 expanding ring, 3 spinning square.  speeds/sizes given in "PC units", scaled here */
static void burst(float x, float y, int n, float smin, float smax, int kind, Color c, float life, float size) {
    for (int i = 0; i < n; i++) {
        float a = rr(0, PI2), s = rr(smin, smax) * 0.42f;
        emit(kind, x, y, cosf(a) * s, sinf(a) * s, rr(life * 0.6f, life), size * 0.55f * pScale() * rr(0.6f, 1.2f), c, 0, rr(-500, 500));
    }
}
static void stepParticles(float dt) {
    for (int i = 0; i < MAXP; i++) {
        Particle *p = &PT[i];
        if (p->life <= 0) continue;
        p->life -= dt;
        p->x += p->vx * dt; p->y += p->vy * dt;
        if (p->kind == 1 || p->kind == 3) { float d = 1.0f - 2.2f * dt; p->vx *= d; p->vy *= d; }
        p->rot += p->spin * dt;
    }
}
static void drawParticles(void) {
    for (int i = 0; i < MAXP; i++) {
        Particle *p = &PT[i];
        if (p->life <= 0) continue;
        float k = p->life / p->max;
        Color c = Fade(p->c, clampf(k * 1.4f, 0, 1));
        switch (p->kind) {
        case 0: { float s = p->size * (0.3f + 0.7f * k) * 1.6f; dRect(p->x - s / 2, p->y - s / 2, s, s, c); } break;
        case 1: dLine(p->x, p->y, p->x - p->vx * 0.05f, p->y - p->vy * 0.05f, 1.0f, c); break;
        case 2: dCircleLines(p->x, p->y, p->size + p->grow * (p->max - p->life), 1.0f, c); break;
        case 3: { float s = p->size * (0.4f + 0.6f * k); dPolyFill(p->x, p->y, 4, s, p->rot, c); } break;
        }
    }
}

/* --------------------------------------------------------------- hazards */
static Hazard *nh(void) {
    for (int i = 0; i < MAXH; i++)
        if (HZ[i].type == H_NONE) { memset(&HZ[i], 0, sizeof(Hazard)); return &HZ[i]; }
    return NULL;
}
/* flag: 0 plain, 1 homing (steers toward you for 'life' seconds), 3 sine-wave.  split = wall bounces left. */
static Hazard *bulletH(float x, float y, float ang, float sp, float r, Color c, int flag, float life, int bounces) {
    Hazard *h = nh(); if (!h) return NULL;
    if (!hard) { sp *= LV[cur].nspd; r *= 0.85f; }
    h->type = H_BULLET; h->x = x; h->y = y; h->vx = cosf(ang) * sp; h->vy = sinf(ang) * sp; h->r = r; h->c = c;
    h->flag = flag; h->life = life; h->split = bounces;
    return h;
}
static void bullet(float x, float y, float ang, float sp, float r, Color c) { bulletH(x, y, ang, sp, r, c, 0, 0, 0); }
static void rect(float x, float y, float w, float hh, float vx, float vy, float warn, float life, int wall, Color c) {
    if (w <= 0 || hh <= 0) return;
    Hazard *h = nh(); if (!h) return;
    h->type = H_RECT; h->x = x; h->y = y; h->w = w; h->h = hh; h->vx = vx; h->vy = vy;
    h->warn = warn; h->thick = warn; h->life = life; h->flag = wall; h->c = c;
}
static void ring(float x, float y, float warn, float vr, float thick, float maxR, Color c) {
    Hazard *h = nh(); if (!h) return;
    h->type = H_RING; h->x = x; h->y = y; h->warn = warn; h->life = warn; h->vr = vr; h->thick = thick; h->w = maxR; h->c = c;
}
static void bomb(float x, float y, float r, float fuse, int split, int dmg, Color c) {
    Hazard *h = nh(); if (!h) return;
    h->type = H_BOMB; h->x = x; h->y = y; h->r = r; h->warn = fuse; h->life = fuse; h->split = split; h->dmg = dmg; h->c = c;
}
static void line(float x1, float y1, float x2, float y2, float thick, float warn, float life, Color c) {
    Hazard *h = nh(); if (!h) return;
    h->type = H_LINE; h->x = x1; h->y = y1; h->x2 = x2; h->y2 = y2; h->thick = thick;
    h->warn = warn; h->vr = warn; h->life = life; h->c = c;
}
static void saw(float x, float y, float r, float vx, float vy, int rail, float amp, float ph, float w, Color c) {
    Hazard *h = nh(); if (!h) return;
    h->type = H_SAW; h->x = x; h->y = y; h->r = r; h->vx = vx; h->vy = vy; h->flag = rail;
    h->y0 = y; h->amp = amp; h->ph = ph; h->vr = w; h->c = c;
}

static void spin(float x, float y, int arms, float ang, float w, float len, float thick, float warn, float life, int follow, Color c) {
    Hazard *h = nh(); if (!h) return;                 /* spinning beams: 'arms' rays rotating around a pivot */
    h->type = H_SPIN; h->x = x; h->y = y; h->split = arms; h->rot = ang; h->vr = w; h->r = len;
    h->thick = thick; h->warn = warn; h->w = warn; h->life = life; h->flag = follow; h->c = c;
}
static void cannonEx(float x, float y, float ang, float warn, int shots, Color c, int locked) {
    Hazard *h = nh(); if (!h) return;                 /* wall cannon: locks on, then fires a rapid burst (locked = fixed direction) */
    h->type = H_CANNON; h->x = x; h->y = y; h->rot = ang; h->warn = warn; h->w = warn; h->split = shots; h->life = 0; h->c = c; h->flag = locked;
}
static void cannon(float x, float y, float ang, float warn, int shots, Color c) { cannonEx(x, y, ang, warn, shots, c, 0); }

static void origin(float *ox, float *oy) {
    if (LV[cur].boss) { *ox = bossX; *oy = bossY; return; }
    for (int k = 0; k < 8; k++) {
        *ox = gr(60, SW - 60); *oy = gr(50, SH * 0.6f);
        if (dist(*ox, *oy, P.x, P.y) > 90) return;
    }
}

static void runPat(int pat, int arg, int b) {
    const Level *L = &LV[cur];
    const Level *CL = (L->boss == 2 && phaseOf(b) < 4) ? &LV[BOSSIDX[phaseOf(b)]] : L;   /* ultimate wears each boss's colours */
    float ox, oy; origin(&ox, &oy);
    Color a = CL->c1, c2 = CL->c2;
    float aim = atan2f(P.y - oy, P.x - ox);
    float bw = beamW(), warn = beamWarn();
    switch (pat) {
    case P_RAIN:
        for (int i = 0; i < arg; i++) bullet(gr(8, SW - 8), -5, PIF / 2 + gr(-0.12f, 0.12f), gr(90, 135), 3, (i & 1) ? a : c2);
        break;
    case P_SIDE:
        for (int i = 0; i < arg; i++) {
            int left = grand() < 0.5f;
            bullet(left ? -5 : SW + 5, gr(12, SH - 12), left ? 0.0f : PIF, gr(95, 135), 3, (i & 1) ? a : c2);
        }
        break;
    case P_RING:
        if (L->boss) {
            float o = b * 0.21f;
            for (int i = 0; i < arg; i++) bullet(ox, oy, o + i * PI2 / arg, 85, 3.5f, (i & 1) ? a : c2);
            burst(ox, oy, 14, 80, 220, 1, c2, 0.4f, 4);
        } else bomb(ox, oy, 10, bl * (hard ? 1.0f : 1.3f), arg, 0, a);
        break;
    case P_AIM: bullet(ox, oy, aim, 140, 4, c2); break;
    case P_FAN:
        for (int i = 0; i < arg; i++) bullet(ox, oy, aim + (i - (arg - 1) * 0.5f) * 0.22f, 100, 3, (i & 1) ? a : c2);
        break;
    case P_HBEAM:
        for (int i = 0; i < arg; i++) {
            float y = i == 0 ? clampf(P.y + gr(-12, 12), 18, SH - 18) : gr(26, SH - 26);
            rect(0, y - bw / 2, SW, bw, 0, 0, warn, bl * 0.55f, 0, (i & 1) ? c2 : a);
        }
        break;
    case P_VBEAM:
        for (int i = 0; i < arg; i++) {
            float x = i == 0 ? clampf(P.x + gr(-12, 12), 18, SW - 18) : gr(26, SW - 26);
            rect(x - bw / 2, 0, bw, SH, 0, 0, warn, bl * 0.55f, 0, (i & 1) ? c2 : a);
        }
        break;
    case P_CROSS:
        rect(0, P.y - bw / 2, SW, bw, 0, 0, warn, bl * 0.55f, 0, a);
        rect(P.x - bw / 2, 0, bw, SH, 0, 0, warn, bl * 0.55f, 0, c2);
        break;
    case P_WALLH: {
        float gap = hard ? 80 : 104, gx = gr(gap / 2 + 18, SW - gap / 2 - 18), sp = hard ? 105 : 85;
        int bot = grand() < 0.5f; float y0 = bot ? SH : -14, vy = bot ? -sp : sp;
        rect(0, y0, gx - gap / 2, 14, 0, vy, 0, 0, 1, a);
        rect(gx + gap / 2, y0, SW - (gx + gap / 2), 14, 0, vy, 0, 0, 1, a);
    } break;
    case P_WALLV: {
        float gap = hard ? 72 : 96, gy = gr(gap / 2 + 18, SH - gap / 2 - 18), sp = hard ? 105 : 85;
        int right = grand() < 0.5f; float x0 = right ? SW : -14, vx = right ? -sp : sp;
        rect(x0, 0, 14, gy - gap / 2, vx, 0, 0, 0, 1, c2);
        rect(x0, gy + gap / 2, 14, SH - (gy + gap / 2), vx, 0, 0, 0, 1, c2);
    } break;
    case P_BOMB:
        for (int i = 0; i < arg; i++) {
            float x = i == 0 ? clampf(P.x + gr(-25, 25), 38, SW - 38) : gr(38, SW - 38);
            float y = i == 0 ? clampf(P.y + gr(-25, 25), 38, SH - 38) : gr(38, SH - 38);
            bomb(x, y, (hard ? 34 : 30) + (L->boss ? 8 : 0), bl * (hard ? 2.0f : 2.6f), L->boss ? 14 : 10, 1, (i & 1) ? c2 : a);
        }
        break;
    case P_SPIRAL: {
        float base = b * 0.5f;
        for (int k = 0; k < arg; k++)
            for (int j = 0; j < 5; j++)
                bullet(ox, oy, base + k * PI2 / arg + j * 0.28f, 65 + j * 12, 3, (j & 1) ? a : c2);
    } break;
    case P_HELIX: {                          /* two counter-rotating spiral arms */
        float base = b * 0.45f;
        for (int j = 0; j < arg * 3; j++) {
            float sp = 65 + j * 8;
            bullet(ox, oy, base + j * 0.32f, sp, 3, a);
            bullet(ox, oy, PIF - base - j * 0.32f, sp, 3, c2);
        }
    } break;
    case P_TRI: {                            /* three straight spikes forming a triangle */
        float base = b * 0.35f;
        for (int k = 0; k < 3; k++)
            for (int j = 0; j < arg; j++)
                bullet(ox, oy, base + k * PI2 / 3, 55 + j * 16, 3.5f, (j & 1) ? a : c2);
    } break;
    case P_TRIGRID: {                        /* a triangular lattice of beams, one family of parallel lines per beat */
        float spc = hard ? 58 : 68, th = hard ? 9 : 8;
        int base = (b / 8) % 3;
        for (int k = 0; k < arg && k < 3; k++) {
            int f = (base + k) % 3;
            float ang = f * PIF / 3, dx = cosf(ang), dy = sinf(ang), nx = -dy, ny = dx, w = warn + k * bl;
            for (float d = -260; d <= 260; d += spc) {
                float cx = SW / 2 + nx * d, cy = SH / 2 + ny * d;
                line(cx - dx * 600, cy - dy * 600, cx + dx * 600, cy + dy * 600, th, w, bl * 0.5f, (k & 1) ? c2 : a);
            }
        }
    } break;
    case P_SAWROW: {                         /* a wall of saws rolling in from the right, with a gap */
        int n = 7, gap = arg < 1 ? 1 : arg, g = (int)gr(0, (float)(n - gap + 1));
        if (g > n - gap) g = n - gap;
        for (int i = 0; i < n; i++) if (i < g || i >= g + gap) saw(SW + 16, 18 + i * 36, 14, -105 * spdMul(), 0, 0, 0, 0, 0, (Color){190, 195, 210, 255});
    } break;
    case P_SAWRAIL:                          /* saws that bob up and down as they roll in */
        for (int i = 0; i < arg; i++) {
            float y0 = gr(70, SH - 70), amp = gr(35, 60);
            saw(SW + 16 + i * 70, y0, 13, -105 * spdMul(), 0, 1, amp, gr(0, PI2), 2.4f, (Color){190, 195, 210, 255});
        }
        break;
    case P_SAWTHROW:                         /* the boss hurls saws at you */
        for (int i = 0; i < arg; i++)
            saw(ox, oy, 10, cosf(aim + (i - (arg - 1) * 0.5f) * 0.25f) * 150 * spdMul(), sinf(aim + (i - (arg - 1) * 0.5f) * 0.25f) * 150 * spdMul(),
                0, 0, 0, 0, c2);
        break;
    case P_SPINBEAM: {                       /* rotating laser arms, alternating direction */
        int arms = arg < 1 ? 1 : (arg > 4 ? 4 : arg);
        float w = (hard ? 1.2f : 0.85f) * (((b / 8) & 1) ? 1.0f : -1.0f);
        spin(ox, oy, arms, gr(0, PI2), w, 420, beamW() * 0.85f, warn, bl * 8, L->boss ? 1 : 0, (b & 8) ? c2 : a);
    } break;
    case P_CANNON:                           /* cannons on the walls aim at you and fire a burst */
        for (int i = 0; i < arg; i++) {
            int side = ((int)gr(0, 4) + i) % 4; float x, y;
            if (side == 0) { x = gr(30, SW - 30); y = 6; } else if (side == 1) { x = SW - 6; y = gr(30, SH - 30); }
            else if (side == 2) { x = gr(30, SW - 30); y = SH - 6; } else { x = 6; y = gr(30, SH - 30); }
            cannon(x, y, atan2f(P.y - y, P.x - x), bl * (hard ? 1.5f : 2.0f), hard ? 6 : 4, (i & 1) ? c2 : a);
        }
        break;
    case P_BOUNCE:                           /* bullets that ricochet off the walls three times */
        for (int i = 0; i < arg; i++) {
            float x, y, ang; int side = (int)gr(0, 4) % 4;
            if (side == 0) { x = gr(20, SW - 20); y = 4; ang = gr(0.6f, PIF - 0.6f); }
            else if (side == 1) { x = SW - 4; y = gr(20, SH - 20); ang = gr(PIF - 0.9f, PIF + 0.9f); }
            else if (side == 2) { x = gr(20, SW - 20); y = SH - 4; ang = -gr(0.6f, PIF - 0.6f); }
            else { x = 4; y = gr(20, SH - 20); ang = gr(-0.9f, 0.9f); }
            bulletH(x, y, ang, 105, 4, (i & 1) ? a : c2, 0, 0, 3);
        }
        break;
    case P_HOMING:                           /* slow bullets that curve toward you for a few seconds */
        for (int i = 0; i < arg; i++) bulletH(ox, oy, aim + gr(-0.7f, 0.7f), 70, 4, (i & 1) ? a : c2, 1, 3.0f, 0);
        break;
    case P_CHASE:                            /* a big slow orb that hunts you */
        for (int i = 0; i < arg; i++) bulletH(ox, oy, aim + (i - (arg - 1) * 0.5f) * 0.6f, 42, 9, c2, 1, 6.0f, 0);
        break;
    case P_WAVE:                             /* a snake of bullets weaving across the screen */
        for (int wv = 0; wv < arg; wv++) {
            int left = grand() < 0.5f;
            float y = gr(40, SH - 40), dir = left ? 1.0f : -1.0f, ph0 = gr(0, PI2);
            for (int i = 0; i < 7; i++) {
                Hazard *h = bulletH((left ? -6.0f : SW + 6.0f) - dir * 22 * i, y, left ? 0.0f : PIF, 90, 3.5f, (i & 1) ? a : c2, 3, 0, 0);
                if (h) { h->x2 = 0; h->y2 = 1; h->amp = hard ? 70.0f : 56.0f; h->vr = 3.0f; h->ph = ph0 - 3.0f * (22.0f * i / fabsf(h->vx)); }
            }
        }
        break;
    case P_LANES: {                          /* rhythm lanes: rows flash, then a train of bullets shoots along each */
        static const float LY[5] = {30, 75, 120, 165, 210};
        int used = 0;
        for (int i = 0; i < arg && i < 5; i++) {
            int lane = (int)gr(0, 5) % 5, tries = 0;
            while (((used >> lane) & 1) && tries++ < 8) lane = (lane + 1) % 5;
            used |= 1 << lane;
            int fromLeft = (i & 1) == 0;
            cannonEx(fromLeft ? 6 : SW - 6, LY[lane], fromLeft ? 0.0f : PIF, bl * (hard ? 1.5f : 2.0f) + i * bl * 0.5f, 8, (i & 1) ? c2 : a, 1);
        }
    } break;
    case P_BOMBLINE: {                       /* a row of bombs that go off one after another */
        int horiz = grand() < 0.5f, rev = grand() < 0.5f, n = arg < 3 ? 3 : arg;
        float fixedc = horiz ? gr(50, SH - 50) : gr(70, SW - 70), fuse = bl * (hard ? 2.0f : 2.6f);
        for (int i = 0; i < n; i++) {
            float t = (i + 0.5f) / n; if (rev) t = 1 - t;
            bomb(horiz ? 20 + t * (SW - 40) : fixedc, horiz ? fixedc : 20 + t * (SH - 40), 24, fuse + i * 0.13f, 6, 1, (i & 1) ? c2 : a);
        }
    } break;
    case P_STAR: {                           /* five-pointed star of bullets */
        float base = b * 0.3f;
        for (int k = 0; k < 5; k++)
            for (int j = 0; j < arg; j++) bullet(ox, oy, base + k * PI2 / 5, 55 + j * 18, 3.5f, (j & 1) ? a : c2);
    } break;
    case P_PINWHEEL: {                       /* a spinning sprinkler that sprays bullets */
        Hazard *h = nh(); if (!h) break;
        h->type = H_EMIT; h->x = ox; h->y = oy; h->split = arg < 2 ? 2 : (arg > 4 ? 4 : arg);
        h->rot = gr(0, PI2); h->vr = (hard ? 1.6f : 1.2f) * (((b / 8) & 1) ? 1.0f : -1.0f);
        h->warn = bl; h->w = h->warn; h->life = bl * 6; h->thick = 0; h->c = c2;
    } break;
    case P_PULSE:
        for (int i = 0; i < arg; i++) {
            float x = ox, y = oy;
            if (i > 0 || !L->boss) { x = gr(50, SW - 50); y = gr(50, SH - 50); }
            ring(x, y, bl * (1 + i), hard ? 95 : 76, hard ? 6 : 5, 300, (i & 1) ? c2 : a);
        }
        break;
    }
}

/* ------------------------------------------------------------- sequencer */
static void onStepSong(int st) {
    const Level *L = &LV[cur];
    const Song *S = &SONGS[L->song];
    int bar = st >> 4, s = st & 15;
    char sec = S->form[bar & 7];
    if (sec == 'i' && bar >= 8) sec = 'A';                /* the intro only happens once */
    const Groove *G = &DRM[sec == 'B' ? S->gB : S->gA];
    if (sec == 'A' || sec == 'B') {
        if (G->kick[s] == 'x') play(drum[S_KICK], 0.9f);
        if (G->snare[s] == 'x') play(drum[S_SNARE], 0.45f);
        if (G->hat[s] == 'x') play(drum[S_HAT], 0.22f);
        if (G->ohat[s] == 'x') play(drum[S_OHAT], 0.24f);
        if (G->clap[s] == 'x') play(drum[S_CLAP], 0.3f);
    } else if (sec == 'i') {
        if (G->hat[s] == 'x' && (s & 1) == 0) play(drum[S_HAT], 0.14f);
    } else {
        if (G->clap[s] == 'x' || s == 8) play(drum[S_CLAP], 0.28f);
        if (G->ohat[s] == 'x') play(drum[S_OHAT], 0.16f);
    }
    if (audio) {
        int ins = 0, n;
        if (sec == 'A' || sec == 'B') { n = songNote(S, 0, bar, s, L->root, &ins); if (n >= 0) play(getNote(ins, n), 0.55f); }
        if (sec != 'b') { n = songNote(S, 1, bar, s, L->root, &ins); if (n >= 0) play(getNote(ins, n), sec == 'i' ? 0.16f : 0.22f); }
        if (sec == 'B' || sec == 'b') { n = songNote(S, 2, bar, s, L->root, &ins); if (n >= 0) play(getNote(ins, n), 0.26f); }
        if (s == 0) for (int k = 0; k < 3; k++) playPad(getNote(S->iPad, padNote(S, bar, k, L->root)), 0.15f);
    }
    if ((bar & 7) == 7 && s >= 8) play(drum[S_SNARE], 0.12f + 0.03f * (s - 8));        /* build-up into the next section */
}
static void onStep(int st) {
    const Level *L = &LV[cur];
    const Style *Y = &STY[L->style];
    if (st < 0) { if ((st & 3) == 0) play(drum[S_KICK], 0.9f); return; }       /* count-in */
    if (L->song >= 0) { onStepSong(st); return; }
    int s = st & 15;
    if (Y->kick[s] == 'x') play(drum[S_KICK], 0.95f);
    if (Y->snare[s] == 'x') play(drum[S_SNARE], 0.5f);
    if (Y->hat[s] == 'x') play(drum[S_HAT], 0.26f);
    if (Y->ohat[s] == 'x') play(drum[S_OHAT], 0.28f);
    if (Y->clap[s] == 'x') play(drum[S_CLAP], 0.34f);
    if (audio) {
        if (Y->bass[s] >= '0' && Y->bass[s] <= '7') play(bassS[Y->bass[s] - '0'], 0.7f);
        if (Y->lead[s] >= '0' && Y->lead[s] <= '7') play(leadS[Y->lead[s] - '0'], 0.2f);
    }
    if (L->boss && ((st >> 2) & 15) == 15) play(drum[S_SNARE], 0.22f + 0.08f * (st & 3));   /* build-up roll before every 16th beat */
}

static void spawnCheckpoint(void) {
    for (int k = 0; k < 10; k++) {
        CP.x = gr(50, SW - 50); CP.y = gr(40, SH - 40);
        if (dist(CP.x, CP.y, P.x, P.y) > 110) break;
    }
    CP.r = 0; CP.on = 1;
}

static void onBeat(int b) {
    const Level *L = &LV[cur];
    if (b >= 0 && b < L->beats) {
        for (int i = 0; i < L->nev; i++) {
            Ev e = adjEv(&L->ev[i]);
            if (b >= e.from && b < e.to && (b - e.from) % e.every == 0) runPat(e.pat, e.arg, b);
        }
        if (!L->boss && (b == L->beats / 3 || b == L->beats * 2 / 3) && !CP.on && b * bl > cpT + 1) spawnCheckpoint();
    }
    if (cur == ULT && finPhase == 6 && b >= 640) {         /* Hollow keeps attacking - harmlessly, you're shielded */
        int bb = 512 + (b - 640) % 128;
        for (int i = 0; i < L->nev; i++) {
            Ev e = adjEv(&L->ev[i]);
            if (e.from >= 512 && bb >= e.from && bb < e.to && (bb - e.from) % e.every == 0) runPat(e.pat, e.arg, bb);
        }
    }
    /* beat visuals: shockwave from the centre and floating motes */
    emit(2, SW / 2, SH / 2, 0, 0, 0.7f, 8, Fade(L->c1, 0.35f), 340, 0);
    for (int i = 0; i < 4; i++) emit(0, rr(0, SW), SH + 3, rr(-6, 6), rr(-60, -25), rr(1.5f, 3), rr(1, 2) * pScale(), Fade(L->c2, 0.5f), 0, 0);
    if (L->boss && b >= 0) {                              /* the boss throbs confetti on every beat */
        const Level *BC = (L->boss == 2 && phaseOf(b) < 4) ? &LV[BOSSIDX[phaseOf(b)]] : L;
        burst(bossX, bossY, 5, 50, 200, 3, BC->c2, 0.8f, 9);
        emit(2, bossX, bossY, 0, 0, 0.5f, 12, Fade(BC->c1, 0.5f), 260, 0);
    }
}

/* ---------------------------------------------------------- game control */
static void saveP(void) {
    mkdir(SAVE_DIR, 0777);
    FILE *f = fopen(SAVE_FILE, "wb");
    if (f) { fwrite(clr, 1, sizeof clr, f); fputc(hard, f); fputc(casual, f); fclose(f); }
}
static void loadP(void) {
    FILE *f = fopen(SAVE_FILE, "rb");
    if (f) {
        if (fread(clr, 1, sizeof clr, f) != sizeof clr) memset(clr, 0, sizeof clr);
        else { int h = fgetc(f), c = fgetc(f); hard = (h == 1); casual = (c == 1); }
        fclose(f);
    }
}

static void startLevel(int i, int fromCp) {
    cur = i; bl = 60.0f / LV[i].bpm;
    memset(HZ, 0, sizeof(HZ)); memset(PT, 0, sizeof(PT));
    memset(&P, 0, sizeof(P));
    P.x = SW / 2; P.y = SH * 0.75f; P.fy = -1; P.ang = -PIF / 2;
    P.maxhp = P.hp = startHP(i);
    shake = flash = deadT = winT = bgScroll = 0;
    healT = ballCD = cpMsg = 0; tsc = 1.0f; CP.on = 0;
    finPhase = 0; curtainY = crushL = 0; finFlash = hitFlash = charge = 0; memset(BM, 0, sizeof BM);
    rs = 12345u + (unsigned)i * 977u;
    bossX = SW / 2; bossY = 40;
    if (fromCp && cpReached) {                       /* resume from the last checkpoint */
        T = cpT; lastStep = (int)floorf(T / bl * 4.0f); curBeat = lastStep >> 2;
        rs = cpRs; P.hp = cpHp; P.inv = 2.0f;
    } else {
        cpReached = 0; cpT = 0;
        T = -4 * bl; lastStep = -17; curBeat = -5;
    }
    buildLevelSounds(i);
    vidMode = 0;
    state = ST_PLAY;
}

static void heal(void) {
    if (P.healCD > 0) return;
    P.healCD = 0.35f;
    if (P.hp < P.maxhp) P.hp++;
    Color b2 = {90, 190, 255, 255};
    for (int k = 0; k < 14; k++) emit(1, P.x + rr(-6, 6), P.y + rr(-4, 4), rr(-20, 20), rr(-110, -40), 0.6f, 3, b2, 0, 0);
    emit(2, P.x, P.y, 0, 0, 0.5f, 4, b2, 80, 0);
    play(drum[S_CLEAR], 0.25f);
}

static void hurt(void) {
    if (state != ST_PLAY && state != ST_VID) return;
    if (finPhase) return;                               /* the finale: you can take no damage */
    if (healT > 0) { heal(); return; }                 /* blue ball active: attacks heal instead */
    if (P.inv > 0) return;
    Color red = {255, 60, 70, 255};
    P.hp--; P.inv = hard ? 1.5f : 2.0f; shake = 6; flash = 1;
    play(drum[S_HIT], 1.0f);
    /* particles flying everywhere */
    burst(P.x, P.y, 26, 150, 560, 1, red, 0.7f, 5);
    burst(P.x, P.y, 14, 100, 480, 1, WHITE, 0.55f, 4);
    burst(P.x, P.y, 12, 80, 420, 1, LV[cur].c1, 0.6f, 4);
    burst(P.x, P.y, 8, 60, 300, 3, red, 0.9f, 12);
    emit(2, P.x, P.y, 0, 0, 0.5f, 4, red, 200, 0);
    emit(2, P.x, P.y, 0, 0, 0.7f, 4, WHITE, 120, 0);
    /* mercy: nearby bullets shatter */
    for (int i = 0; i < MAXH; i++)
        if (HZ[i].type == H_BULLET && dist(HZ[i].x, HZ[i].y, P.x, P.y) < (hard ? 45 : 60)) {
            burst(HZ[i].x, HZ[i].y, 3, 40, 200, 0, HZ[i].c, 0.5f, 4);
            HZ[i].type = H_NONE;
        }
    if (P.hp <= 0) {
        state = vidMode ? ST_VOVER : ST_OVER; deadT = 0; shake = 12;
        burst(P.x, P.y, 110, 100, 800, 1, red, 1.2f, 6);
        burst(P.x, P.y, 30, 60, 500, 1, WHITE, 1.0f, 5);
        burst(P.x, P.y, 20, 60, 400, 3, LV[cur].c1, 1.4f, 14);
        emit(2, P.x, P.y, 0, 0, 0.9f, 4, WHITE, 280, 0);
        emit(2, P.x, P.y, 0, 0, 1.2f, 4, red, 200, 0);
    }
}

static void useBall(void) {                            /* the BLUE BALL: slow time + attacks heal for 20s, 60s cooldown */
    if (cur != ULT || state != ST_PLAY || ballCD > 0 || healT > 0 || finPhase) return;
    healT = 20.0f; ballCD = 60.0f;
    Color b2 = {90, 190, 255, 255};
    burst(P.x, P.y, 60, 80, 500, 1, b2, 0.9f, 5);
    emit(2, P.x, P.y, 0, 0, 0.8f, 6, b2, 400, 0);
    emit(2, P.x, P.y, 0, 0, 1.1f, 6, WHITE, 300, 0);
    play(drum[S_HIT], 0.8f); play(drum[S_CLEAR], 0.5f);
    shake = 5;
}

static int hitRect(const Hazard *h) {
    float cx = fmaxf(h->x, fminf(P.x, h->x + h->w)), cy = fmaxf(h->y, fminf(P.y, h->y + h->h));
    return (P.x - cx) * (P.x - cx) + (P.y - cy) * (P.y - cy) < PR * PR;
}

static void stepHazards(float dt, int canHit) {
    for (int i = 0; i < MAXH; i++) {
        Hazard *h = &HZ[i];
        switch (h->type) {
        case H_BULLET:
            if (h->flag == 1 && h->life > 0) {                 /* homing: steer toward the player for a while */
                float ang = atan2f(h->vy, h->vx), want = atan2f(P.y - h->y, P.x - h->x), df = want - ang;
                while (df > PIF) { df -= PI2; }
                while (df < -PIF) { df += PI2; }
                float tr = 1.7f * dt;
                df = clampf(df, -tr, tr); ang += df;
                float sp = hypotf(h->vx, h->vy);
                h->vx = cosf(ang) * sp; h->vy = sinf(ang) * sp; h->life -= dt;
            }
            if (h->flag == 3) {                                /* sine wave: wobble sideways */
                h->ph += h->vr * dt;
                h->x += (h->vx + h->x2 * h->amp * cosf(h->ph)) * dt; h->y += (h->vy + h->y2 * h->amp * cosf(h->ph)) * dt;
            } else { h->x += h->vx * dt; h->y += h->vy * dt; }
            if (h->split > 0) {                                /* ricochet */
                if ((h->x < h->r && h->vx < 0) || (h->x > SW - h->r && h->vx > 0)) { h->vx = -h->vx; h->split--; }
                if ((h->y < h->r && h->vy < 0) || (h->y > SH - h->r && h->vy > 0)) { h->vy = -h->vy; h->split--; }
            }
            if (h->x < -30 || h->x > SW + 30 || h->y < -30 || h->y > SH + 30) { h->type = H_NONE; break; }
            if (rand() % 12 == 0) emit(0, h->x, h->y, 0, 0, 0.25f, 1.5f, Fade(h->c, 0.6f), 0, 0);
            if (canHit && dist(h->x, h->y, P.x, P.y) < h->r + PR) {
                hurt();
                if (healT > 0) { burst(h->x, h->y, 3, 40, 160, 0, (Color){90, 190, 255, 255}, 0.4f, 3); h->type = H_NONE; }
            }
            break;
        case H_EMIT:                                           /* pinwheel sprinkler */
            if (h->warn > 0) { h->warn -= dt; h->rot += h->vr * dt * 0.3f; break; }
            h->rot += h->vr * dt;
            h->thick -= dt;
            if (h->thick <= 0) {
                h->thick = hard ? 0.13f : 0.17f;
                for (int k = 0; k < h->split; k++) bulletH(h->x, h->y, h->rot + k * PI2 / h->split, 80, 3.5f, h->c, 0, 0, 0);
            }
            h->life -= dt; if (h->life <= 0) h->type = H_NONE;
            break;
        case H_RECT:
            if (h->warn > 0) {
                h->warn -= dt;
                if (h->warn <= 0 && h->flag == 0) {          /* beam fires */
                    shake += 2;
                    for (int k = 0; k < 20; k++)
                        emit(1, h->x + rr(0, h->w), h->y + rr(0, h->h), rr(-50, 50), rr(-50, 50), 0.5f, 2, h->c, 0, 0);
                }
                break;
            }
            h->x += h->vx * dt; h->y += h->vy * dt;
            if (canHit && hitRect(h)) hurt();
            if (h->flag == 0) { h->life -= dt; if (h->life <= 0) h->type = H_NONE; }
            else if (h->x + h->w < -30 || h->x > SW + 30 || h->y + h->h < -30 || h->y > SH + 30) h->type = H_NONE;
            break;
        case H_LINE:
            if (h->warn > 0) {
                h->warn -= dt;
                if (h->warn <= 0) for (int k = 0; k < 2; k++) {
                    float t = rr(0, 1);
                    emit(1, h->x + (h->x2 - h->x) * t, h->y + (h->y2 - h->y) * t, rr(-40, 40), rr(-40, 40), 0.4f, 2, h->c, 0, 0);
                }
                break;
            }
            if (canHit && segDist(P.x, P.y, h->x, h->y, h->x2, h->y2) < h->thick * 0.5f + PR) hurt();
            h->life -= dt; if (h->life <= 0) h->type = H_NONE;
            break;
        case H_SAW:
            h->x += h->vx * dt;
            if (h->flag == 1) { h->ph += h->vr * dt; h->y = h->y0 + h->amp * sinf(h->ph); }
            else h->y += h->vy * dt;
            h->rot += 8.0f * dt;
            if (h->x < -50 || h->x > SW + 50 || h->y < -50 || h->y > SH + 50) { h->type = H_NONE; break; }
            if (rand() % 10 == 0) emit(1, h->x, h->y, rr(-30, 30), rr(-30, 30), 0.25f, 2, (Color){255, 200, 90, 255}, 0, 0);
            if (canHit && dist(h->x, h->y, P.x, P.y) < h->r * 0.9f + PR) hurt();
            break;
        case H_SPIN:
            if (h->flag == 1) { h->x = bossX; h->y = bossY; }        /* pivot follows the boss */
            h->rot += h->vr * dt;
            if (h->warn > 0) {
                h->warn -= dt;
                if (h->warn <= 0) { shake += 2; burst(h->x, h->y, 16, 60, 260, 1, h->c, 0.5f, 4); }
                break;
            }
            if (canHit)
                for (int k = 0; k < h->split; k++) {
                    float a = h->rot + k * PI2 / h->split;
                    if (segDist(P.x, P.y, h->x, h->y, h->x + cosf(a) * h->r, h->y + sinf(a) * h->r) < h->thick * 0.5f + PR) { hurt(); break; }
                }
            if (rand() % 3 == 0) {
                float a = h->rot + (rand() % h->split) * PI2 / h->split, d = rr(20, 200);
                emit(1, h->x + cosf(a) * d, h->y + sinf(a) * d, rr(-30, 30), rr(-30, 30), 0.4f, 2 * pScale(), h->c, 0, 0);
            }
            h->life -= dt; if (h->life <= 0) h->type = H_NONE;
            break;
        case H_CANNON:
            if (h->warn > 0) {
                h->warn -= dt;
                if (h->flag == 0 && h->warn > h->w * 0.4f) h->rot = atan2f(P.y - h->y, P.x - h->x);   /* tracks you, then locks */
                break;
            }
            h->life -= dt;
            if (h->life <= 0) {
                if (h->split <= 0) { h->type = H_NONE; break; }
                bullet(h->x + cosf(h->rot) * 10, h->y + sinf(h->rot) * 10, h->rot, 230, 5, h->c);
                for (int k = 0; k < 3; k++) emit(1, h->x + cosf(h->rot) * 12, h->y + sinf(h->rot) * 12,
                                                  cosf(h->rot) * rr(40, 120) + rr(-30, 30), sinf(h->rot) * rr(40, 120) + rr(-30, 30), 0.3f, 2 * pScale(), h->c, 0, 0);
                shake += 0.5f; play(drum[S_CLAP], 0.3f);
                h->split--; h->life = 0.11f;
            }
            break;
        case H_RING:
            if (h->warn > 0) {
                h->warn -= dt;
                if (h->warn <= 0) burst(h->x, h->y, 12, 60, 240, 1, h->c, 0.4f, 4);
                break;
            }
            h->r += h->vr * dt;
            if (canHit && fabsf(dist(h->x, h->y, P.x, P.y) - h->r) < h->thick * 0.5f + PR) hurt();
            if (h->r > h->w) h->type = H_NONE;
            break;
        case H_BOMB:
            if (!h->flag) {
                h->warn -= dt;
                if (h->warn <= 0) {
                    h->flag = 1; h->life = 0.22f; h->thick = 0.22f;
                    float off = rr(0, PI2);
                    for (int k = 0; k < h->split; k++) bullet(h->x, h->y, off + k * PI2 / h->split, 95, 3, h->c);
                    burst(h->x, h->y, 20, 100, 400, 1, h->c, 0.6f, 4);
                    burst(h->x, h->y, 6, 60, 260, 3, WHITE, 0.7f, 8);
                    if (h->dmg) { emit(2, h->x, h->y, 0, 0, 0.5f, h->r * 0.5f, h->c, 80, 0); shake += 2.5f; }
                    play(drum[S_SNARE], 0.6f);
                }
            } else {
                h->life -= dt;
                if (h->dmg && canHit && dist(h->x, h->y, P.x, P.y) < h->r + PR) hurt();
                if (h->life <= 0) h->type = H_NONE;
            }
            break;
        }
    }
}

static float squeezeInset(void) {                          /* BASSLINE: the side walls pulse in and out every 8 beats */
    if (vidMode || !(LV[cur].gim & GIM_SQUEEZE) || T < 0 || finPhase) return 0;
    float s = 0.5f + 0.5f * sinf(T / (bl * 8.0f) * PI2 - PIF / 2);
    return 30.0f * s * s;
}
static void stepPlayer(float dt) {
    if (P.inv > 0) P.inv -= dt;
    if (P.dashCD > 0) P.dashCD -= dt;
    float dx = 0, dy = 0;
    if (kHeld & KEY_DRIGHT) dx += 1;
    if (kHeld & KEY_DLEFT) dx -= 1;
    if (kHeld & KEY_DDOWN) dy += 1;
    if (kHeld & KEY_DUP) dy -= 1;
    if (abs(cpx) > 20 || abs(cpy) > 20) { dx = cpx / 156.0f; dy = -cpy / 156.0f; }   /* circle pad: y is up-positive */
    float m = hypotf(dx, dy);
    if (m > 1) { dx /= m; dy /= m; m = 1; }
    if (m > 0.05f) { P.fx = dx / m; P.fy = dy / m; P.ang = atan2f(dy, dx); }

    if ((kDown & (KEY_A | KEY_B | KEY_L | (finPhase ? 0 : KEY_R))) && P.dashCD <= 0 && T > -bl) {   /* R is the beam button in the finale */
        float fr = T / bl - floorf(T / bl);
        int perfect = (fr < 0.12f || fr > 0.88f) && T >= 0;
        P.dashT = DASH_TIME; P.dashCD = perfect ? dashCd() * 0.45f : dashCd();
        P.dx = m > 0.05f ? dx / m : P.fx; P.dy = m > 0.05f ? dy / m : P.fy;
        if (P.inv < DASH_TIME + 0.05f) P.inv = DASH_TIME + 0.05f;
        play(drum[S_DASH], 0.7f);
        burst(P.x, P.y, perfect ? 22 : 10, 80, 380, 1, perfect ? WHITE : LV[cur].c1, 0.4f, 4);
        emit(2, P.x, P.y, 0, 0, 0.35f, 4, perfect ? WHITE : LV[cur].c1, 100, 0);
    }
    if (P.dashT > 0) {
        P.x += P.dx * 380 * dt; P.y += P.dy * 380 * dt; P.dashT -= dt;
        for (int k = 0; k < 2; k++) emit(0, P.x + rr(-2, 2), P.y + rr(-2, 2), 0, 0, 0.4f, 3.5f, LV[cur].c1, 0, 0);
    } else {
        int gim = vidMode ? 0 : LV[cur].gim;
        if (gim & GIM_ICE) {                               /* ice physics: you keep sliding */
            float k = clampf(dt * 2.4f, 0, 1);
            P.mvx += (dx * 150 - P.mvx) * k; P.mvy += (dy * 150 - P.mvy) * k;
            P.x += P.mvx * dt; P.y += P.mvy * dt;
        } else { P.x += dx * 140 * dt; P.y += dy * 140 * dt; P.mvx = P.mvy = 0; }
        if ((gim & (GIM_WIND | GIM_WINDS)) && T > 0) {     /* a crosswind that swings left and right */
            float amp = (gim & GIM_WINDS) ? 48.0f : 26.0f, w = amp * sinf(T / (bl * 8.0f) * PIF);
            P.x += w * dt;
            if (rand() % 4 == 0) emit(1, w > 0 ? 0 : SW, rr(0, SH), w * 3, rr(-4, 4), 0.5f, 1.5f, Fade(LV[cur].c1, 0.5f), 0, 0);
        }
        P.trail -= dt;
        if (m > 0.05f && P.trail <= 0) { P.trail = 0.04f; emit(0, P.x, P.y, rr(-8, 8), rr(-8, 8), 0.35f, 2, Fade(LV[cur].c1, 0.7f), 0, 0); }
    }
    float in = squeezeInset();
    P.x = clampf(P.x, 6 + in, SW - 6 - in); P.y = clampf(P.y, 6, SH - 6);
    if (P.x <= 6 + in + 0.01f || P.x >= SW - 6 - in - 0.01f) P.mvx = 0;
    if (P.y <= 6.01f || P.y >= SH - 6.01f) P.mvy = 0;
}

static void claimCheckpoint(void) {
    CP.on = 0; cpReached = 1; cpT = T; cpHp = P.hp; cpRs = rs; cpMsg = 1.6f;
    Color g = {200, 200, 205, 255};
    burst(P.x, P.y, 30, 60, 300, 1, g, 0.7f, 4);
    emit(2, P.x, P.y, 0, 0, 0.6f, 4, g, 160, 0);
    play(drum[S_CLEAR], 0.5f);
}

static float winDelay(void) { return cur == 13 ? 7.6f : 0.6f; }


/* ---------------- HOLLOW FINALE ---------------- */
static float ss(float x) { x = clampf(x, 0, 1); return x * x * (3 - 2 * x); }
static void setFin(int ph) { finPhase = ph; phT = 0; }

static void winLevel(void) {
    const Level *L = &LV[cur];
    state = ST_WIN; winT = 0; CP.on = 0; healT = 0; tsc = 1; finPhase = 0; curtainY = 0; crushL = 0;
    winX0 = bossX; winY0 = bossY;
    for (int i = 0; i < MAXH; i++)
        if (HZ[i].type) { burst(HZ[i].x + HZ[i].w * 0.5f, HZ[i].y + HZ[i].h * 0.5f, 2, 40, 240, 0, HZ[i].c, 0.8f, 4); HZ[i].type = H_NONE; }
    if (L->boss && cur != 13) {
        if (L->boss == 2) { burst(bossX, bossY, 140, 100, 1000, 1, WHITE, 1.6f, 6); burst(bossX, bossY, 70, 100, 900, 1, L->c2, 1.6f, 6); }
        burst(bossX, bossY, 140, 100, 900, 1, L->c2, 1.4f, 6);
        burst(bossX, bossY, 40, 80, 500, 3, L->c1, 1.6f, 16);
        emit(2, bossX, bossY, 0, 0, 1.2f, 8, WHITE, 320, 0);
        shake = 10;
    } else if (!L->boss) burst(P.x, P.y, 50, 100, 500, 1, L->c1, 1.0f, 5);
    play(drum[S_CLEAR], 1.0f);
    clr[hard][cur] |= casual ? 1 : 2;
    saveP();
}

static void ballPos(float u, float *x, float *y) {     /* the blue ball flies from your item slot to Hollow */
    float sx = 14, sy = SH - 16, cx = 70, cy = SH * 0.35f, a = (1 - u) * (1 - u), b = 2 * (1 - u) * u, c = u * u;
    *x = a * sx + b * cx + c * bossX; *y = a * sy + b * cy + c * bossY;
}
static void startFinale(void) {
    finPhase = 1; phT = 0; eaten = 0; reformed = 0; healT = 0; tsc = 1; ballCD = 0;
    finBX0 = bossX; finBY0 = bossY; curtainY = 0; crushL = 0; finFlash = 0; hitFlash = 0; charge = 0; fireCD = 0; finAng = 0;
    memset(BM, 0, sizeof BM);
    for (int i = 0; i < MAXH; i++)                       /* clear the arena */
        if (HZ[i].type) { burst(HZ[i].x + HZ[i].w * 0.5f, HZ[i].y + HZ[i].h * 0.5f, 2, 40, 240, 0, HZ[i].c, 0.8f, 4); HZ[i].type = H_NONE; }
    play(drum[S_HIT], 0.8f); shake = 6;
}
static void finaleBoss(void) {
    if (finPhase == 1) { float u = ss(phT / 1.0f); bossX = finBX0 + (SW / 2 - finBX0) * u; bossY = finBY0 + (58 - finBY0) * u; }
    else if (finPhase < 6) { bossX = SW / 2; bossY = 58 + sinf(phT * 2) * 2; }
    else { bossX = SW / 2 + sinf(finAng) * 130; bossY = 55 + sinf(finAng * 2.2f) * 6; }
}
static void fireBeam(float ch) {                       /* R released: tap = thin beam, longer charge = wider, stronger beam */
    static const float W[4] = {3, 7, 13, 24};
    static const int D[4] = {1, 3, 8, 20};
    int lvl = ch < 0.2f ? 0 : (ch < 0.6f ? 1 : (ch < 1.2f ? 2 : 3));
    Color b2 = {90, 190, 255, 255};
    float w = W[lvl];
    for (int i = 0; i < 8; i++)
        if (BM[i].life <= 0) { BM[i].x = P.x; BM[i].y = P.y - 8; BM[i].w = w; BM[i].life = BM[i].max = 0.18f + 0.08f * lvl; BM[i].lvl = lvl; break; }
    for (int i = 0; i < MAXH; i++) {                   /* the beam wipes out bullets and saws in its path */
        Hazard *h = &HZ[i];
        if ((h->type == H_BULLET || h->type == H_SAW) && h->y < P.y && fabsf(h->x - P.x) < w * 0.5f + h->r) {
            burst(h->x, h->y, 3, 40, 200, 0, b2, 0.5f, 3); h->type = H_NONE;
        }
    }
    burst(P.x, P.y - 8, 6 + lvl * 8, 60, 320, 1, b2, 0.5f, 4);
    if (fabsf(P.x - bossX) < 22 + w * 0.5f && bossY < P.y) {            /* hit Hollow */
        finHp -= D[lvl]; hitFlash = 0.15f;
        burst(bossX, bossY, 10 + lvl * 14, 80, 500, 1, WHITE, 0.7f, 5);
        burst(bossX, bossY, 4 + lvl * 4, 60, 300, 3, b2, 0.9f, 8);
        emit(2, bossX, bossY, 0, 0, 0.5f, 10, b2, 200 + lvl * 60, 0);
        shake += 1.5f + lvl * 1.5f;
        if (lvl >= 2) play(drum[S_HIT], 0.9f);
    }
    shake += 0.5f + lvl;
    play(lvl == 0 ? drum[S_DASH] : (lvl == 1 ? drum[S_SNARE] : drum[S_CLAP]), 0.6f + 0.1f * lvl);
    if (lvl == 3) play(drum[S_CLEAR], 0.5f);
}

static void updateFinale(float dt) {
    Color b2 = {90, 190, 255, 255}, ember = {255, 190, 80, 255};
    phT += dt; hintT += dt;
    if (hitFlash > 0) hitFlash -= dt;
    if (finFlash > 0) finFlash -= dt * 1.5f;
    if (fireCD > 0) fireCD -= dt;
    for (int i = 0; i < 8; i++) if (BM[i].life > 0) BM[i].life -= dt;
    switch (finPhase) {
    case 1:                                            /* HOLLOW EATS THE BLUE BALL */
        if (phT > 0.6f && phT < 2.2f) {
            float bx, by; ballPos((phT - 0.6f) / 1.6f, &bx, &by);
            emit(0, bx, by, rr(-10, 10), rr(-10, 10), 0.5f, 3, b2, 0, 0);
        }
        if (phT >= 2.2f && !eaten) {
            eaten = 1; finFlash = 1; shake = 8;
            burst(bossX, bossY, 80, 80, 500, 1, b2, 1.0f, 6);
            emit(2, bossX, bossY, 0, 0, 0.9f, 10, b2, 340, 0); emit(2, bossX, bossY, 0, 0, 1.2f, 10, WHITE, 240, 0);
            play(drum[S_HIT], 1.0f); play(drum[S_CLEAR], 0.7f);
        }
        if (phT >= 4.0f) setFin(2);
        break;
    case 2:                                            /* SAWBLADES COME DOWN FROM THE TOP */
        curtainY = 160.0f * ss(phT / 6.0f);
        P.y = clampf(P.y, curtainY + 16, SH - 8);
        for (int k = 0; k < 2; k++) emit(1, rr(0, SW), curtainY + 4, rr(-40, 40), rr(20, 80), 0.3f, 2, ember, 0, 0);
        if (shake < 1.2f) shake = 1.2f;
        if (phT >= 6.5f) setFin(3);
        break;
    case 3: {                                          /* THE CRUSHER CLOSES IN */
        float u = clampf(phT / 3.2f, 0, 1);
        crushL = 190.0f * u * u;
        float Le = crushL + 10, Re = SW - crushL - 10;
        P.x = clampf(P.x, Le + 7, Re - 7); P.y = clampf(P.y, curtainY + 16, SH - 8);
        emit(1, Le, rr(curtainY + 10, SH), rr(-10, 60), rr(-30, 30), 0.3f, 2, ember, 0, 0);
        emit(1, Re, rr(curtainY + 10, SH), rr(-60, 10), rr(-30, 30), 0.3f, 2, ember, 0, 0);
        if (shake < 1.5f + u * 4) shake = 1.5f + u * 4;
        if ((int)(phT * 4) != (int)((phT - dt) * 4)) play(drum[S_KICK], 0.5f);
        if (Re - Le <= 16) {                           /* CRUSHED: you shatter */
            px0 = P.x; py0 = P.y; setFin(4); flash = 1; shake = 14; reformed = 0;
            burst(px0, py0, 70, 120, 700, 3, WHITE, 1.8f, 10);
            burst(px0, py0, 40, 100, 600, 1, b2, 1.4f, 5);
            burst(px0, py0, 30, 100, 500, 1, (Color){255, 60, 70, 255}, 1.2f, 5);
            emit(2, px0, py0, 0, 0, 0.7f, 6, WHITE, 300, 0);
            play(drum[S_HIT], 1.0f); play(drum[S_SNARE], 0.8f);
        }
    } break;
    case 4:                                            /* SHATTERED - a blue ring spins and pulls you back together */
        P.x = px0; P.y = py0;
        curtainY = 160.0f * (1 - ss((phT - 0.6f) / 1.6f));
        crushL = 190.0f * (1 - ss((phT - 0.4f) / 1.4f)) - 14.0f * ss((phT - 1.6f) / 0.8f);
        if (phT > 0.4f && phT < 2.2f) {
            float a = rr(0, PI2), d = rr(28, 60);
            emit(0, px0 + cosf(a) * d, py0 + sinf(a) * d, -cosf(a) * 70, -sinf(a) * 70, 0.5f, 2.5f, b2, 0, 0);
        }
        if (phT >= 2.0f && !reformed) {
            reformed = 1; finFlash = 0.8f;
            burst(px0, py0, 30, 80, 400, 1, b2, 0.8f, 4);
            emit(2, px0, py0, 0, 0, 0.6f, 6, b2, 200, 0);
            play(drum[S_CLEAR], 0.8f);
        }
        if (phT >= 2.6f) setFin(5);
        break;
    case 5: {                                          /* YOU RISE BACK UP TO THE BATTLEFIELD */
        float u = clampf(phT / 1.4f, 0, 1), e = 1 - (1 - u) * (1 - u) * (1 - u);
        curtainY = 0; crushL = 0;
        P.x = px0 + (SW / 2 - px0) * e; P.y = py0 + (SH * 0.72f - py0) * e;
        emit(0, P.x + rr(-4, 4), P.y + 8, rr(-8, 8), rr(20, 60), 0.4f, 2.5f, b2, 0, 0);
        if (phT >= 1.4f) { setFin(6); finMax = finHp = hard ? 200 : 150; charge = 0; hintT = 0; finAng = 0; }
    } break;
    case 6: {                                          /* FIGHT BACK: R = beam, hold R to charge */
        finAng += (0.6f + 0.6f * (1.0f - finHp / (float)finMax)) * dt;
        if (kHeld & KEY_R) {
            float old = charge; charge += dt; if (charge > 1.5f) charge = 1.5f;
            if ((old < 0.2f && charge >= 0.2f) || (old < 0.6f && charge >= 0.6f) || (old < 1.2f && charge >= 1.2f)) {
                play(drum[S_HAT], 0.6f); burst(P.x, P.y - 12, 6, 40, 200, 1, b2, 0.4f, 3);
            }
        }
        if (kUp & KEY_R) { if (fireCD <= 0) { fireBeam(charge); fireCD = 0.22f; } charge = 0; }
        else if (!(kHeld & KEY_R)) charge = 0;
        if (finHp <= 0) winLevel();                    /* HOLLOW SHATTERS */
    } break;
    }
}

static void updatePlay(float dt) {
    const Level *L = &LV[cur];
    int alive = (state == ST_PLAY);
    float wdt = dt;                                     /* world time (slowed by the blue ball) */
    if (alive) {
        if (healT > 0) healT -= dt;
        if (ballCD > 0) ballCD -= dt;
        if (P.healCD > 0) P.healCD -= dt;
        if (cur == ULT && ((kDown & KEY_Y) || (touchDown && dist(tx, ty, 160, 150) < 34))) useBall();
        float target = healT > 0 ? 0.45f : 1.0f;
        tsc += (target - tsc) * clampf(dt * 8.0f, 0, 1);
        wdt = dt * tsc;
        T += wdt;
        int ns = (int)floorf(T / bl * 4.0f);
        while (lastStep < ns) {
            lastStep++; onStep(lastStep);
            if ((lastStep & 3) == 0) { curBeat = lastStep >> 2; onBeat(curBeat); }
        }
        if (finPhase != 4 && finPhase != 5) stepPlayer(dt);
        if (cur == ULT && finPhase == 0 && T >= L->beats * bl) startFinale();
        if (finPhase) updateFinale(dt);
        if (inSawPhase()) bgScroll += (L->scroll > 0 ? L->scroll : 105.0f) * wdt;
        if (CP.on) {
            CP.r += 95.0f * wdt;
            if (dist(P.x, P.y, CP.x, CP.y) <= CP.r || CP.r >= 470.0f) claimCheckpoint();
        }
        if (cpMsg > 0) cpMsg -= dt;
        if (cur != ULT && T >= (L->beats + 2) * bl) winLevel();     /* level cleared (HOLLOW ends via the finale) */
    } else { deadT += dt; wdt = dt * 0.35f; }
    if (finPhase) finaleBoss();
    else if (L->scroll > 0) { bossX = SW - 30; bossY = SH / 2 + sinf(T * 0.9f) * 80.0f; }     /* the saw boss roams the right edge */
    else { bossX = SW / 2 + sinf(T * 0.5f) * 125.0f; bossY = 40 + sinf(T * 1.1f) * 6.0f; }
    stepHazards(wdt, alive && !finPhase);
    stepParticles(wdt);
    shake *= expf(-dt * 9); flash *= expf(-dt * 5);
}

/* ---------------------------------------------------------------- drawing */
static void drawFactory(float t) {
    Color steel = {70, 74, 88, 255}, orange = {255, 150, 40, 255};
    for (int i = 0; i < 3; i++) {                       /* slow parallax gears */
        float gx = fmodf(i * 190.0f - bgScroll * 0.4f, 570.0f);
        if (gx < 0) gx += 570.0f;
        gx -= 90; float gy = (i & 1) ? 175 : 65;
        dPolyLines(gx, gy, 12, 46, t * ((i & 1) ? -14 : 14), 2, Fade(steel, 0.55f));
        dPolyLines(gx, gy, 12, 30, t * ((i & 1) ? 14 : -14), 2, Fade(steel, 0.4f));
    }
    dRect(0, 0, SW, 8, (Color){36, 38, 46, 255}); dRect(0, SH - 8, SW, 8, (Color){36, 38, 46, 255});
    float o = fmodf(bgScroll, 40.0f);
    for (float x = -40; x < SW + 40; x += 40) { dRect(x - o, 1, 20, 6, Fade(orange, 0.55f)); dRect(x - o, SH - 7, 20, 6, Fade(orange, 0.55f)); }
}
static void drawLattice(const Level *L, float pulse) {   /* faint triangular grid behind the triangle bosses */
    Color g = Fade(L->c1, 0.10f + 0.10f * pulse);
    for (int f = 0; f < 3; f++) {
        float ang = f * PIF / 3, dx = cosf(ang), dy = sinf(ang), nx = -dy, ny = dx;
        for (float d = -260; d <= 260; d += 58)
            dLine(SW / 2 + nx * d - dx * 500, SH / 2 + ny * d - dy * 500, SW / 2 + nx * d + dx * 500, SH / 2 + ny * d + dy * 500, 1, g);
    }
}
static unsigned hsh(unsigned v) { v ^= v >> 16; v *= 0x7feb352dU; v ^= v >> 15; v *= 0x846ca68bU; v ^= v >> 16; return v; }
static float wrapf(float v, float m) { v = fmodf(v, m); return v < 0 ? v + m : v; }
static void drawBG(const Level *L, float t, float pulse) {
    dGradV(0, 0, SW, SH, mul(L->c1, 0.10f + 0.04f * pulse), mul(L->c2, 0.04f));
    int bgs = vidMode ? 0 : L->bgs;
    float fb = bl > 0 ? t / bl : 0;
    switch (bgs) {
    case 1:                                                /* drifting stars */
        for (int i = 0; i < 40; i++)
            dRect(wrapf(i * 61.7f - t * (5.0f + (i % 5) * 6.0f), SW), fmodf(i * 47.3f, SH), 1.0f + (i % 3) * 0.6f, 1.0f + (i % 3) * 0.6f, Fade(WHITE, 0.22f + 0.12f * (i % 4)));
        break;
    case 2:                                                /* shockwave rings */
        for (int k = 0; k < 4; k++) {
            float ph = wrapf(fb * 0.5f + k * 0.25f, 1.0f);
            dCircleLines(SW / 2, SH / 2, ph * 270, 1.5f, Fade(L->c1, 0.22f * (1 - ph)));
        }
        break;
    case 3:                                                /* speed lines */
        for (int i = 0; i < 14; i++) { float x = wrapf(i * 44.0f + t * 36.0f, SW + 260) - 130; dLine(x, 0, x - 130, SH, 1.5f, Fade(L->c1, 0.10f + 0.10f * pulse)); }
        break;
    case 4:                                                /* rain */
        for (int i = 0; i < 44; i++) { float x = fmodf(i * 37.7f, SW), y = wrapf(i * 91.3f + t * (120.0f + (i % 5) * 45.0f), SH + 30) - 15; dLine(x, y, x - 2, y + 12, 1, Fade(L->c2, 0.30f)); }
        break;
    case 5:                                                /* sound waves */
        for (int w = 0; w < 4; w++) {
            float y0 = 40.0f + w * 48.0f, py = y0 + sinf(t * 2.0f + w) * 8;
            for (int x = 20; x <= SW; x += 20) {
                float ny = y0 + sinf(x * 0.03f + t * 2.0f + w) * (8 + 6 * pulse);
                dLine(x - 20, py, x, ny, 1, Fade(L->c1, 0.22f)); py = ny;
            }
        }
        break;
    case 6:                                                /* glitch blocks */
        for (int i = 0; i < 9; i++) {
            unsigned h = hsh((unsigned)((int)(fb * 2)) * 131u + (unsigned)i * 977u);
            dRect((float)(h % (SW - 60)), (float)((h >> 8) % (SH - 20)), 20.0f + (h >> 16) % 70, 3.0f + (h >> 20) % 14, Fade((i & 1) ? L->c1 : L->c2, 0.10f + 0.05f * pulse));
        }
        break;
    default: {                                             /* neon grid */
        float off = fmodf(fabsf(t) * 10.0f, 20.0f);
        Color g = Fade(L->c1, 0.06f + 0.09f * pulse);
        for (int x = 0; x <= SW; x += 20) dRect(x, 0, 1, SH, g);
        for (int y = -20; y <= SH; y += 20) dRect(0, y + off, SW, 1, g);
    } break;
    }
    for (int i = 0; i < 20; i++) {
        float hgt = (0.15f + 0.85f * fabsf(sinf(i * 1.7f + t * 3.0f))) * pulse * 36.0f + 3.0f;
        dRect(i * 20 + 1, SH - hgt, 18, hgt, Fade(L->c2, 0.13f));
    }
}

static void drawHazards(void) {
    for (int i = 0; i < MAXH; i++) {
        Hazard *h = &HZ[i];
        if (!h->type) continue;
        Color c = h->c;
        if (healT > 0) { c.r = 80; c.g = 170; c.b = 255; }        /* blue ball: every attack turns blue (= healing) */
        if (finPhase) c = mul(c, 0.6f);                             /* finale: attacks are harmless ghosts */
        if (!vidMode && !finPhase && (LV[cur].gim & GIM_DARK) && (h->type == H_BULLET || h->type == H_SAW)) {   /* BLACKOUT: bullets hide until close / on the beat */
            float fr = T / bl - floorf(T / bl);
            float v = clampf(1.35f - dist(h->x, h->y, P.x, P.y) / 95.0f, 0.10f, 1.0f);
            c = mul(c, clampf(v + 0.55f * expf(-fr * 5.0f), 0, 1));
        }
        switch (h->type) {
        case H_BULLET:
            dCircle(h->x, h->y, h->r, c);
            dRect(h->x - 1, h->y - 1, 2, 2, WHITE);
            break;
        case H_SAW: dSaw(h->x, h->y, h->r, h->rot, c); break;
        case H_EMIT: {
            for (int a2 = 0; a2 < h->split; a2++) {
                float a = h->rot + a2 * PI2 / h->split;
                dLine(h->x, h->y, h->x + cosf(a) * 14, h->y + sinf(a) * 14, 2, Fade(c, h->warn > 0 ? 0.5f : 1.0f));
            }
            dCircle(h->x, h->y, 6, Fade(c, 0.9f)); dRect(h->x - 1, h->y - 1, 2, 2, WHITE);
            if (h->warn > 0) dCircleLines(h->x, h->y, 10 + 14 * (h->warn / h->w), 1, Fade(c, 0.7f));
        } break;
        case H_SPIN: {
            float k = h->warn > 0 ? 1.0f - h->warn / h->w : 1.0f, fl = ((int)(h->warn * 14) & 1) ? 0.15f : 0.0f;
            float fade = h->warn > 0 ? 1.0f : clampf(h->life / 0.35f, 0, 1);
            for (int a2 = 0; a2 < h->split; a2++) {
                float a = h->rot + a2 * PI2 / h->split, ex = h->x + cosf(a) * h->r, ey = h->y + sinf(a) * h->r;
                if (h->warn > 0) dLine(h->x, h->y, ex, ey, 1.0f + 2 * k, Fade(c, 0.25f + 0.5f * k + fl));
                else {
                    dLine(h->x, h->y, ex, ey, h->thick * 1.7f, Fade(c, 0.28f * fade));
                    dLine(h->x, h->y, ex, ey, h->thick, Fade(c, 0.85f * fade));
                    dLine(h->x, h->y, ex, ey, h->thick * 0.35f, Fade(WHITE, 0.9f * fade));
                }
            }
            dCircle(h->x, h->y, h->warn > 0 ? 5.0f : 8.0f, Fade(c, 0.9f));
            dRect(h->x - 1.5f, h->y - 1.5f, 3, 3, WHITE);
        } break;
        case H_CANNON: {
            float ex = h->x + cosf(h->rot) * 14, ey = h->y + sinf(h->rot) * 14;
            if (h->warn > 0) {
                float k = 1.0f - h->warn / h->w;
                dLine(h->x, h->y, h->x + cosf(h->rot) * 600, h->y + sinf(h->rot) * 600, 1.0f + k * 1.5f, Fade(c, 0.15f + 0.45f * k));
            }
            dCircle(h->x, h->y, 9, (Color){40, 40, 52, 255});
            dCircleLines(h->x, h->y, 9, 1.5f, c);
            dLine(h->x, h->y, ex, ey, 6, c);
            if (h->warn <= 0) dCircle(ex, ey, 5, Fade(WHITE, 0.85f));
        } break;
        case H_LINE:
            if (h->warn > 0) {
                float k = 1.0f - h->warn / h->vr, fl = ((int)(h->warn * 14) & 1) ? 0.15f : 0.0f;
                dLine(h->x, h->y, h->x2, h->y2, 1.0f + k * 2, Fade(c, 0.25f + 0.5f * k + fl));
            } else {
                float k = clampf(h->life / (bl * 0.5f), 0, 1);
                dLine(h->x, h->y, h->x2, h->y2, h->thick * 1.6f, Fade(c, 0.3f * k));
                dLine(h->x, h->y, h->x2, h->y2, h->thick, Fade(c, 0.6f + 0.4f * k));
                dLine(h->x, h->y, h->x2, h->y2, h->thick * 0.35f, Fade(WHITE, 0.9f * k));
            }
            break;
        case H_RECT:
            if (h->warn > 0) {
                float k = 1.0f - h->warn / h->thick;
                float fl = ((int)(h->warn * 14) & 1) ? 0.06f : 0.0f;
                dRect(h->x, h->y, h->w, h->h, Fade(c, 0.05f + 0.22f * k + fl));
                dRectLines(h->x, h->y, h->w, h->h, 1, Fade(c, 0.35f + 0.5f * k));
            } else if (h->flag == 0) {
                float k = clampf(h->life / (bl * 0.55f), 0, 1);
                dRect(h->x, h->y, h->w, h->h, Fade(c, 0.5f + 0.5f * k));
                if (h->w > h->h) dRect(h->x, h->y + h->h * 0.3f, h->w, h->h * 0.4f, Fade(WHITE, 0.9f * k));
                else dRect(h->x + h->w * 0.3f, h->y, h->w * 0.4f, h->h, Fade(WHITE, 0.9f * k));
            } else {
                dRect(h->x, h->y, h->w, h->h, Fade(c, 0.8f));
                dRectLines(h->x, h->y, h->w, h->h, 1, WHITE);
            }
            break;
        case H_RING:
            if (h->warn > 0) {
                float k = 1.0f - h->warn / h->life;
                dCircleLines(h->x, h->y, 4 + 12 * (1 - k), 1, Fade(c, 0.9f));
                dCircle(h->x, h->y, 2 + 2 * k, Fade(c, 0.9f));
            } else {
                dCircleLines(h->x, h->y, h->r, h->thick * 1.6f, Fade(c, 0.25f));
                dCircleLines(h->x, h->y, h->r, h->thick * 0.9f, c);
            }
            break;
        case H_BOMB:
            if (!h->flag) {
                float k = 1.0f - h->warn / h->life;
                if (h->dmg) {
                    dCircle(h->x, h->y, h->r, Fade(c, 0.06f + 0.14f * k));
                    dCircle(h->x, h->y, h->r * k, Fade(c, 0.35f));
                    dCircleLines(h->x, h->y, h->r, 1, Fade(c, 0.4f + 0.6f * k));
                } else {
                    float s = 5 + 3 * ((int)(h->warn * 10) & 1);
                    dPolyLines(h->x, h->y, 4, s, 45 + k * 90, 1, c);
                    dCircleLines(h->x, h->y, 16 * (1 - k) + 5, 1, Fade(c, 0.8f));
                }
            } else if (h->dmg) {
                float k = h->life / h->thick;
                dCircle(h->x, h->y, h->r, Fade(WHITE, 0.9f * k));
                dCircleLines(h->x, h->y, h->r, 1, c);
            }
            break;
        }
    }
}

static int innerSides(int sides) { return sides == 6 ? 3 : (sides == 8 ? 4 : sides); }
static void drawBossShape(float x, float y, int sides, float r, Color c1, Color c2, float t, int hollow) {
    dCircle(x, y, r * 1.8f, Fade(c2, 0.10f));
    dPolyLines(x, y, sides, r, hollow ? -90 + t * 25 : t * 40, hollow ? 3.0f : 2.0f, c2);
    dPolyLines(x, y, innerSides(sides), r * (hollow ? 0.62f : 0.65f), hollow ? 90 - t * 45 : -t * 70, hollow ? 2.0f : 1.5f, c1);
    if (!hollow) dPolyFill(x, y, sides, r * 0.35f, t * 90, Fade(WHITE, 0.85f));   /* HOLLOW stays hollow */
}
static void drawBoss(const Level *L, float pulse) {
    float t = T > 0 ? T : 0;
    if (L->boss == 2 && finPhase) {                     /* Hollow during the finale: bloated and blue once it has eaten the ball */
        float r = 26 + 4 * pulse + (eaten ? 8 : 0);
        if (finPhase == 1 && eaten) r += 8 * expf(-(phT - 2.2f) * 3);
        Color c1 = eaten ? (Color){120, 200, 255, 255} : L->c1, c2 = eaten ? WHITE : L->c2;
        drawBossShape(bossX, bossY, 3, r, c1, c2, t, 1);
        if (hitFlash > 0) dPolyFill(bossX, bossY, 3, r * 0.9f, -90 + t * 25, Fade(WHITE, 0.5f));
        return;
    }
    if (L->boss == 2) {
        const Level *BL = &LV[BOSSIDX[phaseOf(curBeat) > 3 ? 3 : phaseOf(curBeat)]];
        Color c1 = phaseOf(curBeat) == 4 ? L->c1 : BL->c1, c2 = phaseOf(curBeat) == 4 ? L->c2 : BL->c2;
        drawBossShape(bossX, bossY, 3, 26 + 5 * pulse, c1, c2, t, 1);
    } else drawBossShape(bossX, bossY, L->shape, (cur == 13 ? 16 : 20) + 4 * pulse, L->c1, L->c2, t, 0);
}

/* Saw boss ending: a huge saw shoves the boss into the saw machine, which turns him into a saw */
static float sawBossX(float t) {
    if (t < 0.8f) return winX0 + (240 - winX0) * (t / 0.8f);
    if (t < 2.6f) return 240;
    if (t < 4.4f) { float v = (t - 2.6f) / 1.8f; return 240 + (SW - 30 - 240) * v * v; }
    return SW - 30;
}
static float sawPushX(float t) {
    if (t < 0.8f) return -60;
    if (t < 2.6f) { float u = (t - 0.8f) / 1.8f, e = 1 - (1 - u) * (1 - u); return -60 + (240 - 56 + 60) * e; }
    if (t < 4.4f) return sawBossX(t) - 56;
    if (t < 6.0f) { float u = (t - 4.4f) / 1.6f; return (SW - 30 - 56) - (SW - 30 - 56 + 80) * u; }
    return -80;
}
static void sawEndUpdate(void) {
    float t = winT;
    if (t > 2.6f && t < 4.4f) emit(1, sawBossX(t) - 18, 120 + rr(-10, 10), rr(-90, -20), rr(-60, 60), 0.35f, 2, (Color){255, 200, 90, 255}, 0, 0);
    if (t > 4.4f && t < 5.0f) emit(1, SW - 40, 120 + rr(-25, 25), rr(-120, -30), rr(-80, 80), 0.4f, 2, (Color){255, 170, 60, 255}, 0, 0);
    static int burstDone = 0;
    if (t < 0.1f) burstDone = 0;
    if (t >= 5.0f && !burstDone) { burstDone = 1; burst(SW - 30, 120, 60, 80, 500, 1, (Color){255, 170, 60, 255}, 0.9f, 5); shake = 6; }
}
static void drawSawEnd(void) {
    float t = winT, fl = ((int)(t * 12) & 1) ? 0.1f : 0.0f;
    Color steel = {190, 195, 210, 255}, dark = {45, 46, 56, 255}, orange = {255, 150, 40, 255};
    dRect(SW - 40, 60, 40, 120, dark);                                   /* the saw machine */
    dSaw(SW - 40, 85, 20, t * 9, steel); dSaw(SW - 40, 155, 20, -t * 9, steel);
    dRectLines(SW - 40, 60, 40, 120, 1, Fade(orange, 0.6f + fl));
    if (t > 4.4f) dRect(SW - 40, 60, 40, 120, Fade(orange, 0.18f + fl));
    float bx = sawBossX(t), by = t < 0.8f ? winY0 + (120 - winY0) * (t / 0.8f) : 120;
    if (t < 4.4f) drawBossShape(bx, by, 4, 16, LV[13].c1, LV[13].c2, t, 0);
    else if (t < 5.0f) drawBossShape(bx, by, 4, 16 * (1 - (t - 4.4f) / 0.6f), LV[13].c1, LV[13].c2, t, 0);
    float px = sawPushX(t);
    if (px > -60) dSaw(px, 120, 34, t * 10, steel);                       /* the pusher */
    if (t >= 5.0f) {                                                       /* the boss, now a saw, rolls out */
        float u = clampf((t - 5.0f) / 1.6f, 0, 1), e = 1 - (1 - u) * (1 - u);
        dSaw(SW - 50 - (SW - 50 - 200) * e, 120, 22, t * 10, LV[13].c2);
    }
    if (t > 5.6f) textC(SW / 2, 40, 0.5f, Fade(WHITE, clampf((t - 5.6f) * 2, 0, 1)), "THE BOSS BECAME A SAW");
}

static void drawShieldRing(float x, float y, float r, float rot, float a) {     /* the spinning blue ring */
    Color b2 = {90, 190, 255, 255};
    for (int i = 0; i < 6; i++) {
        float a0 = rot + i * PI2 / 6;
        for (int k = 0; k < 3; k++) {
            float t0 = a0 + k * 0.2f, t1 = a0 + (k + 1) * 0.2f;
            dLine(x + cosf(t0) * r, y + sinf(t0) * r, x + cosf(t1) * r, y + sinf(t1) * r, 2.0f, Fade(b2, a));
        }
    }
    dCircleLines(x, y, r + 2, 1, Fade(b2, 0.2f * a));
}
static void drawPlayer(void) {
    Color c = LV[cur].c1;
    if (P.dashT > 0) c = WHITE;
    if (healT > 0) { c.r = 120; c.g = 200; c.b = 255; }
    float pulse = T > 0 ? expf(-fmodf(T / bl, 1.0f) * 4) : 0;
    if (finPhase >= 4) drawShieldRing(P.x, P.y, finPhase == 4 ? 6 + 14 * ss(phT / 0.7f) : 15, T * 6, 0.9f);
    if (finPhase == 4 && phT < 2.0f) return;                            /* shattered: only the ring remains */
    dCircle(P.x, P.y, 11 + 3 * pulse, Fade(c, 0.18f));
    if (!finPhase && P.inv > 0 && P.dashT <= 0 && ((int)(P.inv * 16) & 1)) return;   /* blink after damage */
    float deg = P.ang * 180.0f / PIF;
    dPolyFill(P.x, P.y, 3, 7, deg, Fade(c, 0.35f));
    dPolyLines(P.x, P.y, 3, 7, deg, 1.5f, c);
    dRect(P.x - 1, P.y - 1, 2, 2, WHITE);
    if (finPhase == 6 && charge > 0.02f) {                              /* beam charge orb */
        float k = clampf(charge / 1.2f, 0, 1);
        Color oc = k >= 1 ? WHITE : (Color){90, 190, 255, 255};
        dCircle(P.x, P.y - 12, 4 + 10 * k, Fade(oc, 0.22f));
        dCircle(P.x, P.y - 12, 2 + 6 * k, Fade(oc, 0.85f));
    }
    if (P.dashCD > 0) {                       /* cooldown arc */
        float k = 1.0f - P.dashCD / dashCd();
        int n = (int)(16 * k);
        for (int i = 0; i < n; i++) {
            float a = -PIF / 2 + i * PI2 / 16;
            dRect(P.x + cosf(a) * 10 - 0.5f, P.y + sinf(a) * 10 - 0.5f, 1.5f, 1.5f, Fade(WHITE, 0.6f));
        }
    }
}

static void drawBallIcon(float x, float y, float r) {
    Color b2 = {70, 160, 255, 255};
    float pulse = 0.5f + 0.5f * sinf(menuT * 6 + T * 6);
    if (ballCD <= 0) { dCircle(x, y, r * (1.5f + 0.2f * pulse), Fade(b2, 0.25f)); dCircle(x, y, r, b2); dCircle(x - r * 0.3f, y - r * 0.3f, r * 0.3f, Fade(WHITE, 0.8f)); }
    else if (healT > 0) { dCircle(x, y, r * 1.6f, Fade(b2, 0.3f)); dCircle(x, y, r, Fade(WHITE, 0.9f)); }
    else { dCircle(x, y, r, mul(b2, 0.35f)); dCircle(x, y, r * (1.0f - ballCD / 60.0f), Fade(b2, 0.7f)); }
}

static void drawHUD(const Level *L) {
    float total = L->beats * bl, prog = clampf(T / total, 0, 1);
    dRect(0, 0, SW, 2, Fade(WHITE, 0.1f));
    dRect(0, 0, SW * prog, 2, L->c1);
    for (int i = 0; i < P.maxhp; i++) {
        if (i < P.hp) dRect(6 + i * 10, 7, 7, 7, (Color){255, 60, 90, 255});
        else dRectLines(6 + i * 10, 7, 7, 7, 1, Fade(WHITE, 0.3f));
    }
    char buf[64];
    snprintf(buf, sizeof buf, "%d  %s", cur + 1, L->name);
    text(6, 15, 0.4f, Fade(WHITE, 0.8f), buf);
    text(SW - 62, 4, 0.4f, hard ? (Color){255, 70, 80, 255} : Fade(WHITE, 0.6f), hard ? "HARDCORE" : "NORMAL");
    if (casual) text(SW - 50, 15, 0.4f, (Color){120, 255, 160, 255}, "CASUAL");
    if (L->boss) {
        float bw = 170, bx = (SW - bw) / 2;
        float fillv = (cur == ULT && finPhase) ? (finPhase == 6 ? clampf(finHp / (float)finMax, 0, 1) : 1.0f) : (1 - prog);
        dRect(bx, 6, bw, 5, Fade(WHITE, 0.12f));
        dRect(bx, 6, bw * fillv, 5, (cur == ULT && finPhase && eaten) ? (Color){110, 190, 255, 255} : L->c2);
        dRectLines(bx, 6, bw, 5, 1, WHITE);
        textC(SW / 2, 11, 0.4f, Fade(WHITE, 0.8f), L->name);
    }
    if (cur == ULT && !finPhase) {                          /* blue ball item */
        drawBallIcon(14, SH - 16, 7);
        if (healT > 0) { dRect(26, SH - 20, 60 * (healT / 20.0f), 4, (Color){70, 160, 255, 255}); text(26, SH - 16, 0.3f, WHITE, "HEALING"); }
        else if (ballCD > 0) { char cb[16]; snprintf(cb, sizeof cb, "%ds", (int)ceilf(ballCD)); text(26, SH - 22, 0.35f, Fade(WHITE, 0.6f), cb); }
        else text(26, SH - 22, 0.35f, Fade(WHITE, 0.85f), "Y: BLUE BALL");
    }
    if (cur == ULT && finPhase) {
        const char *msg = NULL; Color mc = WHITE;
        switch (finPhase) {
        case 1: if (phT > 2.4f) msg = "HOLLOW ATE THE BLUE BALL"; mc = (Color){120, 200, 255, 255}; break;
        case 2: msg = "THE SAWS ARE COMING DOWN"; break;
        case 3: msg = "NOWHERE LEFT TO GO"; mc = (Color){255, 90, 100, 255}; break;
        case 4: if (phT > 1.0f) msg = "..."; break;
        case 5: msg = "BACK ON YOUR FEET - NO DAMAGE"; mc = (Color){120, 200, 255, 255}; break;
        case 6: if (hintT < 9.0f) msg = "R: SHOOT    HOLD R: CHARGE"; mc = (Color){120, 200, 255, 255}; break;
        }
        if (msg) textC(SW / 2, 208, 0.5f, mc, msg);
    }
    if (cpMsg > 0) textC(SW / 2, 30, 0.6f, Fade((Color){215, 215, 220, 255}, clampf(cpMsg, 0, 1)), "CHECKPOINT");
    if (T < 0) {
        char n[8]; snprintf(n, sizeof n, "%d", (int)ceilf(-T / bl));
        textC(SW / 2, 60, 0.7f, WHITE, "GET READY");
        textC(SW / 2, 90, 1.3f, L->c1, n);
        textC(SW / 2, 160, 0.4f, Fade(WHITE, 0.6f), "Circle Pad move   A/B/L/R dash (on the beat = PERFECT)");
    }
}

static void overlayTop(const char *big, Color c, const char *l1) {
    dRect(0, 0, SW, SH, Fade(BLACK, 0.55f));
    textC(SW / 2, 70, 1.1f, c, big);
    textC(SW / 2, 130, 0.5f, WHITE, l1);
}

static void drawCrusher(void) {                          /* saw curtain from the top + the crusher plates */
    Color steel = {70, 74, 88, 255}, dark = {30, 32, 40, 255}, orange = {255, 150, 40, 255}, blade = {190, 195, 210, 255};
    if (curtainY > 1) {
        dRect(0, 0, SW, curtainY - 10, dark);
        for (float y = 12; y < curtainY - 12; y += 24) dRect(0, y, SW, 2, Fade(steel, 0.7f));
        for (int r = 0; r < 2; r++)
            for (float x = r ? 14.0f : 0.0f; x < SW + 28; x += 28) dSaw(x, curtainY - 6 - r * 13, 14, (r ? -1 : 1) * T * 9, blade);
    }
    float Le = crushL + 10, Re = SW - crushL - 10, top = curtainY + 8;
    if (Le > 0) {
        dRect(0, top, Le, SH - top, steel); dRect(Re, top, SW - Re, SH - top, steel);
        dRectLines(0, top, Le, SH - top, 1, Fade(orange, 0.8f)); dRectLines(Re, top, SW - Re, SH - top, 1, Fade(orange, 0.8f));
        for (float y = top; y < SH - 8; y += 14) {
            dTri(Le, y, Le, y + 14, Le + 9, y + 7, orange);
            dTri(Re, y, Re, y + 14, Re - 9, y + 7, orange);
        }
    }
}
static void drawBallFlight(void) {                        /* the blue ball on its way into Hollow */
    Color b2 = {70, 160, 255, 255};
    if (phT >= 2.8f) return;
    float bx, by, r;
    if (phT < 0.6f) { bx = 14; by = SH - 16; r = 7 + 2 * ss(phT / 0.6f); }
    else if (phT < 2.2f) { ballPos((phT - 0.6f) / 1.6f, &bx, &by); r = 9; }
    else { bx = bossX; by = bossY; r = 9 * (1 - (phT - 2.2f) / 0.6f); }
    dCircle(bx, by, r * 1.7f, Fade(b2, 0.28f)); dCircle(bx, by, r, b2); dCircle(bx - r * 0.3f, by - r * 0.3f, r * 0.3f, Fade(WHITE, 0.85f));
}
static void drawBeams(void) {
    Color b2 = {90, 190, 255, 255};
    for (int i = 0; i < 8; i++) {
        if (BM[i].life <= 0) continue;
        float a = BM[i].life / BM[i].max, w = BM[i].w * (0.5f + 0.7f * a);
        dRect(BM[i].x - w / 2 - 2, 0, w + 4, BM[i].y, Fade(b2, 0.25f * a));
        dRect(BM[i].x - w / 2, 0, w, BM[i].y, Fade(b2, 0.85f * a));
        dRect(BM[i].x - w * 0.2f, 0, w * 0.4f, BM[i].y, Fade(WHITE, 0.95f * a));
    }
}
static void drawPlayScene(void) {
    const Level *L = &LV[cur];
    float fr = T / bl - floorf(T / bl);
    float pulse = expf(-fr * 4.0f);
    OX = rr(-1, 1) * shake; OY = rr(-1, 1) * shake;
    if (inSawPhase()) OY += sinf(T * 0.7f) * 2.5f;           /* the "camera" sways as you move through the factory */
    drawBG(L, T, pulse);
    if (inGridPhase()) drawLattice(cur == ULT ? &LV[BOSSIDX[2]] : L, pulse);
    if (inSawPhase()) drawFactory(T);
    if (L->boss && state != ST_WIN) drawBoss(L, pulse);
    if (finPhase == 1) drawBallFlight();
    if (state == ST_WIN && cur == 13) drawSawEnd();
    drawHazards();
    if (finPhase >= 2 && finPhase <= 4) drawCrusher();
    {                                                      /* BASSLINE: squeezing side walls */
        float in = squeezeInset();
        if (in > 0.5f) {
            Color dk = {24, 22, 34, 255};
            dRect(0, 0, in, SH, dk); dRect(SW - in, 0, in, SH, dk);
            for (float y = 0; y < SH; y += 16) { dRect(in - 3, y, 3, 8, Fade(L->c2, 0.85f)); dRect(SW - in, y, 3, 8, Fade(L->c2, 0.85f)); }
        }
    }
    if (CP.on) {                                           /* checkpoint: grey circle that expands until you touch it / it fills the screen */
        Color g = {200, 200, 205, 255};
        dCircle(CP.x, CP.y, CP.r, Fade(g, 0.14f));
        dCircleLines(CP.x, CP.y, CP.r, 2, Fade(g, 0.75f));
        dCircle(CP.x, CP.y, 3, Fade(g, 0.9f));
    }
    if (state != ST_OVER && !finPhase && (L->gim & GIM_DARK)) dCircle(P.x, P.y, 46, Fade(L->c1, 0.07f));   /* your little light */
    if (state != ST_OVER) drawPlayer();
    drawBeams();
    drawParticles();
    OX = OY = 0;
    if (finFlash > 0.01f) dRect(0, 0, SW, SH, Fade((Color){140, 200, 255, 255}, clampf(finFlash, 0, 1) * 0.55f));
    if (healT > 0) dRect(0, 0, SW, SH, Fade((Color){60, 140, 255, 255}, 0.10f));
    if (flash > 0.01f) dRect(0, 0, SW, SH, Fade((Color){255, 30, 40, 255}, flash * 0.35f));
    drawHUD(L);
}

/* ---------------- ULTIMATE BOSS CUTSCENE: the four bosses fuse into a hollow triangle ---------------- */
static const float HX[4] = {75, 325, 75, 325}, HY[4] = {55, 55, 185, 185};
static void startCutscene(void) {
    cur = ULT; bl = 60.0f / LV[cur].bpm;
    memset(HZ, 0, sizeof(HZ)); memset(PT, 0, sizeof(PT));
    cutT = 0; whiteF = 0; shake = 0; flash = 0; cutFused = 0; cutTick = -1;
    memset(cutFired, 0, sizeof cutFired); memset(cutBeam, 0, sizeof cutBeam);
    T = 0; curBeat = 0; healT = 0; vidMode = 0;
    state = ST_CUT;
}
static void bossPos(int i, float t, float *x, float *y, float *r) {
    float cx = SW / 2.0f, cy = SH / 2.0f, dx = HX[i] - cx, dy = HY[i] - cy;
    *r = 16;
    if (t < 1.4f) {                                        /* fly in from off-screen */
        float u = t / 1.4f, e = 1 - (1 - u) * (1 - u) * (1 - u), off = 1.0f + (1 - e) * 1.6f;
        *x = cx + dx * off; *y = cy + dy * off;
    } else if (t < 5.8f) { *x = HX[i]; *y = HY[i] + sinf(t * 2 + i) * 3; }      /* each boss shows its attack */
    else {                                                 /* spiral into the centre */
        float u = clampf((t - 5.8f) / 3.0f, 0, 1), e = u * u, ang = e * 7.0f, vx = dx * (1 - e), vy = dy * (1 - e);
        *x = cx + vx * cosf(ang) - vy * sinf(ang); *y = cy + vx * sinf(ang) + vy * cosf(ang);
        *r = 16 * (1 - 0.35f * e);
    }
}
static void fireDemo(int i, float x, float y, Color c1, Color c2) {    /* each boss's signature attack (visual only) */
    if (i == 0) {                                          /* METRONOME: bullet ring */
        for (int k = 0; k < 20; k++) { float a = k * PI2 / 20; emit(1, x, y, cosf(a) * 135, sinf(a) * 135, 0.9f, 3, k & 1 ? c1 : c2, 0, 0); }
        emit(2, x, y, 0, 0, 0.7f, 8, c1, 176, 0);
    } else if (i == 1) {                                   /* AFTERBEAT: spirals */
        for (int arm = 0; arm < 5; arm++) for (int j = 0; j < 7; j++) {
            float a = arm * PI2 / 5 + j * 0.3f, sp = 50 + j * 19; emit(1, x, y, cosf(a) * sp, sinf(a) * sp, 1.0f, 3, j & 1 ? c1 : c2, 0, 0); }
        emit(2, x, y, 0, 0, 0.8f, 8, c2, 160, 0);
    } else if (i == 2) {                                   /* TRIGRID: lattice beams */
        cutBeam[i] = 0.6f;
        for (int k = 0; k < 3; k++) { float a = k * PIF / 3;
            for (int s = 0; s < 10; s++) { emit(1, x, y, cosf(a) * 340, sinf(a) * 340, 0.6f, 3, s & 1 ? c1 : c2, 0, 0); emit(1, x, y, -cosf(a) * 340, -sinf(a) * 340, 0.6f, 3, s & 1 ? c2 : c1, 0, 0); } }
    } else {                                               /* SAWMILL: thrown saws (sparks) */
        for (int j = 0; j < 12; j++) { float a = j * PI2 / 12; emit(1, x, y, cosf(a) * 150, sinf(a) * 150, 1.0f, 3, (Color){255, 200, 90, 255}, 0, 0); }
        emit(2, x, y, 0, 0, 0.9f, 4, c2, 150, 0); emit(2, x, y, 0, 0, 1.1f, 4, c1, 120, 0);
    }
}
static void updateCut(float dt) {
    cutT += dt;
    if ((kDown & (KEY_A | KEY_START)) && cutT > 0.8f) { startLevel(ULT, 0); return; }
    for (int i = 0; i < 4; i++) {
        float x, y, r; bossPos(i, cutT, &x, &y, &r);
        Color c1 = LV[BOSSIDX[i]].c1, c2 = LV[BOSSIDX[i]].c2;
        if (cutT < 8.8f && !cutFired[i] && cutT >= 1.9f + i * 1.05f) {
            cutFired[i] = 1; fireDemo(i, x, y, c1, c2);
            play(drum[S_KICK], 0.9f); play(drum[S_SNARE], 0.6f); shake += 3;
        }
        if (cutBeam[i] > 0) cutBeam[i] -= dt;
        if (cutT >= 5.8f && cutT < 8.8f) emit(0, x, y, (SW / 2 - x) * 1.6f, (SH / 2 - y) * 1.6f, 0.4f, 2.5f, c1, 0, 0);
    }
    int tk = (int)(cutT * 4);
    if (tk != cutTick) {
        cutTick = tk;
        if (cutT >= 5.8f && cutT < 8.8f) { play(drum[S_HAT], 0.5f); if (tk & 1) play(drum[S_KICK], 0.8f); }
        else if (cutT < 5.8f && tk % 4 == 0) play(drum[S_KICK], 0.4f);
    }
    if (cutT >= 5.8f && cutT < 8.8f) { float u = (cutT - 5.8f) / 3.0f; shake = 1.5f + u * u * 6; }
    if (cutT >= 8.8f && !cutFused) {                       /* FUSION */
        cutFused = 1; whiteF = 1; shake = 12;
        burst(SW / 2, SH / 2, 120, 100, 1000, 1, WHITE, 1.4f, 6);
        for (int i = 0; i < 4; i++) burst(SW / 2, SH / 2, 30, 100, 800, 1, LV[BOSSIDX[i]].c2, 1.2f, 5);
        emit(2, SW / 2, SH / 2, 0, 0, 1.0f, 8, WHITE, 380, 0);
        emit(2, SW / 2, SH / 2, 0, 0, 1.4f, 8, LV[ULT].c2, 250, 0);
        play(drum[S_HIT], 1.0f); play(drum[S_CLEAR], 0.8f);
    }
    if (cutFused) shake *= expf(-dt * 4);
    whiteF *= expf(-dt * 2.2f);
    stepParticles(dt);
    if (cutT >= 13.6f) startLevel(ULT, 0);
}
static void drawCutscene(void) {
    const Level *L = &LV[ULT];
    float pulse = expf(-fmodf(cutT, 0.5f) * 8.0f);
    OX = rr(-1, 1) * shake; OY = rr(-1, 1) * shake;
    dRect(0, 0, SW, SH, (Color){6, 4, 12, 255});
    Color g = Fade(L->c2, 0.05f + 0.05f * pulse);
    for (int x = 0; x <= SW; x += 20) dRect(x, 0, 1, SH, g);
    for (int y = 0; y <= SH; y += 20) dRect(0, y, SW, 1, g);
    if (cutT < 8.8f)
        for (int i = 0; i < 4; i++) {
            float x, y, r; bossPos(i, cutT, &x, &y, &r);
            const Level *BL = &LV[BOSSIDX[i]];
            drawBossShape(x, y, BL->shape, r, BL->c1, BL->c2, cutT, 0);
            if (cutT < 5.8f) text(x - textW(BL->name, 0.35f) / 2, y - 34, 0.35f, Fade(WHITE, 0.8f * clampf((cutT - 1.4f) * 2, 0, 1)), BL->name);
            if (cutBeam[i] > 0) {
                float k = cutBeam[i] / 0.6f;
                for (int f = 0; f < 3; f++) {
                    float a = f * PIF / 3;
                    dLine(x - cosf(a) * 500, y - sinf(a) * 500, x + cosf(a) * 500, y + sinf(a) * 500, 8 * k, Fade(BL->c1, 0.7f * k));
                }
            }
        }
    drawParticles();
    if (cutFused) {                                        /* the hollow triangle */
        float u = clampf((cutT - 8.8f) / 1.2f, 0, 1);
        float e = 1 + 2.70158f * powf(u - 1, 3) + 1.70158f * powf(u - 1, 2);   /* ease-out-back */
        float r = 66 * e, rot = -90 + cutT * 25;
        dCircle(SW / 2, SH / 2 - 8, r * 1.5f, Fade(L->c2, 0.10f + 0.05f * pulse));
        dPolyLines(SW / 2, SH / 2 - 8, 3, r, rot, 4, WHITE);
        dPolyLines(SW / 2, SH / 2 - 8, 3, r * 1.05f, rot, 1.5f, L->c2);
        dPolyLines(SW / 2, SH / 2 - 8, 3, r * 0.62f, -rot, 2, L->c2);   /* hollow: outline only */
    }
    OX = OY = 0;
    if (cutT < 1.0f) dRect(0, 0, SW, SH, Fade(BLACK, 1.0f - cutT));
    if (whiteF > 0.01f) dRect(0, 0, SW, SH, Fade(WHITE, whiteF));
    if (cutT > 0.8f && cutT < 5.6f) textC(SW / 2, 108, 0.5f, Fade(WHITE, clampf((cutT - 0.8f) * 2, 0, 0.9f)), "FOUR BOSSES. ONE BEAT.");
    if (cutT > 5.8f && cutT < 8.8f) textC(SW / 2, 214, 0.5f, (Color){255, 80, 100, 255}, "THEY'RE COMBINING...");
    if (cutT > 10.0f) textC(SW / 2, 170, 1.0f, Fade(WHITE, clampf((cutT - 10.0f) * 1.5f, 0, 1)), "H O L L O W");
    if (cutT > 10.8f) textC(SW / 2, 212, 0.5f, Fade(L->c2, clampf((cutT - 10.8f) * 1.5f, 0, 1)), "ULTIMATE BOSS");
    if (cutT > 0.8f) text(SW - 68, SH - 14, 0.3f, Fade(WHITE, 0.4f), "A: skip");
}

/* ------------------------------------------------------------------- menu */

/* =====================================================================================
 * VIDEO LEVEL EDITOR
 *  - import an .abv video (made on your PC with tools/vid2abv.py), copied to SD:/3ds/afterbeat/videos/
 *  - pick the colours that HURT (eyedropper on the video, or an HSV colour picker) + a tolerance
 *  - PLAY: the video is the level; any pixel matching a picked colour damages you
 * ===================================================================================== */
#define VMAXW 200
#define VMAXH 120
#define VMAXRECT 3500
#define VDIR "sdmc:/3ds/afterbeat/videos"
#define ASAMP 11025
typedef struct { unsigned char x, y, w, h; unsigned short c; } VRect;
static FILE *vf, *af;
static char vname[64], vpath[160];
static int vw, vh, vfps, vframeIdx = -1, vAudioOn, vAudioStarted;
static unsigned vframes, vaudioBytes, aRemain;
static long vdataOff = 18, vaudioOff;
static unsigned short vpix[VMAXW * VMAXH];
static unsigned char vmask[VMAXW * VMAXH], vbuf[VMAXW * VMAXH * 2];
static VRect vrect[VMAXRECT], vmrect[VMAXRECT];
static int nvrect, nvmrect;
static Color vcol[6];
static int nvcol, vtol = 35;
static float vT;
static Color pickCol = {255, 0, 0, 255};
static float pickH = 0, pickS = 1, pickV = 1;
static int pickMode, edFrame, edDrag;
static char files[32][64];
static int nfiles, fsel;
static int touchHeld, hx, hy;
static s16 *abuf[2];
static ndspWaveBuf awb[2];

static Color c565(unsigned short c) {
    int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    Color o = {(unsigned char)((r << 3) | (r >> 2)), (unsigned char)((g << 2) | (g >> 4)), (unsigned char)((b << 3) | (b >> 2)), 255};
    return o;
}
static Color hsv2rgb(float h, float sat, float v) {
    h = fmodf(h, 360.0f); if (h < 0) h += 360.0f;
    float c = v * sat, x = c * (1 - fabsf(fmodf(h / 60.0f, 2) - 1)), m = v - c, r = 0, g = 0, b = 0;
    if (h < 60) { r = c; g = x; } else if (h < 120) { r = x; g = c; } else if (h < 180) { g = c; b = x; }
    else if (h < 240) { g = x; b = c; } else if (h < 300) { r = x; b = c; } else { r = c; b = x; }
    Color o = {(unsigned char)((r + m) * 255 + 0.5f), (unsigned char)((g + m) * 255 + 0.5f), (unsigned char)((b + m) * 255 + 0.5f), 255};
    return o;
}
static void rgb2hsv(Color c, float *h, float *sat, float *v) {
    float r = c.r / 255.0f, g = c.g / 255.0f, b = c.b / 255.0f, mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b)), d = mx - mn;
    *v = mx; *sat = mx > 0 ? d / mx : 0;
    if (d < 1e-6f) *h = 0;
    else if (mx == r) *h = 60 * fmodf((g - b) / d, 6);
    else if (mx == g) *h = 60 * ((b - r) / d + 2);
    else *h = 60 * ((r - g) / d + 4);
    if (*h < 0) *h += 360;
}

/* ---- streamed video audio (ndsp channel 23, two half-second buffers) ---- */
static void aInit(void) {
    if (!audio) return;
    abuf[0] = (s16 *)linearAlloc(ASAMP * 2); abuf[1] = (s16 *)linearAlloc(ASAMP * 2);
    float mix[12]; memset(mix, 0, sizeof mix); mix[0] = mix[1] = 0.9f;
    ndspChnReset(ACH); ndspChnSetInterp(ACH, NDSP_INTERP_LINEAR); ndspChnSetRate(ACH, (float)SR);
    ndspChnSetFormat(ACH, NDSP_FORMAT_MONO_PCM16); ndspChnSetMix(ACH, mix);
}
static void aStop(void) {
    if (vAudioOn && audio) ndspChnWaveBufClear(ACH);
    if (af) { fclose(af); af = NULL; }
    vAudioOn = 0;
}
static void aPause(int p) { if (audio && vAudioOn) ndspChnSetPaused(ACH, p != 0); }
static void aPump(void) {
    if (!audio || !vAudioOn || !af) return;
    for (int i = 0; i < 2; i++) {
        if (awb[i].status != NDSP_WBUF_DONE && awb[i].status != NDSP_WBUF_FREE) continue;
        if (aRemain == 0) continue;
        size_t n = aRemain < ASAMP * 2 ? aRemain : ASAMP * 2;
        size_t got = fread(abuf[i], 1, n, af);
        if (got < 2) { aRemain = 0; continue; }
        aRemain -= (unsigned)got;
        DSP_FlushDataCache(abuf[i], (u32)got);
        memset(&awb[i], 0, sizeof(ndspWaveBuf));
        awb[i].data_vaddr = abuf[i]; awb[i].nsamples = (u32)(got / 2);
        ndspChnWaveBufAdd(ACH, &awb[i]);
    }
}
static void aStart(void) {
    aStop();
    if (!audio || !vaudioBytes) return;
    af = fopen(vpath, "rb");
    if (!af) return;
    fseek(af, vaudioOff, SEEK_SET);
    aRemain = vaudioBytes; memset(awb, 0, sizeof awb); vAudioOn = 1;
    ndspChnSetPaused(ACH, false);
    aPump();
}

/* ---- video decoding: RGB565 frames -> merged rectangles (fast to draw) + a damage mask ---- */
static void recomputeMask(void) {
    float thr = vtol * 4.42f, thr2 = thr * thr;                  /* tolerance 0-100 -> RGB distance 0-442 */
    int n = vw * vh;
    for (int i = 0; i < n; i++) {
        Color c = c565(vpix[i]);
        unsigned char m = 0;
        for (int k = 0; k < nvcol && !m; k++) {
            float dr = (float)c.r - vcol[k].r, dg = (float)c.g - vcol[k].g, db = (float)c.b - vcol[k].b;
            if (dr * dr + dg * dg + db * db <= thr2) m = 1;
        }
        vmask[i] = m;
    }
}
/* greedy cover: merge equal-coloured cells (or, with onlyMask, mask cells) into as few rectangles as possible */
static int cover(const unsigned short *src, int onlyMask, VRect *out, int cap, int *ovf) {
    static unsigned char used[VMAXW * VMAXH];
    int n = 0, W = vw, H = vh;
    *ovf = 0;
    memset(used, 0, (size_t)W * H);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int i = y * W + x;
            if (used[i] || (onlyMask && !vmask[i])) continue;
            unsigned short c = src ? src[i] : 0;
            int w = 1, h = 1;
            while (x + w < W) { int j = i + w; if (used[j] || (onlyMask && !vmask[j]) || (src && src[j] != c)) break; w++; }
            while (y + h < H) {
                int ok = 1;
                for (int k = 0; k < w; k++) { int j = (y + h) * W + x + k; if (used[j] || (onlyMask && !vmask[j]) || (src && src[j] != c)) { ok = 0; break; } }
                if (!ok) break;
                h++;
            }
            for (int yy = 0; yy < h; yy++) for (int xx = 0; xx < w; xx++) used[(y + yy) * W + x + xx] = 1;
            if (n >= cap) { *ovf = 1; continue; }
            out[n].x = (unsigned char)x; out[n].y = (unsigned char)y; out[n].w = (unsigned char)w; out[n].h = (unsigned char)h; out[n].c = c;
            n++;
        }
    return n;
}
static void buildRects(void) {
    static unsigned short q[VMAXW * VMAXH];
    int n = vw * vh, ovf = 0;
    for (int sh = 0; sh <= 4; sh++) {                              /* posterize more and more until it fits the rectangle budget */
        for (int i = 0; i < n; i++) {
            unsigned short c = vpix[i];
            int r = (((c >> 11) & 31) >> sh) << sh, g = (((c >> 5) & 63) >> sh) << sh, b = ((c & 31) >> sh) << sh;
            q[i] = (unsigned short)((r << 11) | (g << 5) | b);
        }
        nvrect = cover(q, 0, vrect, VMAXRECT, &ovf);
        if (!ovf) break;
    }
}
static void refreshMask(void) { int ovf; recomputeMask(); nvmrect = cover(NULL, 1, vmrect, VMAXRECT, &ovf); }
static void loadFrame(int idx) {
    if (!vf || vframes == 0) return;
    if (idx < 0) idx = 0;
    if (idx >= (int)vframes) idx = (int)vframes - 1;
    if (idx == vframeIdx) return;
    size_t fb = (size_t)vw * vh * 2;
    fseek(vf, vdataOff + (long)idx * (long)fb, SEEK_SET);
    if (fread(vbuf, 1, fb, vf) != fb) memset(vbuf, 0, fb);
    for (int i = 0; i < vw * vh; i++) vpix[i] = (unsigned short)(vbuf[2 * i] | (vbuf[2 * i + 1] << 8));
    vframeIdx = idx;
    buildRects();
    refreshMask();
}

/* ---- config (.abc next to the .abv): tolerance + damaging colours ---- */
static void cfgPath(char *out, size_t n) {
    snprintf(out, n, "%s", vpath);
    char *dot = strrchr(out, '.');
    if (dot) strcpy(dot, ".abc"); else strncat(out, ".abc", n - strlen(out) - 1);
}
static void saveCfg(void) {
    char path[170]; cfgPath(path, sizeof path);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "tol=%d\n", vtol);
    for (int i = 0; i < nvcol; i++) fprintf(f, "color=%d,%d,%d\n", vcol[i].r, vcol[i].g, vcol[i].b);
    fclose(f);
}
static void loadCfg(void) {
    char path[170], line[96]; cfgPath(path, sizeof path);
    nvcol = 0; vtol = 35;
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        int t, r, g, b;
        if (sscanf(line, "tol=%d", &t) == 1) vtol = (int)clampf((float)t, 0, 100);
        else if (sscanf(line, "color=%d,%d,%d", &r, &g, &b) == 3 && nvcol < 6) {
            Color c = {(unsigned char)r, (unsigned char)g, (unsigned char)b, 255}; vcol[nvcol++] = c;
        }
    }
    fclose(f);
}

static int openVideo(const char *name) {
    if (vf) { fclose(vf); vf = NULL; }
    aStop();
    snprintf(vname, sizeof vname, "%s", name);
    snprintf(vpath, sizeof vpath, "%s/%s", VDIR, name);
    vf = fopen(vpath, "rb");
    if (!vf) return 0;
    unsigned char h[18];
    if (fread(h, 1, 18, vf) != 18 || memcmp(h, "ABV1", 4) != 0) { fclose(vf); vf = NULL; return 0; }
    vw = h[4] | (h[5] << 8); vh = h[6] | (h[7] << 8); vfps = h[8] | (h[9] << 8);
    vframes = (unsigned)h[10] | ((unsigned)h[11] << 8) | ((unsigned)h[12] << 16) | ((unsigned)h[13] << 24);
    vaudioBytes = (unsigned)h[14] | ((unsigned)h[15] << 8) | ((unsigned)h[16] << 16) | ((unsigned)h[17] << 24);
    if (vw < 8 || vw > VMAXW || vh < 8 || vh > VMAXH || vfps < 1 || vfps > 60 || vframes == 0) { fclose(vf); vf = NULL; return 0; }
    vdataOff = 18; vaudioOff = vdataOff + (long)vframes * vw * vh * 2;
    vframeIdx = -1; edFrame = 0; edDrag = 0;
    loadCfg();
    loadFrame(0);
    return 1;
}
static void scanVideos(void) {
    mkdir("sdmc:/3ds/afterbeat", 0777); mkdir(VDIR, 0777);
    nfiles = 0;
    DIR *d = opendir(VDIR);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && nfiles < 32) {
        size_t L = strlen(e->d_name);
        if (L > 4 && L < 60 && strcmp(e->d_name + L - 4, ".abv") == 0) strcpy(files[nfiles++], e->d_name);
    }
    closedir(d);
    for (int i = 0; i < nfiles; i++)
        for (int j = i + 1; j < nfiles; j++)
            if (strcmp(files[i], files[j]) > 0) { char t[64]; strcpy(t, files[i]); strcpy(files[i], files[j]); strcpy(files[j], t); }
}
static void openEditor(void) { scanVideos(); fsel = 0; vidMode = 0; state = ST_EDFILES; }

/* ---- drawing the video ---- */
static void drawVideoRects(float x0, float y0, float sx, float sy, int mask) {
    if (!mask) {
        for (int i = 0; i < nvrect; i++) dRect(x0 + vrect[i].x * sx, y0 + vrect[i].y * sy, vrect[i].w * sx, vrect[i].h * sy, c565(vrect[i].c));
    } else {
        Color mk = {255, 40, 60, 255};
        float a = 0.26f + 0.10f * sinf(menuT * 8.0f);
        for (int i = 0; i < nvmrect; i++) dRect(x0 + vmrect[i].x * sx, y0 + vmrect[i].y * sy, vmrect[i].w * sx, vmrect[i].h * sy, Fade(mk, a));
    }
}
static int previewScale(void) { int a = 200 / vw, b = 120 / vh; int s = a < b ? a : b; return s < 1 ? 1 : s; }

/* ---- the file list ---- */
static void updateFiles(float dt) {
    menuT += dt;
    if (kDown & KEY_DDOWN) { if (fsel + 1 < nfiles) fsel++; }
    if (kDown & KEY_DUP) { if (fsel > 0) fsel--; }
    if (kDown & KEY_B) { state = ST_MENU; return; }
    int open = (kDown & KEY_A) != 0;
    if (touchDown) {
        int first = fsel < 7 ? 0 : fsel - 6;
        for (int r = 0; r < 8; r++) {
            int i = first + r;
            if (i < nfiles && tx >= 8 && tx < 312 && ty >= 30 + r * 24 && ty < 30 + r * 24 + 22) { if (fsel == i) open = 1; fsel = i; }
        }
        if (tx >= 244 && tx < 312 && ty >= 2 && ty < 18) { state = ST_MENU; return; }
    }
    if (open && nfiles > 0) { if (openVideo(files[fsel])) state = ST_EDIT; }
}
static void drawFilesTop(void) {
    dGradV(0, 0, SW, SH, (Color){12, 16, 34, 255}, (Color){6, 6, 12, 255});
    textC(SW / 2, 14, 1.0f, WHITE, "LEVEL EDITOR");
    textC(SW / 2, 58, 0.45f, Fade(WHITE, 0.75f), "Turn any video into a level:");
    textC(SW / 2, 84, 0.4f, Fade(WHITE, 0.6f), "1. On your PC:  python vid2abv.py video.mp4");
    textC(SW / 2, 102, 0.4f, Fade(WHITE, 0.6f), "2. Copy the .abv to SD:/3ds/afterbeat/videos/");
    textC(SW / 2, 120, 0.4f, Fade(WHITE, 0.6f), "3. Pick it here, choose the colours that hurt, play!");
    char b[64]; snprintf(b, sizeof b, "%d video%s found", nfiles, nfiles == 1 ? "" : "s");
    textC(SW / 2, 160, 0.5f, nfiles ? (Color){120, 255, 160, 255} : (Color){255, 120, 120, 255}, b);
    textC(SW / 2, 200, 0.4f, Fade(WHITE, 0.5f), "A: open   B: back");
}
static void drawFilesBottom(void) {
    dGradV(0, 0, 320, SH, (Color){16, 20, 40, 255}, (Color){8, 8, 14, 255});
    text(8, 2, 0.5f, Fade(WHITE, 0.8f), "CHOOSE A VIDEO");
    dRect(244, 2, 68, 16, (Color){40, 40, 55, 255}); dRectLines(244, 2, 68, 16, 1, Fade(WHITE, 0.4f)); textC(278, 3, 0.4f, WHITE, "BACK (B)");
    if (nfiles == 0) { textC(160, 90, 0.45f, (Color){255, 140, 140, 255}, "no .abv files in SD:/3ds/afterbeat/videos/"); return; }
    int first = fsel < 7 ? 0 : fsel - 6;
    for (int r = 0; r < 8; r++) {
        int i = first + r; if (i >= nfiles) break;
        int on = (i == fsel);
        dRect(8, 30 + r * 24, 304, 22, on ? (Color){50, 90, 160, 255} : (Color){28, 30, 44, 255});
        if (on) dRectLines(8, 30 + r * 24, 304, 22, 1, WHITE);
        text(16, 33 + r * 24, 0.5f, on ? WHITE : Fade(WHITE, 0.75f), files[i]);
    }
}

/* ---- the editor ---- */
static void addColor(Color c) {
    if (nvcol >= 6) return;
    for (int i = 0; i < nvcol; i++) if (vcol[i].r == c.r && vcol[i].g == c.g && vcol[i].b == c.b) return;
    vcol[nvcol++] = c; refreshMask();
}
static void startVideoPlay(void) {
    vidMode = 1; cur = 0; bl = 0.5f; finPhase = 0; healT = 0; tsc = 1; CP.on = 0;
    memset(PT, 0, sizeof PT); memset(HZ, 0, sizeof HZ); memset(&P, 0, sizeof P);
    P.x = SW / 2; P.y = SH * 0.75f; P.fy = -1; P.ang = -PIF / 2; P.maxhp = P.hp = startHP(0);
    shake = flash = deadT = winT = 0; vT = -2.0f; vAudioStarted = 0; T = -0.4f; curBeat = 0;
    aStop(); vframeIdx = -1; loadFrame(0);
    state = ST_VID;
}
static void seekTo(int f) {
    if (f < 0) f = 0;
    if (f >= (int)vframes) f = (int)vframes - 1;
    edFrame = f; loadFrame(f);
}
static void pickAt(int px, int py) {                         /* eyedropper: sample a pixel of the video frame */
    if (px < 0 || py < 0 || px >= vw || py >= vh) return;
    pickCol = c565(vpix[py * vw + px]);
    rgb2hsv(pickCol, &pickH, &pickS, &pickV);
}
static void updateEdit(float dt) {
    menuT += dt;
    int ps = previewScale();
    if (kDown & KEY_DRIGHT) seekTo(edFrame + 1);
    if (kDown & KEY_DLEFT) seekTo(edFrame - 1);
    if (kDown & KEY_R) seekTo(edFrame + vfps);
    if (kDown & KEY_L) seekTo(edFrame - vfps);
    if (kDown & KEY_B) { saveCfg(); state = ST_EDFILES; return; }
    if (kDown & KEY_X) saveCfg();
    if (kDown & KEY_Y) pickMode = !pickMode;
    if (kDown & KEY_A) { saveCfg(); startVideoPlay(); return; }
    if (touchDown) {
        edDrag = 0;
        if (inRect(tx, ty, 76, 130, 232, 18)) edDrag = 1;                      /* tolerance slider */
        else if (inRect(tx, ty, 60, 150, 260, 18)) edDrag = 2;                 /* timeline slider */
        else if (inRect(tx, ty, 216, 38, 46, 24)) addColor(pickCol);
        else if (inRect(tx, ty, 266, 38, 46, 24)) { if (nvcol > 0) { nvcol--; refreshMask(); } }
        else if (inRect(tx, ty, 216, 66, 96, 22)) pickMode = !pickMode;
        else if (inRect(tx, ty, 8, 210, 96, 24)) { saveCfg(); startVideoPlay(); return; }
        else if (inRect(tx, ty, 112, 210, 96, 24)) saveCfg();
        else if (inRect(tx, ty, 216, 210, 96, 24)) { saveCfg(); state = ST_EDFILES; return; }
        else for (int i = 0; i < nvcol; i++)
            if (inRect(tx, ty, 8 + i * 26, 172, 22, 18)) { for (int j = i; j + 1 < nvcol; j++) vcol[j] = vcol[j + 1]; nvcol--; refreshMask(); break; }
    }
    if (touchHeld) {
        if (edDrag == 1) { vtol = (int)clampf((hx - 76) / 232.0f * 100.0f + 0.5f, 0, 100); refreshMask(); }
        else if (edDrag == 2) seekTo((int)(clampf((hx - 76) / 232.0f, 0, 1) * (vframes - 1) + 0.5f));
        else if (pickMode == 0) { if (inRect(hx, hy, 8, 6, vw * ps, vh * ps)) pickAt((hx - 8) / ps, (hy - 6) / ps); }
        else {                                                                  /* HSV palette */
            if (inRect(hx, hy, 8, 6, 200, 16)) { pickH = clampf((hx - 8) / 200.0f, 0, 0.999f) * 360.0f; pickCol = hsv2rgb(pickH, pickS, pickV); }
            else if (inRect(hx, hy, 8, 28, 200, 98)) { pickS = clampf((hx - 8) / 200.0f, 0, 1); pickV = 1.0f - clampf((hy - 28) / 98.0f, 0, 1); pickCol = hsv2rgb(pickH, pickS, pickV); }
        }
    } else edDrag = 0;
}
static void drawEditTop(void) {
    drawVideoRects(0, 0, 400.0f / vw, 240.0f / vh, 0);
    drawVideoRects(0, 0, 400.0f / vw, 240.0f / vh, 1);
    dRect(0, 0, SW, 14, Fade(BLACK, 0.65f));
    char b[96]; snprintf(b, sizeof b, "%s   frame %d/%u   %.1fs", vname, edFrame + 1, vframes, edFrame / (float)vfps);
    text(4, 0, 0.4f, WHITE, b);
    dRect(0, SH - 14, SW, 14, Fade(BLACK, 0.65f));
    text(4, SH - 14, 0.36f, Fade(WHITE, 0.85f), "red tint = will HURT.   D-Pad/L/R: seek   A: play");
}
static void drawEditBottom(void) {
    dGradV(0, 0, 320, SH, (Color){16, 20, 40, 255}, (Color){8, 8, 14, 255});
    int ps = previewScale();
    Color green = {120, 255, 160, 255}, red = {255, 90, 100, 255};
    if (pickMode == 0) {
        dRect(6, 4, vw * ps + 4, vh * ps + 4, (Color){60, 64, 80, 255});
        drawVideoRects(8, 6, (float)ps, (float)ps, 0);
        drawVideoRects(8, 6, (float)ps, (float)ps, 1);
    } else {
        for (int i = 0; i < 25; i++) dRect(8 + i * 8, 6, 8, 16, hsv2rgb(i * 360.0f / 25, 1, 1));
        dRect(8 + pickH / 360.0f * 200 - 1, 4, 3, 20, WHITE);
        for (int yy = 0; yy < 14; yy++) for (int xx = 0; xx < 25; xx++) dRect(8 + xx * 8, 28 + yy * 7, 8, 7, hsv2rgb(pickH, xx / 24.0f, 1.0f - yy / 13.0f));
        dRectLines(8 + pickS * 200 - 3, 28 + (1 - pickV) * 98 - 3, 6, 6, 1, WHITE);
    }
    dRect(216, 6, 96, 28, pickCol); dRectLines(216, 6, 96, 28, 1, WHITE);
    char b[64]; snprintf(b, sizeof b, "#%02X%02X%02X", pickCol.r, pickCol.g, pickCol.b);
    textC(264, 10, 0.5f, (pickCol.r + pickCol.g + pickCol.b) > 380 ? BLACK : WHITE, b);
    dRect(216, 38, 46, 24, (Color){40, 100, 60, 255}); dRectLines(216, 38, 46, 24, 1, green); textC(239, 42, 0.42f, WHITE, "ADD");
    dRect(266, 38, 46, 24, (Color){100, 40, 50, 255}); dRectLines(266, 38, 46, 24, 1, red); textC(289, 42, 0.42f, WHITE, "UNDO");
    dRect(216, 66, 96, 22, (Color){40, 40, 60, 255}); dRectLines(216, 66, 96, 22, 1, Fade(WHITE, 0.5f));
    textC(264, 69, 0.4f, WHITE, pickMode ? "PALETTE (Y)" : "EYEDROPPER (Y)");
    text(216, 94, 0.32f, Fade(WHITE, 0.6f), pickMode ? "tap hue bar / square" : "tap the video");
    text(216, 106, 0.32f, Fade(WHITE, 0.6f), "then press ADD");
    snprintf(b, sizeof b, "TOLERANCE %d", vtol);
    text(8, 130, 0.36f, WHITE, b);
    dRect(76, 134, 232, 10, Fade(WHITE, 0.15f)); dRect(76, 134, 232 * vtol / 100.0f, 10, (Color){255, 190, 60, 255});
    text(8, 150, 0.36f, WHITE, "TIME");
    dRect(76, 154, 232, 10, Fade(WHITE, 0.15f)); dRect(76, 154, 232 * (vframes > 1 ? edFrame / (float)(vframes - 1) : 0), 10, (Color){90, 190, 255, 255});
    for (int i = 0; i < 6; i++) {
        if (i < nvcol) { dRect(8 + i * 26, 172, 22, 18, vcol[i]); dRectLines(8 + i * 26, 172, 22, 18, 1, WHITE); }
        else dRectLines(8 + i * 26, 172, 22, 18, 1, Fade(WHITE, 0.2f));
    }
    text(168, 173, 0.34f, nvcol ? red : Fade(WHITE, 0.5f), nvcol ? "= colours that HURT" : "add a colour to make it hurt");
    text(168, 185, 0.3f, Fade(WHITE, 0.45f), "tap a swatch to remove it");
    dRect(8, 210, 96, 24, (Color){40, 100, 60, 255}); dRectLines(8, 210, 96, 24, 1, green); textC(56, 215, 0.42f, WHITE, "PLAY (A)");
    dRect(112, 210, 96, 24, (Color){40, 40, 60, 255}); dRectLines(112, 210, 96, 24, 1, Fade(WHITE, 0.5f)); textC(160, 215, 0.42f, WHITE, "SAVE (X)");
    dRect(216, 210, 96, 24, (Color){60, 40, 40, 255}); dRectLines(216, 210, 96, 24, 1, Fade(WHITE, 0.5f)); textC(264, 215, 0.42f, WHITE, "BACK (B)");
}

/* ---- playing the video level ---- */
static int videoHit(float x, float y, float r) {             /* does the player's circle touch a damaging cell? */
    float cw = 400.0f / vw, ch = 240.0f / vh;
    int x0 = (int)floorf((x - r) / cw), x1 = (int)floorf((x + r) / cw), y0 = (int)floorf((y - r) / ch), y1 = (int)floorf((y + r) / ch);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > vw - 1) x1 = vw - 1;
    if (y1 > vh - 1) y1 = vh - 1;
    for (int cy = y0; cy <= y1; cy++)
        for (int cx = x0; cx <= x1; cx++)
            if (vmask[cy * vw + cx]) {
                float rx = cx * cw, ry = cy * ch, nx = clampf(x, rx, rx + cw), ny = clampf(y, ry, ry + ch);
                if (dist(x, y, nx, ny) < r) return 1;
            }
    return 0;
}
static void updateVideo(float dt) {
    vT += dt;
    if (vT >= 0 && !vAudioStarted) { vAudioStarted = 1; aStart(); }
    int idx = vT < 0 ? 0 : (int)(vT * vfps);
    if (idx >= (int)vframes) {                                /* video finished: you win */
        state = ST_VWIN; winT = 0; aStop();
        burst(P.x, P.y, 60, 100, 500, 1, WHITE, 1.0f, 5); play(drum[S_CLEAR], 1.0f);
        return;
    }
    loadFrame(idx);
    aPump();
    stepPlayer(dt);
    if (vT >= 0 && videoHit(P.x, P.y, PR)) hurt();
    stepParticles(dt);
    shake *= expf(-dt * 9); flash *= expf(-dt * 5);
}
static void drawVideoScene(void) {
    OX = rr(-1, 1) * shake; OY = rr(-1, 1) * shake;
    drawVideoRects(0, 0, 400.0f / vw, 240.0f / vh, 0);
    drawVideoRects(0, 0, 400.0f / vw, 240.0f / vh, 1);
    if (state != ST_VOVER) drawPlayer();
    drawParticles();
    OX = OY = 0;
    if (flash > 0.01f) dRect(0, 0, SW, SH, Fade((Color){255, 30, 40, 255}, flash * 0.35f));
    float prog = vframes ? clampf(vT / (vframes / (float)vfps), 0, 1) : 0;
    dRect(0, 0, SW, 2, Fade(WHITE, 0.15f)); dRect(0, 0, SW * prog, 2, (Color){90, 190, 255, 255});
    for (int i = 0; i < P.maxhp; i++) {
        if (i < P.hp) dRect(6 + i * 10, 7, 7, 7, (Color){255, 60, 90, 255});
        else dRectLines(6 + i * 10, 7, 7, 7, 1, Fade(WHITE, 0.4f));
    }
    if (vT < 0) {
        char n[8]; snprintf(n, sizeof n, "%d", (int)ceilf(-vT));
        textC(SW / 2, 70, 0.7f, WHITE, "GET READY");
        textC(SW / 2, 96, 1.3f, (Color){90, 190, 255, 255}, n);
        textC(SW / 2, 170, 0.4f, Fade(WHITE, 0.7f), "the red-tinted parts hurt - move with the Circle Pad, A/B/L/R dash");
    }
}
static void drawVideoBottom(void) {
    dGradV(0, 0, 320, SH, (Color){16, 20, 40, 255}, (Color){8, 8, 14, 255});
    textC(160, 8, 0.6f, WHITE, vname);
    int per = P.maxhp > 6 ? 6 : P.maxhp;
    for (int i = 0; i < P.maxhp; i++) {
        float x = 160 - per * 15 + (i % per) * 30 + 2, y = 50 + (i / per) * 24;
        if (i < P.hp) dRect(x, y, 22, 18, (Color){255, 60, 90, 255});
        else dRectLines(x, y, 22, 18, 2, Fade(WHITE, 0.3f));
    }
    float dur = vframes / (float)vfps, t = vT < 0 ? 0 : (vT > dur ? dur : vT);
    char b[64]; snprintf(b, sizeof b, "%d:%02d / %d:%02d", (int)t / 60, (int)t % 60, (int)dur / 60, (int)dur % 60);
    textC(160, 110, 0.5f, Fade(WHITE, 0.8f), b);
    dRect(20, 136, 280, 6, Fade(WHITE, 0.12f)); dRect(20, 136, 280 * (dur > 0 ? t / dur : 0), 6, (Color){90, 190, 255, 255});
    for (int i = 0; i < nvcol; i++) { dRect(20 + i * 26, 160, 22, 18, vcol[i]); dRectLines(20 + i * 26, 160, 22, 18, 1, WHITE); }
    text(20 + nvcol * 26 + 4, 162, 0.36f, (Color){255, 90, 100, 255}, "= these hurt");
    if (state == ST_VPAUSE) textC(160, 214, 0.4f, WHITE, "A resume    X restart    B editor");
    else if (state == ST_VOVER && deadT > 0.8f) textC(160, 214, 0.4f, WHITE, "A retry    B editor");
    else if (state == ST_VWIN && winT > 0.8f) textC(160, 214, 0.4f, WHITE, "A editor    X replay");
    else textC(160, 216, 0.35f, Fade(WHITE, 0.4f), "START: pause");
}

static void beginLevel(int i) { if (LV[i].boss == 2) startCutscene(); else startLevel(i, 0); }
static void cardRect(int i, float *x, float *y, float *w, float *h) { *x = 8.0f + (i % 4) * 78; *y = 22.0f + (i / 4) * 42; *w = 72; *h = 38; }
#define BTN_Y 212
#define BTN_H 24

static void updateMenu(float dt) {
    menuT += dt;
    float mb = 60.0f / 110.0f;
    int b = (int)(menuT / mb);
    if (b != lastMenuBeat) {
        lastMenuBeat = b;
        emit(2, SW / 2, 60, 0, 0, 0.8f, 12, Fade(LV[sel].c1, 0.4f), 200, 0);
        for (int i = 0; i < 3; i++) emit(0, rr(0, SW), SH + 3, rr(-6, 6), rr(-60, -25), rr(1.5f, 3), rr(1, 2), Fade(LV[sel].c2, 0.5f), 0, 0);
    }
    stepParticles(dt);
    if (kDown & KEY_DRIGHT) sel = (sel + 1) % NL;
    if (kDown & KEY_DLEFT) sel = (sel + NL - 1) % NL;
    if (kDown & KEY_DDOWN) sel = (sel + 4) % NL;
    if (kDown & KEY_DUP) sel = (sel + NL - 4) % NL;
    if (kDown & KEY_X) { casual = !casual; saveP(); }
    if (kDown & KEY_Y) { hard = !hard; saveP(); }
    if (touchDown) {
        for (int i = 0; i < NL; i++) {
            float x, y, w, h; cardRect(i, &x, &y, &w, &h);
            if (inRect(tx, ty, x, y, w, h)) { if (sel == i) { beginLevel(i); return; } sel = i; }
        }
        if (inRect(tx, ty, 8, BTN_Y, 100, BTN_H)) { hard = !hard; saveP(); }
        if (inRect(tx, ty, 112, BTN_Y, 100, BTN_H)) { casual = !casual; saveP(); }
        if (inRect(tx, ty, 216, BTN_Y, 96, BTN_H)) { beginLevel(sel); return; }
        if (inRect(tx, ty, 244, 2, 68, 16)) { openEditor(); return; }
    }
    if (kDown & KEY_SELECT) { openEditor(); return; }
    if (kDown & KEY_A) { beginLevel(sel); return; }
    if (kDown & KEY_START) quit = 1;
}

static void drawMenuTop(void) {
    const Level *S = &LV[sel];
    float mb = 60.0f / 110.0f, fr = fmodf(menuT, mb) / mb, pulse = expf(-fr * 4.0f);
    Color red = {255, 70, 80, 255};
    drawBG(S, menuT, pulse);
    drawParticles();
    float sc = 1.35f + 0.05f * pulse;
    textC(SW / 2 + 1.5f, 15.5f, sc, Fade(hard ? red : S->c2, 0.8f), "AFTERBEAT");
    textC(SW / 2, 14, sc, WHITE, "AFTERBEAT");
    textC(SW / 2, 62, 0.42f, Fade(WHITE, 0.7f), "choose a level on the touch screen");
    char buf[96];
    snprintf(buf, sizeof buf, "%d  %s", sel + 1, S->name);
    textC(SW / 2, 96, 0.85f, WHITE, buf);
    textC(SW / 2, 128, 0.5f, S->boss ? (S->boss == 2 ? (Color){255, 215, 0, 255} : red) : S->c2, S->sub);
    snprintf(buf, sizeof buf, "HP %d%s     %d BPM     %ds", startHP(sel), S->boss ? " (boss: double)" : "", S->bpm, (int)(S->beats * 60.0f / S->bpm));
    textC(SW / 2, 154, 0.45f, (Color){255, 100, 120, 255}, buf);
    textC(SW / 2, 180, 0.5f, hard ? red : Fade(WHITE, 0.85f), hard ? "HARDCORE  (original difficulty)" : "NORMAL  (easier)");
    if (S->song >= 0) { snprintf(buf, sizeof buf, "song: %s", SONGS[S->song].name); textC(SW / 2, 226, 0.4f, Fade(WHITE, 0.6f), buf); }
    if (casual) textC(SW / 2, 204, 0.5f, (Color){120, 255, 160, 255}, "CASUAL ON  (2x HP)");
}

static void drawMenuBottom(void) {
    const Level *S = &LV[sel];
    float mb = 60.0f / 110.0f, fr = fmodf(menuT, mb) / mb, pulse = expf(-fr * 4.0f);
    Color red = {255, 70, 80, 255}, gold = {255, 215, 0, 255}, green = {120, 255, 160, 255};
    dGradV(0, 0, 320, SH, mul(S->c1, 0.12f), (Color){8, 8, 14, 255});
    textC(110, 2, 0.5f, Fade(WHITE, 0.75f), "SELECT A LEVEL");
    dRect(244, 2, 68, 16, (Color){40, 60, 110, 255}); dRectLines(244, 2, 68, 16, 1, Fade(WHITE, 0.6f)); textC(278, 3, 0.4f, WHITE, "EDITOR (SEL)");
    for (int i = 0; i < NL; i++) {
        const Level *L = &LV[i];
        float x, y, w, h; cardRect(i, &x, &y, &w, &h);
        int on = (i == sel);
        Color edge = L->boss == 2 ? gold : (L->boss ? red : L->c1);
        if (on) { x -= 1.5f * pulse; y -= 1.5f * pulse; w += 3 * pulse; h += 3 * pulse; }
        dRect(x, y, w, h, Fade(mul(L->boss == 2 ? L->c2 : L->c1, 0.25f), on ? 0.95f : 0.65f));
        dRectLines(x, y, w, h, on ? 3 : 1, on ? edge : Fade(edge, 0.5f));
        char n[8]; snprintf(n, sizeof n, "%d", i + 1);
        text(x + 6, y + 2, 0.8f, on ? WHITE : Fade(WHITE, 0.6f), n);
        if (L->boss) text(x + w - 30, y + 3, 0.3f, edge, L->boss == 2 ? "ULT" : "BOSS");
        int c = clr[hard][i];
        if (c) text(x + 6, y + 25, 0.32f, (c & 2) ? gold : green, (c & 2) ? "* CLEAR" : "CLEAR");
    }
    char buf[96];
    snprintf(buf, sizeof buf, "%d %s  -  %s", sel + 1, S->name, S->sub);
    textC(160, 192, 0.4f, WHITE, buf);
    dRect(8, BTN_Y, 100, BTN_H, hard ? (Color){110, 30, 40, 255} : (Color){40, 40, 55, 255});
    dRectLines(8, BTN_Y, 100, BTN_H, 1, hard ? red : Fade(WHITE, 0.4f));
    textC(58, BTN_Y + 5, 0.4f, WHITE, hard ? "HARDCORE (Y)" : "NORMAL (Y)");
    dRect(112, BTN_Y, 100, BTN_H, casual ? (Color){40, 120, 70, 255} : (Color){40, 40, 55, 255});
    dRectLines(112, BTN_Y, 100, BTN_H, 1, casual ? green : Fade(WHITE, 0.4f));
    textC(162, BTN_Y + 5, 0.4f, WHITE, casual ? "CASUAL: ON (X)" : "CASUAL: OFF (X)");
    dRect(216, BTN_Y, 96, BTN_H, mul(S->c1, 0.5f));
    dRectLines(216, BTN_Y, 96, BTN_H, 1, WHITE);
    textC(264, BTN_Y + 5, 0.4f, WHITE, "PLAY (A)");
}

static void drawBottomPlay(void) {
    const Level *L = &LV[cur];
    float fr = T / bl - floorf(T / bl), pulse = T > 0 ? expf(-fr * 4.0f) : 0;
    float total = L->beats * bl, prog = clampf(T / total, 0, 1);
    int ult = (cur == ULT);
    dGradV(0, 0, 320, SH, mul(L->c1, 0.10f + 0.05f * pulse), (Color){8, 8, 14, 255});
    char buf[64];
    snprintf(buf, sizeof buf, "%d  %s", cur + 1, L->name);
    textC(160, 6, 0.7f, WHITE, buf);
    textC(160, 30, 0.4f, L->boss ? (Color){255, 90, 100, 255} : Fade(WHITE, 0.6f), L->sub);
    int per = P.maxhp > 6 ? 6 : P.maxhp;
    for (int i = 0; i < P.maxhp; i++) {
        float x = 160 - per * 15 + (i % per) * 30 + 2, y = 54 + (i / per) * 24;
        if (i < P.hp) dRect(x, y, 22, 18, (Color){255, 60, 90, 255});
        else dRectLines(x, y, 22, 18, 2, Fade(WHITE, 0.3f));
    }
    textC(160, 104, 0.35f, Fade(WHITE, 0.6f), hard ? (casual ? "HP  (casual, hardcore)" : "HP  (hardcore)") : (casual ? "HP  (casual)" : "HP"));
    if (ult && finPhase) {
        static const char *msgs[7] = {"", "HOLLOW ATE THE BLUE BALL", "the saws close in...", "NOWHERE LEFT TO GO", "shattered...", "reborn - you take no damage", "R: shoot    hold R: charge"};
        textC(160, 124, 0.45f, (Color){120, 200, 255, 255}, msgs[finPhase]);
        if (finPhase == 6) {
            float k = clampf(charge / 1.2f, 0, 1);
            dRect(40, 142, 240, 10, Fade(WHITE, 0.12f)); dRect(40, 142, 240 * k, 10, k >= 1 ? WHITE : (Color){90, 190, 255, 255});
            textC(160, 154, 0.32f, Fade(WHITE, 0.55f), "charge");
            dRect(40, 168, 240, 8, Fade(WHITE, 0.12f)); dRect(40, 168, 240 * clampf(finHp / (float)finMax, 0, 1), 8, (Color){255, 90, 110, 255});
            textC(160, 178, 0.32f, Fade(WHITE, 0.55f), "HOLLOW");
        }
    } else if (ult) {
        drawBallIcon(160, 150, 24);
        if (healT > 0) textC(160, 178, 0.4f, (Color){120, 200, 255, 255}, "ATTACKS HEAL - time slowed");
        else if (ballCD > 0) { char cb[32]; snprintf(cb, sizeof cb, "recharging %ds", (int)ceilf(ballCD)); textC(160, 178, 0.4f, Fade(WHITE, 0.6f), cb); }
        else textC(160, 178, 0.4f, WHITE, "BLUE BALL ready - tap or press Y");
    } else if (!L->boss) {
        textC(160, 128, 0.4f, cpReached ? (Color){215, 215, 220, 255} : Fade(WHITE, 0.4f), cpReached ? "CHECKPOINT reached" : "checkpoint: none yet");
    }
    int py = ult ? 192 : 150;
    dRect(20, py, 280, 6, Fade(WHITE, 0.12f)); dRect(20, py, 280 * prog, 6, L->c1);
    float dk = P.dashCD > 0 ? 1.0f - P.dashCD / dashCd() : 1.0f;
    dRect(20, py + 16, 280, 6, Fade(WHITE, 0.12f)); dRect(20, py + 16, 280 * dk, 6, dk >= 1 ? WHITE : Fade(L->c2, 0.8f));
    text(20, py + 24, 0.3f, Fade(WHITE, 0.45f), "song / dash");
    if (state == ST_PAUSE) textC(160, 222, 0.4f, WHITE, "A resume    X restart    B menu");
    else if (state == ST_OVER && deadT > 0.8f) textC(160, 222, 0.4f, WHITE, cpReached ? "A checkpoint   X restart   B menu" : "A retry   B menu");
    else if (state == ST_WIN && winT > winDelay()) textC(160, 222, 0.4f, WHITE, cur + 1 < NL ? "A next    X replay    B menu" : "A menu    X replay");
    else textC(160, 224, 0.35f, Fade(WHITE, 0.4f), "START: pause");
}

/* ------------------------------------------------------------------- main */
int main(void) {
    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(6000);
    C2D_Prepare();
    C3D_RenderTarget *top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    C3D_RenderTarget *bot = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    tbuf = C2D_TextBufNew(8192);
    buildUltimate();
    initTables();

    if (R_SUCCEEDED(ndspInit())) {
        audio = 1;
        ndspSetOutputMode(NDSP_OUTPUT_STEREO);
        for (int i = 0; i < VOICES; i++) {
            ndspChnReset(i);
            ndspChnSetInterp(i, NDSP_INTERP_LINEAR);
            ndspChnSetRate(i, (float)SR);
            ndspChnSetFormat(i, NDSP_FORMAT_MONO_PCM16);
        }
        makeDrums();
        aInit();
    }
    loadP();
    u64 last = osGetTime();

    while (aptMainLoop() && !quit) {
        hidScanInput();
        kDown = hidKeysDown(); kHeld = hidKeysHeld(); kUp = hidKeysUp();
        circlePosition cp; hidCircleRead(&cp); cpx = cp.dx; cpy = cp.dy;
        touchDown = (kDown & KEY_TOUCH) != 0;
        touchHeld = (kHeld & KEY_TOUCH) != 0;
        if (touchDown || touchHeld) { touchPosition tp; hidTouchRead(&tp); hx = tp.px; hy = tp.py; if (touchDown) { tx = hx; ty = hy; } }

        u64 now = osGetTime();
        float dt = (float)(now - last) / 1000.0f;
        last = now;
        if (dt > 0.05f) dt = 0.05f;
        if (dt < 0) dt = 0;

        switch (state) {
        case ST_MENU: updateMenu(dt); break;
        case ST_CUT: updateCut(dt); break;
        case ST_EDFILES: updateFiles(dt); break;
        case ST_EDIT: updateEdit(dt); break;
        case ST_VID:
            updateVideo(dt);
            if (state == ST_VID && (kDown & KEY_START)) { state = ST_VPAUSE; aPause(1); }
            break;
        case ST_VPAUSE:
            if (kDown & (KEY_START | KEY_A)) { state = ST_VID; aPause(0); }
            else if (kDown & KEY_X) startVideoPlay();
            else if (kDown & (KEY_B | KEY_SELECT)) { aStop(); state = ST_EDIT; }
            break;
        case ST_VOVER:
            deadT += dt; stepParticles(dt * 0.35f); shake *= expf(-dt * 9); flash *= expf(-dt * 5); aStop();
            if (deadT > 0.8f) {
                if (kDown & (KEY_A | KEY_X)) startVideoPlay();
                else if (kDown & KEY_B) state = ST_EDIT;
            }
            break;
        case ST_VWIN:
            winT += dt; stepParticles(dt); aStop();
            if (winT > 0.8f) {
                if (kDown & KEY_A) state = ST_EDIT;
                else if (kDown & KEY_X) startVideoPlay();
            }
            break;
        case ST_PLAY:
            updatePlay(dt);
            if (kDown & KEY_START) state = ST_PAUSE;
            break;
        case ST_PAUSE:
            if (kDown & (KEY_START | KEY_A)) state = ST_PLAY;
            if (kDown & KEY_X) startLevel(cur, 0);
            if (kDown & (KEY_B | KEY_SELECT)) { state = ST_MENU; freeLevelSounds(); }
            break;
        case ST_OVER:
            updatePlay(dt);
            if (deadT > 0.8f) {
                if (kDown & KEY_A) startLevel(cur, 1);                 /* from the last checkpoint (or the start) */
                else if (kDown & KEY_X) startLevel(cur, 0);
                else if (kDown & KEY_B) { state = ST_MENU; sel = cur; freeLevelSounds(); }
            }
            break;
        case ST_WIN:
            winT += dt;
            stepParticles(dt); shake *= expf(-dt * 9);
            if (cur == 13) sawEndUpdate();
            if (winT > winDelay()) {
                if ((kDown & KEY_A) && cur + 1 < NL) beginLevel(cur + 1);
                else if (kDown & KEY_A) { state = ST_MENU; sel = cur; freeLevelSounds(); }
                else if (kDown & KEY_X) startLevel(cur, 0);
                else if (kDown & KEY_B) { state = ST_MENU; sel = cur; freeLevelSounds(); }
            }
            break;
        }

        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C2D_TextBufClear(tbuf);
        C2D_TargetClear(top, C2D_Color32(0, 0, 0, 255));
        C2D_SceneBegin(top);
        if (state == ST_MENU) drawMenuTop();
        else if (state == ST_CUT) drawCutscene();
        else if (state == ST_EDFILES) drawFilesTop();
        else if (state == ST_EDIT) drawEditTop();
        else if (state == ST_VID || state == ST_VPAUSE || state == ST_VOVER || state == ST_VWIN) {
            drawVideoScene();
            if (state == ST_VPAUSE) overlayTop("PAUSED", WHITE, "A resume   X restart   B editor");
            if (state == ST_VOVER && deadT > 0.8f) overlayTop("YOU DIED", (Color){255, 70, 80, 255}, "A retry   B back to the editor");
            if (state == ST_VWIN && winT > 0.8f) overlayTop("VIDEO CLEARED", (Color){120, 255, 160, 255}, "A editor   X replay");
        }
        else {
            drawPlayScene();
            if (state == ST_PAUSE) overlayTop("PAUSED", WHITE, "A resume   X restart   B menu");
            if (state == ST_OVER && deadT > 0.8f)
                overlayTop("YOU DIED", (Color){255, 70, 80, 255}, cpReached ? "A checkpoint   X restart   B menu" : (casual ? "A retry   B menu" : "A retry   B menu   (X in menu: casual = 2x HP)"));
            if (state == ST_WIN && winT > winDelay())
                overlayTop(LV[cur].boss == 2 ? "HOLLOW SHATTERED" : (LV[cur].boss ? "BOSS DEFEATED" : "LEVEL CLEAR"), (Color){120, 255, 160, 255},
                           cur + 1 < NL ? "A next   X replay   B menu" : "A menu   X replay");
        }
        C2D_TargetClear(bot, C2D_Color32(0, 0, 0, 255));
        C2D_SceneBegin(bot);
        if (state == ST_MENU) drawMenuBottom();
        else if (state == ST_EDFILES) drawFilesBottom();
        else if (state == ST_EDIT) drawEditBottom();
        else if (state == ST_VID || state == ST_VPAUSE || state == ST_VOVER || state == ST_VWIN) drawVideoBottom();
        else if (state == ST_CUT) { dRect(0, 0, 320, SH, (Color){8, 8, 14, 255}); textC(160, 100, 0.5f, Fade(WHITE, 0.6f), "ULTIMATE BOSS"); textC(160, 130, 0.4f, Fade(WHITE, 0.4f), "A: skip"); }
        else drawBottomPlay();
        C3D_FrameEnd(0);
    }

    aStop();
    if (vf) fclose(vf);
    freeLevelSounds();
    if (audio) {
        stopVoices();
        linearFree(abuf[0]); linearFree(abuf[1]);
        for (int i = 0; i < S_COUNT; i++) linearFree(drum[i].d);
        ndspExit();
    }
    C2D_TextBufDelete(tbuf);
    C2D_Fini(); C3D_Fini(); gfxExit();
    return 0;
}
