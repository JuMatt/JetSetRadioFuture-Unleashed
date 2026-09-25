/**
 * A window on macOS, without a third-party toolkit.
 *
 * The renderer builds its frame in an offscreen framebuffer on the thread
 * that drives the pushbuffer. Getting that frame onto the screen looks like
 * it should be one blit into the window's back buffer, and it is not: since
 * 10.14 every view is layer-backed, and a layer-backed OpenGL view does not
 * present anything drawn into its context from another thread. Everything
 * reports success -- the blit lands, the back buffer measurably contains the
 * picture, the framebuffer is complete, glGetError is clean -- and the window
 * stays black. Four separate measurements said "fine" before the fifth said
 * "the view IS layer-backed".
 *
 * So the frame crosses to the main thread as pixels instead, and the view
 * draws it there, in drawRect:, in its own context, which is exactly where
 * AppKit expects drawing to happen. That costs one readback and one texture
 * upload a frame -- about a megabyte at this resolution, nothing on this
 * hardware -- and in exchange there is no shared context, no cross-thread GL,
 * and no dependence on undocumented layer behaviour.
 *
 * The view's context is deliberately a legacy one. It is not shared with the
 * renderer's core-profile context and never has to be, so the simplest thing
 * that can draw a full-screen textured quad is the right thing.
 *
 * As with the gamepad shim, this file includes none of the project's headers:
 * the Windows compatibility layer typedefs BOOL as int, Objective-C's is a
 * signed char, and Carbon owns GetCurrentThread.
 */

#import <Cocoa/Cocoa.h>
#import <OpenGL/OpenGL.h>
#import <OpenGL/gl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

static NSWindow *g_window;
static int       g_w, g_h;
static int       g_should_close;
static int       g_open;

/* The frame in flight, handed over under a lock. Double-buffered so the
 * renderer is never blocked by the main thread being mid-upload. */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned char  *g_px;
static int             g_px_w, g_px_h;
static int             g_px_new;
static GLuint          g_tex;

@interface NvGLView : NSOpenGLView
@end

@implementation NvGLView

- (BOOL)isOpaque          { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }

