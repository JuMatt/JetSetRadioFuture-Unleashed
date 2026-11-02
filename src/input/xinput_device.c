/**
 * Xbox Input Compatibility Layer
 *
 * Translates the Xbox controller API to a host gamepad backend.
 * Handles the structural differences between the Xbox gamepad
 * (analog buttons as bytes, separate trigger channels) and the host
 * (digital face buttons, trigger axes).
 *
 *   _WIN32 -> Windows XInput
 *   POSIX  -> SDL2 GameController
 */

#include "xinput_xbox.h"
#include <string.h>
#include <stdio.h>


/* ---- scripted input ------------------------------------------------------
 *
 * A way to press buttons without hands.
 *
 * "Does it get past the title screen" cannot be answered by a headless run,
 * and answering it by hand means a person watching a window for two minutes
 * every time something changes. RECOMP_AUTO_INPUT is a list of
 * <seconds>:<button> pairs -- "12:start,16:a,20:a" -- each held for half a
 * second at that point after the process starts.
 *
 * It overrides the real pad only while a press is due, so a controller in
 * someone's hands still works alongside it.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

typedef struct { double at; WORD buttons; BYTE analog_index; } ScriptedPress;

static double input_now(void)
{
    static double t0 = -1.0;
    struct timespec ts;
    double now;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    now = (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
    if (t0 < 0.0) t0 = now;
    return now - t0;
}

/* Returns 1 if it filled pState, 0 to let the real pad answer. */
static int input_script(DWORD port, XBOX_INPUT_STATE *pState)
{
    #define SCRIPT_MAX 16
    static ScriptedPress press[SCRIPT_MAX];
    static int n = -1;
    static const double hold = 0.5;
    double t;
    int i;

    if (n < 0) {
        const char *spec = getenv("RECOMP_AUTO_INPUT");
        n = 0;
        if (spec) {
            char buf[512], *p, *save = NULL;
            snprintf(buf, sizeof buf, "%s", spec);
            for (p = strtok_r(buf, ",", &save); p && n < SCRIPT_MAX;
                 p = strtok_r(NULL, ",", &save)) {
                char name[32] = "";
                double at = 0.0;
                if (sscanf(p, "%lf:%31s", &at, name) != 2) continue;
                press[n].at = at;
                press[n].buttons = 0;
                press[n].analog_index = 0xFF;
                if      (!strcmp(name, "start")) press[n].buttons = XBOX_GAMEPAD_START;
                else if (!strcmp(name, "back"))  press[n].buttons = XBOX_GAMEPAD_BACK;
                else if (!strcmp(name, "up"))    press[n].buttons = XBOX_GAMEPAD_DPAD_UP;
                else if (!strcmp(name, "down"))  press[n].buttons = XBOX_GAMEPAD_DPAD_DOWN;
                else if (!strcmp(name, "left"))  press[n].buttons = XBOX_GAMEPAD_DPAD_LEFT;
                else if (!strcmp(name, "right")) press[n].buttons = XBOX_GAMEPAD_DPAD_RIGHT;
                else if (!strcmp(name, "a"))     press[n].analog_index = XBOX_BUTTON_A;
                else if (!strcmp(name, "b"))     press[n].analog_index = XBOX_BUTTON_B;
                else continue;
                n++;
            }
            fprintf(stderr, "[INPUT] scripted: %d press(es) from \"%s\"\n",
                    n, spec);
        }
    }
    if (!n || port != 0) return 0;

    t = input_now();
    for (i = 0; i < n; i++) {
        if (t < press[i].at || t > press[i].at + hold) continue;
        {
            static int said[SCRIPT_MAX];
            if (!said[i]) { said[i] = 1;
                fprintf(stderr, "[INPUT] scripted press %d at %.1fs\n", i, t); }
        }
        memset(pState, 0, sizeof *pState);
        pState->dwPacketNumber = (DWORD)(t * 1000.0);
        pState->Gamepad.wButtons = press[i].buttons;
        if (press[i].analog_index != 0xFF)
            pState->Gamepad.bAnalogButtons[press[i].analog_index] = 255;
        return 1;
    }
    return 0;
}

