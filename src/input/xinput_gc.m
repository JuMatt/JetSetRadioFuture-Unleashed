/**
 * Apple GameController shim -- the Objective-C half.
 *
 * This file deliberately includes none of the project's headers. The Windows
 * compatibility layer typedefs BOOL as int and declares GetCurrentThread and
 * GetCurrentProcess; Objective-C's BOOL is a signed char and Carbon declares
 * both of those functions with other signatures, so a translation unit that
 * pulls in both worlds cannot compile. Keeping the boundary at plain C types
 * -- no BOOL, no DWORD, no framework types -- means neither side has to know
 * the other exists.
 *
 * The C half lives in xinput_device.c and maps these neutral values onto the
 * Xbox gamepad structure.
 */

#import <Foundation/Foundation.h>
#import <GameController/GameController.h>
#include <stdio.h>
#include <math.h>

/* Button bits, in this file's own order; xinput_device.c translates them. */
enum {
    GC_DPAD_UP = 1u << 0, GC_DPAD_DOWN = 1u << 1,
    GC_DPAD_LEFT = 1u << 2, GC_DPAD_RIGHT = 1u << 3,
    GC_START = 1u << 4, GC_BACK = 1u << 5,
    GC_LTHUMB = 1u << 6, GC_RTHUMB = 1u << 7
};

/* The framework delivers controller connect/disconnect and input on the main
 * run loop. A game that owns its own loop -- and a headless run with no
 * NSApplication at all -- never returns to it, so nothing is ever delivered
 * and the pad looks absent. Draining it with a zero deadline on each poll
 * costs nothing and makes both cases behave the same. */
static void pump_runloop(void)
{
    [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                             beforeDate:[NSDate distantPast]];
}

/* Say which controller serves each port, once per change: "the pad does
 * nothing" has too many possible causes to leave this unsaid. The pointer is
 * only compared, never followed. */
static void note_pad(unsigned port, GCController *c)
{
    static const void *seen[4];
    const void *id = (__bridge const void *)c;
    if (port >= 4 || seen[port] == id) return;
    if (c) {
        const char *cat = "?";
        if (@available(macOS 10.15, iOS 13.0, *))
            cat = c.productCategory ? [c.productCategory UTF8String] : "?";
        fprintf(stderr, "[PAD] port %u: %s (%s)\n", port,
                c.vendorName ? [c.vendorName UTF8String] : "unnamed controller", cat);
    } else if (seen[port]) {
        fprintf(stderr, "[PAD] port %u: controller gone\n", port);
    }
    fflush(stderr);
    seen[port] = id;
}

static GCExtendedGamepad *pad_for(unsigned port)
{
    NSUInteger slot = 0;
    for (GCController *c in [GCController controllers]) {
        GCExtendedGamepad *g = c.extendedGamepad;
        if (!g) continue;               /* a remote, or a micro gamepad */
        if (slot == (NSUInteger)port) { note_pad(port, c); return g; }
        slot++;
    }
    note_pad(port, nil);
    return nil;
}

/* GameController reports every button as a float in [0,1] -- the triggers
 * genuinely analogue, the face buttons effectively digital. The Xbox pad
 * wants a byte per button and a signed short per stick axis. */
static unsigned char b255(GCControllerButtonInput *b)
{
    float v = b ? b.value : 0.0f;
    if (v <= 0.0f) return 0;
    if (v >= 1.0f) return 255;
    return (unsigned char)(v * 255.0f + 0.5f);
}

static short axis16(GCControllerAxisInput *a)
{
    float v = a ? a.value : 0.0f;
    if (v >=  1.0f) return  32767;
    if (v <= -1.0f) return -32768;
    return (short)(v * 32767.0f);
}

void nv_gc_init(void)
{
    @autoreleasepool {
        /* Keep reading the pad when the window is not focused: a run that
         * renders into an offscreen surface has no window to focus. */
        if (@available(macOS 11.0, iOS 14.0, *))
            GCController.shouldMonitorBackgroundEvents = YES;
        pump_runloop();
    }
}

/* 1 if a pad is present on this port, 0 if not. analog[] is
 * A, B, X, Y, Black, White, LT, RT; axes[] is LX, LY, RX, RY.
 *
 * Every controller macOS knows arrives here as an extended gamepad -- the
 * DualSense and DualShock 4 are subclasses of it, as are the Xbox, Switch Pro
 * and MFi pads -- so there is nothing per-model to do: Cross is A, Options is
 * Menu (Start), Create/Share is Options (Back). The PS button stays with the
 * system and the touchpad is not used. */