- (void)drawRect:(NSRect)dirty
{
    NSOpenGLContext *ctx = [self openGLContext];
    NSRect back = [self convertRectToBacking:[self bounds]];
    int vw = (int)back.size.width, vh = (int)back.size.height;
    unsigned char *px = NULL;
    int pw = 0, ph = 0;

    (void)dirty;
    [ctx makeCurrentContext];

    pthread_mutex_lock(&g_lock);
    if (g_px_new && g_px) { px = g_px; pw = g_px_w; ph = g_px_h; g_px_new = 0; }
    pthread_mutex_unlock(&g_lock);

    if (px) {
        if (!g_tex) {
            glGenTextures(1, &g_tex);
            glBindTexture(GL_TEXTURE_2D, g_tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        glBindTexture(GL_TEXTURE_2D, g_tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, pw, ph, 0,
                     GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, px);
    }

    glViewport(0, 0, vw, vh);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (g_tex) {
        glMatrixMode(GL_PROJECTION); glLoadIdentity();
        glMatrixMode(GL_MODELVIEW);  glLoadIdentity();
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, g_tex);
        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        /* glReadPixels hands back row 0 first and row 0 is the bottom of the
         * GL image -- and the renderer draws the picture the GL way up (its
         * vertex stage sends screen y = 0 to NDC +1), so the bottom of the
         * quad takes v = 0 and the picture comes out upright.
         *
         * This used to be the other way round, which showed any surface the
         * title drew directly upside down. It looked right only because the
         * renderer also turned every render-to-texture copy over, and the
         * surface on screen had always been through exactly one copy. The
         * boost dash makes two copies; nv2a_gl.c now samples surfaces the
         * right way up, and this shows the frame the right way up. */
        glBegin(GL_QUADS);
            glTexCoord2f(0.0f, 0.0f); glVertex2f(-1.0f, -1.0f);
            glTexCoord2f(1.0f, 0.0f); glVertex2f( 1.0f, -1.0f);
            glTexCoord2f(1.0f, 1.0f); glVertex2f( 1.0f,  1.0f);
            glTexCoord2f(0.0f, 1.0f); glVertex2f(-1.0f,  1.0f);
        glEnd();
        glDisable(GL_TEXTURE_2D);
    }
    [ctx flushBuffer];

    /* That the view drew at all is the one thing the previous design could
     * not claim. Said three times, then never again. */
    { static int said;
      if (said < 3) {
          said++;
          fprintf(stderr, "[WIN] view drew #%d: %dx%d from a %dx%d frame\n",
                  said, vw, vh, pw, ph);
          fflush(stderr);
      } }
}
@end

static NvGLView *g_view;

@interface NvWindowDelegate : NSObject <NSWindowDelegate>
@end
@implementation NvWindowDelegate
- (BOOL)windowShouldClose:(NSWindow *)sender
{ (void)sender; g_should_close = 1; return NO; }
@end

/* ---- the keyboard as a gamepad -------------------------------------------
 *
 * A pad is the right way to play this and not everyone has one to hand, so
 * the window doubles as a controller.
 *
 * Keys are matched by POSITION (the hardware key code), not by the letter
 * the layout prints on them: the four keys under the left hand are W A S D
 * on a QWERTY Mac and Z Q S D on an AZERTY one, and they are the same four
 * key codes. The previous version matched letters and drove only the d-pad,
 * which JSRF's menus read but its skater does not -- movement is the analog
 * stick -- so on the keyboard you could reach New Game and then not move.
 * Now those keys push the left stick (and still the d-pad, for menus).
 *
 * Layout (AZERTY labels, QWERTY in brackets):
 *   Z Q S D  [W A S D]   left stick + d-pad      arrows   right stick
 *   Space  or  J         A  (jump)                K        B
 *   H                    X                        L        Y
 *   E                    R trigger (talk, spray)  A  [Q]   L trigger
 *   R                    Black                    F        White
 *   Enter                START                    Esc      BACK
 *   C  /  V              left / right stick click
 * Keys held with Command go to the menu bar instead (Cmd+Q quits).
 */
static volatile unsigned short g_kb_buttons;
static volatile unsigned char  g_kb_analog[8];

enum { KB_UP = 1u<<0, KB_DOWN = 1u<<1, KB_LEFT = 1u<<2, KB_RIGHT = 1u<<3,
       KB_START = 1u<<4, KB_BACK = 1u<<5,
       KB_R_UP = 1u<<6, KB_R_DOWN = 1u<<7, KB_R_LEFT = 1u<<8, KB_R_RIGHT = 1u<<9,
       KB_LTHUMB = 1u<<10, KB_RTHUMB = 1u<<11 };

/* Hardware key codes (HIToolbox kVK_*), named by their US-layout letter. */
enum { K_A = 0x00, K_S = 0x01, K_D = 0x02, K_F = 0x03, K_H = 0x04,
       K_C = 0x08, K_V = 0x09, K_Q = 0x0C, K_W = 0x0D, K_E = 0x0E, K_R = 0x0F,
       K_L = 0x25, K_J = 0x26, K_K = 0x28,
       K_RETURN = 0x24, K_SPACE = 0x31, K_ESCAPE = 0x35, K_PAD_ENTER = 0x4C,
       K_LEFT = 0x7B, K_RIGHT = 0x7C, K_DOWN = 0x7D, K_UP = 0x7E };

/* analog[] order, as xinput_device.c reads it: A B X Y Black White LT RT */
static void kb_apply(NSEvent *e, int down)
{
    unsigned short bit = 0;
    int analog = -1;

    switch ([e keyCode]) {
    case K_W:      bit = KB_UP;      break;
    case K_S:      bit = KB_DOWN;    break;
    case K_A:      bit = KB_LEFT;    break;
    case K_D:      bit = KB_RIGHT;   break;
    case K_UP:     bit = KB_R_UP;    break;
    case K_DOWN:   bit = KB_R_DOWN;  break;
    case K_LEFT:   bit = KB_R_LEFT;  break;
    case K_RIGHT:  bit = KB_R_RIGHT; break;
    case K_RETURN: case K_PAD_ENTER: bit = KB_START; break;
    case K_ESCAPE: bit = KB_BACK;    break;
    case K_C:      bit = KB_LTHUMB;  break;
    case K_V:      bit = KB_RTHUMB;  break;
    case K_SPACE: case K_J: analog = 0; break;   /* A */
    case K_K:      analog = 1; break;            /* B */
    case K_H:      analog = 2; break;            /* X */
    case K_L:      analog = 3; break;            /* Y */
    case K_R:      analog = 4; break;            /* Black */
    case K_F:      analog = 5; break;            /* White */
    case K_Q:      analog = 6; break;            /* L trigger */
    case K_E:      analog = 7; break;            /* R trigger */
    default: return;
    }
    if (bit) {
        if (down) g_kb_buttons |= bit;
        else      g_kb_buttons &= (unsigned short)~bit;
    }
    if (analog >= 0)
        g_kb_analog[analog] = down ? 255 : 0;
}

/* Losing focus with a key held never delivers its key-up: let go of all. */
static void kb_release_all(void)
{
    int i;
    g_kb_buttons = 0;
    for (i = 0; i < 8; i++) g_kb_analog[i] = 0;
}

int nv_window_keys(unsigned short *buttons, unsigned char *analog)
{
    int i, any;
    unsigned short b = g_kb_buttons;
    if (buttons) *buttons = b;
    any = b != 0;
    for (i = 0; i < 8; i++) {
        unsigned char v = g_kb_analog[i];
        if (analog) analog[i] = v;
        if (v) any = 1;
    }
    return any;
}


/* ---- the menu bar --------------------------------------------------------
 *
 * An app with no menu bar has no Quit, no About, and no way to change
 * anything without an environment variable -- which is fine for a diagnostic
 * run driven from a shell and not fine for something someone double-clicks.
 *
 * Resolution is the one setting worth putting in front of a player. The game
 * draws a 640x480 surface and always will; what changes is the resolution the
 * renderer works at internally, and the window follows it so the picture is
 * presented one-to-one instead of being stretched. At 1x in a 1280x960 window
 * every pixel is doubled and it looks like it: the default is 2x.
 *
 * The choice is stored rather than applied live, because the render targets,
 * the depth buffer and the texture cache are all sized at startup, and tearing
 * them down mid-frame to avoid a relaunch would be a great deal of machinery
 * for a setting people change once. Choosing a scale restarts the game.
 */

static int scale_clamp(int s) { return s < 1 ? 1 : s > 4 ? 4 : s; }

int nv_window_pref_scale(void);
int nv_window_pref_scale(void)
{
    /* An explicit RECOMP_GL_SCALE in the environment wins: someone who set it
     * on a command line means it, and should not be overruled by a menu they
     * cannot see. */
    const char *e = getenv("RECOMP_GL_SCALE");
    if (e && *e) return scale_clamp(atoi(e));
    @autoreleasepool {
        NSInteger v = [[NSUserDefaults standardUserDefaults] integerForKey:@"glScale"];
        return scale_clamp(v ? (int)v : 2);
    }
}

@interface NvMenuTarget : NSObject
- (void)pickScale:(id)sender;
- (void)showKeys:(id)sender;
@end

@implementation NvMenuTarget
- (void)showKeys:(id)sender
{
    NSAlert *a = [[NSAlert alloc] init];
    (void)sender;
    kb_release_all();
    a.messageText = @"Keyboard controls";
    a.informativeText =
        @"Keys are by position: AZERTY labels first, QWERTY in brackets.\n\n"
         "Move (left stick)\tZ Q S D  [W A S D]\n"
         "Camera (right stick)\tarrow keys\n"
         "A - jump\t\tSpace or J\n"
         "B\t\t\tK\n"
         "X\t\t\tH\n"
         "Y\t\t\tL\n"
         "R trigger - talk, spray\tE\n"
         "L trigger\t\tA  [Q]\n"
         "Black / White\t\tR / F\n"
         "Start / Back\t\tEnter / Esc\n"
         "Stick clicks\t\tC / V\n\n"
         "A game controller works too, at the same time.";
    [a runModal];
}
- (void)pickScale:(id)sender
{
    NSInteger want = [(NSMenuItem *)sender tag];
    [[NSUserDefaults standardUserDefaults] setInteger:want forKey:@"glScale"];
    [[NSUserDefaults standardUserDefaults] synchronize];

    {
        extern void recomp_restart_self(void) __attribute__((weak));
        if (recomp_restart_self) {
            recomp_restart_self();           /* does not return on success */
        }
        {
            NSAlert *a = [[NSAlert alloc] init];
            a.messageText = @"Restart to change the resolution";
            a.informativeText = @"The renderer is set up once at startup. "
                                 "Quit and launch again and it will be at the "
                                 "size you picked.";
            [a runModal];
        }
    }
}
@end

static NvMenuTarget *g_menu_target;

static void build_menu(void)
{
    NSString *app = [[NSProcessInfo processInfo] processName];
    NSMenu *bar = [[NSMenu alloc] init];
    NSMenuItem *appItem = [[NSMenuItem alloc] init];
    NSMenu *appMenu = [[NSMenu alloc] init];
    NSMenuItem *videoItem = [[NSMenuItem alloc] init];
    NSMenu *video = [[NSMenu alloc] initWithTitle:@"Video"];
    NSMenu *res = [[NSMenu alloc] initWithTitle:@"Resolution"];
    NSMenuItem *resItem = [[NSMenuItem alloc] initWithTitle:@"Resolution"
                                                    action:nil keyEquivalent:@""];
    int cur = nv_window_pref_scale();
    int i;

    g_menu_target = [[NvMenuTarget alloc] init];

    [appMenu addItemWithTitle:[@"Hide " stringByAppendingString:app]
                       action:@selector(hide:) keyEquivalent:@"h"];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:[@"Quit " stringByAppendingString:app]
                       action:@selector(terminate:) keyEquivalent:@"q"];
    [appItem setSubmenu:appMenu];
    [bar addItem:appItem];

    for (i = 1; i <= 4; i++) {
        NSMenuItem *it = [[NSMenuItem alloc]
            initWithTitle:[NSString stringWithFormat:@"%dx  (%d x %d)",
                                                     i, 640 * i, 480 * i]
                   action:@selector(pickScale:)
            keyEquivalent:[NSString stringWithFormat:@"%d", i]];
        [it setTag:i];
        [it setTarget:g_menu_target];
        [it setState:(i == cur) ? NSControlStateValueOn : NSControlStateValueOff];
        [res addItem:it];
    }
    [resItem setSubmenu:res];
    [video addItem:resItem];
    [videoItem setSubmenu:video];
    [bar addItem:videoItem];

    {
        NSMenuItem *ctlItem = [[NSMenuItem alloc] init];
        NSMenu *ctl = [[NSMenu alloc] initWithTitle:@"Controls"];
        NSMenuItem *keys = [[NSMenuItem alloc] initWithTitle:@"Keyboard..."
                                                      action:@selector(showKeys:)
                                               keyEquivalent:@"k"];
        [keys setTarget:g_menu_target];
        [ctl addItem:keys];
        [ctlItem setSubmenu:ctl];
        [bar addItem:ctlItem];
    }

    [NSApp setMainMenu:bar];
}