#if !defined(_WIN32) || defined(XBOXRECOMP_WIN_GL)
/* The window's keyboard, merged in rather than instead.
 *
 * Someone with a pad in their hands and someone using the keyboard should
 * both work, and a run with neither should behave exactly as before. So
 * the keys are OR-ed over whatever the controller reported: nothing held
 * changes nothing. Only available in a windowed run -- there is no keyboard
 * to read otherwise -- so the symbol is weak (the AppKit window on the Mac,
 * the SDL one elsewhere). Returns 1 if a key was held. */
/* The iPhone app's on-screen stick (port/ios/jsrf_ios.m): 1, and where it
 * is, while a thumb is on it. A weak definition that says "none", which the
 * app's own replaces -- Mach-O will not link a weak reference that nothing
 * defines. */
__attribute__((weak)) int nv_window_touch_stick(short *x, short *y);
__attribute__((weak)) int nv_window_touch_stick(short *x, short *y)
{
    (void)x; (void)y;
    return 0;
}

static int kb_merge(DWORD dwPort, WORD *out, unsigned char analog[8], short axes[4])
{
    extern int nv_window_keys(unsigned short *b, unsigned char *a)
        __attribute__((weak));
    unsigned short kb = 0;
    unsigned char ka[8] = { 0 };
    int i;
    if (!(nv_window_keys && dwPort == 0 && nv_window_keys(&kb, ka))) return 0;
    if (kb & 0x0001u) *out |= XBOX_GAMEPAD_DPAD_UP;
    if (kb & 0x0002u) *out |= XBOX_GAMEPAD_DPAD_DOWN;
    if (kb & 0x0004u) *out |= XBOX_GAMEPAD_DPAD_LEFT;
    if (kb & 0x0008u) *out |= XBOX_GAMEPAD_DPAD_RIGHT;
    if (kb & 0x0010u) *out |= XBOX_GAMEPAD_START;
    if (kb & 0x0020u) *out |= XBOX_GAMEPAD_BACK;
    if (kb & 0x0400u) *out |= XBOX_GAMEPAD_LEFT_THUMB;
    if (kb & 0x0800u) *out |= XBOX_GAMEPAD_RIGHT_THUMB;
    for (i = 0; i < 8; i++) if (ka[i]) analog[i] = ka[i];
    /* The same four keys also push the left stick, and the arrows the right
     * one: the skater only reads the stick (the d-pad is for menus). Full
     * deflection, a diagonal scaled onto the circle. A held key replaces the
     * pad's stick; nothing held leaves it. */
    {
        int lx = ((kb & 0x0008u) ? 1 : 0) - ((kb & 0x0004u) ? 1 : 0);
        int ly = ((kb & 0x0001u) ? 1 : 0) - ((kb & 0x0002u) ? 1 : 0);
        int rx = ((kb & 0x0200u) ? 1 : 0) - ((kb & 0x0100u) ? 1 : 0);
        int ry = ((kb & 0x0040u) ? 1 : 0) - ((kb & 0x0080u) ? 1 : 0);
        if (lx || ly) {
            short v = (lx && ly) ? 23170 : 32767;
            axes[0] = (short)(lx * v); axes[1] = (short)(ly * v);
        }
        if (rx || ry) {
            short v = (rx && ry) ? 23170 : 32767;
            axes[2] = (short)(rx * v); axes[3] = (short)(ry * v);
        }
    }
    /* The on-screen stick moves the left stick by as much as the thumb
     * does -- over the full deflection its d-pad bits gave above. */
    {
        short tx = 0, ty = 0;
        if (nv_window_touch_stick(&tx, &ty)) { axes[0] = tx; axes[1] = ty; }
    }
    return 1;
}

/* RECOMP_HOST_PAD=0 leaves the host's controllers alone entirely, for
 * unattended runs driven by a script. */
