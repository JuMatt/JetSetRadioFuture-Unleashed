/*
 * ohci.c -- OHCI 1.0a host controller registers for the MCPX.
 *
 * See ohci.h for what this is and is not. The short version: enough for a
 * title's own USB driver to find a controller and a populated root hub port,
 * plus a trace of every register access, because what the driver does after
 * that decides how the rest gets built.
 */
#include "ohci.h"
#include "../platform/mmio_decode.h"
#include "../kernel/xbox_memory_layout.h"
#include "usb_gamepad.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

/* The runtime maps guest memory at a fixed host offset. */
extern ptrdiff_t xbox_GetMemoryOffset(void);

/* Calling the title's interrupt service routine.
 *
 * A recompiled function reads its arguments off the guest stack and keeps its
 * registers in thread-local storage, so it can only be called from a thread
 * that has both. xbox_worker_stack_alloc hands out a guest stack slice for
 * exactly this -- a host thread calling recompiled code -- and recomp_lookup
 * turns a guest address into something callable.
 */
typedef void (*recomp_func_t)(void);
extern recomp_func_t recomp_lookup(uint32_t xbox_va);
extern int  xbox_worker_stack_alloc(void);
extern void xbox_worker_stack_free(int slot);
extern uint32_t xbox_GetConnectedInterrupt(uint32_t vector);

/* Declared in the generated runtime; thread-local, so the values below are
 * this thread's and not the guest thread's. */
#if defined(_MSC_VER)
#  define OHCI_TLS __declspec(thread)
#else
#  define OHCI_TLS __thread
#endif
extern OHCI_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern OHCI_TLS uint32_t g_ebx, g_esi, g_edi;
extern OHCI_TLS uint32_t g_fs_base;
extern uint32_t xbox_AllocThreadTib(void);

/* The vector XPP takes for USB0. HalGetInterruptVector(1) returns 1 here, and
 * the bus interrupt level is what the XDK passes. */
#define OHCI_VECTOR  1

/* ---- OHCI 1.0a operational registers, by byte offset ------------------- */
#define HcRevision              0x00
#define HcControl               0x04
#define HcCommandStatus         0x08
#define HcInterruptStatus       0x0C
#define HcInterruptEnable       0x10
#define HcInterruptDisable      0x14
#define HcHCCA                  0x18
#define HcPeriodCurrentED       0x1C
#define HcControlHeadED         0x20
#define HcControlCurrentED      0x24
#define HcBulkHeadED            0x28
#define HcBulkCurrentED         0x2C
#define HcDoneHead              0x30
#define HcFmInterval            0x34
#define HcFmRemaining           0x38
#define HcFmNumber              0x3C
#define HcPeriodicStart         0x40
#define HcLSThreshold           0x44
#define HcRhDescriptorA         0x48
#define HcRhDescriptorB         0x4C
#define HcRhStatus              0x50
#define HcRhPortStatus1         0x54
/* Through port 4. The root hub here reports two downstream ports, and the
 * driver reads four status registers regardless -- so the file has to cover
 * them or the last one falls outside the array and is answered by the
 * not-a-register path instead of by an empty port. Same value either way,
 * reached honestly. */
#define OHCI_REG_MAX            0x64

/* HcCommandStatus */
#define CS_HCR                  0x00000001u   /* host controller reset       */

/* HcInterruptStatus / Enable */
#define INTR_SO                 0x00000001u   /* scheduling overrun          */
#define INTR_WDH                0x00000002u   /* writeback done head         */
#define INTR_SF                 0x00000004u   /* start of frame              */
#define INTR_SF                 0x00000004u   /* start of frame              */
#define INTR_RD                 0x00000008u   /* resume detected             */
#define INTR_UE                 0x00000010u   /* unrecoverable error         */
#define INTR_FNO                0x00000020u   /* frame number overflow       */
#define INTR_RHSC               0x00000040u   /* root hub status change      */
#define INTR_MIE                0x80000000u   /* master interrupt enable     */

/* HcRhPortStatus */
#define PORT_CCS                0x00000001u   /* current connect status      */
#define PORT_PES                0x00000002u   /* port enable status          */
#define PORT_PSS                0x00000004u   /* port suspend status         */
#define PORT_PPS                0x00000100u   /* port power status           */
#define PORT_LSDA               0x00000200u   /* low speed device attached   */
#define PORT_CSC                0x00010000u   /* connect status change       */
#define PORT_PESC               0x00020000u   /* enable status change        */
#define PORT_PRSC               0x00100000u   /* reset status change         */

/* Writes to HcRhPortStatus set/clear by bit position rather than by value. */
#define PORT_W_CCS_CLEAR_ENABLE 0x00000001u   /* ClearPortEnable             */
#define PORT_W_PES_SET_ENABLE   0x00000002u   /* SetPortEnable               */
#define PORT_W_PRS_SET_RESET    0x00000010u   /* SetPortReset                */
#define PORT_W_PPS_SET_POWER    0x00000100u   /* SetPortPower                */
#define PORT_W_CLEAR_POWER      0x00000200u   /* ClearPortPower              */

/* The MCPX gives each controller a small root hub. Two ports apiece covers
 * the console's four front sockets, which is what a title enumerates over. */
#define OHCI_PORTS              2

typedef struct {
    uint32_t base;                      /* Xbox VA of the register block   */
    uint32_t reg[OHCI_REG_MAX / 4];
    unsigned reads, writes, decode_fail;
    int      index;
} OhciController;

