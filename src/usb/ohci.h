/*
 * ohci.h -- the Xbox USB host controllers, enough of them to be found.
 *
 * A title reaches its gamepad through XAPI, which is statically linked into
 * the image and drives the OHCI controller registers directly rather than
 * going through anything this runtime can shim. Half-Life 2 registers a 4 KB
 * block at 0xFED00000 and takes an interrupt vector for it, then reads
 * HcRevision out of an aperture backed by zeroed RAM, concludes there is a
 * host controller with no root hub ports, and enumerates nothing.
 *
 * This is the register half of the answer: a controller that reports a real
 * revision, a root hub with ports, and the handful of registers that have
 * behaviour rather than storage. It does not yet walk the endpoint and
 * transfer descriptor lists, which is what actually moves a report, so a
 * driver that gets this far will find a port and then have nothing to talk to.
 * That is deliberate -- the register trace is what says whether the driver
 * polls HcInterruptStatus or waits on the IRQ, and that answers how much of
 * the rest is needed before any of it is written.
 *
 * Written from the OHCI 1.0a specification and this title's own code.
 */
#ifndef XBOX_OHCI_H
#define XBOX_OHCI_H

#include <stdint.h>

/* The two MCPX host controllers, as the XDK addresses them. */
#define XBOX_OHCI0_BASE   0xFED00000u
#define XBOX_OHCI1_BASE   0xFED08000u
#define XBOX_OHCI_SIZE    0x00001000u   /* 4 KB, the length XAPI registers */

/* Bring the models up. Safe to call more than once; does nothing unless
 * RECOMP_USB is set, so a title that was working without a controller keeps
 * behaving exactly as it did. */
void xbox_OhciInit(void);

/* 1 if the address is inside a controller this model owns. */
int  xbox_OhciOwnsAddress(uint32_t xbox_va);

/* Service a trapped access. Returns 1 if the faulting instruction was decoded
 * and stepped over. `ctx` is a PCONTEXT; void * keeps windows.h out of here. */
int  xbox_OhciHandleMmio(void *ctx, uint32_t xbox_va);

/* Served through the runtime's MMIO function pointers rather than by
 * faulting, which is how the APU is served and is both faster and portable.
 * xbox_OhciEnabled() says whether the model is on at all. */
int      xbox_OhciEnabled(void);
uint32_t xbox_OhciRead(uint32_t xbox_va, int size);
void     xbox_OhciWrite(uint32_t xbox_va, uint32_t val, int size);

/* Report counts at exit, so a run says whether the driver ever looked. */
void xbox_OhciReport(void);

#endif /* XBOX_OHCI_H */
