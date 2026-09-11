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

static GCExtendedGamepad *pad_for(unsigned port)
{
    NSUInteger slot = 0;
    for (GCController *c in [GCController controllers]) {
        GCExtendedGamepad *g = c.extendedGamepad;
        if (!g) continue;               /* a remote, or a micro gamepad */
        if (slot == (NSUInteger)port) return g;
        slot++;
    }
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
 * A, B, X, Y, LB, RB, LT, RT; axes[] is LX, LY, RX, RY. */
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
        analog[4] = b255(g.leftShoulder);
        analog[5] = b255(g.rightShoulder);
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