static OhciController s_hc[2];
static int s_enabled;
static int s_trace;

static uint32_t *reg_of(OhciController *hc, uint32_t off)
{
    return (off < OHCI_REG_MAX) ? &hc->reg[off / 4] : NULL;
}

/* ---- register semantics ------------------------------------------------ */

static uint64_t ohci_read(void *dev, uint32_t off, int size)
{
    OhciController *hc = (OhciController *)dev;
    uint32_t aligned = off & ~3u;
    uint32_t *r = reg_of(hc, aligned);
    uint32_t v = r ? *r : 0;

    hc->reads++;

    /* HcFmRemaining and HcFmNumber advance on their own. A driver that waits
     * for the frame counter to move is waiting for the controller to be
     * running, and a counter that never changes is a controller that is not.
     * Derived from the read count rather than a timer: it only has to be
     * monotonic, and a timer here would need a thread to be worth having. */
    if (aligned == HcFmNumber)
        v = (hc->reads >> 3) & 0xFFFFu;
    else if (aligned == HcFmRemaining)
        v = (hc->reads * 977u) & 0x3FFFu;

    /* Sub-dword reads take their slice of the containing register. */
    if (size < 4) {
        unsigned shift = (off & 3u) * 8u;
        v >>= shift;
        if (size == 1) v &= 0xFFu;
        else if (size == 2) v &= 0xFFFFu;
    }

    if (s_trace) {
        static unsigned n;
        if (n++ < 600)
            fprintf(stderr, "  [OHCI%d] read  +0x%02X = %08X\n",
                    hc->index, off, (uint32_t)v);
    }
    return v;
}

static void ohci_write(void *dev, uint32_t off, uint64_t val, int size)
{
    OhciController *hc = (OhciController *)dev;
    uint32_t aligned = off & ~3u;
    uint32_t *r = reg_of(hc, aligned);
    uint32_t v = (uint32_t)val;

    hc->writes++;
    if (!r)
        return;

    if (s_trace) {
        static unsigned n;
        if (n++ < 600)
            fprintf(stderr, "  [OHCI%d] write +0x%02X = %08X\n",
                    hc->index, off, v);
    }

    switch (aligned) {
    case HcRevision:                    /* read-only */
        return;

    case HcCommandStatus:
        /* HCR is self-clearing: the controller resets and drops the bit, and
         * a driver polls for exactly that. Leaving it set is a hang, and it
         * is the first thing a driver does, so it would be the only thing
         * anyone ever saw of this file. */
        *r |= v;
        if (*r & CS_HCR) {
            *r &= ~CS_HCR;
            hc->reg[HcControl / 4] &= ~0xC0u;    /* back to UsbReset state  */
            hc->reg[HcInterruptStatus / 4] = 0;
            hc->reg[HcInterruptEnable / 4] = 0;
        }
        return;

    case HcInterruptStatus:
        *r &= ~v;                       /* write 1 to clear                 */
        return;

    case HcInterruptEnable:
        hc->reg[HcInterruptEnable / 4] |= v;
        return;

    case HcInterruptDisable:
        hc->reg[HcInterruptEnable / 4] &= ~v;
        return;

    case HcFmNumber:
    case HcFmRemaining:
        return;                         /* driven by the controller         */

    case HcRhDescriptorA:
        /* NumberDownstreamPorts is ours; the driver may set the power and
         * over-current policy bits above it. */
        *r = (*r & 0x000000FFu) | (v & ~0x000000FFu);
        return;

    case HcRhPortStatus1:
    case HcRhPortStatus1 + 4: {
        /* Writes here are set/clear requests by bit position, not a value to
         * store. Getting that wrong looks like a port that will not enable. */
        unsigned port = (aligned - HcRhPortStatus1) / 4;
        uint32_t *ps = &hc->reg[(HcRhPortStatus1 + port * 4) / 4];

        if (v & PORT_W_CCS_CLEAR_ENABLE) *ps &= ~PORT_PES;
        if (v & PORT_W_PES_SET_ENABLE)   *ps |= (*ps & PORT_CCS) ? PORT_PES : 0;
        if (v & PORT_W_PPS_SET_POWER)    *ps |= PORT_PPS;
        if (v & PORT_W_CLEAR_POWER)      *ps &= ~PORT_PPS;
        if (v & PORT_W_PRS_SET_RESET) {
            /* Reset completes immediately -- there is no wire to settle -- so
             * PRS is never observed set. What matters is what the driver
             * checks afterwards: a present device comes back enabled, and the
             * reset-change bit says the reset finished.
             *
             * Only the status bit is set here. Delivering the interrupt is the
             * controller thread's job, because this runs on the guest's own
             * thread inside a fault handler, and pointing g_esp at a worker
             * stack from here would overwrite the stack pointer of the thread
             * being interrupted. */
            if (*ps & PORT_CCS)
                *ps |= PORT_PES;
            *ps |= PORT_PRSC;
            hc->reg[HcInterruptStatus / 4] |= INTR_RHSC;
        }
        /* The change bits are write-1-to-clear, in the high half. */
        *ps &= ~(v & 0xFFFF0000u);
        return;
    }

    default:
        break;
    }

    if (size == 4) {
        *r = v;
    } else {
        unsigned shift = (off & 3u) * 8u;
        uint32_t mask = ((size == 1) ? 0xFFu : 0xFFFFu) << shift;
        *r = (*r & ~mask) | ((v << shift) & mask);
    }
}

