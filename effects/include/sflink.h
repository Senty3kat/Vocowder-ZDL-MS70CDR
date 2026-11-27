/*
 * sflink.h -- "mailbox" link between effects of one patch, MS-70CDR 2.10.
 *
 * No audio channel is used. The source effect (sForm, Out = Mem) writes its
 * LFO into a small block inside ITS OWN ctx[3] arena; a reader (XDuck)
 * finds that block through the firmware's per-slot arena descriptor table.
 *
 * Why this is safe (firmware 2.10 disassembly + MIDISTOMP RE + NAMProf):
 *   - The audio ctx of slot s is the per-slot state block at
 *     0x11F03000 + 212*s (L1D SRAM); ctx[0] = s (template writer
 *     0xC00C8AC0 stores the loop counter in word 0).
 *   - ctx[3] = 0xC00EE430 + 12*s: one 12-byte descriptor {base, end, span}
 *     per slot, six in a row. So ctx[3] - 12*ctx[0] is the whole table.
 *   - The arenas are a static zero-initialised pool, one 0xAC440-byte block
 *     per slot at 0xC022D000 + 0xAC440*s (pool ends 0xC0636980). They exist
 *     whatever effect sits in the slot, so reading one can never fault.
 * A reader only READS the table and a foreign arena, after checking the
 * pointer is inside the pool. Nothing is written outside one's own arena.
 *
 * Liveness: the writer bumps `beat` once per audio block. A reader trusts a
 * box only after it has seen `beat` change, and drops it when `beat` has
 * not moved for SFL_LOST blocks (effect deleted / patch changed).
 *
 * Reading it from any effect (four steps):
 *   1. #include "../include/sflink.h"
 *   2. put an SFLReader in the effect's arena state; on init set
 *      slot = SFL_NONE, beat = stale = scan = 0; bump the state version
 *   3. once per audio block, after the arena checks:
 *        volatile SFLink *lk = sfl_read(ctx, base, &st->link);
 *      lk == 0: no live sForm in the patch (use your own default)
 *      lk->on == 0: sForm is switched off
 *   4. per sample lk->a[i] / lk->b[i] / lk->u[i]; per block the scalars
 *      (lk->ver >= 2 only): phase, cycles, hz, depth, wave, pitch, gate, env;
 *      (lk->ver >= 3 only): envR, gateR, and per sample inL[i], inR[i];
 *      (lk->ver >= 4 only): LFO B: aB[i], bB[i], uB[i], pitchR, hzB, phaseB, cyclesB;
 *      (lk->ver >= 5 only): bpm, taps, bSrc. For the tempo use sfl_bpm(lk):
 *      (lk->ver >= 6 only): t[i] — tempo LFO, 1 cycle per beat.
 *      it falls back to the pedal's patch TEMPO when there is no sForm.
 * Never write through lk. Cost: a few loads per block.
 */
#ifndef SFLINK_H
#define SFLINK_H

#include <stdint.h>

#define SFL_DO_PRAGMA(x) _Pragma(#x)

#define SFL_MAGIC0   0x73464C6Bu      /* 'sFLk' */
#define SFL_MAGIC1   0x8CB9B394u      /* ~SFL_MAGIC0 */
#define SFL_OFFSET   0x1000u          /* box offset inside the writer's arena */
#define SFL_SLOTS    6u
#ifndef SFL_POOL_LO                   /* overridable for the PC test harness */
#define SFL_POOL_LO  0xC0200000u      /* arena pool bounds (generous) */
#define SFL_POOL_HI  0xC0640000u
#endif
#define SFL_NONE     255u
#define SFL_FRESH    32u              /* found, not yet seen beating */
#define SFL_LIVE     16u              /* stale count below this = live */
#define SFL_LOST     64u              /* ~12 ms without a beat = gone */

/* Box layout. Fields up to b[] are version 1 (sForm 1.3m); later fields
 * were appended, so an older reader still works. Readers that use them
 * must check ver >= 2. */