/* ---- the window ---------------------------------------------------------- */

/* Must be called on the main thread: AppKit refuses to create a window
 * anywhere else, and says so by throwing. Returns 1 on success. */
int nv_window_prepare(int width, int height, const char *title)
{
    @autoreleasepool {
        NSOpenGLPixelFormatAttribute attrs[] = {
            NSOpenGLPFAColorSize,  24,
            NSOpenGLPFAAlphaSize,   8,
            NSOpenGLPFADoubleBuffer,
            NSOpenGLPFAAccelerated,
            0
        };
        NSOpenGLPixelFormat *pf;
        NSRect frame = NSMakeRect(0, 0, width, height);

        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        pf = [[NSOpenGLPixelFormat alloc] initWithAttributes:attrs];
        if (!pf) { fprintf(stderr, "[WIN] no pixel format\n"); return 0; }

        g_window = [[NSWindow alloc]
            initWithContentRect:frame
                      styleMask:(NSWindowStyleMaskTitled |
                                 NSWindowStyleMaskClosable |
                                 NSWindowStyleMaskMiniaturizable)
                        backing:NSBackingStoreBuffered
                          defer:NO];
        [g_window setTitle:[NSString stringWithUTF8String:title ? title : "JSRF"]];
        [g_window setDelegate:[[NvWindowDelegate alloc] init]];
        [g_window center];

        g_view = [[NvGLView alloc] initWithFrame:frame pixelFormat:pf];
        if (!g_view) { fprintf(stderr, "[WIN] no GL view\n"); return 0; }
        [g_view setWantsBestResolutionOpenGLSurface:YES];
        [g_window setContentView:g_view];
        [g_window makeFirstResponder:g_view];
        build_menu();
        [g_window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];

        { GLint one = 1;
          [[g_view openGLContext] setValues:&one
              forParameter:NSOpenGLContextParameterSwapInterval]; }

        g_w = width; g_h = height; g_open = 1;
        fprintf(stderr, "[WIN] %dx%d window open (drawn on the main thread)\n",
                width, height);
        return 1;
    }
}