/* ---- the control list -------------------------------------------------- */
/*
 * A host controller is a bus master: the driver builds endpoint and transfer
 * descriptors in RAM, points HcControlHeadED at them and sets ControlListFilled,
 * and the controller walks that list itself. Nothing arrives through MMIO, so
 * none of this is visible to the register trace -- which is why the driver
 * looked idle after the port came up.
 *
 * OHCI 1.0a, section 4. An endpoint descriptor is four dwords:
 *
 *   +0  FA | EN<<7 | D<<11 | S<<13 | K<<14 | F<<15 | MPS<<16
 *   +4  TailP        queue tail, 16-byte aligned
 *   +8  HeadP        queue head, with Halted in bit 0 and toggleCarry in bit 1
 *   +C  NextED
 *
 * and a general transfer descriptor is four more:
 *
 *   +0  ... DP<<19 | DI<<21 | T<<24 | EC<<26 | CC<<28
 *   +4  CBP          current buffer pointer, 0 when the transfer moved nothing
 *   +8  NextTD
 *   +C  BE           last byte of the buffer, inclusive
 *
 * A transfer is done when HeadP reaches TailP. Completed descriptors go on the
 * done queue, newest first, and the controller publishes it in the HCCA and
 * raises WritebackDoneHead.
 */
#define ED_SKIP        (1u << 14)
#define ED_HEAD_HALT   (1u << 0)
#define ED_HEAD_TOGGLE (1u << 1)
#define ED_PTR_MASK    0xFFFFFFF0u

#define TD_DP_SETUP    0u
#define TD_DP_OUT      1u
#define TD_DP_IN       2u
#define TD_CC_NOERROR  0u
#define TD_CC_STALL    4u

#define HCCA_DONE_HEAD 0x84

static uint32_t g_setup_pending;      /* wLength of the last SETUP seen */
static uint32_t g_setup_sent;   /* how much of the response has gone */
static UsbSetup g_setup;

/* Every address below came out of guest memory, so none of them are trusted.
 *
 * This is not theoretical. The driver writes HcControlHeadED twice during
 * bring-up, and the second write is 0xCCCCCCCC -- MSVC's uninitialised-memory
 * fill, left there by a local that was never assigned. Following it lands far
 * outside the mapping and takes the runtime down with an access violation,
 * which is a crash the title itself would never have had. A device model
 * following a pointer a title left lying around has to check it first.
 */
/*
 * Where the driver's descriptors actually are.
 *
 * A host controller is a bus master: every pointer it follows -- the HCCA,
 * the endpoint descriptors, the transfer descriptors, the buffers -- is a
 * physical address, and the console's USB driver allocates them from
 * contiguous memory, which this runtime places in its own sixty-four megabyte
 * window at Xbox VA 0x80000000. The range check below compared against the
 * size of RAM, so every one of those pointers was out of range and the
 * controller read zero from all of them: it found a null control list and an
 * empty interrupt table, forever, and reported no error because reading zero
 * from an empty list is exactly what an idle controller does.
 *
 * The result was a port where a gamepad enumerates -- descriptors, address,
 * configuration -- and is then never asked for a single report, which is why
 * no button has ever reached this game. The diagnostic meant to catch it says
 * "the guest is reading the pad" on the first report, so it printed nothing
 * at all, in every log ever captured.
 */
#define OHCI_CONTIG_BASE 0x80000000u   /* kernel.h's XBOX_CONTIG_BASE, which
                                        * this file does not include: the
                                        * device models are built without the
                                        * kernel's surface on purpose. */
#define OHCI_CONTIG_SIZE (64u * 1024u * 1024u)

static int guest_ok(uint32_t va, uint32_t bytes)
{
    size_t mapped = xbox_GetMappedSize();
    if (va == 0)
        return 0;
    if (va >= OHCI_CONTIG_BASE)
        return (uint64_t)(va - OHCI_CONTIG_BASE) + bytes <= OHCI_CONTIG_SIZE;
    return mapped != 0 && (uint64_t)va + bytes <= (uint64_t)mapped;
}

static uint32_t rd32(uint32_t va)
{
    if (!guest_ok(va, 4))
        return 0;
    return *(uint32_t *)((uint8_t *)xbox_GetMemoryOffset() + va);
}
static void wr32(uint32_t va, uint32_t v)
{
    if (!guest_ok(va, 4))
        return;
    *(uint32_t *)((uint8_t *)xbox_GetMemoryOffset() + va) = v;
}
static uint8_t *guest_ptr(uint32_t va, uint32_t bytes)
{
    return guest_ok(va, bytes)
         ? (uint8_t *)xbox_GetMemoryOffset() + va : NULL;
}