typedef struct SFLink {
    uint32_t magic0;
    uint32_t magic1;
    uint32_t beat;      /* +1 every audio block */
    uint32_t on;        /* 1 = values valid; 0 = writer effect is off */
    float a[8];         /* modulation 0..1, one per sample (Out=Mem: Depth
                           applied; Dr+M / DS+M: full scale) */
    float b[8];         /* the same 180 degrees later, 0..1 */
    /* ---- version 2 (sForm 1.4m) */
    uint32_t ver;       /* SFL_VER */
    uint32_t cycles;    /* oscillator cycles completed (+1 at each wrap) */
    float phase;        /* oscillator phase 0..1 at the end of the block */
    float hz;           /* oscillator frequency, Hz */
    float depth;        /* Depth knob 0..1 */
    float wave;         /* Wave knob: 0 Sine 1 Tri 2 SawUp 3 SawDn 4 Square 5 S&H 6 Drift */
    float pitch;        /* pitch tracked from IN L, Hz (held between notes) */
    float gate;         /* IN L gate 0..1 (open while that channel sounds) */
    float env;          /* IN L peak level (fast attack, ~45 ms decay) */
    float u[8];         /* the wave 0..1 without Depth, one per sample */
    /* ---- version 3 (sForm 1.5m) */
    float envR;         /* IN R peak level (as env for L) */
    float gateR;        /* IN R gate 0..1 (as gate for L) */
    float trkCh;        /* sForm 1.6m only (one switchable tracker); 0 since 1.7m */
    float inL[8];       /* the chain signal as it reached sForm, L ... */
    float inR[8];       /* ... and R: with sForm first in the patch, the
                           clean pedal input (ctx[4] has no R) */
    /* ---- version 4 (sForm 1.7m): LFO B. In sForm's "in" modes LFO A
     * follows the pitch of IN L (with the octave shift) and LFO B the pitch
     * of IN R (unshifted since 1.8m); otherwise B = A. a/b/u/cycles/phase/hz
     * above are LFO A. */
    float pitchR;       /* pitch tracked from IN R, Hz (always running) */
    float hzB;          /* LFO B frequency, Hz */
    float phaseB;       /* LFO B phase 0..1 at the end of the block */
    uint32_t cyclesB;   /* LFO B cycles completed */
    float aB[8];        /* LFO B 0..1 (as a: Depth applied for Out=Mem) */
    float bB[8];        /* LFO B 180 degrees later */
    float uB[8];        /* LFO B without Depth */
    /* ---- version 5 (sForm 1.9m): tempo, and LFO B follows IN R or IN L */
    float bpm;          /* sForm's tempo (footswitch taps / patch TEMPO), BPM */
    uint32_t taps;      /* footswitch presses counted (+1 per tap) */
    float bSrc;         /* LFO B follows: 1 IN R, 0 IN L (R same as L or silent) */
    /* ---- version 6 (sForm 2.6m): tempo LFO, always locked to bpm.
     * One cycle per beat (quarter note). Shape: triangle lightly mixed
     * toward sine (0.8 tri + 0.2 sine), 0..1, one value per sample. */
    float t[8];         /* tempo LFO 0..1, 1 cycle/beat; t[7] is end of block */
} SFLink;

#define SFL_VER 7u   /* 7 (sForm 3.0m): fields are computed on request, see SFLNeed */

#ifndef SFL_TEMPO_ADDR
#define SFL_TEMPO_ADDR 0xC009C06Cu    /* patch TEMPO, int BPM (MS-70CDR 2.10) */
#endif

/* Tempo for any of our effects: sForm's BPM when a live sForm box is found
 * (instant on taps), else the pedal's patch TEMPO, else 120. */
