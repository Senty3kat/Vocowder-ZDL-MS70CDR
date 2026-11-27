/*
 * zoomos.h -- footswitch / cursor / screen helpers for our effects, shared.
 *
 * Firmware words (MIDISTOMP map + our disassembly), plain loads unless said:
 *   footswitch poll word 0xC009DF7C, bit 0 clear = held (the OS polls it every
 *   16 ms); cursor byte 0xC009C064 -> slot word at <cursor-slot table> + 24 x
 *   cursor; pedal tempo screen = overlay mask 0xC009DFA0 bit 0 with state
 *   0xC009DFA8 = 2 or 5; screen redraw = Event_post(*<UI event>, 0x40).
 * The cursor-slot table, UI event and Event_post MOVE between the two OSes
 * on our pedals (MS-70CDR 2.10, MS-50G 3.10 / MS-60B): zos_detect() picks the
 * set by which GPIO bank-pointer slot holds a peripheral address, as sForm
 * 2.8m does. Every address is overridable for the PC harness.
 *
 * Screen frames ([uint32 len][RLE data padded to 4] ...) are copied over the
 * effect's own picture (pic_symbol) -- the runtime screen swap proven by
 * FrogHM5, Vocowder 1.17 and sForm 2.1m+.
 */
#ifndef ZOOMOS_H
#define ZOOMOS_H

#include <stdint.h>

#define ZOS_DO_PRAGMA(x) _Pragma(#x)

#ifndef ZOS_FSW_ADDR
#define ZOS_FSW_ADDR        0xC009DF7Cu  /* footswitch poll word, bit 0 clear = held */
#define ZOS_CURSOR_ADDR     0xC009C064u  /* signed byte: cursor position (view 0) */
#define ZOS_OVL_MASK_ADDR   0xC009DFA0u  /* bit 0: overlay 0 (tempo box) shown */
#define ZOS_OVL_STATE_ADDR  0xC009DFA8u  /* overlay 0 state: 2 / 5 = tempo screen */
#endif
#ifndef ZOS_GPIO_PTR_70                  /* per-OS words: 70CDR 2.10, then 50G 3.10 / 60B */
#define ZOS_GPIO_PTR_70     0x1181DEF0u
#define ZOS_GPIO_PTR_50     0x1181DE70u
#define ZOS_CURSOR_SLOT_70  0xC00EE948u
#define ZOS_CURSOR_SLOT_50  0xC00EEEB0u
#define ZOS_UI_EVENT_70     0xC00EF9D8u
#define ZOS_UI_EVENT_50     0xC00EFF64u
#define ZOS_EVENT_POST_70   0xC00CBF40u
#define ZOS_EVENT_POST_50   0xC00CC680u
#endif
#ifndef ZOS_GPIO_OK
#define ZOS_GPIO_OK(g) (((g) & 3u) == 0u && (g) >= 0x01C00000u && (g) < 0x01F00000u)
#endif

typedef struct ZosState {
    uint32_t cursorSlot;   /* slot table of the detected OS (the other is tried too) */
    uint32_t uiEvent;
    uint32_t eventPost;
    uint32_t ok;           /* 1 = an OS was recognised: redraws allowed */
} ZosState;

/* Once at init. */
ZOS_DO_PRAGMA(FUNC_ALWAYS_INLINE(zos_detect))
static inline void zos_detect(ZosState *z)
{
    uint32_t g70 = *(volatile uint32_t *)(uintptr_t)ZOS_GPIO_PTR_70;
    uint32_t g50 = *(volatile uint32_t *)(uintptr_t)ZOS_GPIO_PTR_50;
    if (ZOS_GPIO_OK(g70)) {
        z->cursorSlot = ZOS_CURSOR_SLOT_70; z->uiEvent = ZOS_UI_EVENT_70;
        z->eventPost = ZOS_EVENT_POST_70;   z->ok = 1u;
    } else if (ZOS_GPIO_OK(g50)) {
        z->cursorSlot = ZOS_CURSOR_SLOT_50; z->uiEvent = ZOS_UI_EVENT_50;
        z->eventPost = ZOS_EVENT_POST_50;   z->ok = 1u;
    } else {
        z->cursorSlot = ZOS_CURSOR_SLOT_70; z->uiEvent = 0u;
        z->eventPost = 0u;                  z->ok = 0u;
    }
}

/* 1 when this slot is under the cursor and the pedal's tempo screen is not
 * open (there the footswitch taps the pedal's tempo). Both slot tables are
 * tried, as sForm 2.8m does (60B firmware variants). */
ZOS_DO_PRAGMA(FUNC_ALWAYS_INLINE(zos_mine))
static inline float zos_mine(const ZosState *z, uint32_t slot)
{
    int32_t cur = (int32_t)*(volatile signed char *)(uintptr_t)ZOS_CURSOR_ADDR;
    if (cur < 0 || cur >= 6) return 0.0f;
    uint32_t ovl = *(volatile uint32_t *)(uintptr_t)ZOS_OVL_STATE_ADDR;
    if ((*(volatile uint32_t *)(uintptr_t)ZOS_OVL_MASK_ADDR & 1u) != 0u && (ovl == 2u || ovl == 5u))
        return 0.0f;
    uint32_t t1 = z->cursorSlot;
    uint32_t t2 = (t1 == ZOS_CURSOR_SLOT_70) ? ZOS_CURSOR_SLOT_50 : ZOS_CURSOR_SLOT_70;
    if (*(volatile uint32_t *)(uintptr_t)(t1 + 24u * (uint32_t)cur) == slot) return 1.0f;
    if (*(volatile uint32_t *)(uintptr_t)(t2 + 24u * (uint32_t)cur) == slot) return 1.0f;
    return 0.0f;
}

ZOS_DO_PRAGMA(FUNC_ALWAYS_INLINE(zos_held))
static inline float zos_held(void)
{
    return ((*(volatile uint32_t *)(uintptr_t)ZOS_FSW_ADDR & 1u) == 0u) ? 1.0f : 0.0f;
}

/* Ask the UI task for a redraw (only on a recognised OS). */
ZOS_DO_PRAGMA(FUNC_ALWAYS_INLINE(zos_redraw))
static inline void zos_redraw(const ZosState *z)
{
    if (z->ok == 0u) return;
    uint32_t ev = *(volatile uint32_t *)(uintptr_t)z->uiEvent;
    if (ev != 0u) ((void (*)(uint32_t, uint32_t))(uintptr_t)z->eventPost)(ev, 0x40u);
}

/* Copy frame k of [len][data]... over the picture (at most picWords words).
 * Never pad one RLE stream with another's zeros: the decoder reads them. */
ZOS_DO_PRAGMA(FUNC_ALWAYS_INLINE(zos_show))
static inline void zos_show(const uint32_t *frames, uint32_t *pic, uint32_t picWords, uint32_t k)
{
    uint32_t j, idx = 0u, w, n;
    for (j = 0u; j < k; j++) idx += 1u + ((frames[idx] + 3u) >> 2);
    n = (frames[idx] + 3u) >> 2;
    if (n > picWords) n = picWords;
    for (w = 0u; w < n; w++) pic[w] = frames[idx + 1u + w];
}

#endif /* ZOOMOS_H */