/* Move one transfer descriptor. Returns the condition code to report. */
static uint32_t ohci_do_td(OhciController *hc, uint32_t ed0, uint32_t td)
{
    uint32_t info = rd32(td);
    uint32_t cbp  = rd32(td + 4);
    uint32_t be   = rd32(td + 12);
    uint32_t dp   = (info >> 19) & 3u;
    int      len  = (cbp && be >= cbp) ? (int)(be - cbp + 1) : 0;
    uint32_t endpoint = (ed0 >> 7) & 0xFu;
    int      moved = 0;

    if (dp == TD_DP_SETUP) {
        /* Eight bytes of setup, kept for the data stage that follows. */
        if (len >= 8) {
            const uint8_t *p = guest_ptr(cbp, 8);
            if (!p) return TD_CC_NOERROR;
            g_setup.bmRequestType = p[0];
            g_setup.bRequest      = p[1];
            g_setup.wValue        = (uint16_t)(p[2] | (p[3] << 8));
            g_setup.wIndex        = (uint16_t)(p[4] | (p[5] << 8));
            g_setup.wLength       = (uint16_t)(p[6] | (p[7] << 8));
            g_setup_pending = 1;
            g_setup_sent = 0;
            if (s_trace) {
                fprintf(stderr, "  [OHCI%d] SETUP %02X %02X value %04X "
                                "index %04X len %u\n",
                        hc->index, g_setup.bmRequestType, g_setup.bRequest,
                        g_setup.wValue, g_setup.wIndex, g_setup.wLength);
                fflush(stderr);
            }
            moved = 8;
        }
    } else if (dp == TD_DP_IN) {
        if (endpoint == 0) {
            /* Data stage of a control transfer, or its status stage when the
             * driver asks for nothing. */
            uint8_t buf[256];
            int n, off, part;

            if (!g_setup_pending)
                return TD_CC_NOERROR;
            n = usb_gamepad_control(&g_setup, buf, (int)sizeof buf);
            if (n < 0) {
                g_setup_pending = 0;
                return TD_CC_STALL;
            }
            /*
             * A control response arrives in packets, and each packet is a
             * descriptor of its own.
             *
             * The device's endpoint zero takes eight bytes at a time, so a
             * thirty-two byte configuration descriptor is four transfer
             * descriptors and the driver queues them one after another.
             * Answering each one from the start of the response sends the
             * first eight bytes four times, which the driver reads as a
             * malformed descriptor -- and its recovery is to reset the port
             * and enumerate again, so the symptom is a device that enumerates
             * forever and never reaches its endpoints. How much has gone is
             * the whole fix.
             */
            off = (int)g_setup_sent;
            part = n - off;
            if (part < 0) part = 0;
            if (part > len) part = len;
            if (part > 0 && guest_ptr(cbp, (uint32_t)part))
                memcpy(guest_ptr(cbp, (uint32_t)part), buf + off, (size_t)part);
            g_setup_sent += (uint32_t)part;
            moved = part;
            if (s_trace) {
                fprintf(stderr, "  [OHCI%d] IN ep0 %d bytes (%u of %d)\n",
                        hc->index, part, g_setup_sent, n);
                fflush(stderr);
            }
        } else {
            /* The pad's report, on its interrupt endpoint. */
            uint8_t rep[32];
            int n = usb_gamepad_report(rep, (int)sizeof rep);
            if (n > len) n = len;
            if (n > 0 && guest_ptr(cbp, (uint32_t)n))
                memcpy(guest_ptr(cbp, (uint32_t)n), rep, (size_t)n);
            moved = n;
        }
    } else {
        /* OUT: the status stage of an IN control transfer, or rumble. Both
         * are accepted and discarded. */
        moved = len;
        g_setup_pending = 0;
    }

    /* CBP is zero when everything asked for moved, and otherwise points past
     * what did. A driver computes the transferred length from it. */
    wr32(td + 4, (moved >= len) ? 0u : cbp + (uint32_t)moved);
    return TD_CC_NOERROR;
}

/* Walk the control list once. Returns 1 if anything completed. */
/*
 * Walk one list of endpoint descriptors and move whatever is queued on them.
 *
 * Split out of ohci_run_control_list because the periodic list needs exactly
 * the same walk: an ED list is an ED list, and the only thing that differs
 * between the control list and the thirty-two interrupt lists is where the
 * head pointer comes from. `done_head` and `completed` accumulate across
 * calls, so one tick's worth of lists produces one done queue and one
 * writeback-done interrupt rather than one per list.
 */
static void ohci_run_ed_list(OhciController *hc, uint32_t ed,
                             uint32_t *done_head, int *completed)
{
    int guard = 0;

    while (guest_ok(ed, 16) && ++guard < 64) {
        uint32_t ed0  = rd32(ed);
        uint32_t tail = rd32(ed + 4) & ED_PTR_MASK;
        uint32_t head = rd32(ed + 8);
        int tguard = 0;

        if (ed0 & ED_SKIP) { ed = rd32(ed + 12) & ED_PTR_MASK; continue; }
        if (head & ED_HEAD_HALT) { ed = rd32(ed + 12) & ED_PTR_MASK; continue; }

        while ((head & ED_PTR_MASK) != tail
            && guest_ok(head & ED_PTR_MASK, 16) && ++tguard < 64) {
            uint32_t td = head & ED_PTR_MASK;
            uint32_t next = rd32(td + 8) & ED_PTR_MASK;
            uint32_t cc = ohci_do_td(hc, ed0, td);

            /* Report the outcome where the driver reads it, then put the
             * descriptor on the done queue, newest first. */
            wr32(td, (rd32(td) & 0x0FFFFFFFu) | (cc << 28));
            wr32(td + 8, *done_head);
            *done_head = td;
            (*completed)++;

            head = next | (head & ED_HEAD_TOGGLE);
            if (cc != TD_CC_NOERROR) {
                head |= ED_HEAD_HALT;       /* a stall halts the endpoint */
                break;
            }
        }
        wr32(ed + 8, head);
        ed = rd32(ed + 12) & ED_PTR_MASK;
    }
}

