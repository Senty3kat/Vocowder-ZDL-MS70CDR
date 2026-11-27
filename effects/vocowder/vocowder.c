/*
 * vocowder.c -- Vocowder: channel vocoder for the MS-70CDR.
 *
 * IN L is the modulator (voice, guitar, drums: what gets "spoken"). The
 * carrier (what gets shaped) is IN R, a noise generator, or a blend of both.
 * 9 knobs (UI value = params[slot] * 100, see manifest_pedal.json):
 *   Bands  (params[5])  - 4 6 8 10 12 16 20 24 32 40 48 64 80 bands,
 *                         120 Hz..7 kHz
 *   Level  (params[6])  - output level: 0 = silent, 50 = 0 dB, 100 = +12 dB
 *                         (gain = (UI/50)^2), before the soft clip
 *   Mix    (params[7])  - dry (modulator) / wet; ignored when Out = Sep
 *   LoCut  (params[8])  - high-pass on the carrier only, 20..800 Hz
 *   HiCut  (params[9])  - low-pass on the carrier only, 16 kHz (0, open)
 *                         down to 1 kHz (100)
 *   Noise  (params[10]) - noise added to IN R in the carrier, 0..1;
 *                         with NType = Freeze: the loop length, 0.1..2.9 s
 *   NType  (params[11]) - White, Pink, Brown, Green (band around 500 Hz),
 *                         Blue (rising), Crush (3-bit, 3 kHz sample & hold),
 *                         Freeze (choosing it starts recording IN R; once
 *                         the loop length (Noise) is recorded it freezes and
 *                         a grain of it loops as the carrier; 1.30),
 *                         FrzL (IN L itself: 0.74 s snapshots of it, each
 *                         smeared by eight overlapping 0.37 s grains and a
 *                         diffuser into a soft texture that morphs slowly
 *                         into the next over 1..8 s (Noise); the guitar
 *                         modulates its own almost frozen copy)
 *                         FRev (freeze-reverb: live wash at once, a loud
 *                         micro-freeze of the last 6 ms after ~6 ms, then
 *                         a smooth crossfade into the FrzL freeze. Vocoder
 *                         (still ~4x longer). A bright, quiet reverb (~12 ms)
 *                         sits just before Level; LoCut / HiCut cut it too)
 *   Out    (params[12]) - Mix: both outputs = dry/wet mix; Sep: L = wet,
 *                         R = clean modulator; T.LFO: pan once per beat at
 *                         sForm BPM (120 if none); LFO A / LFO B: pan from
 *                         sForm's LFO A / B (mailbox); P0.1..P9.7: wet and
 *                         dry pan in opposite directions at 0.1..9.7 Hz
 *   Gain   (params[13]) - vocoder input gain: centre 0 dB; right boosts the
 *                         modulator up to +12 dB, left IN R in the carrier up
 *                         to +12 dB, 0.24 dB steps. The dry signal is not
 *                         affected.
 * The detector (attack 0.5..20 ms, hold 0..50 ms, release 10..400 ms on one
 * scale) is fixed at 23 % of that scale, the user's pick; its knob became
 * Level.
 *
 * Per band: the same 2-pole band-pass (TPT state-variable filter) on modulator
 * and carrier. The modulator band's envelope (peak follower with attack /
 * hold / release) sets the band's level; the carrier band is normalised by its
 * own envelope first, with a floor tied to the whole carrier level so empty
 * bands are not pulled up into hiss. The modulator's band envelopes are taken
 * relative to its overall level, and the result is scaled by the carrier's
 * level: the vocoder sounds as loud as the carrier whatever the guitar's
 * level (a DI guitar is ~-25 dBFS, a synth carrier much hotter; 0.15 followed
 * the modulator and the vocoder all but vanished next to the carrier).
 *
 * IN R with no jack: the pedal copies L into R. When R has matched L to
 * -40 dB for ~50 ms it is left out of the live carrier (only noise remains);
 * Freeze still records it.
 *
 * Processing is band-major over the 8-sample block: per band the filter state
 * stays in registers. Scratch rows for the block live in the ctx[3] arena.
 *
 * Safe-DSP: no switch (float compares only), no math lib (bit-trick
 * log2/exp2, polynomial sine, Newton reciprocals), no static arrays, state in
 * the ctx[3] arena, ctx[11]/ctx[12] shuttle kept.
 */

#include <stdint.h>

#include "../include/zoom_params.h"
#include "vocowder_params.h"

#ifndef VOCOWDER_AUDIO_FUNC
#define VOCOWDER_AUDIO_FUNC Fx_DYN_Vocowder
#endif

#define VC_DO_PRAGMA(x) _Pragma(#x)
#define VC_EXPAND_PRAGMA(x) VC_DO_PRAGMA(x)
#define VC_CODE_SECTION(f) VC_EXPAND_PRAGMA(CODE_SECTION(f, ".audio"))

VC_CODE_SECTION(VOCOWDER_AUDIO_FUNC)

#define ZDL_PTR(type, word) ((type)(uintptr_t)(word))

#define VC_MAGIC   0x566F4331u   /* 'VoC1' */
#define VC_VERSION 32u
#define VC_MAXB    80
#define VC_RING    131072u        /* Freeze ring, 2.97 s (512 KB of the ~705 KB arena) */
#define VC_RMASK   (VC_RING - 1u)
#define VC_SG      16384u         /* FrzL grain, 0.37 s (8 heads, 1/8 apart) */
#define VC_AP1     503u           /* 1.30: halved, the diffuser runs at 22.05 kHz */          /* FrzL diffuser: 4 series all-passes */
#define VC_AP2     863u
#define VC_AP3     1307u
#define VC_AP4     1951u
#define VC_APN     (VC_AP1 + VC_AP2 + VC_AP3 + VC_AP4)
#ifndef VC_FB
#define VC_FB      0.75f
#endif
#define VC_LREG    32768u         /* FrzL snapshot region, 0.74 s (x3 in the ring) */
#define VC_LMASK   (VC_LREG - 1u)

#define VC_PI_SR     0.0000712379f   /* pi / 44100 */
/* Bands run at 22.05 kHz (1.13): the top band is 7 kHz. Halfband FIR, 27
 * taps (Kaiser 6.0): flat to 8 kHz (-0.03 dB), -47 dB from 14 kHz. Side taps
 * at odd distances 1, 3 .. 13 from the 0.5 centre. */
#define VC_HB1  0.3132242f
#define VC_HB2 -0.0915584f
#define VC_HB3  0.0418614f
#define VC_HB4 -0.0193695f
#define VC_HB5  0.0079192f
#define VC_HB6 -0.0024690f
#define VC_HB7  0.0003642f
#ifndef VC_AIR
#define VC_AIR  2.5f          /* air band level (matched to 1.07, noise in) */
#endif
#define VC_FLO       120.0f          /* lowest band centre */
#define VC_LOG2E     1.44269504f
#define VC_INV_SR_MS 22.6757370f     /* 1000 / 44100: samples -> ms */
#define VC_INV_SR    0.0000226757f   /* 1 / 44100 */
#define VC_PI        3.14159265f
#define VC_HALF_PI   1.57079633f

typedef struct VocBand {
    float a1, a2, a3, k;     /* band-pass coefficients (shared mod/car) */
    float m1, m2;            /* modulator SVF state */
    float c1, c2;            /* carrier SVF state */
    float envM;              /* modulator band envelope */
    float envS;              /* envM smoothed (~2 ms): no clicks on attacks */
    float holdM;             /* hold counter, samples */
    float envC;              /* carrier band envelope */
    float gO;                /* output trim: undoes the 22.05 kHz top loss */
    float gPrev;             /* output gain reached at the end of the last block */
} VocBand;

typedef struct VocState {
    uint32_t magic;
    uint32_t version;
    uint32_t initialized;
    uint32_t rng;
    float curBands;          /* band count the coefficients were made for */
    uint32_t coefN;          /* next band whose coefficients are (re)made */
    float curCut;            /* LoCut UI the high-pass was made for */
    float curHi;             /* HiCut UI the low-pass was made for */
    float hpA1, hpA2, hpA3;  /* carrier low-cut (k = sqrt 2) */
    float hc1, hc2;
    float lpA1, lpA2, lpA3;  /* carrier high-cut (k = sqrt 2) */
    float lo1, lo2;
    float envCt;             /* whole-carrier envelope (normalisation floor) */
    float envMt;             /* whole-modulator envelope (output level) */
    float pk0, pk1, pk2;     /* pink-noise filter (Kellett, economy) */
    float brown;             /* brown-noise integrator */
    float crushV, crushN;    /* Crush: held value, samples left */
    float gr1, gr2;          /* Green: band-pass state */
    float blueP;             /* Blue: previous white sample */
    uint32_t panPh;          /* Out = Pan: LFO phase, 2^32 = one cycle */
    uint32_t wPos;           /* Freeze ring write position */
    uint32_t filled;         /* samples written so far (capped at VC_RING) */
    uint32_t frozen;         /* 1 while NType = Freeze */
    uint32_t frzEnd;         /* ring position where the freeze happened */
    float frzP;              /* grain playback position, samples */
    float grainS;            /* loop length, slewed (no jumps when turned) */
    float gateG;             /* Out = Mix: output gate gain 0..1 */
    float lvlP;              /* last block's output level (ramped per sample) */
    uint32_t strOn;          /* 1 while NType = FrzL */
    uint32_t strC;           /* FrzL: grain clock 0..VC_SG-1 */
    uint32_t hb[8], ho[8];   /* FrzL: each head's region base and start offset */
    uint32_t apI[4];         /* FrzL diffuser positions */
    float lpT;               /* FrzL: soft high cut on the texture */
    float tPrev, tCur;       /* FrzL: last two half-rate texture samples */
    float ex[6];             /* this block's 2^x values (one exp2 loop) */
    float picKey;            /* screen picture shown: 0 on, 1 off, -1 fresh (= the loaded cover) */
    uint32_t picArm;         /* probe: callbacks seen switched on (writes need 2 s) */
    uint32_t picPrevOn;      /* probe: on in the previous callback */
    uint32_t picChg;         /* probe: the picture was changed, restore it when on */
    uint32_t picDbg;         /* probe: read-only checksum (keeps the reads) */
    uint32_t rReg, nReg, oReg; /* FrzL: recording / new / old snapshot region */
    uint32_t recOff, nSeam, oSeam; /* write offset; oldest sample of each snapshot */
    uint32_t tmr, nSnap;     /* samples since the last snapshot; snapshots taken */
    float envF, envSl, gTs;  /* FrzL: attack softener on the recording */
    float envDf, envLf;      /* |L - R| and |L| envelopes: is IN R a copy of L? */
    float rW;                /* weight of IN R in the live carrier, 0..1 */
    float rvp, rvq, rva;     /* FRev: output reverb (comb damping) */
    float rhp1, rlp1;        /* FRev: LoCut / HiCut one-poles on the reverb return */
    float frzM;              /* FRev: micro-freeze -> FrzL crossfade 0..1 */
    float tailE;             /* FRev: freeze-tail envelope after the note */
    uint32_t rvI;            /* FRev: reverb line position */
    float mod[8];            /* block scratch: modulator (after Gain) */
    float dry[8];            /* block scratch: modulator as it came in */
    float car[8];            /* block scratch: filtered carrier */
    float wet[8];            /* block scratch: vocoder sum */
    float rvC[512];          /* FrzR: output reverb comb (~12 ms, little pre-delay) */
    float decM[34], decC[34];/* decimator input: 26 past samples + this block */
    float hbk[7];            /* halfband side taps (VC_HB1..7), set at init */
    float lad[13][4];        /* band ladder: count, step, 1/Q, make-up, set at init */
    float modD[4], carD[4];  /* modulator / carrier at 22.05 kHz */
    float wetD[4];           /* band sum at 22.05 kHz */
    float intW[18];          /* interpolator input: 14 past + this block */
    float ac1, ac2;          /* air band: 9 kHz high-pass state on the carrier */
    float aenC;              /* air band: carrier envelope */
    float alp, alp2;         /* air band: two one-pole 8 kHz roll-offs */
    VocBand band[VC_MAXB];
    float apd[VC_APN];       /* FrzL diffuser lines */
    float ring[VC_RING];     /* IN R history for Freeze (not cleared: only
                                the written part is ever read) */
} VocState;

