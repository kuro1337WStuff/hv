#ifndef HV_PIC8259_H
#define HV_PIC8259_H

/** @file
 * Host-side discovery/init subset of a guest-owned legacy 8259A pair.
 * Source: Linux v6.18.60 arch/x86/kernel/i8259.c, probe_8259A() and init_8259A(0).
 * The data ports are mask registers outside ICW programming, not an IRQ
 * delivery interface. No host port I/O, arbitration, cascade signaling, EOI
 * or injection. pit_pic.h's separate IRQ0 service fixture remains narrower.
 */
#include <stdint.h>

enum {
    PIC8259_MASTER_CMD = 0x20, PIC8259_MASTER_DATA = 0x21,
    PIC8259_SLAVE_CMD = 0xa0, PIC8259_SLAVE_DATA = 0xa1,
    PIC8259_MASTER_BASE = 0x30, PIC8259_SLAVE_BASE = 0x38
};
enum { PIC8259_PROBE, PIC8259_ICW2, PIC8259_ICW3, PIC8259_ICW4, PIC8259_READY };
typedef struct { uint8_t phase, imr; } Pic8259Chip;
typedef struct { Pic8259Chip master, slave; } Pic8259;

/** Start both chips masked with byte IMR access enabled for discovery. These
 * are fixture initial conditions, not an assertion about physical power-on. */
static inline void pic8259_init(Pic8259 *p)
{
    const Pic8259Chip masked = { PIC8259_PROBE, 0xff };
    p->master = p->slave = masked;
}

/** Both chips have completed the pinned normal-EOI, cascaded 8086 setup.
 * Does not certify a particular IMR value, a prior probe, or a working IRQ. */
static inline int pic8259_ready(const Pic8259 *p)
{
    return p->master.phase == PIC8259_READY && p->slave.phase == PIC8259_READY;
}

/** One decoded scalar OUT to a guest port, width in bytes. Caller owns p;
 * string/REP instructions must not be lowered to scalar calls. In IMR mode
 * all eight bits are writable, including bytes which resemble ICWs. During
 * programming, only the next pinned ICW is accepted, independently per chip.
 * ICW1 clears that chip's IMR (8259A, Initialization Command Words); ICW2–4
 * are not mask writes. Return zero before effect for every unsupported access,
 * including an attempted restart mid-sequence. This is a fixture refusal,
 * not the behavior of a complete 8259A for every valid chip command.
 */
static inline int pic8259_write(Pic8259 *p, uint16_t port, unsigned width, uint8_t byte)
{
    Pic8259Chip *chip;
    if (width != 1) return 0;
    switch (port) {
    case PIC8259_MASTER_CMD: case PIC8259_MASTER_DATA: chip = &p->master; break;
    case PIC8259_SLAVE_CMD:  case PIC8259_SLAVE_DATA:  chip = &p->slave; break;
    default: return 0;
    }
    if (port == PIC8259_MASTER_CMD || port == PIC8259_SLAVE_CMD) {
        if (byte != 0x11 || (chip->phase != PIC8259_PROBE && chip->phase != PIC8259_READY)) return 0;
        chip->imr = 0;
        chip->phase = PIC8259_ICW2;
        return 1;
    }
    switch (chip->phase) {
    case PIC8259_PROBE: case PIC8259_READY:
        chip->imr = byte;
        return 1;
    case PIC8259_ICW2:
        if (byte != (port == PIC8259_MASTER_DATA ? PIC8259_MASTER_BASE : PIC8259_SLAVE_BASE)) return 0;
        chip->phase = PIC8259_ICW3;
        return 1;
    case PIC8259_ICW3:
        if (byte != (port == PIC8259_MASTER_DATA ? 0x04 : 0x02)) return 0;
        chip->phase = PIC8259_ICW4;
        return 1;
    case PIC8259_ICW4:
        if (byte != 0x01) return 0;
        chip->phase = PIC8259_READY;
        return 1;
    default: return 0;
    }
}

/** One decoded scalar IN, width in bytes. Expose only byte IMR readback;
 * a partial initialization on this chip, command-port reads (IRR/ISR/poll),
 * or an unsupported form returns zero without touching device or output. */
static inline int pic8259_read(const Pic8259 *p, uint16_t port, unsigned width, uint8_t *byte)
{
    const Pic8259Chip *chip;
    if (width != 1 || !byte) return 0;
    if (port == PIC8259_MASTER_DATA) chip = &p->master;
    else if (port == PIC8259_SLAVE_DATA) chip = &p->slave;
    else return 0;
    if (chip->phase != PIC8259_PROBE && chip->phase != PIC8259_READY) return 0;
    *byte = chip->imr;
    return 1;
}
#endif