static int host_pad_off(void)
{
    static int off = -1;
    if (off < 0) {
        const char *e = getenv("RECOMP_HOST_PAD");
        off = (e && e[0] == '0') ? 1 : 0;
    }
    return off;
}
#endif

/* ======================================================================== */
#if defined(_WIN32) && !defined(XBOXRECOMP_WIN_GL)
/* ====================  XInput backend  ================================== */
/* ======================================================================== */

#include <xinput.h>
#pragma comment(lib, "xinput.lib")

static BOOL  g_controller_connected[XBOX_MAX_CONTROLLERS] = { FALSE };
static DWORD g_last_packet[XBOX_MAX_CONTROLLERS] = { 0 };

void xbox_InputInit(void)
{
    for (DWORD i = 0; i < XBOX_MAX_CONTROLLERS; i++) {
        XINPUT_STATE state;
        DWORD result = XInputGetState(i, &state);
        g_controller_connected[i] = (result == ERROR_SUCCESS);
    }
}

DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    if (pState && input_script(dwPort, pState)) return ERROR_SUCCESS;
    XINPUT_STATE xi_state;
    DWORD result;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pState)
        return ERROR_DEVICE_NOT_CONNECTED;

    result = XInputGetState(dwPort, &xi_state);
    if (result != ERROR_SUCCESS) {
        g_controller_connected[dwPort] = FALSE;
        return result;
    }

    g_controller_connected[dwPort] = TRUE;
    g_last_packet[dwPort] = xi_state.dwPacketNumber;

    memset(pState, 0, sizeof(XBOX_INPUT_STATE));
    pState->dwPacketNumber = xi_state.dwPacketNumber;
    pState->Gamepad.wButtons = xi_state.Gamepad.wButtons & 0x00FF;

    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_A] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_A) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_B] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_B) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_X] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_X) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_Y) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] = xi_state.Gamepad.bLeftTrigger;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] = xi_state.Gamepad.bRightTrigger;

    pState->Gamepad.sThumbLX = xi_state.Gamepad.sThumbLX;
    pState->Gamepad.sThumbLY = xi_state.Gamepad.sThumbLY;
    pState->Gamepad.sThumbRX = xi_state.Gamepad.sThumbRX;
    pState->Gamepad.sThumbRY = xi_state.Gamepad.sThumbRY;

    return ERROR_SUCCESS;
}

DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration)
{
    XINPUT_VIBRATION xi_vib;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pVibration)
        return ERROR_DEVICE_NOT_CONNECTED;

    xi_vib.wLeftMotorSpeed = pVibration->wLeftMotorSpeed;
    xi_vib.wRightMotorSpeed = pVibration->wRightMotorSpeed;
    return XInputSetState(dwPort, &xi_vib);
}

BOOL xbox_InputIsConnected(DWORD dwPort)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS) return FALSE;
    return g_controller_connected[dwPort];
}

DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags, XBOX_INPUT_CAPABILITIES *pCaps)
{
    XINPUT_CAPABILITIES xi_caps;
    DWORD result;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pCaps)
        return ERROR_DEVICE_NOT_CONNECTED;

    result = XInputGetCapabilities(dwPort, dwFlags, &xi_caps);
    if (result != ERROR_SUCCESS) return result;

    memset(pCaps, 0, sizeof(XBOX_INPUT_CAPABILITIES));
    pCaps->Type = xi_caps.Type;
    pCaps->SubType = xi_caps.SubType;
    pCaps->Flags = xi_caps.Flags;
    return ERROR_SUCCESS;
}

/* ======================================================================== */
#elif defined(__APPLE__)
/* ====================  Apple GameController backend  ==================== */
/* ======================================================================== */

/* The Objective-C half is in xinput_gc.m and speaks only plain C types --
 * see the note at the top of that file for why the two cannot share a
 * translation unit. */
void nv_gc_init(void);
int  nv_gc_poll(unsigned port, unsigned short *buttons,
                unsigned char *analog, short *axes);