int nv_gc_poll(unsigned port, unsigned short *buttons,
               unsigned char *analog, short *axes)
{
    @autoreleasepool {
        GCExtendedGamepad *g;
        unsigned short btn = 0;

        pump_runloop();
        g = pad_for(port);
        if (!g) return 0;

        if (g.dpad.up.pressed)    btn |= GC_DPAD_UP;
        if (g.dpad.down.pressed)  btn |= GC_DPAD_DOWN;
        if (g.dpad.left.pressed)  btn |= GC_DPAD_LEFT;
        if (g.dpad.right.pressed) btn |= GC_DPAD_RIGHT;
        if (@available(macOS 10.15, iOS 13.0, *)) {
            if (g.buttonMenu.pressed)            btn |= GC_START;
            if (g.buttonOptions.pressed)         btn |= GC_BACK;
            if (g.leftThumbstickButton.pressed)  btn |= GC_LTHUMB;
            if (g.rightThumbstickButton.pressed) btn |= GC_RTHUMB;
        }
        *buttons = btn;

        analog[0] = b255(g.buttonA);
        analog[1] = b255(g.buttonB);
        analog[2] = b255(g.buttonX);
        analog[3] = b255(g.buttonY);
        /* [4] is Black and [5] White, in xinput_device.c's order. The left
         * bumper (L1 on a DualSense) is White and the right one Black, the
         * way xemu maps them; this had them the other way round. */
        analog[4] = b255(g.rightShoulder);
        analog[5] = b255(g.leftShoulder);
        analog[6] = b255(g.leftTrigger);
        analog[7] = b255(g.rightTrigger);

        /* Both APIs put Y up, so no inversion here -- unlike SDL. */
        axes[0] = axis16(g.leftThumbstick.xAxis);
        axes[1] = axis16(g.leftThumbstick.yAxis);
        axes[2] = axis16(g.rightThumbstick.xAxis);
        axes[3] = axis16(g.rightThumbstick.yAxis);
        return 1;
    }
}

/* ---- vibration ---------------------------------------------------------
 * The game sets two motor speeds, as on the Xbox pad: the left motor slow
 * and heavy, the right one fast and light. Core Haptics has no motors: it
 * plays patterns. So each controller gets an engine of its own (its
 * GCDeviceHaptics: a DualSense, a DualShock 4, an Xbox Series pad -- macOS 11
 * and iOS 14 on) playing one long continuous event, looped, whose strength
 * and sharpness are then turned up and down as the game asks: strength the
 * stronger of the two motors, sharpness how much of it is the light one.
 * With no controller, an iPhone's own Taptic Engine does the same while the
 * on-screen controls are in use. Built at the first call that needs it, torn
 * down when its controller goes. A controller without haptics is said once
 * and left alone; any other failure is tried again, after 1, 2, 4... up to 30
 * seconds. A DualSense on a Mac answered the first request of a session with
 * "Couldn't communicate with a helper application" -- the system's haptics
 * helper not ready yet -- and until 0.6.9 that one failure was final: no
 * vibration at all until the game was started again. */
#import <CoreHaptics/CoreHaptics.h>
#include <TargetConditionals.h>

#define RUMBLE_SLOTS 5                  /* 4 controller ports, then the phone */
static CHHapticEngine *g_rb_engine[RUMBLE_SLOTS] API_AVAILABLE(macos(10.15), ios(13.0));
static id g_rb_player[RUMBLE_SLOTS];
static const void *g_rb_owner[RUMBLE_SLOTS];
static int   g_rb_failed[RUMBLE_SLOTS], g_rb_running[RUMBLE_SLOTS];   /* failed: for good (no haptics) */
static int    g_rb_tries[RUMBLE_SLOTS];       /* failures in a row that are worth another try */
static double g_rb_next[RUMBLE_SLOTS];        /* not before this (CFAbsoluteTime) */