typedef union { float f; uint32_t u; } VcBits;

/* halfband side taps as a table (.const, like namlite's weights): the FIRs
 * written out term by term cost 1.7 KB of code (ZDL size cap) */
/* (1.16) No static / const arrays in .audio (matujuice's rule, from a real
 * failure): the band ladder and the halfband taps live in the arena state,
 * written once at state init. */

#ifdef VOCOWDER_PIC_WORDS
/* Vocowder_Pic is the effect's screen picture (RLE, padded to
 * VOCOWDER_PIC_WORDS words), Vocowder_Frames the other picture of the
 * on/off pair, swapped in place (the linker resolves both, as for FrogHM5). */
extern uint32_t Vocowder_Frames[];
extern uint32_t Vocowder_Pic[];
#endif

VC_DO_PRAGMA(FUNC_ALWAYS_INLINE(vc_ui))
static inline float vc_ui(float raw, float def_ui)
{
    if (!(raw >= 0.0f && raw <= 100.0f)) return def_ui;
    if (raw <= 1.0f) return raw * 100.0f;
    return raw;
}

/* 2^x for -60 <= x <= 60. */
VC_DO_PRAGMA(FUNC_ALWAYS_INLINE(vc_exp2))
static inline float vc_exp2(float x)
{
    if (x < -60.0f) x = -60.0f;
    if (x > 60.0f) x = 60.0f;
    int32_t i = (int32_t)x;
    if ((float)i > x) i -= 1;
    float f = x - (float)i;
    float p = 1.0f + f * (0.69606564f + f * (0.22449433f + f * 0.07944023f));
    VcBits v;
    v.u = ((uint32_t)(i + 127)) << 23;
    return v.f * p;
}

/* 1/x for any normal positive x: bit-trick seed (within 12 %) and three
 * Newton steps (to float precision). 1.16: the old log2/exp2 seed put a copy
 * of both into every inlined call, ~1 KB of code each dozen (matujuice's
 * pu_rcp, proven on their pedals). */
VC_DO_PRAGMA(FUNC_ALWAYS_INLINE(vc_recip))
static inline float vc_recip(float x)
{
    VcBits v;
    v.f = x;
    v.u = 0x7EF311C7u - v.u;
    float r = v.f;
    r = r * (2.0f - x * r);
    r = r * (2.0f - x * r);
    r = r * (2.0f - x * r);
    return r;
}

/* tan(w) for 0 <= w <= ~0.6 (series to w^7). */
VC_DO_PRAGMA(FUNC_ALWAYS_INLINE(vc_tan))
static inline float vc_tan(float w)
{
    float w2 = w * w;
    return w * (1.0f + w2 * (0.33333333f + w2 * (0.13333333f + w2 * 0.05396825f)));
}

/* tan(w) for 0 <= w <= ~1.2: double-angle from tan(w/2). */
VC_DO_PRAGMA(FUNC_ALWAYS_INLINE(vc_tan_wide))
static inline float vc_tan_wide(float w)
{
    float t = vc_tan(0.5f * w);
    return 2.0f * t * vc_recip(1.0f - t * t);
}

/* sin(x) for x in [-pi, pi]. */
VC_DO_PRAGMA(FUNC_ALWAYS_INLINE(vc_sin))
static inline float vc_sin(float x)
{
    float ax = x < 0.0f ? -x : x;
    float y = 1.2732395447f * x - 0.4052847346f * x * ax;
    float ay = y < 0.0f ? -y : y;
    return y + 0.225f * (y * ay - y);
}


/* 2-pole Butterworth TPT coefficients for g = tan(pi f / fs). */
VC_DO_PRAGMA(FUNC_ALWAYS_INLINE(vc_svf2))
static inline void vc_svf2(float g, float *a1, float *a2, float *a3)
{
    float d = vc_recip(1.0f + g * (g + 1.41421356f));
    *a1 = d; *a2 = g * d; *a3 = g * g * d;
}

VC_DO_PRAGMA(FUNC_ALWAYS_INLINE(vc_rand))
static inline float vc_rand(uint32_t *s)                   /* [-1, 1) */
{
    *s = *s * 1664525u + 1013904223u;
    VcBits v;
    v.u = (*s >> 9) | 0x40000000u;                         /* [2, 4) */
    return v.f - 3.0f;
}

/* Schroeder all-pass, g = 0.65, on a line of len samples. */
VC_DO_PRAGMA(FUNC_ALWAYS_INLINE(vc_ap))
static inline float vc_ap(float *d, uint32_t *idx, uint32_t len, float x)
{
    uint32_t q = *idx;
    float y = d[q] - 0.65f * x;
    d[q] = x + 0.65f * y;
    q++;
    if (q >= len) q = 0u;
    *idx = q;
    return y;
}