/* Button bits as xinput_gc.m defines them. */
#define GC_DPAD_UP    0x0001u
#define GC_DPAD_DOWN  0x0002u
#define GC_DPAD_LEFT  0x0004u
#define GC_DPAD_RIGHT 0x0008u
#define GC_START      0x0010u
#define GC_BACK       0x0020u
#define GC_LTHUMB     0x0040u
#define GC_RTHUMB     0x0080u

static BOOL  g_connected[XBOX_MAX_CONTROLLERS];
static DWORD g_packet[XBOX_MAX_CONTROLLERS];

/* RECOMP_HOST_PAD=0 (host_pad_off, above) leaves the GameController
 * framework alone entirely. For unattended runs driven by RECOMP_AUTO_INPUT:
 * headless, the guest's main thread is the process's main thread, and the
 * framework is called from whichever guest thread reads the pad while it
 * holds the guest lock. On 23 Sep one such first call never returned (the
 * holder sat in mach_msg2_trap for the whole run, before the title) -- with
 * nobody at the keyboard there is nothing to gain from the host pad and a
 * whole run to lose. */

void xbox_InputInit(void)
{
    if (!host_pad_off())
        nv_gc_init();
}

DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    if (pState && input_script(dwPort, pState)) return ERROR_SUCCESS;
    unsigned short btn = 0;
    unsigned char analog[8];
    short axes[4];
    WORD out = 0;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pState)
        return ERROR_DEVICE_NOT_CONNECTED;

    memset(pState, 0, sizeof(XBOX_INPUT_STATE));
    memset(analog, 0, sizeof analog);
    memset(axes, 0, sizeof axes);

    int have_pad = host_pad_off() ? 0
                 : nv_gc_poll((unsigned)dwPort, &btn, analog, axes);

    /* The window's keyboard (kb_merge, above). */
    if (kb_merge(dwPort, &out, analog, axes)) have_pad = 1;

    if (!have_pad) {
        g_connected[dwPort] = FALSE;
        return ERROR_DEVICE_NOT_CONNECTED;
    }

    /* Say once, per port, that a pad turned up. Bluetooth pairing, macOS
     * privacy prompts and the run loop all have to line up before this is
     * true, and "the game does not respond to the controller" has too many
     * possible causes to leave it unsaid. */
    if (!g_connected[dwPort])
        fprintf(stderr, "[PAD] controller %u connected\n", (unsigned)dwPort);
    g_connected[dwPort] = TRUE;
    pState->dwPacketNumber = ++g_packet[dwPort];

    if (btn & GC_DPAD_UP)    out |= XBOX_GAMEPAD_DPAD_UP;
    if (btn & GC_DPAD_DOWN)  out |= XBOX_GAMEPAD_DPAD_DOWN;
    if (btn & GC_DPAD_LEFT)  out |= XBOX_GAMEPAD_DPAD_LEFT;
    if (btn & GC_DPAD_RIGHT) out |= XBOX_GAMEPAD_DPAD_RIGHT;
    if (btn & GC_START)      out |= XBOX_GAMEPAD_START;
    if (btn & GC_BACK)       out |= XBOX_GAMEPAD_BACK;
    if (btn & GC_LTHUMB)     out |= XBOX_GAMEPAD_LEFT_THUMB;
    if (btn & GC_RTHUMB)     out |= XBOX_GAMEPAD_RIGHT_THUMB;
    pState->Gamepad.wButtons = out;

    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_A]        = analog[0];
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_B]        = analog[1];
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_X]        = analog[2];
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_Y]        = analog[3];
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK]    = analog[4];
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE]    = analog[5];
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] = analog[6];
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] = analog[7];

    pState->Gamepad.sThumbLX = axes[0];
    pState->Gamepad.sThumbLY = axes[1];
    pState->Gamepad.sThumbRX = axes[2];
    pState->Gamepad.sThumbRY = axes[3];
    return ERROR_SUCCESS;
}

/* The vibration goes through Core Haptics (xinput_gc.m): the controller's,
 * or, on an iPhone played with the on-screen controls, the phone's own. */
