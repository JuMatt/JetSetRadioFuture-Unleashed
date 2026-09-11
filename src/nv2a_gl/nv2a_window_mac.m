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
         * GL image, so the bottom of the quad takes v = 0. */
        glBegin(GL_QUADS);
            /* Upside down, deliberately: the frame arrives from
             * glReadPixels, and GL numbers a framebuffer's rows from the
             * bottom while everything that looks at a picture numbers them
             * from the top. The BMP writer flips for the same reason. This was
             * wrong from the first version of this file and invisible until
             * the window started showing one finished frame instead of three
             * half-finished passes a frame. */
            glTexCoord2f(0.0f, 1.0f); glVertex2f(-1.0f, -1.0f);
            glTexCoord2f(1.0f, 1.0f); glVertex2f( 1.0f, -1.0f);
            glTexCoord2f(1.0f, 0.0f); glVertex2f( 1.0f,  1.0f);
            glTexCoord2f(0.0f, 0.0f); glVertex2f(-1.0f,  1.0f);
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
 * the window doubles as a controller. Both ZQSD and WASD are accepted for the
 * d-pad because the Mac's layout decides which set the same four physical
 * keys produce, and asking someone to switch layouts to play is worse than
 * accepting eight keys.
 */
static volatile unsigned short g_kb_buttons;
static volatile unsigned char  g_kb_analog[8];

enum { KB_UP = 1u<<0, KB_DOWN = 1u<<1, KB_LEFT = 1u<<2, KB_RIGHT = 1u<<3,
       KB_START = 1u<<4, KB_BACK = 1u<<5 };

static void kb_apply(NSEvent *e, int down)
{
    NSString *chars = [e charactersIgnoringModifiers];
    unichar c = [chars length] ? [chars characterAtIndex:0] : 0;
    unsigned short bit = 0;
    int analog = -1;

    switch (c) {
    case 'z': case 'Z': case 'w': case 'W': bit = KB_UP;    break;
    case 's': case 'S':                     bit = KB_DOWN;  break;
    case 'q': case 'Q': case 'a': case 'A': bit = KB_LEFT;  break;
    case 'd': case 'D':                     bit = KB_RIGHT; break;
    case ' ':                               bit = KB_START; break;
    case 27:                                bit = KB_BACK;  break;
    case 'h': case 'H': analog = 2; break;   /* X */
    case 'j': case 'J': analog = 0; break;   /* A */
    case 'k': case 'K': analog = 1; break;   /* B */
    case 'l': case 'L': analog = 3; break;   /* Y */
    default: return;
    }
    if (bit) {
        if (down) g_kb_buttons |= bit;
        else      g_kb_buttons &= (unsigned short)~bit;
    }
    if (analog >= 0)
        g_kb_analog[analog] = down ? 255 : 0;
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
            if (t == NSEventTypeKeyDown)    kb_apply(e, 1);
            else if (t == NSEventTypeKeyUp) kb_apply(e, 0);
            /* Key events are consumed rather than forwarded: with no menu bar
             * and no text field, AppKit answers an unhandled key press with a
             * beep, and holding a direction to walk would beep continuously. */
            if (t != NSEventTypeKeyDown && t != NSEventTypeKeyUp)
                [NSApp sendEvent:e];
        }

        pthread_mutex_lock(&g_lock);
        have = g_px_new;
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