static void ohci_finish_lists(OhciController *hc, uint32_t done_head,
                              int completed)
{
    if (completed) {
        uint32_t hcca = hc->reg[HcHCCA / 4];
        if (hcca)
            wr32(hcca + HCCA_DONE_HEAD, done_head);
        hc->reg[HcDoneHead / 4] = done_head;
        hc->reg[HcInterruptStatus / 4] |= INTR_WDH;
    }
}

static int ohci_run_control_list(OhciController *hc)
{
    uint32_t done_head = 0;
    int completed = 0;
    ohci_run_ed_list(hc, hc->reg[HcControlHeadED / 4] & ED_PTR_MASK,
                     &done_head, &completed);
    ohci_finish_lists(hc, done_head, completed);
    return completed;
}

/*
 * The periodic list, which is where the controller finds the pad.
 *
 * This was missing, and its absence is why nothing has ever pressed a button
 * in this port. A gamepad reports on an interrupt endpoint, and an interrupt
 * endpoint is never on the control list: the driver hangs its ED off the
 * thirty-two-entry table at the start of the HCCA and the controller walks
 * one entry per millisecond frame. With only the control list walked, the
 * device enumerated perfectly -- descriptors, address, configuration, all of
 * it over endpoint zero -- and then was never asked for a single report. The
 * diagnostic that was supposed to catch this said "the guest is reading the
 * pad" on the first report and so printed nothing at all, in every log ever
 * captured, which is the quietest possible way for a feature to be absent.
 *
 * All thirty-two entries are walked per tick rather than one, because this
 * ticks at fifty hertz and the hardware at a thousand: taking one entry per
 * tick would poll an endpoint listed in four of them once every eighty
 * milliseconds. An ED reached through several entries is serviced once --
 * hence `seen` -- so a tick delivers one report per endpoint, and the pad is
 * read as often as this loop runs.
 */
static int ohci_run_periodic_list(OhciController *hc)
{
    uint32_t hcca = hc->reg[HcHCCA / 4];
    uint32_t done_head = 0, seen[32];
    int completed = 0, nseen = 0, i;

    if (!hcca || !guest_ok(hcca, 0x100))
        return 0;

    for (i = 0; i < 32; i++) {
        uint32_t ed = rd32(hcca + (uint32_t)i * 4) & ED_PTR_MASK;
        int j, dup = 0;
        if (!ed) continue;
        for (j = 0; j < nseen; j++) if (seen[j] == ed) { dup = 1; break; }
        if (dup) continue;
        if (nseen < 32) seen[nseen++] = ed;
        ohci_run_ed_list(hc, ed, &done_head, &completed);
    }
    ohci_finish_lists(hc, done_head, completed);
    return completed;
}

/* ---- raising an interrupt --------------------------------------------- */

/* Call the title's ISR on this thread, with a guest stack under it.
 *
 * BOOLEAN ServiceRoutine(PKINTERRUPT Interrupt, PVOID ServiceContext), stdcall,
 * so the two arguments go on the stack right to left with a return address on
 * top. The sentinel is what the routine pops on the way out; nothing jumps to
 * it, and a recognisable value beats a real address if it ever shows up in a
 * report.
 *
 * Returns what the routine returned: an ISR that does not claim the interrupt
 * returns FALSE, and that is worth seeing rather than assuming.
 */
static int ohci_call_isr(OhciController *hc)
{
    uint32_t kinterrupt = xbox_GetConnectedInterrupt(OHCI_VECTOR);
    uint32_t routine, context;
    recomp_func_t fn;
    uint8_t *mem;
    int slot;

    if (!kinterrupt)
        return -1;                      /* nothing connected yet            */

    mem     = (uint8_t *)xbox_GetMemoryOffset();
    routine = *(uint32_t *)(mem + kinterrupt + 0);
    context = *(uint32_t *)(mem + kinterrupt + 4);
    if (!routine)
        return -1;

    fn = recomp_lookup(routine);
    if (!fn) {
        fprintf(stderr, "  [OHCI%d] ISR 0x%08X has no translation\n",
                hc->index, routine);
        fflush(stderr);
        return -1;
    }

    /* One slice for the life of the raise. Sixteen exist and this takes one
     * only while the routine runs, so a title using them for its own workers
     * is not starved by a controller that interrupts. */
    slot = xbox_worker_stack_alloc();
    if (slot < 0) {
        fprintf(stderr, "  [OHCI%d] no worker stack for the ISR\n", hc->index);
        fflush(stderr);
        return -1;
    }

    g_esp = XBOX_WORKER_STACK_TOP(slot);
    g_eax = g_ecx = g_edx = g_ebx = g_esi = g_edi = 0;

    g_esp -= 4; *(uint32_t *)(mem + g_esp) = context;      /* arg 2 */
    g_esp -= 4; *(uint32_t *)(mem + g_esp) = kinterrupt;   /* arg 1 */
    g_esp -= 4; *(uint32_t *)(mem + g_esp) = 0xDEADBEEFu;  /* return address */

    fn();

    xbox_worker_stack_free(slot);
    return (int)(g_eax & 1u);
}

/* Set the status bits and, if the driver has unmasked them, call the ISR.
 *
 * MIE is the master enable and HcInterruptEnable is the per-source mask; a
 * controller that interrupts through either of those while they are clear is
 * a controller the driver has every right to be confused by.
 */