int nv_gc_rumble(unsigned port, float lo, float hi, int allow_device);
int nv_window_touch_active(void);   /* the iPhone app's on-screen controls (0 on the Mac) */

DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pVibration)
        return ERROR_DEVICE_NOT_CONNECTED;
    if (!host_pad_off())
        nv_gc_rumble((unsigned)dwPort, pVibration->wLeftMotorSpeed / 65535.0f,
                     pVibration->wRightMotorSpeed / 65535.0f,
                     dwPort == 0 && nv_window_touch_active());
    return g_connected[dwPort] ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
}

BOOL xbox_InputIsConnected(DWORD dwPort)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS) return FALSE;
    return g_connected[dwPort];
}

DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags,
                                XBOX_INPUT_CAPABILITIES *pCaps)
{
    (void)dwFlags;
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pCaps)
        return ERROR_DEVICE_NOT_CONNECTED;
    if (!g_connected[dwPort]) return ERROR_DEVICE_NOT_CONNECTED;
    memset(pCaps, 0, sizeof(XBOX_INPUT_CAPABILITIES));
    pCaps->Type    = 1;   /* gamepad */
    pCaps->SubType = 1;
    pCaps->Flags   = 0;
    return ERROR_SUCCESS;
}

/* ======================================================================== */
#else /* !_WIN32, !__APPLE__ */
/* ====================  SDL2 GameController backend  ===================== */
/* ======================================================================== */

#include <SDL.h>

static SDL_GameController *g_pads[XBOX_MAX_CONTROLLERS];
/* The motors as the game last set them, and when SDL was last told. */
static WORD   g_rumble_lo[XBOX_MAX_CONTROLLERS], g_rumble_hi[XBOX_MAX_CONTROLLERS];
static double g_rumble_sent[XBOX_MAX_CONTROLLERS];
static void rumble_refresh(DWORD port, SDL_GameController *c, double now)
{
    if (!(g_rumble_lo[port] | g_rumble_hi[port]) || now - g_rumble_sent[port] < 0.5) return;
    SDL_GameControllerRumble(c, g_rumble_lo[port], g_rumble_hi[port], 1000);
    g_rumble_sent[port] = now;
}
static BOOL  g_controller_connected[XBOX_MAX_CONTROLLERS];
static DWORD g_packet[XBOX_MAX_CONTROLLERS];

/* Open the attached game controllers into free slots, in the order SDL lists
 * them, and let go of any that went away -- so a pad plugged in (or paired)
 * after the game started is picked up. */
static void list_devices(void)
{
    /* What SDL sees, whenever that changes: a pad that is listed but has no
     * controller mapping is not used, and one that is not listed at all was
     * never seen -- two different reports. Names and USB ids only: a device
     * path can carry a Bluetooth address, and logs end up in public issues. */
    static int last = -1;
    int i, n = SDL_NumJoysticks();
    if (n == last) return;
    last = n;
    fprintf(stderr, "[PAD] SDL sees %d input device(s)\n", n < 0 ? 0 : n);
    for (i = 0; i < n; i++) {
        const char *nm = SDL_JoystickNameForIndex(i);
        fprintf(stderr, "[PAD]   %d: %s (%04x:%04x)%s\n", i, nm ? nm : "unnamed",
                SDL_JoystickGetDeviceVendor(i), SDL_JoystickGetDeviceProduct(i),
                SDL_IsGameController(i) ? "" : " -- no controller mapping, not used");
    }
}