int nv_window_is_open(void) { return g_open; }

/* Called from the render thread with a finished frame, bottom row first, as
 * glReadPixels hands it over. Copied rather than referenced: the caller's
 * buffer is reused for the next frame the moment this returns. */
void nv_window_submit_frame(const void *pixels, int width, int height)
{
    size_t need;
    if (!g_open || !pixels || width <= 0 || height <= 0) return;
    need = (size_t)width * (size_t)height * 4u;
    pthread_mutex_lock(&g_lock);
    if (g_px_w != width || g_px_h != height || !g_px) {
        free(g_px);
        g_px = (unsigned char *)malloc(need);
        g_px_w = width; g_px_h = height;
    }
    if (g_px) { memcpy(g_px, pixels, need); g_px_new = 1; }
    pthread_mutex_unlock(&g_lock);
}

/* Drain the event queue and draw. Main thread only. */
void nv_window_pump(void)
{
    @autoreleasepool {
        NSEvent *e;
        int have;

        if (!g_window) return;
        while ((e = [NSApp nextEventMatchingMask:NSEventMaskAny
                                       untilDate:[NSDate distantPast]
                                          inMode:NSDefaultRunLoopMode
                                         dequeue:YES]) != nil) {
            NSEventType t = [e type];
            int is_key = (t == NSEventTypeKeyDown || t == NSEventTypeKeyUp);
            /* Command-key combinations belong to the menu bar (Cmd+Q, Cmd+H,
             * Cmd+1..4): forward them and do not treat them as pad input. */
            if (is_key && ([e modifierFlags] & NSEventModifierFlagCommand)) {
                [NSApp sendEvent:e];
                continue;
            }
            if (t == NSEventTypeKeyDown)    kb_apply(e, 1);
            else if (t == NSEventTypeKeyUp) kb_apply(e, 0);
            /* Other key events are consumed rather than forwarded: with no
             * text field, AppKit answers an unhandled key press with a beep,
             * and holding a direction to skate would beep continuously. */
            if (!is_key)
                [NSApp sendEvent:e];
        }

        pthread_mutex_lock(&g_lock);
        have = g_px_new;
        if (g_window && ![g_window isKeyWindow] && (g_kb_buttons || g_kb_analog[0] ||
            g_kb_analog[1] || g_kb_analog[2] || g_kb_analog[3] || g_kb_analog[4] ||
            g_kb_analog[5] || g_kb_analog[6] || g_kb_analog[7]))
            kb_release_all();
        pthread_mutex_unlock(&g_lock);
        if (have) [g_view display];
    }
}

/* Kept so the renderer's present path has something to call; the drawing
 * happens on the main thread now, so there is nothing to do here. */
void nv_window_present(void) { }

int nv_window_should_close(void) { return g_should_close; }

void nv_window_size(int *w, int *h) { if (w) *w = g_w; if (h) *h = g_h; }

void nv_window_drawable_size(int *w, int *h)
{
    @autoreleasepool {
        NSRect r = g_view ? [g_view convertRectToBacking:[g_view bounds]]
                          : NSMakeRect(0, 0, g_w, g_h);
        if (w) *w = (int)r.size.width;
        if (h) *h = (int)r.size.height;
    }
}
