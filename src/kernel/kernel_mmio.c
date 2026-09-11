/*
 * Device-register access from lifted code.
 *
 * recomp_types.h routes every guest load/store whose address falls in
 * [RECOMP_MMIO_LO, RECOMP_MMIO_HI) through these two functions instead of the
 * flat memory mapping. Which device answers is decided here, by address; a
 * range nobody claims behaves as the plain memory it always was, so a title
 * that pokes at a register we do not model sees the same thing it did before
 * this existed.
 *
 * The APU is the first customer. Xbox DirectSound programs the MCPX audio
 * processor through its registers -- voice lists, the frame counter mode,
 * the DSP handshake -- and reads the play cursor back through the voice
 * structures the hardware updates in RAM. Backed as plain memory none of
 * that happens: the emulated APU never sees a write, never runs a frame,
 * and the cursor never moves, which CRI's ADX streamer (JSRF's music
 * player) diagnoses after a few hundred frames as a dead drive and the
 * game turns into a "disc may be dirty or damaged" screen.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "xbox_memory_layout.h"

uint32_t (*g_recomp_mmio_read_fn)(uint32_t addr, unsigned size);
void (*g_recomp_mmio_write_fn)(uint32_t addr, uint32_t val, unsigned size);

static int s_mmio_trace = -1;

static inline volatile void *plain(uint32_t addr)
{
    return (volatile void *)((uintptr_t)addr + xbox_GetMemoryOffset());
}

uint32_t recomp_mmio_read(uint32_t addr, unsigned size)
{
    uint32_t v;
    if (s_mmio_trace < 0) s_mmio_trace = getenv("RECOMP_MMIO_TRACE") ? 1 : 0;
    if (g_recomp_mmio_read_fn)
        v = g_recomp_mmio_read_fn(addr, size);
    else switch (size) {
    case 1:  v = *(volatile uint8_t *)plain(addr); break;
    case 2:  v = *(volatile uint16_t *)plain(addr); break;
    default: v = *(volatile uint32_t *)plain(addr); break;
    }
    if (s_mmio_trace)
        fprintf(stderr, "  [MMIO] rd%u 0x%08X -> 0x%08X\n", size * 8, addr, v);
    return v;
}

void recomp_mmio_write(uint32_t addr, uint32_t val, unsigned size)
{
    if (s_mmio_trace < 0) s_mmio_trace = getenv("RECOMP_MMIO_TRACE") ? 1 : 0;
    if (s_mmio_trace)
        fprintf(stderr, "  [MMIO] wr%u 0x%08X <- 0x%08X\n", size * 8, addr, val);
    /* Keep the plain-memory image current as well, so a later read of a
     * register the model does not answer (and the debugger) see the value. */
    switch (size) {
    case 1:  *(volatile uint8_t *)plain(addr) = (uint8_t)val; break;
    case 2:  *(volatile uint16_t *)plain(addr) = (uint16_t)val; break;
    default: *(volatile uint32_t *)plain(addr) = val; break;
    }
    if (g_recomp_mmio_write_fn)
        g_recomp_mmio_write_fn(addr, val, size);
}