/* A failure worth another try: when, and said for the first few. */
static void rumble_retry_later(int s, const char *what, NSError *err)
{
    double wait = g_rb_tries[s] >= 5 ? 30.0 : (double)(1 << g_rb_tries[s]);
    g_rb_tries[s]++;
    g_rb_next[s] = CFAbsoluteTimeGetCurrent() + wait;
    if (g_rb_tries[s] <= 6)
        fprintf(stderr, "[RUMBLE] %s: %s -- tried again in %.0f s\n", what,
                err ? [[err localizedDescription] UTF8String] : "?", wait);
    fflush(stderr);
}
static float g_rb_level[RUMBLE_SLOTS], g_rb_sharp[RUMBLE_SLOTS];

static GCController *controller_for(unsigned port)
{
    NSUInteger slot = 0;
    for (GCController *c in [GCController controllers]) {
        if (!c.extendedGamepad) continue;
        if (slot == (NSUInteger)port) return c;
        slot++;
    }
    return nil;
}

static void rumble_drop(int s) API_AVAILABLE(macos(10.15), ios(13.0))
{
    if (g_rb_player[s]) [(id<CHHapticAdvancedPatternPlayer>)g_rb_player[s] stopAtTime:CHHapticTimeImmediate error:nil];
    if (g_rb_engine[s]) [g_rb_engine[s] stopWithCompletionHandler:nil];
    g_rb_player[s] = nil;
    g_rb_engine[s] = nil;
    g_rb_running[s] = 0;
    g_rb_level[s] = g_rb_sharp[s] = -1.0f;
}

