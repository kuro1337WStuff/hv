#ifndef HV_PIT_CHANNEL2_H
#define HV_PIT_CHANNEL2_H

/** @file
 * Host-only, caller-owned PC PIT channel 2 calibration oracle. No port I/O,
 * host clock, IRQ, audio, TSC or guest execution. Linux stable f45ed76d6791,
 * arch/x86/kernel/tsc.c: quick_pit_calibrate()/pit_verify_msb() read the
 * live LSB then MSB; pit_calibrate_tsc() polls port 61h bit 5 instead.
 * Intel 8254, Mode Definitions, Mode 0 (Fig. 15): the first input pulse
 * loads N even with GATE low; N further enabled pulses raise OUT. OUT stays
 * high while the binary counter wraps. No sub-pulse gate/setup timing.
 */
#include <stdint.h>

enum { PIT2_IDLE, PIT2_LSB, PIT2_MSB, PIT2_LOAD, PIT2_COUNT };
typedef struct {
    uint64_t tick;
    uint16_t count, reload;
    uint8_t phase, control61, out, read_high;
} PitChannel2;

/** Reset at virtual tick zero with GATE, speaker latch, OUT and counter zero.
 * These are fixture start values, not a claim about firmware reset state. */
static inline void pit2_init(PitChannel2 *p)
{
    const PitChannel2 initial = {0};
    *p = initial;
}

/** Advance to an absolute number of completed 1,193,182 Hz input pulses.
 * Equal times are inert; backwards time fails without changing any state.
 * The cursor may reach UINT64_MAX: no absolute deadline addition is needed. */
static inline int pit2_advance(PitChannel2 *p, uint64_t tick)
{
    uint64_t pulses;
    if (tick < p->tick) return 0;
    pulses = tick - p->tick;
    p->tick = tick;
    if (pulses && p->phase == PIT2_LOAD) {
        p->count = p->reload;
        p->phase = PIT2_COUNT;
        --pulses;
    }
    if (p->phase == PIT2_COUNT && (p->control61 & 1)) {
        if (pulses >= (p->count ? (unsigned)p->count : 65536U)) p->out = 1;
        p->count = (uint16_t)(p->count - pulses);
    }
    return 1;
}

/** Apply one scalar byte write at the cursor. Port 61h retains bits 0/1;
 * OUT readback (bit 5) is harmless in Linux's read-modify-write. Other
 * platform bits are absent. Each count requires a fresh B0h mode command;
 * it cancels previous timing and resets byte sequencing and OUT. This is
 * the driver transaction subset, not all legal 8254 programming. Unsupported
 * ports, widths, commands or sequences return zero with no side effects. */
static inline int pit2_write(PitChannel2 *p, uint16_t port, unsigned width, uint8_t byte)
{
    if (width != 1) return 0;
    if (port == 0x61 && !(byte & ~0x23)) {
        p->control61 = byte & 3;
    } else if (port == 0x43 && byte == 0xb0) {
        p->phase = PIT2_LSB;
        p->read_high = 0;
        p->out = 0;
    } else if (port == 0x42 && p->phase == PIT2_LSB) {
        p->reload = byte;
        p->phase = PIT2_MSB;
    } else if (port == 0x42 && p->phase == PIT2_MSB) {
        p->reload |= (uint16_t)((unsigned)byte << 8);
        p->phase = PIT2_LOAD;
    } else return 0;
    return 1;
}

/** Read one scalar byte without advancing time. Count reads require a full
 * count write; before the load pulse the old counting element is visible
 * (fixture convention; Linux discards its first pair). Each unlatched byte
 * samples its own instant. Port 61h reads do not change the count read phase.
 * Refusal preserves state and destination; p must name a live instance. */
static inline int pit2_read(PitChannel2 *p, uint16_t port, unsigned width, uint8_t *byte)
{
    if (width != 1 || !byte) return 0;
    if (port == 0x61) {
        *byte = (uint8_t)(p->control61 | (p->out << 5));
    } else if (port == 0x42 && (p->phase == PIT2_LOAD || p->phase == PIT2_COUNT)) {
        *byte = (uint8_t)(p->count >> (p->read_high ? 8 : 0));
        p->read_high ^= 1;
    } else return 0;
    return 1;
}
#endif