static void open_controllers(void)
{
    int slot, i;
    list_devices();
    for (slot = 0; slot < XBOX_MAX_CONTROLLERS; slot++)
        if (g_pads[slot] && !SDL_GameControllerGetAttached(g_pads[slot])) {
            fprintf(stderr, "[PAD] controller %d gone\n", slot);
            SDL_GameControllerClose(g_pads[slot]);
            g_pads[slot] = NULL;
        }
    for (i = 0; i < SDL_NumJoysticks(); i++) {
        SDL_JoystickID id;
        int have = 0;
        if (!SDL_IsGameController(i)) continue;
        id = SDL_JoystickGetDeviceInstanceID(i);
        for (slot = 0; slot < XBOX_MAX_CONTROLLERS; slot++)
            if (g_pads[slot] && SDL_JoystickInstanceID(
                    SDL_GameControllerGetJoystick(g_pads[slot])) == id) have = 1;
        if (have) continue;
        for (slot = 0; slot < XBOX_MAX_CONTROLLERS; slot++)
            if (!g_pads[slot]) {
                g_pads[slot] = SDL_GameControllerOpen(i);
                if (g_pads[slot])
                    fprintf(stderr, "[PAD] %s in slot %d\n",
                            SDL_GameControllerName(g_pads[slot])
                                ? SDL_GameControllerName(g_pads[slot]) : "controller", slot);
                break;
            }
    }
}

void xbox_InputInit(void)
{
    if (host_pad_off()) return;
    if (!SDL_WasInit(SDL_INIT_GAMECONTROLLER)) {
#if defined(_WIN32)
        /*
         * Windows delivers a pad's input to a window. SDL makes that window
         * on the thread that starts its joystick support -- here the game's
         * own thread, which asks for the pad first -- and reads it only when
         * that thread pumps its messages, which the game's thread never does
         * (the window's events are pumped by the GPU thread). An Xbox pad is
         * claimed by SDL's raw-input driver, so it was found, opened, and
         * never moved: "the controller doesn't work". With this SDL keeps a
         * thread of its own for the window and its messages, which is what
         * the hint is for, and pads plugged in later are found there too.
         */
        SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
#endif
        /* A DualSense or DualShock 4 over Bluetooth only vibrates with its
         * extended reports on, which SDL leaves off by default (they stay on
         * until the controller is switched off, and DirectInput games that
         * do not use SDL read it wrongly meanwhile -- as with Steam).
         * JSRF_PS_RUMBLE=0 leaves them off. */
        { const char *e = getenv("JSRF_PS_RUMBLE");
          if (!(e && e[0] == '0')) {
              SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4_RUMBLE, "1");
              SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1");
          } }
        if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0)
            fprintf(stderr, "[PAD] no controller support: %s\n", SDL_GetError());
    }
    open_controllers();
}

DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    WORD out = 0;
    unsigned char analog[8];
    short axes[4];
    int have_pad = 0;

    if (pState && input_script(dwPort, pState)) return ERROR_SUCCESS;
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pState)
        return ERROR_DEVICE_NOT_CONNECTED;

    memset(pState, 0, sizeof(XBOX_INPUT_STATE));
    memset(analog, 0, sizeof analog);
    memset(axes, 0, sizeof axes);

    if (!host_pad_off()) {
        SDL_GameController *c;
        /* Pads come and go: look again about once a second. */
        { static double next;
          double now = input_now();
          if (dwPort == 0 && now >= next) { next = now + 1.0; open_controllers(); } }
        c = g_pads[dwPort];
        if (c && SDL_GameControllerGetAttached(c)) {
            SDL_GameControllerUpdate();
            have_pad = 1;
            rumble_refresh(dwPort, c, input_now());
            if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_UP))    out |= XBOX_GAMEPAD_DPAD_UP;
            if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_DOWN))  out |= XBOX_GAMEPAD_DPAD_DOWN;
            if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_LEFT))  out |= XBOX_GAMEPAD_DPAD_LEFT;
            if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) out |= XBOX_GAMEPAD_DPAD_RIGHT;
            if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_START))      out |= XBOX_GAMEPAD_START;
            if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_BACK))       out |= XBOX_GAMEPAD_BACK;
            if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_LEFTSTICK))  out |= XBOX_GAMEPAD_LEFT_THUMB;
            if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSTICK)) out |= XBOX_GAMEPAD_RIGHT_THUMB;
            /* analog[] in the order the Mac's backend uses:
             * A B X Y Black White LT RT; the shoulders are Black and White. */
            analog[0] = SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_A) ? 255 : 0;
            analog[1] = SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_B) ? 255 : 0;
            analog[2] = SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_X) ? 255 : 0;
            analog[3] = SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_Y) ? 255 : 0;
            analog[4] = SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) ? 255 : 0;
            analog[5] = SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) ? 255 : 0;
            /* SDL trigger axes are 0..32767 -> Xbox analog button 0..255 */
            analog[6] = (unsigned char)(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) >> 7);
            analog[7] = (unsigned char)(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >> 7);
            /* SDL Y axes point down; the Xbox's up -- invert, as (-1 - v) so
             * v = -32768 does not overflow. */
            axes[0] = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
            axes[1] = (short)(-1 - SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY));
            axes[2] = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTX);
            axes[3] = (short)(-1 - SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTY));
        }
    }

    /* The window's keyboard (kb_merge, above). */
    if (kb_merge(dwPort, &out, analog, axes)) have_pad = 1;

    if (!have_pad) {
        g_controller_connected[dwPort] = FALSE;
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    if (!g_controller_connected[dwPort])
        fprintf(stderr, "[PAD] controller %u connected\n", (unsigned)dwPort);
    g_controller_connected[dwPort] = TRUE;
    pState->dwPacketNumber = ++g_packet[dwPort];
    pState->Gamepad.wButtons = out;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_A]        = analog[0];
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_B]        = analog[1];
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_X]        = analog[2];
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_Y]        = analog[3];
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK]    = analog[4];
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE]    = analog[5];
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] = analog[6];
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] = analog[7];
    pState->Gamepad.sThumbLX = axes[0];
    pState->Gamepad.sThumbLY = axes[1];
    pState->Gamepad.sThumbRX = axes[2];
    pState->Gamepad.sThumbRY = axes[3];
    return ERROR_SUCCESS;
}

DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration)
{
    SDL_GameController *c;
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pVibration)
        return ERROR_DEVICE_NOT_CONNECTED;
    c = g_pads[dwPort];
#if defined(__ANDROID__)
    /* No controller: the phone itself, while its on-screen controls are in
     * use (port/android/jsrf_android.c); a controller's motors otherwise. */
    {
        extern void nv_android_rumble(unsigned lo, unsigned hi);
        if (dwPort == 0) nv_android_rumble(c ? 0 : pVibration->wLeftMotorSpeed, c ? 0 : pVibration->wRightMotorSpeed);
    }
#endif
    g_rumble_lo[dwPort] = pVibration->wLeftMotorSpeed;
    g_rumble_hi[dwPort] = pVibration->wRightMotorSpeed;
    if (!c) return g_controller_connected[dwPort] ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
    /* SDL rumble needs a duration: a second at a time, sent again while it
     * lasts (rumble_refresh, from each poll) -- the game sets the motors
     * when they change, not every frame. */
    SDL_GameControllerRumble(c, pVibration->wLeftMotorSpeed,
                             pVibration->wRightMotorSpeed, 1000);
    g_rumble_sent[dwPort] = input_now();
    return ERROR_SUCCESS;
}

BOOL xbox_InputIsConnected(DWORD dwPort)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS) return FALSE;
    return g_controller_connected[dwPort];
}

DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags, XBOX_INPUT_CAPABILITIES *pCaps)
{
    (void)dwFlags;
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pCaps)
        return ERROR_DEVICE_NOT_CONNECTED;
    if (!g_pads[dwPort] && !g_controller_connected[dwPort])
        return ERROR_DEVICE_NOT_CONNECTED;

    memset(pCaps, 0, sizeof(XBOX_INPUT_CAPABILITIES));
    pCaps->Type    = 1;   /* XINPUT_DEVTYPE_GAMEPAD */
    pCaps->SubType = 1;   /* XINPUT_DEVSUBTYPE_GAMEPAD */
    pCaps->Flags   = 0;
    return ERROR_SUCCESS;
}

#endif /* _WIN32 */
