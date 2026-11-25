#ifndef HV_PIT_PIC_H
#define HV_PIT_PIC_H

#include <stdint.h>

#include "pic8259.h"

enum { PIT_PIC_HZ = 1193182, PIT_PIC_VECTOR = PIC8259_MASTER_BASE };
enum { PIT_PIC_OFF, PIT_PIC_LOW, PIT_PIC_HIGH };

typedef struct {
    uint64_t now, deadline;     /* absolute virtual PIT input ticks since reset */
    uint8_t phase, low;         /* channel 0 binary mode 4, low/high reload */
    uint8_t armed, imr, pending, in_service;
} PitPic;

/* Convert total unsigned elapsed nanoseconds since a fixed epoch, rounding down.
 * Do not round each interval separately. Even for UINT64_MAX ns the whole-second
 * product fits (Hz < 10^9); the fractional numerator is below 10^9 * Hz. */
static inline int pit_pic_ns_to_ticks(uint64_t elapsed_ns, uint64_t *ticks)
{
    const uint64_t billion = UINT64_C(1000000000);
    if (!ticks) return 0;
    *ticks = (elapsed_ns / billion) * PIT_PIC_HZ +
             ((elapsed_ns % billion) * PIT_PIC_HZ) / billion;
    return 1;
}

static inline void pit_pic_init(PitPic *p)
{
    p->now = p->deadline = 0;
    p->phase = PIT_PIC_OFF;
    p->low = p->armed = p->pending = p->in_service = 0;
    p->imr = 0xff;
}

/* Timer shutdown/reprogramming cannot retract an edge already at the PIC. */
static inline void pit_pic_cancel(PitPic *p)
{
    p->deadline = 0;
    p->armed = p->low = 0;
    p->phase = PIT_PIC_OFF;
}

/* Each transaction is a scalar byte. Only IMR bit 0 is guest-controlled; the
 * IRQ0-specific EOI must follow masking in the chosen driver's service path.
 * Refusal is fixture policy, not a claim that a physical 8259 raises a fault. */
static inline int pit_pic_write(PitPic *p, uint16_t port, uint8_t width, uint8_t byte)
{
    if (width != 1) return 0;
    switch (port) {
    case PIC8259_MASTER_DATA:
        if (byte != 0xff && byte != 0xfe) return 0;
        p->imr = byte;
        return 1;
    case PIC8259_MASTER_CMD:
        if (byte != 0x60 || !p->in_service || !(p->imr & 1)) return 0;
        p->in_service = 0;
        return 1;
    case 0x43:
        if (byte != 0x38) return 0;
        pit_pic_cancel(p);
        p->phase = PIT_PIC_LOW;
        return 1;
    case 0x40:
        /* Another count may be written without a mode command. Its low byte
         * alone does not cancel or move an existing deadline. Only the high
         * byte commits a replacement at now + N ticks. This is a scheduling
         * boundary, not a simulation of PIT OUT pin strobes or clock phase. */
        if (p->phase == PIT_PIC_LOW) {
            p->low = byte;
            p->phase = PIT_PIC_HIGH;
            return 1;
        }
        if (p->phase == PIT_PIC_HIGH) {
            uint32_t reload = ((uint32_t)byte << 8) | p->low;
            if (!reload) reload = 65536;  /* binary PIT zero encoding */
            if (UINT64_MAX - p->now < reload) return 0;
            p->deadline = p->now + reload;
            p->armed = 1;
            p->low = 0;
            p->phase = PIT_PIC_LOW;
            return 1;
        }
        return 0;
    default:
        return 0;
    }
}

static inline int pit_pic_read(const PitPic *p, uint16_t port, uint8_t width, uint8_t *byte)
{
    if (port != PIC8259_MASTER_DATA || width != 1 || !byte) return 0;
    *byte = p->imr;
    return 1;
}

/* One semantic IRQ0 edge. The IRR is a bit, not a queue; the mask and ISR
 * inhibit acceptance, not latching. This does not advance either timer. */
static inline void pit_pic_latch_irq0(PitPic *p)
{
    p->pending = 1;
}

/* One rising edge per completed one-shot. */
static inline int pit_pic_advance(PitPic *p, uint64_t absolute_tick)
{
    if (absolute_tick < p->now) return 0;
    p->now = absolute_tick;
    if (p->armed && p->now >= p->deadline) {
        p->armed = 0;
        p->deadline = 0;
        pit_pic_latch_irq0(p);
    }
    return 1;
}

/* Bounded virtual wait for a new expiry, not host sleep or IRQ acceptance.
 * A missing deadline or insufficient budget leaves time and devices unchanged. */
static inline int pit_pic_wake(PitPic *p, uint64_t absolute_limit_tick)
{
    if (!p->armed || p->deadline > absolute_limit_tick) return 0;
    return pit_pic_advance(p, p->deadline);
}

/* Caller checks vCPU delivery eligibility before taking PIT_PIC_VECTOR. */
static inline int pit_pic_take(PitPic *p)
{
    if (!p->pending || p->in_service || (p->imr & 1)) return 0;
    p->pending = 0;
    p->in_service = 1;
    return 1;
}

#endif