static void ohci_raise(OhciController *hc, uint32_t source)
{
    uint32_t enable = hc->reg[HcInterruptEnable / 4];
    int claimed;

    hc->reg[HcInterruptStatus / 4] |= source;

    if (!(enable & INTR_MIE) || !(enable & source))
        return;

    claimed = ohci_call_isr(hc);
    if (s_trace || claimed >= 0) {
        static unsigned n;
        if (n++ < 20) {
            fprintf(stderr, "  [OHCI%d] raised %08X -> ISR %s\n",
                    hc->index, source,
                    claimed < 0 ? "not callable" :
                    claimed ? "claimed it" : "declined it");
            fflush(stderr);
        }
    }
}

/* ---- bring-up ---------------------------------------------------------- */

static void ohci_reset(OhciController *hc, uint32_t base, int index)
{
    memset(hc, 0, sizeof *hc);
    hc->base  = base;
    hc->index = index;

    hc->reg[HcRevision / 4]       = 0x00000010u;   /* OHCI 1.0             */
    hc->reg[HcFmInterval / 4]     = 0x27782EDFu;   /* 11999, FSMPS default */
    hc->reg[HcPeriodicStart / 4]  = 0x00003E67u;   /* 90% of the frame     */
    hc->reg[HcLSThreshold / 4]    = 0x00000628u;

    /* Root hub: OHCI_PORTS downstream, ports always powered, no over-current
     * reporting. NoPowerSwitching keeps a driver from waiting on a power-on
     * sequence that has nothing to switch. */
    hc->reg[HcRhDescriptorA / 4]  = (uint32_t)OHCI_PORTS | (1u << 9);
    hc->reg[HcRhDescriptorB / 4]  = 0x00000000u;
    hc->reg[HcRhStatus / 4]       = 0x00000000u;

    /* Ports powered and empty. The gamepad is not here yet, deliberately.
     *
     * Presenting it as already connected does not work, and the reason is
     * worth keeping: the driver scans the root hub itself during bring-up,
     * sees the connect-status-change bit, clears it, and by the time it
     * unmasks the root hub interrupt there is no change left to report. The
     * pending status bit does not survive either, because the driver resets
     * the controller first and a reset clears interrupt status -- both of
     * those are correct behaviour, and between them a device that was always
     * there is a device that never arrives.
     *
     * A console detects the port change after the controller is running, so
     * that is what the controller thread does: it waits for operational with
     * the interrupt unmasked, and only then plugs the device in. */
    hc->reg[HcRhPortStatus1 / 4]       = PORT_PPS;
    hc->reg[(HcRhPortStatus1 + 4) / 4] = PORT_PPS;
}

/* The controller, running on its own thread.
 *
 * It has to be its own thread for a reason that is easy to get wrong: the
 * guest register file is thread-local, so setting g_esp to a worker stack on
 * the guest's thread would overwrite the guest's own stack pointer mid-call.
 * A separate thread has its own copy, and it also happens to be what the
 * hardware does -- an interrupt arrives when the controller decides, not when
 * the driver next reads a register.
 *
 * ponytail: one delivery, of the root hub status change that is already
 * pending from bring-up. There is nothing behind it yet -- no descriptor list
 * walking, so an enumeration attempt has nothing to answer it -- and the point
 * of this delivery is to find out what the driver does when it finally gets
 * the interrupt it has been waiting for. Repeat delivery and the transfer
 * lists come after that answer, not before it.
 */