SFL_DO_PRAGMA(FUNC_ALWAYS_INLINE(sfl_bpm))
static inline float sfl_bpm(volatile SFLink *lk)
{
    if (lk && lk->ver >= 5u) {
        float b = lk->bpm;
        if (b >= 20.0f && b <= 400.0f) return b;
    }
    int32_t g = *(volatile int32_t *)(uintptr_t)SFL_TEMPO_ADDR;
    if (g >= 40 && g <= 250) return (float)g;
    return 120.0f;
}

/* Reader bookkeeping; lives in the reader's own arena state. */
typedef struct SFLReader {
    uint32_t slot;      /* slot of the box in use, SFL_NONE = none */
    uint32_t beat;      /* last beat seen */
    uint32_t stale;     /* blocks since the beat last moved */
    uint32_t scan;      /* next slot to look at while searching */
} SFLReader;

/* Box of slot s, or 0 when that slot's descriptor or box is not valid. */
SFL_DO_PRAGMA(FUNC_ALWAYS_INLINE(sfl_box))
static inline volatile SFLink *sfl_box(volatile uint32_t *tab, uint32_t s, uintptr_t ownBase)
{
    volatile uint32_t *d = tab + 3u * s;
    uintptr_t b = (uintptr_t)d[0];
    uintptr_t e = (uintptr_t)d[1];
    if (b < SFL_POOL_LO || b >= SFL_POOL_HI || (b & 3u) != 0u) return 0;
    if (e <= b || e > SFL_POOL_HI) return 0;
    if (e - b < SFL_OFFSET + (uintptr_t)sizeof(SFLink)) return 0;
    if (b == ownBase) return 0;
    volatile SFLink *m = (volatile SFLink *)(b + SFL_OFFSET);
    if (m->magic0 != SFL_MAGIC0 || m->magic1 != SFL_MAGIC1) return 0;
    return m;
}

/* Once per audio block. Returns the live box or 0. Checks at most one
 * slot per block while searching, so the cost stays flat. */
SFL_DO_PRAGMA(FUNC_ALWAYS_INLINE(sfl_read))
static inline volatile SFLink *sfl_read(unsigned int *ctx, uintptr_t ownBase, SFLReader *r)
{
    uint32_t self = (uint32_t)ctx[0];
    if (self >= SFL_SLOTS) { r->slot = SFL_NONE; return 0; }
    volatile uint32_t *tab = (volatile uint32_t *)(uintptr_t)ctx[3] - 3u * self;
    volatile SFLink *m = 0;

    if (r->slot < SFL_SLOTS) {
        m = sfl_box(tab, r->slot, ownBase);
        if (m) {
            uint32_t b = m->beat;
            if (b != r->beat) { r->beat = b; r->stale = 0u; }
            else if (r->stale < 1000u) r->stale += 1u;
            if (r->stale > SFL_LOST) m = 0;
        }
        if (!m) r->slot = SFL_NONE;
    }
    if (r->slot >= SFL_SLOTS) {
        uint32_t s = r->scan;
        if (s >= SFL_SLOTS) s = 0u;
        r->scan = s + 1u;
        if (s != self) {
            volatile SFLink *c = sfl_box(tab, s, ownBase);
            if (c) { r->slot = s; r->beat = c->beat; r->stale = SFL_FRESH; }
        }
        return 0;
    }
    return (r->stale < SFL_LIVE) ? m : 0;
}

/* Writer: the box inside one's own arena (caller checked the arena size). */
SFL_DO_PRAGMA(FUNC_ALWAYS_INLINE(sfl_own))
static inline volatile SFLink *sfl_own(uintptr_t base)
{
    return (volatile SFLink *)(base + SFL_OFFSET);
}

/* ---- Requests (sForm 3.0m, box version 7). sForm computes only what some
 * effect asks for: every reader writes, once per audio block, an SFLNeed
 * record into the LAST 64 bytes of ITS OWN arena (sfl_need); sForm looks at
 * one other slot per block and ORs the bits of the readers that are alive
 * (beat moving). A reader that does not publish gets only what sForm needs
 * for its own sound plus the per-block scalars (bpm, taps, hz of A when its
 * rate does not need a tracker). Bits: */