/* The engine and its looping player, for slot s: c's, or the device's. */
static int rumble_ready(int s, GCController *c) API_AVAILABLE(macos(10.15), ios(13.0))
{
    NSError *err = nil;
    CHHapticEngine *e;
    if (g_rb_player[s]) return 1;
    if (g_rb_failed[s]) return 0;
    if (g_rb_tries[s] && CFAbsoluteTimeGetCurrent() < g_rb_next[s]) return 0;
    e = nil;
    if (c) {
        if (@available(macOS 11.0, iOS 14.0, *)) {
            GCDeviceHaptics *h = c.haptics;
            if (h) e = [h createEngineWithLocality:GCHapticsLocalityDefault];
        }
    } else {
        if (CHHapticEngine.capabilitiesForHardware.supportsHaptics)
            e = [[CHHapticEngine alloc] initAndReturnError:&err];
    }
    if (!e) {
        g_rb_failed[s] = 1;
        fprintf(stderr, "[RUMBLE] %s: no haptics%s%s\n", c ? (c.vendorName ? [c.vendorName UTF8String] : "the controller") : "this device",
                err ? " -- " : "", err ? [[err localizedDescription] UTF8String] : "");
        fflush(stderr);
        return 0;
    }
    e.playsHapticsOnly = YES;
    {
        int slot = s;
        e.resetHandler = ^{ g_rb_player[slot] = nil; g_rb_running[slot] = 0; g_rb_level[slot] = -1.0f; };
        e.stoppedHandler = ^(CHHapticEngineStoppedReason r) { (void)r; g_rb_running[slot] = 0; g_rb_level[slot] = -1.0f; };
    }
    if (![e startAndReturnError:&err]) {
        rumble_retry_later(s, "the haptic engine did not start", err);
        return 0;
    }
    {
        CHHapticEventParameter *in = [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity value:1.0f];
        CHHapticEventParameter *sh = [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticSharpness value:0.5f];
        CHHapticEvent *ev = [[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous
                                                         parameters:@[in, sh] relativeTime:0 duration:30.0];
        CHHapticPattern *p = [[CHHapticPattern alloc] initWithEvents:@[ev] parameters:@[] error:&err];
        id<CHHapticAdvancedPatternPlayer> pl = nil;
        id<CHHapticPatternPlayer> plain = nil;
        if (c) {
            /* A game controller gets a plain player. On the Mac (a DualSense,
             * macOS 26) the GameController haptics service makes no advanced
             * one -- "Couldn't communicate with a helper application", every
             * time, and the engine is no good after that ("Client not
             * registered") -- while a plain one plays, and takes the same
             * intensity and sharpness changes. A plain player does not loop:
             * one hour of vibration, started again whenever it is asked for
             * anew. */
            CHHapticEvent *ev1 = [[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous
                                                              parameters:@[in, sh] relativeTime:0 duration:3600.0];
            CHHapticPattern *p1 = [[CHHapticPattern alloc] initWithEvents:@[ev1] parameters:@[] error:&err];
            plain = p1 ? [e createPlayerWithPattern:p1 error:&err] : nil;
        } else {
            pl = p ? [e createAdvancedPlayerWithPattern:p error:&err] : nil;   /* the phone's own: it loops */
            if (pl) pl.loopEnabled = YES;
        }
        if (!pl && !plain) {
            rumble_retry_later(s, "no pattern player", err);
            [e stopWithCompletionHandler:nil];
            return 0;
        }
        g_rb_engine[s] = e;
        g_rb_player[s] = pl ? (id)pl : (id)plain;
    }
    fprintf(stderr, "[RUMBLE] %s: haptics ready%s\n", c ? (c.vendorName ? [c.vendorName UTF8String] : "the controller") : "this device",
            g_rb_tries[s] ? " (after a failed try)" : "");
    g_rb_tries[s] = 0;
    fflush(stderr);
    return 1;
}

/* The game's two motors (0..1) for this port. allow_device: no controller
 * there and the on-screen controls in use, so the device itself may shake.
 * Returns 1 when something played (or stopped) it. */
int nv_gc_rumble(unsigned port, float lo, float hi, int allow_device)
{
    if (port >= 4) return 0;
    if (@available(macOS 10.15, iOS 13.0, *)) {
        @autoreleasepool {
            GCController *c = controller_for(port);
            int s = c ? (int)port : (allow_device && port == 0 ? 4 : -1);
            const void *owner = (__bridge const void *)c;
            float level = lo > hi ? lo : hi, sharp;
            id<CHHapticAdvancedPatternPlayer> pl;
            NSError *err = nil;
#if !TARGET_OS_IPHONE
            if (s == 4) s = -1;                          /* a Mac does not shake */
#endif
            if (c && g_rb_owner[port] != owner) {        /* another controller on this port */
                rumble_drop((int)port);
                g_rb_owner[port] = owner;
                g_rb_failed[port] = 0;
                g_rb_tries[port] = 0;
            }
            if (!c && g_rb_engine[port]) { rumble_drop((int)port); g_rb_owner[port] = NULL; }
            if (s != 4 && g_rb_engine[4] && g_rb_running[4]) {   /* a controller took over from the device */
                [(id<CHHapticAdvancedPatternPlayer>)g_rb_player[4] stopAtTime:CHHapticTimeImmediate error:nil];
                g_rb_running[4] = 0;
            }
            if (s < 0) return 0;
            if (level < 0.02f) {
                if (g_rb_running[s]) {
                    [(id<CHHapticAdvancedPatternPlayer>)g_rb_player[s] stopAtTime:CHHapticTimeImmediate error:nil];
                    g_rb_running[s] = 0;
                }
                return 1;
            }
            if (!rumble_ready(s, c)) return 0;
            pl = g_rb_player[s];
            if (level > 1.0f) level = 1.0f;
            sharp = (lo + hi) > 0.0f ? hi / (lo + hi) : 0.5f;   /* 0: all the heavy motor, 1: all the light one */
            if (!g_rb_running[s]) {
                if (![pl startAtTime:CHHapticTimeImmediate error:&err]) {
                    /* the engine may have stopped (the app in the background): start it again once */
                    [g_rb_engine[s] startAndReturnError:nil];
                    if (![pl startAtTime:CHHapticTimeImmediate error:&err]) { g_rb_player[s] = nil; return 0; }
                }
                g_rb_running[s] = 1;
                g_rb_level[s] = g_rb_sharp[s] = -1.0f;
            }
            if (fabsf(level - g_rb_level[s]) > 0.03f || fabsf(sharp - g_rb_sharp[s]) > 0.05f) {
                CHHapticDynamicParameter *pi = [[CHHapticDynamicParameter alloc]
                    initWithParameterID:CHHapticDynamicParameterIDHapticIntensityControl value:level relativeTime:0];
                CHHapticDynamicParameter *ps = [[CHHapticDynamicParameter alloc]
                    initWithParameterID:CHHapticDynamicParameterIDHapticSharpnessControl value:(sharp - 0.5f) * 0.8f relativeTime:0];
                [pl sendParameters:@[pi, ps] atTime:CHHapticTimeImmediate error:nil];
                g_rb_level[s] = level;
                g_rb_sharp[s] = sharp;
            }
            return 1;
        }
    }
    return 0;
}