static DWORD WINAPI ohci_thread(LPVOID unused)
{
    /* This thread calls recompiled code, so it needs what any thread running
     * recompiled code needs: its own TIB. The guest register set is already
     * thread-local and the ISR gets a worker stack, but fs:[0] is the SEH
     * chain head and fs:[4] reaches the CRT's per-thread data -- and a guest
     * function with an SEH prologue on a thread whose g_fs_base is zero
     * dereferences null before it executes a line of its own body. That is
     * what killed the process here, two interrupts in, with no fault report
     * because the fault was in the runtime rather than in the title. */
    unsigned waited = 0;
    int plugged = 0;
    uint32_t last_status = 0;
    unsigned repeats = 0;

    (void)unused;
    {
        uint32_t tib = xbox_AllocThreadTib();
        if (!tib) {
            fprintf(stderr, "  [OHCI0] no TIB for the controller thread; "
                            "not delivering interrupts\n");
            fflush(stderr);
            return 0;
        }
        g_fs_base = tib;
    }

    for (;;) {
        OhciController *hc = &s_hc[0];
        uint32_t control, enable, status;

        Sleep(20);
        /* The guest-lock watchdog rides here because this loop is the only
         * one in the process that ticks steadily and never wants the lock
         * itself -- a watchdog that can be blocked by the thing it is
         * watching reports nothing at exactly the moment it matters. */
        { extern void xbox_guest_lock_report(void) __attribute__((weak));
          extern void xbox_guest_lock_stats(void) __attribute__((weak));
          extern void xbox_guest_cs_report(void) __attribute__((weak));
          extern void recomp_fp_report(void) __attribute__((weak));
          if (xbox_guest_lock_report) xbox_guest_lock_report();
          if (xbox_guest_lock_stats)  xbox_guest_lock_stats();
          if (xbox_guest_cs_report)   xbox_guest_cs_report();
          /* Same reasoning: the x87 census has to reach stderr on a run that
           * is killed, which is every run. */
          if (recomp_fp_report)       recomp_fp_report(); }
        control = hc->reg[HcControl / 4];
        enable  = hc->reg[HcInterruptEnable / 4];

        /* Operational is HCFS == 10b in bits 7:6. Interrupting a controller
         * the driver has not started yet is not a test of anything. */
        if ((control & 0xC0u) != 0x80u) {
            if (++waited > 1500)              /* 30 s and it never started */
                break;
            continue;
        }
        if (!(enable & INTR_MIE))
            continue;

        /* Plug the device in once, after the driver is running and listening.
         * Presenting it earlier does not work: the driver clears the connect
         * change during its own bring-up scan, and a reset clears interrupt
         * status, so a device that was always there is one that never
         * arrives. */
        if (!plugged && (enable & INTR_RHSC)) {
            hc->reg[HcRhPortStatus1 / 4] |= PORT_CCS | PORT_CSC;
            hc->reg[HcInterruptStatus / 4] |= INTR_RHSC;
            plugged = 1;
            fprintf(stderr, "  [OHCI0] operational after %u ms; device "
                            "arriving on port 1\n", waited * 20);
            fflush(stderr);
        }

        /* Be the bus master. ControlListEnable in HcControl says the driver
         * wants the list walked; ControlListFilled says it has put something
         * on it. Walking on the tick rather than only when CLF is written
         * costs a read of a guest dword and means a descriptor queued without
         * rewriting CLF is still moved -- which is legal, and drivers do it. */
        /* What the driver has actually put in front of the controller.
         *
         * "No transfer ever happened" has three causes that look identical
         * from outside -- the list is not enabled, the head is null, or the
         * head points at an endpoint with nothing queued on it -- and this
         * says which, once a second, without following the walk. */
        if (s_trace) {
            static unsigned n;
            if ((n++ % 50) == 0) {
                uint32_t ch = hc->reg[HcControlHeadED / 4] & ED_PTR_MASK;
                uint32_t hcca = hc->reg[HcHCCA / 4];
                uint32_t it = 0; int k, live = 0;
                for (k = 0; k < 32; k++)
                    if ((it = rd32(hcca + (uint32_t)k * 4) & ED_PTR_MASK) != 0)
                        live++;
                fprintf(stderr, "  [OHCI%d] control=%08X headED=%08X (%s) "
                        "hcca=%08X %d/32 interrupt entries\n",
                        hc->index, control, ch,
                        guest_ok(ch, 16) ? "reachable" : "unreachable",
                        hcca, live);
                if (guest_ok(ch, 16))
                    fprintf(stderr, "  [OHCI%d]   ED: %08X %08X %08X %08X\n",
                            hc->index, rd32(ch), rd32(ch + 4), rd32(ch + 8),
                            rd32(ch + 12));
                fflush(stderr);
            }
        }

        if ((control & 0x10u)
         && guest_ok(hc->reg[HcControlHeadED / 4] & ED_PTR_MASK, 16)) {
            if (ohci_run_control_list(hc))
                hc->reg[HcCommandStatus / 4] &= ~0x02u;   /* CLF consumed */
        }

        /*
         * A frame, and the interrupt that says one started.
         *
         * The driver waits on StartOfFrame to time the pauses the USB
         * specification requires -- two milliseconds of settling after
         * SET_ADDRESS, for one -- and it waits by enabling SF and blocking.
         * A controller that never reports a frame therefore enumerates
         * exactly as far as the first mandatory wait and stops: this port got
         * the device descriptor and the address assigned, and then sat there
         * while the driver waited for a frame that never came.
         *
         * One frame per tick rather than the thousand a second real hardware
         * produces. What the driver does with SF is count it, so a slower
         * clock makes the settling waits longer and nothing else; a thousand
         * interrupts a second into a recompiled ISR would cost more than the
         * whole USB stack is worth.
         */
        {
            uint32_t hcca = hc->reg[HcHCCA / 4];
            hc->reg[HcFmNumber / 4] = (hc->reg[HcFmNumber / 4] + 1) & 0xFFFFu;
            if (hcca && guest_ok(hcca + 0x80, 4))
                wr32(hcca + 0x80, hc->reg[HcFmNumber / 4]);
            hc->reg[HcInterruptStatus / 4] |= INTR_SF;
        }

        /* The periodic list, if the driver has enabled it. PeriodicListEnable
         * is bit 2 of HcControl. */
        if (control & 0x04u)
            ohci_run_periodic_list(hc);

        /* Level-triggered, which is what OHCI is: while an enabled source is
         * set, the line is asserted. The handler clears the status bit, so
         * this stops on its own -- and if it ever does not, the cap below says
         * so rather than spinning the ISR forever. */
        status = hc->reg[HcInterruptStatus / 4] & enable & 0x7Fu;
        if (!status)
            continue;

        /* A stuck source is one the handler never clears, which shows up as
         * the same status delivered over and over. Counting deliveries alone
         * would trip on a device that is simply busy. */
        if (status == last_status) {
            if (++repeats > 200) {
                fprintf(stderr, "  [OHCI0] status %08X delivered 200 times "
                                "without being cleared; stopping\n", status);
                fflush(stderr);
                break;
            }
        } else {
            last_status = status;
            repeats = 0;
        }
        ohci_raise(hc, status);
    }
    return 0;
}