#define SFL_NEED_A_HZ   0x001u  /* hz / phase / cycles of LFO A (its rate source alive) */
#define SFL_NEED_A      0x002u  /* a[] u[] per sample (implies A_HZ) */
#define SFL_NEED_A180   0x004u  /* b[]: LFO A 180 degrees later */
#define SFL_NEED_B_HZ   0x008u  /* hzB / phaseB / cyclesB / bSrc (LFO B's tracker alive) */
#define SFL_NEED_B      0x010u  /* aB[] uB[] per sample (implies B_HZ) */
#define SFL_NEED_B180   0x020u  /* bB[] */
#define SFL_NEED_T      0x040u  /* t[]: tempo LFO */
#define SFL_NEED_IN     0x080u  /* inL[] inR[] */
#define SFL_NEED_PITCH  0x100u  /* pitch / pitchR (both trackers) */
#define SFL_NEED_ENV    0x200u  /* env / gate / envR / gateR */
#define SFL_NEED_ALL    0x3FFu

#define SFL_NEED_MAGIC  0x73464E64u   /* 'sFNd' */
#define SFL_NEED_BACK   64u           /* the record sits at arena end - 64 */

typedef struct SFLNeed {
    uint32_t magic;
    uint32_t bits;
    uint32_t beat;      /* +1 every audio block of the reader */
    uint32_t pad;
} SFLNeed;

/* Reader: publish what you read this block. desc = ctx[3]; writes only
 * inside your own arena (its last 64 bytes -- keep your state below). */
SFL_DO_PRAGMA(FUNC_ALWAYS_INLINE(sfl_need))
static inline void sfl_need(volatile unsigned int *desc, uint32_t bits)
{
    uintptr_t b = (uintptr_t)desc[0];
    uintptr_t e = (uintptr_t)desc[1];
    if (b == 0u || e <= b + 4096u || (e & 3u) != 0u) return;
    volatile SFLNeed *n = (volatile SFLNeed *)(e - SFL_NEED_BACK);
    n->bits = bits;
    n->magic = SFL_NEED_MAGIC;
    n->beat += 1u;
}

/* Writer (sForm): what the live readers ask for. One slot per block; a
 * reader counts while its beat moves. Bits are ORed over the current and
 * the previous round of 6 scans, so a request disappears within 12 blocks
 * (~2 ms) after the reader stops asking. */
typedef struct SFLNeedScan {
    uint32_t slot;          /* next slot to look at */
    uint32_t cur, prev;     /* bits of this round / the last one */
    uint32_t beat[SFL_SLOTS];
} SFLNeedScan;

SFL_DO_PRAGMA(FUNC_ALWAYS_INLINE(sfl_need_scan))
static inline uint32_t sfl_need_scan(unsigned int *ctx, SFLNeedScan *sc)
{
    uint32_t self = (uint32_t)ctx[0];
    if (self >= SFL_SLOTS) return SFL_NEED_ALL;
    uint32_t s = sc->slot;
    if (s >= SFL_SLOTS) { s = 0u; sc->prev = sc->cur; sc->cur = 0u; }
    sc->slot = s + 1u;
    if (s != self) {
        volatile uint32_t *d = (volatile uint32_t *)(uintptr_t)ctx[3] + 3u * (s - self);
        uintptr_t b = (uintptr_t)d[0];
        uintptr_t e = (uintptr_t)d[1];
        if (b >= SFL_POOL_LO && e <= SFL_POOL_HI && e > b + 4096u && (e & 3u) == 0u) {
            volatile SFLNeed *n = (volatile SFLNeed *)(e - SFL_NEED_BACK);
            uint32_t bt = n->beat;
            if (n->magic == SFL_NEED_MAGIC && bt != sc->beat[s]) { sc->beat[s] = bt; sc->cur |= n->bits; }
        }
    }
    return sc->cur | sc->prev;
}

#endif /* SFLINK_H */