void VOCOWDER_AUDIO_FUNC(unsigned int *ctx)
{
    float *params = ZDL_PTR(float *, ctx[1]);
    float *fxBuf  = ZDL_PTR(float *, ctx[5]);

    unsigned int *magicSrc = ZDL_PTR(unsigned int *, ctx[12]);
    unsigned int *magicDst = ZDL_PTR(unsigned int *, *(unsigned int *)ZDL_PTR(unsigned int *, ctx[11]));
    *magicDst = *magicSrc;

    /* bypass (params[0] < 0.5) returns below, after the screen picture is
     * updated: switched off shows the first, plain cow */

    volatile unsigned int *desc = ZDL_PTR(volatile unsigned int *, ctx[3]);
    if (!desc) return;
    uintptr_t base = (uintptr_t)desc[0];
    uintptr_t end  = (uintptr_t)desc[1];
    if (base == 0u || end <= base) return;
    if ((base & 3u) != 0u) return;
    if (end - base < (uintptr_t)sizeof(VocState)) return;

    VocState *st = (VocState *)base;
    int b, i;
    if (st->magic != VC_MAGIC || st->version != VC_VERSION || !st->initialized) {
        st->magic = VC_MAGIC;
        st->version = VC_VERSION;
        st->rng = 0x13579BDFu;
        st->curBands = -1.0f;
        st->coefN = 0u;
        st->curCut = -1.0f;
        st->curHi = -1.0f;
        st->hc1 = 0.0f; st->hc2 = 0.0f; st->lo1 = 0.0f; st->lo2 = 0.0f;
        st->envCt = 0.0f;
        st->envMt = 0.0f;
        st->pk0 = 0.0f; st->pk1 = 0.0f; st->pk2 = 0.0f;
        st->brown = 0.0f; st->crushV = 0.0f; st->crushN = 0.0f;
        st->gr1 = 0.0f; st->gr2 = 0.0f; st->blueP = 0.0f;
        st->panPh = 0u;
        st->wPos = 0u; st->filled = 0u; st->frozen = 0u; st->frzEnd = 0u; st->frzP = 0.0f;
        st->grainS = 0.0f;
        st->gateG = 1.0f;
        st->lvlP = 0.0f;
        st->strOn = 0u;
        st->envDf = 0.0f; st->envLf = 0.0f; st->rW = 1.0f;
        st->rvp = 0.0f; st->rvq = 0.0f; st->rva = 0.0f;
        st->rhp1 = 0.0f; st->rlp1 = 0.0f;
        st->frzM = 0.0f;
        st->tailE = 0.0f;
        st->rvI = 0u;
        for (b = 0; b < VC_MAXB; b++) {
            VocBand *bd = &st->band[b];
            bd->m1 = 0.0f; bd->m2 = 0.0f; bd->c1 = 0.0f; bd->c2 = 0.0f;
            bd->envM = 0.0f; bd->envS = 0.0f; bd->holdM = 0.0f; bd->envC = 0.0f;
            /* silent until its coefficients are made (one band per callback):
             * the arena is not cleared on load */
            bd->a1 = 0.0f; bd->a2 = 0.0f; bd->a3 = 0.0f; bd->k = 0.0f; bd->gO = 0.0f;
            bd->gPrev = 0.0f;
        }
        st->picKey = -1.0f;
        st->picArm = 0u; st->picPrevOn = 0u; st->picChg = 0u; st->picDbg = 0u;
        for (b = 0; b < 34; b++) { st->decM[b] = 0.0f; st->decC[b] = 0.0f; }
        for (b = 0; b < 18; b++) st->intW[b] = 0.0f;
        st->ac1 = 0.0f; st->ac2 = 0.0f;
        st->aenC = 0.0f; st->alp = 0.0f; st->alp2 = 0.0f;
        st->hbk[0] = VC_HB1; st->hbk[1] = VC_HB2; st->hbk[2] = VC_HB3; st->hbk[3] = VC_HB4;
        st->hbk[4] = VC_HB5; st->hbk[5] = VC_HB6; st->hbk[6] = VC_HB7;
        /* band ladder: count, log2 step between centres (120 Hz..7 kHz), 1/Q
         * so that neighbours cross near -3 dB, output make-up (simulated: wet
         * RMS = carrier RMS for a vowel-like modulator on a saw carrier) */
        st->lad[0][0] = 4.0f; st->lad[0][1] = 1.9553f; st->lad[0][2] = 1.462f; st->lad[0][3] = 1.18f;
        st->lad[1][0] = 6.0f; st->lad[1][1] = 1.1732f; st->lad[1][2] = 0.836f; st->lad[1][3] = 1.20f;
        st->lad[2][0] = 8.0f; st->lad[2][1] = 0.8380f; st->lad[2][2] = 0.589f; st->lad[2][3] = 1.22f;
        st->lad[3][0] = 10.0f; st->lad[3][1] = 0.6518f; st->lad[3][2] = 0.4556f; st->lad[3][3] = 1.24f;
        st->lad[4][0] = 12.0f; st->lad[4][1] = 0.5333f; st->lad[4][2] = 0.3717f; st->lad[4][3] = 1.27f;
        st->lad[5][0] = 16.0f; st->lad[5][1] = 0.3911f; st->lad[5][2] = 0.2717f; st->lad[5][3] = 1.30f;
        st->lad[6][0] = 20.0f; st->lad[6][1] = 0.3087f; st->lad[6][2] = 0.2145f; st->lad[6][3] = 1.40f;
        st->lad[7][0] = 24.0f; st->lad[7][1] = 0.2550f; st->lad[7][2] = 0.1771f; st->lad[7][3] = 1.50f;
        st->lad[8][0] = 32.0f; st->lad[8][1] = 0.1892f; st->lad[8][2] = 0.1313f; st->lad[8][3] = 1.68f;
        st->lad[9][0] = 40.0f; st->lad[9][1] = 0.1504f; st->lad[9][2] = 0.1043f; st->lad[9][3] = 1.80f;
        st->lad[10][0] = 48.0f; st->lad[10][1] = 0.1248f; st->lad[10][2] = 0.0866f; st->lad[10][3] = 1.90f;
        st->lad[11][0] = 64.0f; st->lad[11][1] = 0.0931f; st->lad[11][2] = 0.0645f; st->lad[11][3] = 2.10f;
        st->lad[12][0] = 80.0f; st->lad[12][1] = 0.0743f; st->lad[12][2] = 0.0515f; st->lad[12][3] = 2.30f;
        st->initialized = 1u;
    }
#if defined(VOCOWDER_PIC_WORDS) && defined(VOCOWDER_PROBE)
    {
        /* Probe (1.12): every picture write so far froze the pedal, at the
         * switch-off (1.08/1.09) or at power-on (1.10/1.11, probably saved
         * as switched off). This build never writes at load: only at a
         * switch-off after 2 s switched on. Bands picks the action at that
         * moment; the default (12) and unlisted counts do nothing:
         *   4  read the picture only        6  rewrite one word, same value
         *   8  rewrite all, same content   10  negative (FrogHM5's method)
         *   16 the plain-cow frame
         * Switching on restores the cover if it was changed. Audio: dry. */
        volatile uint32_t *pic = (volatile uint32_t *)Vocowder_Pic;
        const uint32_t *onF = Vocowder_Frames;
        const uint32_t *offF = Vocowder_Frames + VOCOWDER_PIC_WORDS;
        uint32_t on = (params[0] < 0.5f) ? 0u : 1u;
        if (on != 0u) {
            if (st->picArm < 11025u) st->picArm++;
            if (st->picChg != 0u) {
                for (i = 0; i < VOCOWDER_PIC_WORDS; i++) pic[i] = onF[i];
                st->picChg = 0u;
            }
        } else if (st->picPrevOn != 0u && st->picArm >= 11025u) {
            float v = vc_ui(params[VOCOWDER_BANDS_SLOT], (float)VOCOWDER_BANDS_UI_DEFAULT);
            if (v < 0.5f) {                                   /* 4: read only */
                uint32_t sum = 0u;
                for (i = 0; i < VOCOWDER_PIC_WORDS; i++) sum += pic[i];
                st->picDbg = sum;
            } else if (v < 1.5f) {                            /* 6: one word, same value */
                pic[40] = pic[40];
            } else if (v < 2.5f) {                            /* 8: all, same content */
                for (i = 0; i < VOCOWDER_PIC_WORDS; i++) pic[i] = onF[i];
                st->picChg = 1u;
            } else if (v < 3.5f) {                            /* 10: negative */
                const unsigned char *sb = (const unsigned char *)onF;
                volatile unsigned char *db = (volatile unsigned char *)Vocowder_Pic;
                int nb = VOCOWDER_PIC_WORDS * 4, made = 0, k = 0;
                while (k < nb && made < 1024) {
                    unsigned char c = sb[k];
                    if (k + 2 < nb && sb[k + 1] == c) {
                        db[k] = (unsigned char)~c; db[k + 1] = (unsigned char)~c; db[k + 2] = sb[k + 2];
                        made += (int)sb[k + 2] + 2;
                        k += 3;
                    } else {
                        db[k] = (unsigned char)~c;
                        made += 1;
                        k += 1;
                    }
                }
                for (; k < nb; k++) db[k] = 0;
                st->picChg = 1u;
            } else if (v > 4.5f && v < 5.5f) {                /* 16: plain cow */
                for (i = 0; i < VOCOWDER_PIC_WORDS; i++) pic[i] = offF[i];
                st->picChg = 1u;
            }
            st->picArm = 0u;          /* one action per switch-off after 2 s on */
        }
        st->picPrevOn = on;
    }
    return;                   /* probe build: no audio processing (dry passes) */
#elif defined(VOCOWDER_PIC_WORDS) && (defined(VOCOWDER_PICTEST) || defined(VOCOWDER_PICNEG))
    {
        /* Picture test (1.10): the effect hangs the pedal when switched off
         * (1.06..1.09). Vocowder_Frames = on + off frames. When switched off
         * the Bands knob picks what goes into the picture: 4 = the cover again
         * (does any write hang?), 6 = the cover inverted at run time exactly
         * like FrogHM5 0.17 (proven), 8 = the plain-cow off frame. Switching
         * on always restores the cover. */
        float key = (params[0] < 0.5f) ? 1.0f : 0.0f;
        float cur = (st->picKey < -0.5f) ? 0.0f : st->picKey;
        if (key != cur) {
            const uint32_t *onF = Vocowder_Frames;
            const uint32_t *offF = Vocowder_Frames + VOCOWDER_PIC_WORDS;
#ifdef VOCOWDER_PICNEG
            float v = 1.0f;       /* 1.11: off = the cover's negative, as FrogHM5 */
#else
            float v = vc_ui(params[VOCOWDER_BANDS_SLOT], (float)VOCOWDER_BANDS_UI_DEFAULT);
#endif
            if (key < 0.5f || v < 0.5f) {
                for (i = 0; i < VOCOWDER_PIC_WORDS; i++) Vocowder_Pic[i] = onF[i];
            } else if (v < 1.5f) {
                const unsigned char *sb = (const unsigned char *)onF;
                unsigned char *db = (unsigned char *)Vocowder_Pic;
                int nb = VOCOWDER_PIC_WORDS * 4, made = 0, k = 0;
                while (k < nb && made < 1024) {
                    unsigned char c = sb[k];
                    if (k + 2 < nb && sb[k + 1] == c) {
                        db[k] = (unsigned char)~c; db[k + 1] = (unsigned char)~c; db[k + 2] = sb[k + 2];
                        made += (int)sb[k + 2] + 2;
                        k += 3;
                    } else {
                        db[k] = (unsigned char)~c;
                        made += 1;
                        k += 1;
                    }
                }
                for (; k < nb; k++) db[k] = 0;
            } else {
                for (i = 0; i < VOCOWDER_PIC_WORDS; i++) Vocowder_Pic[i] = offF[i];
            }
            st->picKey = key;
        }
    }
#elif defined(VOCOWDER_PIC_WORDS)
    {
        /* 1.07: the screen picture and the stored off picture trade places
         * (one copy fewer than on + off frames: the size cap). Which one is on
         * screen is read from the picture itself, at a byte where the two
         * differ, so no state can get it wrong; at load with the effect on
         * nothing is written. */
        uint32_t want = (params[0] < 0.5f) ? 1u : 0u;
        uint32_t have = (((const unsigned char *)Vocowder_Pic)[VOCOWDER_PIC_SIG_IDX]
                         == (unsigned char)VOCOWDER_PIC_SIG_OFF) ? 1u : 0u;
        if (want != have) {
            uint32_t *other = Vocowder_Frames;
            VC_DO_PRAGMA(UNROLL(1))
            for (i = 0; i < VOCOWDER_PIC_WORDS; i++) {
                uint32_t t = Vocowder_Pic[i];
                Vocowder_Pic[i] = other[i];
                other[i] = t;
            }
        }
    }
#endif
#ifdef VOCOWDER_PICTEST
    return;               /* picture test build: no audio processing (dry passes) */
#endif
    if (params[0] < 0.5f) return;

    float bandsUi = vc_ui(params[VOCOWDER_BANDS_SLOT], (float)VOCOWDER_BANDS_UI_DEFAULT);
    float cutUi   = vc_ui(params[VOCOWDER_LOCUT_SLOT], (float)VOCOWDER_LOCUT_UI_DEFAULT);
    float hiUi    = vc_ui(params[VOCOWDER_HICUT_SLOT], (float)VOCOWDER_HICUT_UI_DEFAULT);
    float noise   = vc_ui(params[VOCOWDER_NOISE_SLOT], (float)VOCOWDER_NOISE_UI_DEFAULT) * 0.01f;
    float ntUi    = vc_ui(params[VOCOWDER_NTYPE_SLOT], (float)VOCOWDER_NTYPE_UI_DEFAULT);
    float gainUi  = vc_ui(params[VOCOWDER_GAIN_SLOT],  (float)VOCOWDER_GAIN_UI_DEFAULT);
    if (gainUi > 100.0f) gainUi = 100.0f;
    gainUi = (float)(int32_t)(gainUi + 0.5f);
    /* Gain: 0.24 dB per step from the centre (+-12 dB); 2^(dB/6.0206). */
    float gDb = (gainUi - 50.0f) * 0.24f;
    if (noise > 1.0f) noise = 1.0f;
    if (cutUi > 100.0f) cutUi = 100.0f;
    if (hiUi > 100.0f) hiUi = 100.0f;
    bandsUi = (float)(int32_t)(bandsUi + 0.5f);
    cutUi = (float)(int32_t)(cutUi + 0.5f);
    hiUi = (float)(int32_t)(hiUi + 0.5f);

    /* Band ladder: count, log2 step between centres (120 Hz..7 kHz), 1/Q so
     * that neighbours cross near -3 dB, and output make-up (simulated: wet RMS
     * = carrier RMS for a vowel-like modulator on a saw carrier). */
    int bi = (int)bandsUi;
    if (bi < 0) bi = 0;
    if (bi > 12) bi = 12;
    float nb = st->lad[bi][0], step = st->lad[bi][1];
    float kq = st->lad[bi][2], makeup = st->lad[bi][3];
    /* 96 and 128 bands (1.03) crackled on the pedal: 80 is the CPU limit */
    int nbi = (int)nb;

    /* new band count: one band's coefficients per callback (80 bands in
     * ~15 ms). The whole-ladder loop's code size swung by 1.5 KB with
     * unrelated edits (pipelined or not), which the ZDL size cap can't take. */
    if (nb != st->curBands) { st->curBands = nb; st->coefN = 0u; }

    /* 1.30: this block's 2^x values in one loop -- one inlined vc_exp2 copy
     * instead of six (ZDL size); same values as before */
    st->ex[0] = (gDb < 0.0f ? -gDb : gDb) * 0.16609640f;  /* Gain */
    st->ex[1] = step * (float)(int32_t)st->coefN;          /* band being made */
    st->ex[2] = cutUi * 0.05321928f;                       /* LoCut, log2(40)/100 */
    st->ex[3] = -hiUi * 0.04f;                             /* HiCut, log2(16)/100 */
    st->ex[4] = noise * 3.0f;                              /* FrzL period */
    st->ex[5] = noise * 4.858f;                            /* grain / loop length */
    VC_DO_PRAGMA(UNROLL(1))
    for (b = 0; b < 6; b++) st->ex[b] = vc_exp2(st->ex[b]);
    float gMod = (gDb > 0.0f) ? st->ex[0] : 1.0f;          /* one exp2 for both sides */
    float gCar = (gDb < 0.0f) ? st->ex[0] : 1.0f;
    if (st->coefN < (uint32_t)nbi) {
        {
            b = (int)st->coefN;
            st->coefN++;
            VocBand *bd = &st->band[b];
            float fc = VC_FLO * st->ex[1];
            float g = vc_tan_wide(fc * (2.0f * VC_PI_SR));    /* at 22.05 kHz */
            /* at 22.05 kHz the detectors see few samples per period of a high
             * band and the filter skirts narrow near 11 kHz: the band sum lost
             * (noise in, vs 1.07) 0.5 dB at 1 kHz, 1 at 2k, 2 at 4k, 5 at 6.3k.
             * Trim, piecewise linear in octaves above 125 Hz. */
            /* octaves above 125 Hz: the centres are 120 Hz x 2^(step b) */
            float oc = step * (float)b - 0.0589f;
            /* piecewise linear (slopes 0.1, 0.5, 1.1, 1.2, 7.9 dB/oct with
             * knees at 3, 4, 5, 5.33 octaves) as a sum of hinges */
            float h3 = oc - 3.0f, h4 = oc - 4.0f, h5 = oc - 5.0f, h6 = oc - 5.33f;
            float tdb = 0.2f + 0.1f * oc + 0.4f * (h3 > 0.0f ? h3 : 0.0f)
                      + 0.6f * (h4 > 0.0f ? h4 : 0.0f) + 0.1f * (h5 > 0.0f ? h5 : 0.0f)
                      + 6.7f * (h6 > 0.0f ? h6 : 0.0f);
            /* narrow bands lose more than wide ones (1/Q 0.05 .. 1.46) */
            tdb *= 1.3f - 0.6f * kq;
            /* 1.30: block-rate envelopes read ~0.1..1 dB lower, rising with
             * the band frequency (measured, noise in): made up here */
            tdb += 0.10f + 0.18f * oc;
            /* 10^(dB/20), cubic: within 0.05 dB up to 8 dB */
            bd->gO = 1.0f + tdb * (0.115129f + tdb * (0.0066276f + tdb * 0.000254f));
            float d = vc_recip(1.0f + g * (g + kq));
            bd->a1 = d; bd->a2 = g * d; bd->a3 = g * g * d; bd->k = kq;
            bd->m1 = 0.0f; bd->m2 = 0.0f; bd->c1 = 0.0f; bd->c2 = 0.0f;
            bd->envM = 0.0f; bd->envS = 0.0f; bd->holdM = 0.0f; bd->envC = 0.0f;
        }
    }

    /* Carrier low cut 20..800 Hz, exponential (UI 0 = 20 Hz, 100 = 800 Hz). */
    if (cutUi != st->curCut) {
        float fc = 20.0f * st->ex[2];
        vc_svf2(vc_tan(fc * VC_PI_SR), &st->hpA1, &st->hpA2, &st->hpA3);
        st->curCut = cutUi;
    }
    /* Carrier high cut, exponential: UI 0 = 16 kHz (open) .. 100 = 1 kHz. */
    if (hiUi != st->curHi) {
        float fc = 16000.0f * st->ex[3];
        vc_svf2(vc_tan_wide(fc * VC_PI_SR), &st->lpA1, &st->lpA2, &st->lpA3);
        st->curHi = hiUi;
    }

    const float aCa = 0.0224f;                             /* carrier env: ~1 ms up */
    const float aCr = 0.00075f;                            /*              ~30 ms down */


    /* ---- pre-pass: carrier = IN R + noise, low and high cut */
    float h1 = st->hpA1, h2 = st->hpA2, h3 = st->hpA3;
    float hc1 = st->hc1, hc2 = st->hc2;
    float q1 = st->lpA1, q2 = st->lpA2, q3 = st->lpA3;
    float lo1 = st->lo1, lo2 = st->lo2;
    float envCt = st->envCt;
    /* noise generator states are used straight from the arena (only the
     * chosen type runs): fewer locals, smaller stack frame (1.13) */
    float *ring = st->ring;
    uint32_t wPos = st->wPos, filled = st->filled;
    /* Freeze: entering it freezes the ring; the grain size comes from Noise. */
    uint32_t frozen = st->frozen, frzEnd = st->frzEnd;
    float frzP = st->frzP;
    /* FrzL: the ring becomes three 0.74 s regions. IN L is recorded into
     * one; every 1..8 s (Noise) it becomes the new snapshot, the previous
     * new one the old, and recording moves to the region the old one used.
     * Four heads a quarter grain apart play 186 ms Hann grains (the windows
     * sum to 2) from random points of a snapshot, picking the new one with
     * a probability that ramps 0..1 over 80 % of the period: the cloud
     * morphs slowly from chord to chord instead of replaying the playing
     * (1.02/1.03 took grains straight from the live ring: a delay).
     * Attacks are softened in the recording, 1.5 ms look-ahead. */
    uint32_t stretch = (ntUi > 6.5f) ? 1u : 0u;
    /* FRev (1.18+): freeze-reverb on top of the FrzL machinery. */
    uint32_t frzR = (ntUi > 7.5f) ? 1u : 0u;
    /* FrzL state is used straight from the arena (st->...), not copied into
     * locals: in 1.05 the compiler spilled them to a 308-byte stack frame
     * (largest proven on the pedal: 252), the main suspect for 1.05 freezing
     * the pedal on load */
    if (stretch != 0u && st->strOn == 0u) {
        st->strC = 0u; st->recOff = 0u; st->tmr = 0u; st->nSnap = 0u;
        st->rReg = 0u; st->nReg = 1u; st->oReg = 2u; st->nSeam = 0u; st->oSeam = 0u;
        st->envF = 0.0f; st->envSl = 0.0f; st->gTs = 1.0f;
        frzP = 0.0f;
        st->frzM = 0.0f;
        for (b = 0; b < 8; b++) { st->hb[b] = 0u; st->ho[b] = 0u; }
        for (b = 0; b < 4; b++) st->apI[b] = 0u;
        for (b = 0; b < (int)VC_APN; b++) st->apd[b] = 0.0f;
        st->lpT = 0.0f;
        st->tPrev = 0.0f; st->tCur = 0.0f;
    }
    if (stretch == 0u && st->strOn != 0u) filled = 0u;    /* ring held regions */
    st->strOn = stretch;
    float period = 44100.0f * st->ex[4];                   /* 1 .. 8 s */
    float rRamp = vc_recip(0.8f * period);
    /* the first snapshot as soon as a region is full, then every period;
     * FRev freezes after one grain (0.37 s) so the note's reverb becomes
     * the freeze quickly (FrzL waited for a full 0.74 s region) */
    uint32_t perI = (st->nSnap == 0u) ? (((frzR != 0u) ? VC_SG : VC_LREG)) : (uint32_t)(int32_t)period;
    float grainT = 4410.0f * st->ex[5];                    /* 0.1 .. 2.9 s */
    /* 1.30: the Freeze ring (512 KB) is written only while Freeze is chosen
     * (it used to record IN R all the time, streaming through the cache in
     * every mode). Choosing Freeze starts the recording; it freezes once
     * 0.2 s and the loop length (Noise) are recorded, IN R playing live
     * until then. */
    uint32_t frzSel = (stretch == 0u && ntUi > 5.5f) ? 1u : 0u;
    if (frzSel == 0u) {
        frozen = 0u;
        if (stretch == 0u) filled = 0u;                    /* history restarts at Freeze */
    } else if (frozen == 0u && filled >= 8820u && (float)(int32_t)filled >= grainT + 8.0f) {
        frozen = 1u; frzEnd = wPos; frzP = 0.0f;
    }
    float maxG = (float)(int32_t)filled - 8.0f;
    if (grainT > maxG) grainT = maxG;
    if (grainT < 441.0f) grainT = 441.0f;
    float grain = st->grainS;
    if (!(grain >= 441.0f)) grain = grainT;
    grain += 0.02f * (grainT - grain);                     /* ~10 ms glide */
    grain = (float)(int32_t)grain;
    st->grainS = grain;
    if (frzP >= grain) frzP = 0.0f;
    float rGrain = vc_recip(grain);
    uint32_t rng = st->rng;
    float envMt = st->envMt;
    /* No jack in IN R: the pedal feeds R with a copy of L, and the guitar
     * vocoded by itself is just the guitar, loud next to the noise. When R
     * has matched L to -40 dB for ~50 ms it is left out of the live carrier
     * (Freeze still records it: freezing your own chord is useful). */
    float envDf = st->envDf, envLf = st->envLf, rW = st->rW;
    float rT = 1.0f;
    if (envLf > 0.0005f) rT = (envDf < 0.01f * envLf) ? 0.0f : 1.0f;
    else                 rT = (rW > 0.5f) ? 1.0f : 0.0f;    /* silence: keep */
    for (i = 0; i < 8; i++) {
        float xm = gMod * fxBuf[i];
        st->mod[i] = xm;                                   /* vocoder input */
        st->dry[i] = fxBuf[i];                             /* dry: not boosted */
        float am0 = xm < 0.0f ? -xm : xm;
        envMt += ((am0 > envMt) ? 0.01f : 0.00011f) * (am0 - envMt);     /* ~2 ms / ~200 ms */
        float dLR = fxBuf[i] - fxBuf[i + 8];
        float aL = fxBuf[i] < 0.0f ? -fxBuf[i] : fxBuf[i];
        envDf += 0.0005f * ((dLR < 0.0f ? -dLR : dLR) - envDf);    /* ~45 ms */
        envLf += 0.0005f * (aL - envLf);
        rW += 0.002f * (rT - rW);                                  /* ~10 ms fade */

        float wn = vc_rand(&rng);
        float rin = gCar * fxBuf[i + 8];
        float nz;
        if (ntUi < 0.5f) {                                             /* White */
            nz = 0.2f * wn;
        } else if (ntUi < 1.5f) {                                      /* Pink */
            st->pk0 = 0.99765f * st->pk0 + wn * 0.0990460f;
            st->pk1 = 0.96300f * st->pk1 + wn * 0.2965164f;
            st->pk2 = 0.57000f * st->pk2 + wn * 1.0526913f;
            nz = 0.03f * (st->pk0 + st->pk1 + st->pk2 + wn * 0.1848f);
        } else if (ntUi < 2.5f) {                                      /* Brown */
            st->brown = 0.997f * st->brown + 0.06f * wn;
            nz = 0.64f * st->brown;
        } else if (ntUi < 3.5f) {                                      /* Green: BP ~500 Hz */
            float g3 = wn - st->gr2;
            float g1 = 0.9513f * st->gr1 + 0.0339f * g3;
            float g2 = st->gr2 + 0.0339f * st->gr1 + 0.00121f * g3;
            st->gr1 = 2.0f * g1 - st->gr1;
            st->gr2 = 2.0f * g2 - st->gr2;
            nz = 1.1f * g1;
        } else if (ntUi < 4.5f) {                                      /* Blue */
            nz = 0.16f * (wn - 0.7f * st->blueP);
            st->blueP = wn;
        } else if (ntUi < 5.5f) {                                      /* Crush */
            st->crushN -= 1.0f;
            if (st->crushN <= 0.0f) {
                st->crushN = 15.0f;                                        /* ~2.9 kHz */
                st->crushV = 0.05f * (float)(int32_t)(wn * 4.0f);         /* 3 bits */
            }
            nz = st->crushV;
        } else {                                                       /* Freeze */
            nz = 0.0f;
        }

        /* Freeze ring: IN R is recorded until Freeze is chosen, then the last
         * stretch of it loops with two half-offset heads under sine windows,
         * so every splice is a long crossfade, never a cut. */
        float frz = 0.0f;
        if (frozen == 0u && frzSel != 0u) {
            ring[wPos & VC_RMASK] = rin;
            wPos++;
            if (filled < VC_RING) filled++;
        }
        if (stretch != 0u) {
            float x = gCar * fxBuf[i];
            uint32_t rb = st->rReg << 15;
            ring[rb + st->recOff] = x;
            /* attack softener: gain 2 x slow / fast envelope, applied to the
             * sample written 64 earlier (the snapshot is read much later) */
            float ax = x < 0.0f ? -x : x;
            st->envF += ((ax > st->envF) ? 0.0224f : 0.0011f) * (ax - st->envF);
            st->envSl += 0.0005f * (ax - st->envSl);
            float gT = 1.0f;
            if (st->envF > 2.0f * st->envSl + 1.0e-6f) gT = 2.0f * st->envSl * vc_recip(st->envF);
            st->gTs += 0.05f * (gT - st->gTs);
            ring[rb + ((st->recOff - 64u) & VC_LMASK)] *= st->gTs;
            st->recOff = (st->recOff + 1u) & VC_LMASK;
            st->tmr++;
            if (st->tmr >= perI) {
                st->oReg = st->nReg; st->oSeam = st->nSeam;
                st->nReg = st->rReg; st->nSeam = st->recOff;                  /* oldest sample */
                st->rReg = 3u - st->oReg - st->nReg;
                st->tmr = 0u;
                if (st->nSnap < 2u) st->nSnap++;
            }
            /* 1.30: the grain cloud, the micro-freeze and the diffuser run at
             * 22.05 kHz (even samples) -- half their cost; odd samples
             * interpolate. Recording, attack softener and timers stay at
             * 44.1 kHz, so grain / snapshot times are unchanged. */
            if ((i & 1) == 0) {
            /* head k is k eighth grains ahead: Hann window sin^2(th + k pi/8)
             * = 0.5 - 0.5 (C cos(k pi/4) - S sin(k pi/4)), C S = cos, sin 2th;
             * the eight sum to 4 */
            float ph = VC_PI * (float)(int32_t)st->strC * (2.0f / (float)VC_SG) - VC_PI;   /* 2th - pi */
            float aph = ph < 0.0f ? -ph : ph;
            float S = -vc_sin(ph);
            float C = -vc_sin(VC_HALF_PI - aph);
            /* no stack array (1.05 had hv[8]): (cos, sin)(k pi/4) by rotation */
            float ck = 1.0f, sk = 0.0f, tex = 0.0f;
            /* FRev: live wash, then a 93 ms micro-freeze (holds the note).
             * No growing chain / crossfade here: those pushed the stack frame
             * past the proven 252 B and hung the pedal on load (like 1.05). */
            if (frzR != 0u && st->nSnap == 0u) {
                if (st->recOff >= 4096u) {
                    float w = frzP * 0.00048828125f;
                    w = (w > 1.0f) ? (2.0f - w) : w;
                    tex = (ring[rb + ((st->recOff - 4096u + (uint32_t)(int32_t)frzP) & VC_LMASK)] * w
                         + ring[rb + ((st->recOff - 2048u + (uint32_t)(int32_t)frzP) & VC_LMASK)] * (1.0f - w)) * 1.55f;
                    frzP += 2.0f;
                    if (frzP >= 4096.0f) frzP -= 4096.0f;
                } else {
                    tex = x;
                }
            } else {
                VC_DO_PRAGMA(UNROLL(1))
                for (b = 0; b < 8; b++) {
                    float hw = 0.5f - 0.5f * (C * ck - S * sk);
                    tex += hw * ring[st->hb[b] + ((st->ho[b] + ((st->strC + 2048u * (uint32_t)b) & (VC_SG - 1u))) & VC_LMASK)];
                    float nc = 0.70710678f * (ck - sk);
                    sk = 0.70710678f * (ck + sk);
                    ck = nc;
                }
            }
            /* diffuser: four series all-passes (23..89 ms, g 0.65) blur the
             * grain edges and what is left of the attacks into a wash */
            float *ad = st->apd;
            /* the chain sits in a feedback loop (0.75 through the soft high
             * cut, ~4 s tail): grains run into one continuous wash */
            float xa = 0.3f * tex + VC_FB * st->lpT;
            {
                uint32_t len = VC_AP1, j;
                VC_DO_PRAGMA(UNROLL(1))
                for (j = 0u; j < 4u; j++) {
                    xa = vc_ap(ad, &st->apI[j], len, xa);
                    ad += len;
                    /* 1009, 1733, 2617, 3923 */
                    len = (j == 0u) ? VC_AP2 : ((j == 1u) ? VC_AP3 : VC_AP4);
                }
            }
            float lpT = st->lpT + 0.698f * (xa - st->lpT);     /* ~4.2 kHz at 22.05 kHz (was 0.45 at 44.1) */
            st->lpT = lpT;
            st->tPrev = st->tCur; st->tCur = lpT;
            st->strC = (st->strC + 2u) & (VC_SG - 1u);
            /* the head whose phase comes round to 0 (window 0) takes a new
             * grain: snapshot by the morph ramp, random start that does not
             * cross the snapshot's seam */
            if ((st->strC & 2047u) == 0u) {
                uint32_t k = ((VC_SG - st->strC) & (VC_SG - 1u)) >> 11;   /* 0..7 */
                float wN = (float)(int32_t)st->tmr * rRamp;
                float u = 0.5f + 0.5f * vc_rand(&rng);
                uint32_t useN = (st->nSnap < 2u || u < wN) ? 1u : 0u;
                uint32_t r = (uint32_t)(int32_t)((0.5f + 0.5f * vc_rand(&rng)) * (float)(VC_LREG - VC_SG - 1u));
                st->hb[k] = ((useN != 0u) ? st->nReg : st->oReg) << 15;
                st->ho[k] = ((useN != 0u) ? st->nSeam : st->oSeam) + r;
            }
            }
            /* even sample: halfway between the last two texture samples,
             * odd sample: the newest (linear interpolation, 1 sample late) */
            frz = ((i & 1) == 0) ? 0.5f * (st->tPrev + st->tCur) : st->tCur;
            if (st->nSnap == 0u && frzR == 0u) frz = 0.0f;
        } else if (frozen != 0u) {
            float p2 = frzP + 0.5f * grain;
            if (p2 >= grain) p2 -= grain;
            uint32_t i1 = (frzEnd - (uint32_t)(int32_t)grain + (uint32_t)(int32_t)frzP) & VC_RMASK;
            uint32_t i2 = (frzEnd - (uint32_t)(int32_t)grain + (uint32_t)(int32_t)p2) & VC_RMASK;
            /* equal-power windows: sin^2 + cos^2 = 1, so the splice keeps the
             * level of unrelated material (the 0.20 triangles dipped 3 dB) */
            float wA = vc_sin(VC_PI * frzP * rGrain);
            float wB = vc_sin(VC_PI * p2 * rGrain);
            frz = ring[i1] * wA + ring[i2] * wB;
            frzP += 1.0f;
            if (frzP >= grain) frzP -= grain;
        }

        float y = (frozen != 0u || stretch != 0u) ? frz : rW * rin + noise * nz;  /* noise is added, R stays */
        float v3 = y - hc2;
        float v1 = h1 * hc1 + h2 * v3;
        float v2 = hc2 + h2 * hc1 + h3 * v3;
        hc1 = 2.0f * v1 - hc1;
        hc2 = 2.0f * v2 - hc2;
        float hpo = y - 1.41421356f * v1 - v2;
        v3 = hpo - lo2;
        v1 = q1 * lo1 + q2 * v3;
        v2 = lo2 + q2 * lo1 + q3 * v3;
        lo1 = 2.0f * v1 - lo1;
        lo2 = 2.0f * v2 - lo2;
        float cy = v2;
        st->car[i] = cy;
        float ac = cy < 0.0f ? -cy : cy;
        envCt += ((ac > envCt) ? aCa : aCr) * (ac - envCt);
        st->wet[i] = 0.0f;
    }
    st->hc1 = hc1; st->hc2 = hc2;
    st->lo1 = lo1; st->lo2 = lo2;
    st->envCt = envCt;
    st->envMt = envMt;
    /* FRev: freeze tail lives after the note is muted */
    if (frzR != 0u) {
        float act = (envMt > 0.008f) ? 1.0f : 0.0f;
        float tE = st->tailE + ((act > st->tailE) ? 0.01f : 0.00007f) * (act - st->tailE);
        if (!(tE >= 0.0f)) tE = 0.0f;
        st->tailE = tE;
    } else {
        st->tailE = 0.0f;
    }
    if (!(st->envF == st->envF && st->envSl == st->envSl && st->gTs == st->gTs)) {
        st->envF = 0.0f; st->envSl = 0.0f; st->gTs = 1.0f;
    }
    st->wPos = wPos; st->filled = filled; st->frozen = frozen; st->frzEnd = frzEnd; st->frzP = frzP;
    st->rng = rng;
    if (!(envDf == envDf && envLf == envLf)) { envDf = 0.0f; envLf = 0.0f; }
    st->envDf = envDf; st->envLf = envLf; st->rW = rW;

    /* Normalisation floor: a band far below the whole carrier is not pulled
     * up to full level (that only lifts hiss). */
    float floorC = 0.15f * envCt + 0.0003f;

    /* ---- 44.1 -> 22.05 kHz: halfband FIR on the modulator and the carrier,
     * one output per odd input sample */
    {
        float *hm = st->decM, *hc = st->decC;
        for (i = 0; i < 8; i++) { hm[26 + i] = st->mod[i]; hc[26 + i] = st->car[i]; }
        for (i = 0; i < 4; i++) {
            int c = 14 + 2 * i;                            /* centre: newest odd - 13 */
            float am = 0.5f * hm[c], ac = 0.5f * hc[c];
            int k;
            VC_DO_PRAGMA(UNROLL(1))
            for (k = 0; k < 7; k++) {
                am += st->hbk[k] * (hm[c - 1 - 2 * k] + hm[c + 1 + 2 * k]);
                ac += st->hbk[k] * (hc[c - 1 - 2 * k] + hc[c + 1 + 2 * k]);
            }
            st->modD[i] = am;
            st->carD[i] = ac;
            st->wetD[i] = 0.0f;
        }
        for (i = 0; i < 26; i++) { hm[i] = hm[i + 8]; hc[i] = hc[i + 8]; }
    }

    /* Detector: one knob for attack, hold and release together (computed
     * here, not at the top: shorter live ranges, smaller stack frame). */
    /* 1.30: the detector setting is fixed (23 %), so its coefficients are
     * constants, computed once with the same vc_coef / vc_exp2 (saves four
     * inlined exp2 copies). Note: VC_INV_SR_MS is 1000x the 1/44.1 the
     * comment meant, so attack is instant and release ~0.5 sample: the band
     * detector has always been a peak hold (holdLen ~11.5 ms) followed by
     * the ~2 ms envS smoothing -- that is the sound the user tuned. */
    float aAtk = 1.0f;                                     /* vc_coef(0.584 ms) */
    float aRel = (frzR != 0u) ? 0.215545774f : 0.856542051f; /* vc_coef(93.4 / 11.7 ms) */
    float holdLen = 253.574982f;                           /* 0.23 x 1102.5 */
    /* 1.30: the band envelopes run once per block (4 samples at 22.05 kHz,
     * 0.18 ms) on the block's peak: one-pole a per sample -> 1 - (1 - a)^4 */
    float aAtk4 = 1.0f - aAtk; aAtk4 *= aAtk4; aAtk4 = 1.0f - aAtk4 * aAtk4;
    float aRel4 = 1.0f - aRel; aRel4 *= aRel4; aRel4 = 1.0f - aRel4 * aRel4;
    const float aCa4 = 0.16587f;                           /* carrier env: ~1 ms up */
    const float aCr4 = 0.0059865f;                         /*              ~30 ms down */
    const float aS4 = 0.07730f;                            /* band gain: ~2 ms */

    /* ---- bands, band-major, two at a time (every band count is even). The
     * envelopes run once per block (1.30), so the per-sample loop holds only
     * the two band-passes; a second, independent band fills the idle slots. */
#define VC_BAND_LOAD(S, bd)                                                        \
        float a1##S = bd->a1, a2##S = bd->a2, a3##S = bd->a3, k##S = bd->k;       \
        float m1##S = bd->m1, m2##S = bd->m2, c1##S = bd->c1, c2##S = bd->c2;     \
        float envM##S = bd->envM, envS##S = bd->envS, holdM##S = bd->holdM;       \
        float envC##S = bd->envC;                                                  \
        /* gain ramps from last block's end to this block's target */            \
        float gP##S = bd->gPrev;                                                   \
        float gE##S = vc_recip(envC##S + floorC) * bd->gO * envS##S;               \
        float dg##S = 0.25f * (gE##S - gP##S);                                     \
        float pk##S = 0.0f, pc##S = 0.0f;
/* per sample: the two band-passes, the block peaks, the ramped gain (1.30:
 * the envelope recurrences left this loop for once-per-block updates) */
#define VC_BAND_STEP(S, x, y, out)                                                 \
        {                                                                          \
            float v3 = x - m2##S;                                                  \
            float v1 = a1##S * m1##S + a2##S * v3;                                 \
            float v2 = m2##S + a2##S * m1##S + a3##S * v3;                         \
            m1##S = 2.0f * v1 - m1##S;                                             \
            m2##S = 2.0f * v2 - m2##S;                                             \
            float bm = k##S * v1;                                                  \
            float am = bm < 0.0f ? -bm : bm;                                       \
            pk##S = (am > pk##S) ? am : pk##S;                                     \
            v3 = y - c2##S;                                                        \
            v1 = a1##S * c1##S + a2##S * v3;                                       \
            v2 = c2##S + a2##S * c1##S + a3##S * v3;                               \
            c1##S = 2.0f * v1 - c1##S;                                             \
            c2##S = 2.0f * v2 - c2##S;                                             \
            float bc = k##S * v1;                                                  \
            float ac = bc < 0.0f ? -bc : bc;                                       \
            pc##S = (ac > pc##S) ? ac : pc##S;                                     \
            gP##S += dg##S;                                                        \
            out = bc * gP##S;                                                      \
        }
/* once per block: attack / hold / release on the block peak, the carrier
 * envelope, the ~2 ms gain smoothing */
#define VC_BAND_SAVE(S, bd)                                                        \
        {                                                                          \
            float rise = (pk##S > envM##S) ? 1.0f : 0.0f;                          \
            float held = (holdM##S > 0.0f) ? 1.0f : 0.0f;                          \
            float aE = rise * aAtk4 + (1.0f - rise) * (1.0f - held) * aRel4;       \
            envM##S += aE * (pk##S - envM##S);                                     \
            float hn = holdM##S - 4.0f;                                            \
            hn = hn < 0.0f ? 0.0f : hn;                                            \
            holdM##S = hn + rise * (holdLen - hn);                                 \
            envC##S += ((pc##S > envC##S) ? aCa4 : aCr4) * (pc##S - envC##S);      \
            envS##S += aS4 * (envM##S - envS##S);                                  \
        }                                                                          \
        if (!(m1##S == m1##S && c1##S == c1##S && envM##S == envM##S && envC##S == envC##S && gP##S == gP##S)) { \
            m1##S = 0.0f; m2##S = 0.0f; c1##S = 0.0f; c2##S = 0.0f;               \
            envM##S = 0.0f; envS##S = 0.0f; holdM##S = 0.0f; envC##S = 0.0f;      \
            gP##S = 0.0f;                                                          \
        }                                                                          \
        bd->m1 = m1##S; bd->m2 = m2##S; bd->c1 = c1##S; bd->c2 = c2##S;           \
        bd->envM = envM##S; bd->envS = envS##S; bd->holdM = holdM##S; bd->envC = envC##S; \
        bd->gPrev = gP##S;
    for (b = 0; b < nbi; b += 2) {
        VocBand *bdA = &st->band[b];
        VocBand *bdB = &st->band[b + 1];
        VC_BAND_LOAD(A, bdA)
        VC_BAND_LOAD(B, bdB)
        for (i = 0; i < 4; i++) {
            float x = st->modD[i];
            float y = st->carD[i];
            float oA, oB;
            VC_BAND_STEP(A, x, y, oA)
            VC_BAND_STEP(B, x, y, oB)
            st->wetD[i] += oA + oB;
        }
        VC_BAND_SAVE(A, bdA)
        VC_BAND_SAVE(B, bdB)
    }
#undef VC_BAND_LOAD
#undef VC_BAND_STEP
#undef VC_BAND_SAVE

    /* ---- 22.05 -> 44.1 kHz: the same halfband. Per input t: the even output
     * is 2 x the side taps, the odd one the input 6 back (centre 0.5 x 2) */
    {
        float *w = st->intW;
        for (i = 0; i < 4; i++) w[14 + i] = st->wetD[i];
        for (i = 0; i < 4; i++) {
            int t = 14 + i;
            float ev = 0.0f;
            int k;
            VC_DO_PRAGMA(UNROLL(1))
            for (k = 0; k < 7; k++) ev += st->hbk[k] * (w[t - 6 + k] + w[t - 7 - k]);
            st->wet[2 * i] = 2.0f * ev;
            st->wet[2 * i + 1] = w[t - 6];
        }
        for (i = 0; i < 14; i++) w[i] = w[i + 4];
    }

    /* ---- air band at 44.1 kHz: what lies above the half-rate bands. The
     * old full-rate top band's upper skirt carried the carrier's hiss and
     * sibilance up to 16 kHz; here the carrier above 9 kHz (Butterworth
     * high-pass), normalised by its own envelope as in every band, shaped by
     * the top band's modulator envelope, rolled off -12 dB/oct above 8 kHz.
     * Level matched to 1.07 with noise in. */
    {
        float c1 = st->ac1, c2 = st->ac2, eC = st->aenC, lp = st->alp, lp2 = st->alp2;
        float gA = VC_AIR * st->band[nbi - 1].envS * vc_recip(eC + floorC);   /* per block */
        for (i = 0; i < 8; i++) {
            float v3 = st->car[i] - c2;
            float v1 = 0.38266f * c1 + 0.28569f * v3;
            float v2 = c2 + 0.28569f * c1 + 0.21329f * v3;
            c1 = 2.0f * v1 - c1;
            c2 = 2.0f * v2 - c2;
            float hc = st->car[i] - 1.41421356f * v1 - v2;
            float a = hc < 0.0f ? -hc : hc;
            eC += ((a > eC) ? aCa : aCr) * (a - eC);
            lp += 0.68f * (hc * gA - lp);
            lp2 += 0.68f * (lp - lp2);
            st->wet[i] += lp2;
        }
        if (!(c1 == c1 && eC == eC && lp2 == lp2)) {
            c1 = 0.0f; c2 = 0.0f; eC = 0.0f; lp = 0.0f; lp2 = 0.0f;
        }
        st->ac1 = c1; st->ac2 = c2; st->aenC = eC;
        st->alp = lp; st->alp2 = lp2;
    }

    /* Output-stage knobs, read here rather than at the top (shorter live
     * ranges across the band loop: smaller stack frame, 1.13). */
    float levUi   = vc_ui(params[VOCOWDER_LEVEL_SLOT], (float)VOCOWDER_LEVEL_UI_DEFAULT);
    float mix     = vc_ui(params[VOCOWDER_MIX_SLOT],   (float)VOCOWDER_MIX_UI_DEFAULT) * 0.01f;
    /* Out has 102 positions: the LineSel handler sends UI*0.01, so 101 arrives
     * as 1.01, which vc_ui would take as UI 1 (Sep). */
    float outRaw  = params[VOCOWDER_OUT_SLOT];
    float outUi;
    if (!(outRaw >= 0.0f && outRaw <= 200.0f)) outUi = (float)VOCOWDER_OUT_UI_DEFAULT;
    else if (outRaw < 1.5f)                    outUi = outRaw * 100.0f;
    else                                       outUi = outRaw;
    if (outUi > 101.0f) outUi = 101.0f;
    outUi = (float)(int32_t)(outUi + 0.5f);
    if (mix > 1.0f) mix = 1.0f;
    if (levUi > 100.0f) levUi = 100.0f;
    float level = levUi * 0.02f;
    level = level * level;                                 /* 0 .. 4, 50 = 1 */

    /* Output level = carrier level, modulator taken relative to its own. */
    /* The +0.002 floor (-54 dBFS) is where the vocoder starts to fade with
     * a fading modulator; 0.01 (-40 dB) acted like a noise gate on guitar
     * decays (heard in Sep, where no dry signal masks it). */
    float lvl = makeup * envCt * vc_recip(envMt + 0.002f);
    /* ramped from the last block's value: a step every 8 samples was the
     * main click at note attacks */
    float lvlR = st->lvlP;
    if (!(lvlR >= 0.0f && lvlR < 1.0e6f)) lvlR = lvl;
    st->lvlP = lvl;
    float lvlD = 0.125f * (lvl - lvlR);

    /* FRev: quiet bright reverb before Level; LoCut/HiCut cut it too. */
    if (frzR != 0u) {
        float gH = 0.04f + 0.0003f * cutUi;
        float gL = 0.95f - 0.008f * hiUi;
        for (i = 0; i < 8; i++) {
            float x = st->wet[i];
            float d = st->rvC[st->rvI];
            st->rvC[st->rvI] = x + 0.75f * d;
            st->rvI = (st->rvI + 1u) & 511u;
            st->rlp1 += gL * (d - st->rlp1);
            st->rhp1 += gH * (st->rlp1 - st->rhp1);
            st->wet[i] = x + 0.22f * (st->rlp1 - st->rhp1);
        }
    }

    /* Out: 0 Mix, 1 Sep, 2 T.LFO, 3 LFO A, 4 LFO B, 5..101 Pan 0.1..9.7 Hz.
     * T.LFO / LFO A / LFO B read sForm's mailbox (Out=Mem); no box: tempo
     * 120, LFO A 0.5 Hz, LFO B 4 Hz. */
    /* Out: 0 Mix, 1 Sep, 2 T.LFO, 3 LFO A, 4 LFO B, 5..101 Pan 0.1..9.7 Hz.
     * (sForm mailbox is not used here: it hung this pedal on load.) */
    float sep = (outUi > 0.5f && outUi < 1.5f) ? 1.0f : 0.0f;
    float pan = (outUi > 1.5f) ? 1.0f : 0.0f;
    float pr;
    if (outUi < 2.5f) {
        pr = 1.0f;
    } else if (outUi < 3.5f) {
        pr = 2.0f;                                                 /* T.LFO ~120 BPM */
    } else if (outUi < 4.5f) {
        pr = 0.5f;                                                 /* LFO A */
    } else if (outUi < 5.5f) {
        pr = 4.0f;                                                 /* LFO B */
    } else {
        pr = 0.1f * (outUi - 4.0f);                                /* P0.1 .. P9.7 */
    }
    if (!(pr >= 0.0f && pr <= 12.0f)) pr = 1.0f;
    uint32_t pinc = ((uint32_t)(int32_t)(pr * 8.0f * VC_INV_SR * 2147483648.0f)) << 1;
    uint32_t pph = st->panPh + pinc;
    st->panPh = pph;
    float panPh = (float)(int32_t)(pph >> 1) * 4.656612873e-10f;
    float pos = 0.5f + 0.5f * vc_sin(2.0f * VC_PI * panPh - VC_PI);
    if (!(pos >= 0.0f)) pos = 0.0f;
    if (pos > 1.0f) pos = 1.0f;
    float th = pos * VC_HALF_PI;
    /* sin / cos on 0..pi/2 by one odd polynomial (Taylor to x^7, error
     * < 2e-4): branch-free, 0.4 KB less code than two vc_sin (1.13) */
    float tB = VC_HALF_PI - th;
    float t2 = th * th, u2 = tB * tB;
    float gA = th * (1.0f - t2 * (0.16666667f - t2 * (0.00833333f - t2 * 0.00019841f)));
    float gB = tB * (1.0f - u2 * (0.16666667f - u2 * (0.00833333f - u2 * 0.00019841f)));
    if (!(gA >= 0.0f)) gA = 0.0f;
    if (gA > 1.0f) gA = 1.0f;
    if (!(gB >= 0.0f)) gB = 0.0f;
    if (gB > 1.0f) gB = 1.0f;
    float wG = mix, dG = 1.0f - mix;                       /* Pan: same law as Mix */
    /* Out = Mix only (user): noise gate at -54 dBFS on the modulator level,
     * ~1 ms open, ~40 ms close, 3 dB hysteresis. Sep and Pan stay ungated. */
    float mixMode = (sep < 0.5f && pan < 0.5f) ? 1.0f : 0.0f;
    float gT = (envMt > 0.002f) ? 1.0f : ((envMt < 0.0014f) ? 0.0f : ((st->gateG > 0.5f) ? 1.0f : 0.0f));
    float gateG = st->gateG;

    float *restrict fxOut = fxBuf;    /* 1.30: no alias with st-> (lets the loop pipeline) */
    for (i = 0; i < 8; i++) {
        float dry = st->dry[i];
        lvlR += lvlD;
        float wet = lvlR * st->wet[i];
        /* soft clip: x - 4/27 x^3, flat at +-1 beyond +-1.5 */
        if (wet > 1.5f) wet = 1.0f;
        else if (wet < -1.5f) wet = -1.0f;
        else wet = wet - 0.14814815f * wet * wet * wet;
        float mixed = dry + mix * (wet - dry);
        float oL = mixed + sep * (wet - mixed);            /* Sep: L = wet */
        float oR = mixed + sep * (dry - mixed);            /* Sep: R = clean modulator */
        /* Pan: wet goes L->R while dry goes R->L */
        float pL = wG * wet * gB + dG * dry * gA;
        float pR = wG * wet * gA + dG * dry * gB;
        gateG += ((gT > gateG) ? 0.0224f : 0.00057f) * (gT - gateG);
        float gg = 1.0f + mixMode * (gateG - 1.0f);
        if (frzR != 0u) gg = 1.0f;   /* FRev: freeze tail must not be gated */
        oL = gg * level * (oL + pan * (pL - oL));
        oR = gg * level * (oR + pan * (pR - oR));
        /* FRev: pure freeze under (and after) the vocoder, so muting the
         * strings leaves a held, slowly fading freeze tail */
        if (frzR != 0u) {
            float tA = level * 0.50f * st->tailE * st->car[i];
            oL += tA;
            oR += tA;
        }
        /* final soft clip on both outputs */
        if (oL > 1.5f) oL = 1.0f; else if (oL < -1.5f) oL = -1.0f; else oL = oL - 0.14814815f * oL * oL * oL;
        if (oR > 1.5f) oR = 1.0f; else if (oR < -1.5f) oR = -1.0f; else oR = oR - 0.14814815f * oR * oR * oR;
        if (!(oL == oL)) oL = 0.0f;                        /* never send NaN out */
        if (!(oR == oR)) oR = 0.0f;
        fxOut[i]     = oL;
        fxOut[i + 8] = oR;
    }
    st->gateG = gateG;
}


/* Value labels (1.16): tables of the values the 1.13 label code computed
 * (generated by running that code; same text on screen), printed without
 * float maths. 1.05..1.13 compiled the float versions with
 * FUNCTION_OPTIONS --opt_for_speed=0 to save room, and those size-compiled
 * labels broke the effect in the pedal's effect list (T-LBL test: the slot
 * showed the "wire" placeholder and would not change). Compiled normally now. */
static const uint16_t vc_lblLoCut[101] = {
    20, 21, 22, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 34, 35,
    36, 37, 39, 40, 42, 43, 45, 47, 48, 50, 52, 54, 56, 58, 60, 63,
    65, 68, 70, 73, 75, 78, 81, 84, 87, 91, 94, 98, 101, 105, 109, 113,
    117, 122, 126, 131, 136, 141, 147, 152, 158, 164, 170, 176, 183, 190, 197, 204,
    212, 220, 228, 237, 246, 255, 265, 274, 285, 296, 307, 318, 330, 343, 355, 369,
    383, 397, 412, 427, 443, 460, 477, 495, 514, 533, 553, 574, 596, 618, 641, 665,
    690, 716, 743, 771, 800
};
static const uint8_t vc_lblHiCut[101] = {
    160, 160, 150, 150, 140, 140, 140, 130, 130, 120, 120, 120, 110, 110, 110, 110,
    100, 100, 97, 94, 92, 89, 87, 85, 82, 80, 78, 76, 74, 72, 70, 68,
    66, 64, 62, 61, 59, 57, 56, 54, 53, 51, 50, 49, 47, 46, 45, 43,
    42, 41, 40, 39, 38, 37, 36, 35, 34, 33, 32, 31, 30, 29, 29, 28,
    27, 26, 26, 25, 24, 24, 23, 22, 22, 21, 21, 20, 19, 19, 18, 18,
    17, 17, 16, 16, 16, 15, 15, 14, 14, 14, 13, 13, 12, 12, 12, 11,
    11, 11, 11, 10, 10
};
static const int8_t vc_lblLevel[101] = {
    0, -68, -56, -49, -44, -40, -37, -34, -32, -30, -28, -26, -25, -23, -22, -21,
    -20, -19, -18, -17, -16, -15, -14, -13, -13, -12, -11, -11, -10, -10, -9, -8,
    -8, -7, -7, -6, -6, -5, -5, -4, -4, -3, -3, -3, -2, -2, -1, -1,
    -1, 0, 0, 0, 1, 1, 1, 2, 2, 2, 3, 3, 3, 3, 4, 4,
    4, 5, 5, 5, 5, 6, 6, 6, 6, 7, 7, 7, 7, 8, 8, 8,
    8, 8, 9, 9, 9, 9, 9, 10, 10, 10, 10, 10, 11, 11, 11, 11,
    11, 12, 12, 12, 12
};
static const uint8_t vc_lblGain[50] = {
    2, 5, 7, 10, 12, 14, 17, 19, 22, 24, 26, 29, 31, 34, 36, 38,
    41, 43, 46, 48, 50, 53, 55, 58, 60, 62, 65, 67, 70, 72, 74, 77,
    79, 82, 84, 86, 89, 91, 94, 96, 98, 100, 100, 110, 110, 110, 110, 120,
    120, 120
};

/* up to three digits without divides (repeated subtraction) */
VC_DO_PRAGMA(FUNC_ALWAYS_INLINE(vc_digits))
static inline unsigned int vc_digits(unsigned int v, char *out, unsigned int n)
{
    unsigned int h = 0u, t = 0u;
    while (v >= 100u) { v -= 100u; h++; }
    while (v >= 10u)  { v -= 10u;  t++; }
    if (h > 0u) out[n++] = (char)('0' + h);
    if (h > 0u || t > 0u) out[n++] = (char)('0' + t);
    out[n++] = (char)('0' + v);
    return n;
}

/* LoCut value label in Hz (20 * 40^(v/100)). Stock GetString ABI:
 * value, destination -> length. */
int ZDL_GetLabel_3(unsigned int value, char *out)
{
    unsigned int n;
    if (value > 100u) value = 100u;
    n = vc_digits(vc_lblLoCut[value], out, 0u);
    out[n] = 0;
    return (int)n;
}

/* HiCut value label (16000 * 16^(-v/100)): "16k" .. "10k", "9.9k" .. "1.0k". */
int ZDL_GetLabel_4(unsigned int value, char *out)
{
    unsigned int n, t, w = 0u;
    if (value > 100u) value = 100u;
    t = vc_lblHiCut[value];                                /* 0.1 kHz units */
    if (t < 100u) {
        while (t >= 10u) { t -= 10u; w++; }
        out[0] = (char)('0' + w); out[1] = '.'; out[2] = (char)('0' + t);
        n = 3u;
    } else {
        n = vc_digits((t >= 155u) ? 16u : (t >= 145u) ? 15u : (t >= 135u) ? 14u
                    : (t >= 125u) ? 13u : (t >= 115u) ? 12u : (t >= 105u) ? 11u : 10u, out, 0u);
    }
    out[n++] = 'k';
    out[n] = 0;
    return (int)n;
}

/* Out label: "Mix", "Sep", "T.LFO", "LFO A", "LFO B", then "P0.1".."P9.7". */
int ZDL_GetLabel_7(unsigned int value, char *out)
{
    unsigned int w = 0u, f;
    if (value == 0u) { out[0] = 'M'; out[1] = 'i'; out[2] = 'x'; out[3] = 0; return 3; }
    if (value == 1u) { out[0] = 'S'; out[1] = 'e'; out[2] = 'p'; out[3] = 0; return 3; }
    if (value == 2u) {
        out[0] = 'T'; out[1] = '.'; out[2] = 'L'; out[3] = 'F'; out[4] = 'O'; out[5] = 0;
        return 5;
    }
    if (value == 3u) {
        out[0] = 'L'; out[1] = 'F'; out[2] = 'O'; out[3] = ' '; out[4] = 'A'; out[5] = 0;
        return 5;
    }
    if (value == 4u) {
        out[0] = 'L'; out[1] = 'F'; out[2] = 'O'; out[3] = ' '; out[4] = 'B'; out[5] = 0;
        return 5;
    }
    if (value > 101u) value = 101u;
    f = value - 4u;                                        /* tenths of Hz */
    out[0] = 'P';
    while (f >= 10u) { f -= 10u; w++; }
    out[1] = (char)('0' + w); out[2] = '.'; out[3] = (char)('0' + f); out[4] = 0;
    return 4;
}

/* Gain label: "C+12".."C+0.2", "0", "M+0.2".."M+12" (dB, 0.24 dB steps). */
int ZDL_GetLabel_8(unsigned int value, char *out)
{
    unsigned int n, d, w = 0u;
    if (value > 100u) value = 100u;
    if (value == 50u) { out[0] = '0'; out[1] = 0; return 1; }
    d = (value > 50u) ? value - 50u : 50u - value;
    d = vc_lblGain[d - 1u];                                /* tenths of dB */
    out[0] = (value > 50u) ? 'M' : 'C';
    out[1] = '+';
    if (d >= 100u) {                                       /* 10 dB and up: whole dB */
        n = vc_digits((d >= 115u) ? 12u : (d >= 105u) ? 11u : 10u, out, 2u);
    } else {
        while (d >= 10u) { d -= 10u; w++; }
        out[2] = (char)('0' + w); out[3] = '.'; out[4] = (char)('0' + d);
        n = 5u;
    }
    out[n] = 0;
    return (int)n;
}

/* Level label in dB (40 log10(v/50)); "Off" at 0. */
int ZDL_GetLabel_1(unsigned int value, char *out)
{
    unsigned int n;
    int d;
    if (value == 0u) { out[0] = 'O'; out[1] = 'f'; out[2] = 'f'; out[3] = 0; return 3; }
    if (value > 100u) value = 100u;
    d = vc_lblLevel[value];
    if (d == 0) { out[0] = '0'; out[1] = 0; return 1; }
    out[0] = (d < 0) ? '-' : '+';
    n = vc_digits((unsigned int)(d < 0 ? -d : d), out, 1u);
    out[n] = 0;
    return (int)n;
}