void xbox_OhciInit(void)
{
    static int done;

    if (done)
        return;
    done = 1;

    /* Opt-in. Without a descriptor list walker behind it a driver that finds
     * a port has nothing to enumerate, so this must not change how a title
     * behaves until the rest of it exists. */
    if (!getenv("RECOMP_USB"))
        return;

    s_enabled = 1;
    s_trace   = getenv("RECOMP_USB_TRACE") != NULL;
    ohci_reset(&s_hc[0], XBOX_OHCI0_BASE, 0);
    ohci_reset(&s_hc[1], XBOX_OHCI1_BASE, 1);

#if defined(_WIN32)
    /* The registers have to fault to be answered. The MCPX aperture is mapped
     * as plain committed memory, so both blocks are made inaccessible here and
     * the title's VEH routes the faults back to xbox_OhciHandleMmio.
     *
     * A failed protect switches the model off rather than leaving it half on:
     * a controller whose registers read as zero out of RAM is exactly the
     * situation this exists to end, and it would look identical. */
    {
        ptrdiff_t off = xbox_GetMemoryOffset();
        int i;

        if (!off) {
            s_enabled = 0;
            fprintf(stderr, "  OHCI: guest memory not mapped yet; disabled\n");
            return;
        }
        for (i = 0; i < 2; i++) {
            DWORD old_protect;
            LPVOID at = (LPVOID)((uintptr_t)off + s_hc[i].base);
            if (!VirtualProtect(at, XBOX_OHCI_SIZE, PAGE_NOACCESS,
                                &old_protect)) {
                s_enabled = 0;
                fprintf(stderr, "  OHCI: cannot trap 0x%08X (error %lu); "
                                "disabled\n", s_hc[i].base, GetLastError());
                return;
            }
        }
    }
#endif

    fprintf(stderr, "  OHCI: two controllers at 0x%08X and 0x%08X, "
                    "%d ports each, one device on HC0 port 1\n",
            XBOX_OHCI0_BASE, XBOX_OHCI1_BASE, OHCI_PORTS);
    fflush(stderr);

    /* The bus master. Nothing else walks the descriptor lists, so without it
     * a driver can find the controller, reset the port, and then wait for a
     * report that never arrives. CreateThread is provided by the platform
     * layer on every host here, so this is not Windows-only. */
    {
        HANDLE th = CreateThread(NULL, 0, ohci_thread, NULL, 0, NULL);
        if (th)
            CloseHandle(th);
    }
}

static OhciController *hc_for(uint32_t va)
{
    int i;

    if (!s_enabled)
        return NULL;
    for (i = 0; i < 2; i++)
        if (va >= s_hc[i].base && va < s_hc[i].base + XBOX_OHCI_SIZE)
            return &s_hc[i];
    return NULL;
}


/* ---- the MMIO-hook path (everywhere that is not Windows) ----------------
 *
 * On Windows the registers are answered by making the page inaccessible and
 * decoding the faulting instruction. That works and it is slow, and it needs
 * a structured-exception handler to route the fault back here. This runtime
 * already has a faster and simpler route for exactly this: the recompiled
 * code funnels every access in the MCPX aperture through a function pointer,
 * which is how the APU is served. The USB block sits at the very top of that
 * aperture and was one byte outside the window -- so every register the
 * title's XAPI read came back as zero out of plain RAM, and it concluded
 * there was no host controller. Hence a pad the guest never once read.
 */
int xbox_OhciEnabled(void) { return s_enabled; }

uint32_t xbox_OhciRead(uint32_t xbox_va, int size)
{
    OhciController *hc = hc_for(xbox_va);
    if (!hc) return 0;
    return (uint32_t)ohci_read(hc, xbox_va - hc->base, size);
}

void xbox_OhciWrite(uint32_t xbox_va, uint32_t val, int size)
{
    OhciController *hc = hc_for(xbox_va);
    if (!hc) return;
    ohci_write(hc, xbox_va - hc->base, val, size);
}

int xbox_OhciOwnsAddress(uint32_t xbox_va)
{
    return hc_for(xbox_va) != NULL;
}

int xbox_OhciHandleMmio(void *ctx, uint32_t xbox_va)
{
#if defined(_WIN32)
    OhciController *hc = hc_for(xbox_va);
    int ok;

    if (!hc)
        return 0;
    ok = mmio_emulate((PCONTEXT)ctx, xbox_va - hc->base, hc,
                      ohci_read, ohci_write);
    if (!ok && hc->decode_fail++ < 20) {
        const uint8_t *ip = (const uint8_t *)((PCONTEXT)ctx)->Rip;
        fprintf(stderr, "  [OHCI%d] undecoded access at +0x%03X: "
                        "%02X %02X %02X %02X %02X %02X\n",
                hc->index, xbox_va - hc->base,
                ip[0], ip[1], ip[2], ip[3], ip[4], ip[5]);
        fflush(stderr);
    }
    return ok;
#else
    (void)ctx; (void)xbox_va;
    return 0;
#endif
}

void xbox_OhciReport(void)
{
    int i;

    if (!s_enabled)
        return;
    for (i = 0; i < 2; i++)
        fprintf(stderr, "  [OHCI%d] %u reads, %u writes, %u undecoded; "
                        "HcControl=%08X HcIntStatus=%08X port1=%08X\n",
                i, s_hc[i].reads, s_hc[i].writes, s_hc[i].decode_fail,
                s_hc[i].reg[HcControl / 4],
                s_hc[i].reg[HcInterruptStatus / 4],
                s_hc[i].reg[HcRhPortStatus1 / 4]);
    fflush(stderr);
}
