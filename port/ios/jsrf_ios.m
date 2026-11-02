/*
 * JSRF Unleashed on iPhone and iPad.
 *
 * On the Mac the engine opens its own window (nv2a_window_mac.m) and a Swift
 * launcher finds the game and sets the environment before handing the
 * process over. iOS allows neither a second program nor a window outside
 * UIKit's life cycle, so this one file is both: the app (a scene, a view
 * controller, a Metal view filling the screen), the launcher (the game in
 * the app's Documents folder, the environment the Mac launcher sets, the
 * logs), and the window functions the renderer and the pad code ask for --
 * the same names nv2a_window_mac.m defines, which the runtime references
 * weakly.
 *
 *   - The game: the folder with default.xbe in it, copied into the app's
 *     Documents (the Files app shows it as On My iPhone > JSRF Unleashed;
 *     a Mac's Finder shows it under the phone's Files tab). A disc image
 *     (.iso) put there is unpacked once, by xiso.c.
 *   - The engine (main.c's host_main) runs on a thread of its own, the
 *     guest's main thread, as the Mac's window path does it.
 *   - The picture: the Metal renderer presents into this view's
 *     CAMetalLayer from its own threads, letterboxed (nv2a_metal.m).
 *   - Input: any controller iOS knows (GameController, read by
 *     xinput_gc.m); without one, on-screen controls of its own (NvTouchPad:
 *     a stick under the left thumb, the pad's buttons under the right, Start
 *     and Back at the top); a hardware keyboard by key position, as on the
 *     Mac.
 *   - In the background nothing is drawn: the GPU thread waits at the next
 *     frame until the app is back (iOS refuses GPU work from the
 *     background), which holds the game where it was.
 *
 * Plain C across the boundary, as in nv2a_window_mac.m: the Windows
 * compatibility layer and Objective-C disagree on BOOL.
 */

#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>
#import <Metal/Metal.h>
#import <GameController/GameController.h>
#import <AVFoundation/AVFoundation.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "xiso.h"
#include "../xbe_check.h"

/* ---- what the runtime asks for ------------------------------------------ */

static int (*g_host_main)(void);
static CAMetalLayer *g_layer;            /* set once, kept for the process's life */
static volatile int  g_open;
static volatile int  g_visible;
static volatile int  g_refresh_hz;
static volatile int  g_dw, g_dh;
static pthread_mutex_t g_vis_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_vis_cv = PTHREAD_COND_INITIALIZER;

/* The keyboard and the on-screen Start / Back, in the bits xinput_device.c's
 * kb_merge reads (the Mac's layout). analog[] is A B X Y Black White LT RT. */
enum { KB_UP = 1u<<0, KB_DOWN = 1u<<1, KB_LEFT = 1u<<2, KB_RIGHT = 1u<<3,
       KB_START = 1u<<4, KB_BACK = 1u<<5,
       KB_R_UP = 1u<<6, KB_R_DOWN = 1u<<7, KB_R_LEFT = 1u<<8, KB_R_RIGHT = 1u<<9,
       KB_LTHUMB = 1u<<10, KB_RTHUMB = 1u<<11 };
static volatile unsigned short g_kb_buttons;
static volatile unsigned char  g_kb_analog[8];
/* The on-screen controls (NvTouchPad, below): the Start / Back buttons, the
 * d-pad bits the stick also gives (menus read the d-pad), the buttons as
 * analog values, the stick itself, and whether they are showing at all --
 * while they are, the pad counts as connected even with nothing held. */
static volatile unsigned short g_pill_buttons, g_stick_dpad;
static volatile unsigned char  g_touch_analog[8];
static volatile short g_stick_x, g_stick_y;
static volatile int   g_stick_on, g_touch_on;

void *nv_window_metal_layer(void);
void *nv_window_metal_layer(void) { return g_open ? (__bridge void *)g_layer : NULL; }

int nv_window_visible(void);
int nv_window_visible(void) { return g_visible; }

int nv_window_refresh_hz(void);
int nv_window_refresh_hz(void) { return g_refresh_hz; }

int nv_window_is_open(void);
int nv_window_is_open(void) { return g_open; }

void nv_window_drawable_size(int *w, int *h);
void nv_window_drawable_size(int *w, int *h) { if (w) *w = g_dw; if (h) *h = g_dh; }

int nv_window_should_close(void);
int nv_window_should_close(void) { return 0; }

/* The renderer's frame boundary (nv2a_metal.m, before it commits a frame):
 * while the app is in the background, wait. */
void nv_window_wait_visible(void);
void nv_window_wait_visible(void)
{
    pthread_mutex_lock(&g_vis_mu);
    while (!g_visible) pthread_cond_wait(&g_vis_cv, &g_vis_mu);
    pthread_mutex_unlock(&g_vis_mu);
}

static void set_visible(int v)
{
    pthread_mutex_lock(&g_vis_mu);
    g_visible = v;
    pthread_cond_broadcast(&g_vis_cv);
    pthread_mutex_unlock(&g_vis_mu);
}

int nv_window_keys(unsigned short *buttons, unsigned char *analog);
int nv_window_keys(unsigned short *buttons, unsigned char *analog)
{
    unsigned short b = (unsigned short)(g_kb_buttons | g_pill_buttons | g_stick_dpad);
    int i, any = b != 0 || g_touch_on;
    if (buttons) *buttons = b;
    for (i = 0; i < 8; i++) {
        unsigned char k = g_kb_analog[i], t = g_touch_analog[i], v = k > t ? k : t;
        if (analog) analog[i] = v;
        if (v) any = 1;
    }
    return any;
}

/* The on-screen stick, for xinput_device.c's kb_merge: analog, while held. */
int nv_window_touch_stick(short *x, short *y);
int nv_window_touch_stick(short *x, short *y)
{
    if (!g_stick_on) return 0;
    *x = g_stick_x;
    *y = g_stick_y;
    return 1;
}

static int scale_clamp(long s) { return s < 1 ? 1 : s > 4 ? 4 : (int)s; }

/* The renderer's resolution, 640x480 times this. Read once, at start. */
int nv_window_pref_scale(void);
int nv_window_pref_scale(void)
{
    const char *e = getenv("RECOMP_GL_SCALE");
    if (e && *e) return scale_clamp(atol(e));
    @autoreleasepool {
        NSInteger v = [[NSUserDefaults standardUserDefaults] integerForKey:@"glScale"];
        return scale_clamp(v ? (long)v : 2);
    }
}

/* The picture: 0 the console's 4:3, 1 16:9 (the default), 2 Fill the screen
 * (every iPhone since the X is about 19.5:9, where 16:9 leaves black bars at
 * both ends). Kept as "picture" since 0.4.4; the "widescreen" switch before
 * it is not read, so a player who had turned it off starts at 16:9, once. */
static int picture_pref(void)
{
    @autoreleasepool {
        NSUserDefaults *d = [NSUserDefaults standardUserDefaults];
        NSInteger v = [d objectForKey:@"picture"] ? [d integerForKey:@"picture"] : 1;
        return v >= 0 && v <= 2 ? (int)v : 1;
    }
}

/* nv2a_widescreen() asks this when RECOMP_WIDESCREEN is not set. */
int nv_window_pref_widescreen(void);
int nv_window_pref_widescreen(void) { return picture_pref() != 0; }

int nv_window_pref_metal(void);
int nv_window_pref_metal(void) { return 1; }

/* The camera on the right stick (port/recomp_manual.c): 1 on (the default),
 * 2 tracking -- it only looks around, the skater keeps his heading -- or 0
 * off, in the settings, as the "rstickCamera" and "rstickTracking" defaults --
 * the Mac's keys. Asked every frame, so kept here once read. */
static volatile int g_rstick_cam = -1;
int nv_window_pref_rstick_camera(void);
int nv_window_pref_rstick_camera(void)
{
    if (g_rstick_cam < 0) {
        @autoreleasepool {
            NSUserDefaults *d = [NSUserDefaults standardUserDefaults];
            id v = [d objectForKey:@"rstickCamera"];
            int on = v ? ([v boolValue] ? 1 : 0) : 1;
            g_rstick_cam = on ? ([d boolForKey:@"rstickTracking"] ? 2 : 1) : 0;
        }
    }
    return g_rstick_cam;
}
static NSString *const k_rcam[3] = { @"off", @"on", @"tracking" };

/* Anti-aliasing (aa_start in nv2a_gl.c asks): SMAA on or off ("smaa"),
 * applied at once, and multisampling's samples a pixel ("msaa", 0 none),
 * read when the renderer starts -- Metal makes its pipelines for one count. */
int nv_window_pref_smaa(void);
int nv_window_pref_smaa(void)
{
    @autoreleasepool { return [[NSUserDefaults standardUserDefaults] boolForKey:@"smaa"] ? 1 : 0; }
}
static int msaa_pref(void)
{
    @autoreleasepool {
        NSInteger v = [[NSUserDefaults standardUserDefaults] integerForKey:@"msaa"];
        return v == 2 || v == 4 ? (int)v : 0;
    }
}
int nv_window_pref_msaa(void);
int nv_window_pref_msaa(void) { return msaa_pref(); }
extern void nv2a_set_smaa(int on);
extern int  nv2a_smaa(void);
extern int  nv2a_msaa_max(void);

extern int  nv2a_widescreen(void);
extern void nv2a_set_widescreen(int on);
extern void nv2a_set_wide_aspect(float aspect);

static int fill_pref(void) { return picture_pref() == 2; }

static NSString *const k_picture[3] = { @"4:3", @"16:9", @"Fill the screen" };
static int picture_mode(void) { return !nv2a_widescreen() ? 0 : fill_pref() ? 2 : 1; }

/* The shape a widened scene is drawn and shown at: the view's own when
 * filling it (the renderer keeps it between 16:9 and 2.4:1, so on an iPad,
 * narrower than 16:9, it is 16:9), else 16:9. */
static void apply_wide_shape(CGSize view)
{
    double w = view.width, h = view.height, a;
    if (w <= 0 || h <= 0) return;
    a = w > h ? w / h : h / w;              /* landscape, whichever way up */
    nv2a_set_wide_aspect(fill_pref() ? (float)a : 16.0f / 9.0f);
}
extern void xa2_resume(void);

/* ---- where things are --------------------------------------------------- */

static NSString *docs_dir(void)
{
    return NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
}

static BOOL is_dir(NSString *p)
{
    BOOL d = NO;
    return [[NSFileManager defaultManager] fileExistsAtPath:p isDirectory:&d] && d;
}

static BOOL has_xbe(NSString *dir)
{
    return [[NSFileManager defaultManager]
               fileExistsAtPath:[dir stringByAppendingPathComponent:@"default.xbe"]];
}

static BOOL skip_name(NSString *n)
{
    return [n hasPrefix:@"."] || [n isEqualToString:@"Logs"] || [n isEqualToString:@"Saves"]
        || [n hasSuffix:@".unpacking"] || [n isEqualToString:@"Inbox"];
}

static NSArray<NSString *> *listing(NSString *dir)
{
    NSArray *a = [[NSFileManager defaultManager] contentsOfDirectoryAtPath:dir error:nil];
    return [a sortedArrayUsingSelector:@selector(localizedStandardCompare:)] ?: @[];
}

/* The game: Documents itself, or a folder in it, or a folder in that (people
 * copy the folder that holds the game's folder as often as the game's). */
static NSString *find_game(void)
{
    NSString *docs = docs_dir();
    const char *e = getenv("JSRF_GAME_DIR");          /* tests (SIMCTL_CHILD_JSRF_GAME_DIR) */
    if (e && *e && has_xbe(@(e))) return @(e);
    if (has_xbe(docs)) return docs;
    for (NSString *k in listing(docs)) {
        NSString *p = [docs stringByAppendingPathComponent:k];
        if (skip_name(k) || !is_dir(p)) continue;
        if (has_xbe(p)) return p;
        for (NSString *k2 in listing(p)) {
            NSString *p2 = [p stringByAppendingPathComponent:k2];
            if (!skip_name(k2) && is_dir(p2) && has_xbe(p2)) return p2;
        }
    }
    return nil;
}

/* A disc image to unpack: Documents, one folder down, or Inbox (where
 * "Open in" and AirDrop leave files). */
static NSString *find_iso(void)
{
    NSString *docs = docs_dir();
    NSMutableArray *dirs = [NSMutableArray arrayWithObject:docs];
    for (NSString *k in listing(docs)) {
        NSString *p = [docs stringByAppendingPathComponent:k];
        if (![k hasPrefix:@"."] && ![k isEqualToString:@"Logs"] && ![k isEqualToString:@"Saves"]
            && ![k hasSuffix:@".unpacking"] && is_dir(p))
            [dirs addObject:p];
    }
    for (NSString *d in dirs)
        for (NSString *k in listing(d)) {
            NSString *ext = k.pathExtension.lowercaseString;
            NSString *p = [d stringByAppendingPathComponent:k];
            if (([ext isEqualToString:@"iso"] || [ext isEqualToString:@"xiso"]) && !is_dir(p)
                && xiso_probe(p.fileSystemRepresentation))
                return p;
        }
    return nil;
}

static unsigned long long free_bytes(NSString *path)
{
    NSDictionary *a = [[NSFileManager defaultManager] attributesOfFileSystemForPath:path error:nil];
    return [a[NSFileSystemFreeSize] unsignedLongLongValue];
}

/* ---- the engine --------------------------------------------------------- */

@class NvGameController;
static NvGameController *g_vc;
/* ---- Photo mode (port/recomp_manual.c): the settings' request, and the help
 * panel -- a label over the game, at its bottom-left corner. */
static volatile int g_photo_request;
void nv_window_request_photo(void);
void nv_window_request_photo(void) { g_photo_request = 1; }
int nv_window_take_photo_request(void);
int nv_window_take_photo_request(void) { int r = g_photo_request; g_photo_request = 0; return r; }
static UILabel *g_help_label;

@interface NvGameController : UIViewController
- (void)engineStopped:(int)rc;
- (void)tryStart;
- (void)releaseKeys;
- (void)updateControls;
@property (nonatomic) BOOL started;
@end

static void *engine_main(void *arg)
{
    int rc;
    (void)arg;
    pthread_setname_np("jsrf-guest");
    rc = g_host_main ? g_host_main() : 1;
    fprintf(stderr, "[IOS] the engine returned %d\n", rc);
    fflush(stderr);
    dispatch_async(dispatch_get_main_queue(), ^{ [g_vc engineStopped:rc]; });
    return NULL;
}
void nv_window_photo_help(const char *text);
void nv_window_photo_help(const char *text)
{
    NSString *str = text ? [NSString stringWithUTF8String:text] : nil;
    dispatch_async(dispatch_get_main_queue(), ^{
        UIView *root = g_vc ? g_vc.view : nil;
        NSMutableAttributedString *as;
        NSRange first;
        CGSize sz;
        if (!root) return;
        if (!str) { [g_help_label removeFromSuperview]; return; }
        if (!g_help_label) {
            g_help_label = [[UILabel alloc] init];
            g_help_label.numberOfLines = 0;
            g_help_label.backgroundColor = [UIColor colorWithWhite:0.05 alpha:0.82];
            g_help_label.userInteractionEnabled = NO;
            g_help_label.layer.cornerRadius = 6.0;
            g_help_label.clipsToBounds = YES;
        }
        as = [[NSMutableAttributedString alloc] initWithString:str attributes:@{
            NSFontAttributeName: [UIFont monospacedSystemFontOfSize:11.0 weight:UIFontWeightMedium],
            NSForegroundColorAttributeName: [UIColor colorWithWhite:0.93 alpha:1.0] }];
        first = [str rangeOfString:@"\n"];
        if (first.location != NSNotFound)
            [as addAttribute:NSForegroundColorAttributeName
                       value:[UIColor colorWithRed:0.75 green:0.95 blue:0.16 alpha:1.0]
                       range:NSMakeRange(0, first.location)];
        g_help_label.attributedText = as;
        sz = [g_help_label sizeThatFits:CGSizeMake(root.bounds.size.width, CGFLOAT_MAX)];
        g_help_label.frame = CGRectMake(root.safeAreaInsets.left + 12,
                                        root.bounds.size.height - root.safeAreaInsets.bottom - sz.height - 28,
                                        sz.width + 20, sz.height + 16);
        g_help_label.textAlignment = NSTextAlignmentCenter;
        if (g_help_label.superview != root) [root addSubview:g_help_label];
        [root bringSubviewToFront:g_help_label];
    });
}

static int start_engine(NSString *game)
{
    NSString *saves = [docs_dir() stringByAppendingPathComponent:@"Saves"];
    const char *sv = getenv("JSRF_SAVE_DIR");
    pthread_t t;
    pthread_attr_t a;
    int ok;

    [[NSFileManager defaultManager] createDirectoryAtPath:saves withIntermediateDirectories:YES
                                               attributes:nil error:nil];
    setenv("JSRF_GAME_DIR", game.fileSystemRepresentation, 1);
    if (!(sv && *sv)) setenv("JSRF_SAVE_DIR", saves.fileSystemRepresentation, 1);
    chdir(game.fileSystemRepresentation);   /* where the Mac launcher runs it */
    fprintf(stderr, "[IOS] game: %s\n[IOS] saves: %s\n", game.UTF8String, getenv("JSRF_SAVE_DIR"));
    fflush(stderr);

    /* The guest's main thread: a large stack, as a process's main thread has. */
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 16u << 20);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_attr_set_qos_class_np(&a, QOS_CLASS_USER_INTERACTIVE, 0);
    ok = pthread_create(&t, &a, engine_main, NULL) == 0;
    pthread_attr_destroy(&a);
    return ok;
}

/* ---- the view ----------------------------------------------------------- */

@interface NvMetalView : UIView
@end

@implementation NvMetalView
+ (Class)layerClass { return [CAMetalLayer class]; }

- (instancetype)initWithFrame:(CGRect)frame
{
    if ((self = [super initWithFrame:frame])) {
        CAMetalLayer *l = (CAMetalLayer *)self.layer;
        CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        l.device = MTLCreateSystemDefaultDevice();
        l.pixelFormat = MTLPixelFormatBGRA8Unorm;
        l.framebufferOnly = YES;
        l.opaque = YES;
        l.colorspace = cs;                  /* the console's output is sRGB */
        CGColorSpaceRelease(cs);
        self.backgroundColor = [UIColor blackColor];
        self.multipleTouchEnabled = YES;
        g_layer = l;
    }
    return self;
}

/* The drawable follows the view, in pixels; the renderer letterboxes the
 * frame into it. */
- (void)layoutSubviews
{
    CAMetalLayer *l = (CAMetalLayer *)self.layer;
    UIScreen *scr;
    CGFloat s;
    CGSize sz;
    [super layoutSubviews];
    scr = self.window.windowScene.screen;
    s = scr ? scr.nativeScale : self.contentScaleFactor;
    if (s <= 0) s = 1;
    l.contentsScale = s;
    sz = CGSizeMake(floor(self.bounds.size.width * s), floor(self.bounds.size.height * s));
    if (sz.width >= 1 && sz.height >= 1 && !CGSizeEqualToSize(l.drawableSize, sz)) {
        l.drawableSize = sz;
        g_dw = (int)sz.width; g_dh = (int)sz.height;
        fprintf(stderr, "[WIN] drawable %.0fx%.0f\n", sz.width, sz.height);
        fflush(stderr);
    }
    apply_wide_shape(self.bounds.size);
    if (scr) g_refresh_hz = (int)scr.maximumFramesPerSecond;
}
@end

/* ---- the controller: everything on screen ------------------------------- */

@interface NvGameController ()
@property (nonatomic, strong) UIView *setup;            /* "add your game" */
@property (nonatomic, strong) UILabel *setupText;
@property (nonatomic, strong) UIButton *checkButton;
@property (nonatomic, strong) UIButton *unpackButton;
@property (nonatomic, strong) UIProgressView *progress;
@property (nonatomic, copy)   NSString *iso;
@property (nonatomic, strong) UIButton *gear, *startButton, *backButton;
@property (nonatomic, strong) UIView *touchPad;         /* NvTouchPad */
@property (nonatomic) BOOL unpacking;
@property (nonatomic) CFTimeInterval startDown, backDown;
@end

static UIButton *pill(NSString *title, CGFloat font)
{
    UIButton *b = [UIButton buttonWithType:UIButtonTypeCustom];
    [b setTitle:title forState:UIControlStateNormal];
    [b setTitleColor:[UIColor colorWithWhite:1 alpha:0.9] forState:UIControlStateNormal];
    [b setTitleColor:[UIColor colorWithWhite:1 alpha:0.5] forState:UIControlStateHighlighted];
    b.titleLabel.font = [UIFont systemFontOfSize:font weight:UIFontWeightSemibold];
    b.backgroundColor = [UIColor colorWithWhite:0.15 alpha:0.55];
    b.layer.cornerRadius = 15;
    b.layer.borderWidth = 1;
    b.layer.borderColor = [UIColor colorWithWhite:1 alpha:0.25].CGColor;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    b.contentEdgeInsets = UIEdgeInsetsMake(5, 14, 5, 14);   /* not a configuration button */
#pragma clang diagnostic pop
    b.translatesAutoresizingMaskIntoConstraints = NO;
    return b;
}

static UIButton *big_button(NSString *title)
{
    UIButton *b = [UIButton buttonWithType:UIButtonTypeSystem];
    [b setTitle:title forState:UIControlStateNormal];
    b.titleLabel.font = [UIFont systemFontOfSize:18 weight:UIFontWeightSemibold];
    b.backgroundColor = [UIColor colorWithRed:0.62 green:0.86 blue:0.16 alpha:1];
    [b setTitleColor:[UIColor blackColor] forState:UIControlStateNormal];
    b.layer.cornerRadius = 10;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    b.contentEdgeInsets = UIEdgeInsetsMake(10, 22, 10, 22);
#pragma clang diagnostic pop
    return b;
}

/* ---- the on-screen controls ---------------------------------------------
 *
 * A stick under the left thumb -- wherever it lands on the left half of the
 * screen, it is the stick's centre, and it moves the pad's left stick by as
 * far as it goes (the skater's speed follows) -- and the pad's buttons under
 * the right: A B X Y as on the controller, R (talk, spray) and L above them
 * and above the stick, and the Xbox's White and Black as a hollow and a
 * filled circle. A thumb can slide from one button to the next. Start and
 * Back are the two buttons at the top (in the controller's view, above
 * this one). Drawn, not images: nothing to ship but this file.
 */
enum { PB_A, PB_B, PB_X, PB_Y, PB_R, PB_L, PB_BLACK, PB_WHITE, PB_COUNT };
static NSString *const k_pb_label[PB_COUNT] = { @"A", @"B", @"X", @"Y", @"R", @"L", @"●", @"○" };
static const int k_pb_analog[PB_COUNT] = { 0, 1, 2, 3, 7, 6, 4, 5 };   /* A B X Y Black White LT RT */
#define PB_TOUCHES 12

@interface NvTouchPad : UIView
- (void)reset;
@end

@implementation NvTouchPad {
    CGPoint _c[PB_COUNT];
    CGFloat _r[PB_COUNT];
    int     _held[PB_COUNT];
    const void *_touch[PB_TOUCHES];   /* the touches being followed, and what each is on: */
    int         _on[PB_TOUCHES];      /* a button, or -1 for the stick */
    const void *_stick;
    CGPoint _home, _centre, _at;
    CGFloat _reach;
}

- (instancetype)initWithFrame:(CGRect)frame
{
    if ((self = [super initWithFrame:frame])) {
        self.backgroundColor = [UIColor clearColor];
        self.opaque = NO;
        self.multipleTouchEnabled = YES;
        self.contentMode = UIViewContentModeRedraw;
    }
    return self;
}

- (void)layoutSubviews
{
    UIEdgeInsets in = self.safeAreaInsets;
    CGFloat W = self.bounds.size.width, H = self.bounds.size.height;
    BOOL ipad = UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad;
    CGFloat r = ipad ? 36 : 29, small = r * 0.78, gap = r * 1.78;
    CGFloat cx = W - in.right - 22 - gap - r, cy = H - fmax(in.bottom, 10) - 12 - gap - r;
    int i;
    [super layoutSubviews];
    _c[PB_A] = CGPointMake(cx, cy + gap);
    _c[PB_B] = CGPointMake(cx + gap, cy);
    _c[PB_X] = CGPointMake(cx - gap, cy);
    _c[PB_Y] = CGPointMake(cx, cy - gap);
    for (i = PB_A; i <= PB_Y; i++) _r[i] = r;
    _c[PB_R] = CGPointMake(cx - gap * 1.25, cy - gap * 2.15);     _r[PB_R] = r;
    _c[PB_BLACK] = CGPointMake(cx + gap * 0.55, cy - gap * 2.3);  _r[PB_BLACK] = small;
    _reach = ipad ? 72 : 58;
    _home = CGPointMake(in.left + 26 + _reach, H - fmax(in.bottom, 10) - 16 - _reach);
    _c[PB_L] = CGPointMake(_home.x + _reach * 0.15, _home.y - _reach - gap * 1.25);  _r[PB_L] = r;
    _c[PB_WHITE] = CGPointMake(_c[PB_L].x + gap * 1.5, _c[PB_L].y - gap * 0.15);    _r[PB_WHITE] = small;
    [self setNeedsDisplay];
}

- (void)reset
{
    int i;
    for (i = 0; i < PB_TOUCHES; i++) _touch[i] = NULL;
    _stick = NULL;
    [self publish];
}

static void draw_circle(CGPoint c, CGFloat r, CGFloat fill, CGFloat line)
{
    UIBezierPath *p = [UIBezierPath bezierPathWithOvalInRect:CGRectMake(c.x - r, c.y - r, 2 * r, 2 * r)];
    [[UIColor colorWithWhite:1 alpha:fill] setFill];
    [p fill];
    [[UIColor colorWithWhite:1 alpha:line] setStroke];
    p.lineWidth = 2;
    [p stroke];
}

- (void)drawRect:(CGRect)rect
{
    int i;
    (void)rect;
    for (i = 0; i < PB_COUNT; i++) {
        NSDictionary *at = @{ NSFontAttributeName : [UIFont systemFontOfSize:_r[i] * 0.78 weight:UIFontWeightBold],
                              NSForegroundColorAttributeName : [UIColor colorWithWhite:1 alpha:0.92] };
        CGSize sz = [k_pb_label[i] sizeWithAttributes:at];
        draw_circle(_c[i], _r[i], _held[i] ? 0.42 : 0.16, 0.55);
        [k_pb_label[i] drawAtPoint:CGPointMake(_c[i].x - sz.width / 2, _c[i].y - sz.height / 2) withAttributes:at];
    }
    {
        CGPoint c = _stick ? _centre : _home, k = c;
        if (_stick) {
            CGFloat dx = _at.x - c.x, dy = _at.y - c.y, d = sqrt(dx * dx + dy * dy);
            if (d > _reach) { dx *= _reach / d; dy *= _reach / d; }
            k = CGPointMake(c.x + dx, c.y + dy);
        }
        draw_circle(c, _reach, 0.08, _stick ? 0.45 : 0.3);
        draw_circle(k, _reach * 0.42, _stick ? 0.45 : 0.25, 0.6);
    }
}

- (int)buttonAt:(CGPoint)p
{
    int i, best = -1;
    CGFloat bd = 1e9;
    for (i = 0; i < PB_COUNT; i++) {
        CGFloat dx = p.x - _c[i].x, dy = p.y - _c[i].y, d = sqrt(dx * dx + dy * dy);
        if (d < _r[i] * 1.3 && d < bd) { bd = d; best = i; }
    }
    return best;
}

/* What is held, out to the pad (xinput_device.c reads it through
 * nv_window_keys and nv_window_touch_stick). */
- (void)publish
{
    int i, held[PB_COUNT] = { 0 };
    unsigned short dpad = 0;
    for (i = 0; i < PB_TOUCHES; i++)
        if (_touch[i] && _on[i] >= 0) held[_on[i]] = 1;
    for (i = 0; i < PB_COUNT; i++) {
        _held[i] = held[i];
        g_touch_analog[k_pb_analog[i]] = held[i] ? 255 : 0;
    }
    if (_stick) {
        CGFloat dx = (_at.x - _centre.x) / _reach, dy = (_at.y - _centre.y) / _reach;
        CGFloat d = sqrt(dx * dx + dy * dy);
        if (d > 1) { dx /= d; dy /= d; }
        g_stick_x = (short)(dx * 32767);
        g_stick_y = (short)(-dy * 32767);           /* the pad's y is up */
        if (dx < -0.6) dpad |= KB_LEFT;
        if (dx >  0.6) dpad |= KB_RIGHT;
        if (dy < -0.6) dpad |= KB_UP;
        if (dy >  0.6) dpad |= KB_DOWN;
        g_stick_on = 1;
    } else {
        g_stick_on = 0;
        g_stick_x = g_stick_y = 0;
    }
    g_stick_dpad = dpad;
    [self setNeedsDisplay];
}

- (int)slotOf:(UITouch *)t
{
    int i;
    for (i = 0; i < PB_TOUCHES; i++) if (_touch[i] == (__bridge const void *)t) return i;
    return -1;
}

- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{
    (void)event;
    for (UITouch *t in touches) {
        CGPoint p = [t locationInView:self];
        int k = [self buttonAt:p], i;
        for (i = 0; i < PB_TOUCHES && _touch[i]; i++) { }
        if (i == PB_TOUCHES) break;
        if (k >= 0) {
            _touch[i] = (__bridge const void *)t; _on[i] = k;
        } else if (!_stick && p.x < self.bounds.size.width * 0.5) {
            /* The stick, centred where the thumb landed (kept on screen). */
            CGFloat lo = self.safeAreaInsets.left + _reach * 0.6;
            _touch[i] = (__bridge const void *)t; _on[i] = -1;
            _stick = (__bridge const void *)t;
            _centre = CGPointMake(fmax(p.x, lo), fmin(p.y, self.bounds.size.height - _reach * 0.6));
            _at = p;
        }
    }
    [self publish];
}

- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{
    (void)event;
    for (UITouch *t in touches) {
        int i = [self slotOf:t], k;
        CGPoint p = [t locationInView:self];
        if (i < 0) continue;
        if (_on[i] < 0) { _at = p; continue; }
        k = [self buttonAt:p];
        if (k >= 0) _on[i] = k;                        /* slid onto another button */
    }
    [self publish];
}

- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{
    (void)event;
    for (UITouch *t in touches) {
        int i = [self slotOf:t];
        if (i < 0) continue;
        if (_touch[i] == _stick) _stick = NULL;
        _touch[i] = NULL;
    }
    [self publish];
}

- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{
    [self touchesEnded:touches withEvent:event];
}
@end

@implementation NvGameController

- (void)loadView { self.view = [[NvMetalView alloc] initWithFrame:[UIScreen mainScreen].bounds]; }

- (BOOL)prefersStatusBarHidden { return YES; }
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }
- (UIRectEdge)preferredScreenEdgesDeferringSystemGestures { return UIRectEdgeAll; }
- (UIInterfaceOrientationMask)supportedInterfaceOrientations { return UIInterfaceOrientationMaskLandscape; }
- (BOOL)canBecomeFirstResponder { return YES; }

- (void)viewDidLoad
{
    UILayoutGuide *safe = self.view.safeAreaLayoutGuide;
    [super viewDidLoad];
    g_vc = self;

    self.touchPad = [[NvTouchPad alloc] initWithFrame:self.view.bounds];
    self.touchPad.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    self.touchPad.hidden = YES;
    [self.view addSubview:self.touchPad];

    self.gear = pill(@"⚙︎", 20);
    [self.gear addTarget:self action:@selector(showSettings) forControlEvents:UIControlEventTouchUpInside];
    self.backButton = pill(@"BACK", 13);
    self.startButton = pill(@"START", 13);
    [self.backButton addTarget:self action:@selector(backDownAction) forControlEvents:UIControlEventTouchDown];
    [self.backButton addTarget:self action:@selector(backUpAction)
              forControlEvents:UIControlEventTouchUpInside | UIControlEventTouchUpOutside | UIControlEventTouchCancel];
    [self.startButton addTarget:self action:@selector(startDownAction) forControlEvents:UIControlEventTouchDown];
    [self.startButton addTarget:self action:@selector(startUpAction)
               forControlEvents:UIControlEventTouchUpInside | UIControlEventTouchUpOutside | UIControlEventTouchCancel];
    for (UIButton *b in @[ self.gear, self.backButton, self.startButton ]) {
        b.hidden = YES;
        [self.view addSubview:b];
    }
    [NSLayoutConstraint activateConstraints:@[
        [self.gear.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:8],
        [self.gear.topAnchor constraintEqualToAnchor:safe.topAnchor constant:8],
        [self.startButton.leadingAnchor constraintEqualToAnchor:self.view.centerXAnchor constant:8],
        [self.startButton.topAnchor constraintEqualToAnchor:safe.topAnchor constant:8],
        [self.backButton.trailingAnchor constraintEqualToAnchor:self.view.centerXAnchor constant:-8],
        [self.backButton.topAnchor constraintEqualToAnchor:safe.topAnchor constant:8],
    ]];
    [self buildSetup];

    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(updateControls)
                                                 name:GCControllerDidConnectNotification object:nil];
    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(updateControls)
                                                 name:GCControllerDidDisconnectNotification object:nil];
}

- (void)viewDidAppear:(BOOL)animated
{
    [super viewDidAppear:animated];
    [self becomeFirstResponder];
    [self setNeedsUpdateOfScreenEdgesDeferringSystemGestures];
    [self setNeedsUpdateOfHomeIndicatorAutoHidden];
    [self tryStart];
}

/* ---- starting ---- */

- (void)tryStart
{
    NSString *game;
    if (self.started || self.unpacking) return;
    game = find_game();
    if (!game) { [self showSetup]; return; }
    {
        /* Another build of the game (European, Japanese, a Sega GT 2002 combo
         * disc) starts and then crashes a few seconds in: the engine is the
         * standalone US disc's code. */
        char why[512];
        if (jsrf_xbe_check_folder(game.fileSystemRepresentation, why, sizeof why) == JSRF_XBE_OTHER) {
            fprintf(stderr, "[IOS] %s\n", why);
            [self showSetup];
            [self alert:@JSRF_XBE_HEADING
                   text:[NSString stringWithFormat:@"%s\n\n%s\n\nThe files are in Files: On My "
                         "iPhone > JSRF Unleashed: replace them with a standalone US disc's.", why,
                         JSRF_XBE_OTHER_ADVICE]];
            return;
        }
    }
    self.setup.hidden = YES;
    g_open = 1;                       /* the renderer may present from now on */
    if (!start_engine(game)) {
        g_open = 0;
        [self alert:@"The game could not start" text:@"No thread for the engine (out of memory?)."];
        return;
    }
    self.started = YES;
    [UIApplication sharedApplication].idleTimerDisabled = YES;   /* no dimming mid-game */
    self.gear.hidden = NO;
    [self updateControls];
}

- (void)engineStopped:(int)rc
{
    [UIApplication sharedApplication].idleTimerDisabled = NO;
    if (rc == 3) {                    /* the engine's own check: not the standalone US disc's files */
        [self alert:@JSRF_XBE_HEADING
               text:[NSString stringWithFormat:@"%s\n\nIts log says what the files are: "
                     "Files: On My iPhone > JSRF Unleashed > Logs > last-run.log.", JSRF_XBE_OTHER_ADVICE]];
        return;
    }
    [self alert:@"The game stopped"
           text:[NSString stringWithFormat:@"The engine could not run the game (code %d). Its log "
                 "is in Files: On My iPhone > JSRF Unleashed > Logs > last-run.log.%@", rc,
                 rc == 1 ? @"\n\nThe usual reasons: the files are not the standalone US disc's, or iOS "
                           "refused the 4 GB of address space the game's memory map needs." : @""]];
}

/* ---- "add your game" ---- */

- (void)buildSetup
{
    UIView *v = [[UIView alloc] init];
    UIStackView *st;
    UILabel *title = [[UILabel alloc] init];
    v.backgroundColor = [UIColor colorWithWhite:0.06 alpha:1];
    v.translatesAutoresizingMaskIntoConstraints = NO;
    v.hidden = YES;
    title.text = @"JSRF Unleashed";
    title.font = [UIFont systemFontOfSize:28 weight:UIFontWeightHeavy];
    title.textColor = [UIColor colorWithRed:0.62 green:0.86 blue:0.16 alpha:1];
    self.setupText = [[UILabel alloc] init];
    self.setupText.numberOfLines = 0;
    self.setupText.textColor = [UIColor colorWithWhite:0.92 alpha:1];
    self.setupText.font = [UIFont systemFontOfSize:15];
    self.setupText.adjustsFontSizeToFitWidth = YES;
    self.setupText.minimumScaleFactor = 0.6;
    self.checkButton = big_button(@"Check again");
    [self.checkButton addTarget:self action:@selector(tryStart) forControlEvents:UIControlEventTouchUpInside];
    self.unpackButton = big_button(@"Unpack");
    [self.unpackButton addTarget:self action:@selector(unpack) forControlEvents:UIControlEventTouchUpInside];
    self.progress = [[UIProgressView alloc] initWithProgressViewStyle:UIProgressViewStyleDefault];
    self.progress.progressTintColor = [UIColor colorWithRed:0.62 green:0.86 blue:0.16 alpha:1];
    self.progress.hidden = YES;
    {
        UIStackView *buttons = [[UIStackView alloc] initWithArrangedSubviews:@[ self.unpackButton, self.checkButton ]];
        buttons.axis = UILayoutConstraintAxisHorizontal;
        buttons.spacing = 16;
        st = [[UIStackView alloc] initWithArrangedSubviews:@[ title, self.setupText, self.progress, buttons ]];
    }
    st.axis = UILayoutConstraintAxisVertical;
    st.alignment = UIStackViewAlignmentLeading;
    st.spacing = 14;
    st.translatesAutoresizingMaskIntoConstraints = NO;
    [v addSubview:st];
    [self.view addSubview:v];
    [NSLayoutConstraint activateConstraints:@[
        [v.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
        [v.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
        [v.topAnchor constraintEqualToAnchor:self.view.topAnchor],
        [v.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor],
        [st.centerXAnchor constraintEqualToAnchor:v.centerXAnchor],
        [st.centerYAnchor constraintEqualToAnchor:v.centerYAnchor],
        [st.widthAnchor constraintLessThanOrEqualToConstant:620],
        [st.leadingAnchor constraintGreaterThanOrEqualToAnchor:v.safeAreaLayoutGuide.leadingAnchor constant:24],
        [st.topAnchor constraintGreaterThanOrEqualToAnchor:v.safeAreaLayoutGuide.topAnchor constant:12],
        [self.progress.widthAnchor constraintEqualToAnchor:st.widthAnchor],
    ]];
    self.setup = v;
}

- (void)showSetup
{
    NSString *device = UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad ? @"iPad" : @"iPhone";
    self.iso = find_iso();
    self.setup.hidden = NO;
    self.unpackButton.hidden = self.iso == nil;
    if (self.iso) {
        [self.unpackButton setTitle:[NSString stringWithFormat:@"Unpack %@", self.iso.lastPathComponent]
                           forState:UIControlStateNormal];
        self.setupText.text = [NSString stringWithFormat:
            @"Found a disc image: %@.\n\nUnpacking it takes a minute or two and needs about %.1f GB "
             "free (%.1f GB is). It happens once; afterwards you can delete the image.",
            self.iso.lastPathComponent,
            [[[NSFileManager defaultManager] attributesOfItemAtPath:self.iso error:nil] fileSize] / 1e9,
            free_bytes(docs_dir()) / 1e9];
    } else {
        self.setupText.text = [NSString stringWithFormat:
            @"This plays your own copy of Jet Set Radio Future (USA). Nothing of the game comes with "
             "the app.\n\n"
             "1. Get the game's files: the folder with default.xbe in it, extracted from your disc, "
             "or the disc image (.iso).\n"
             "2. Put it in this app's folder: in the Files app, On My %@ > JSRF Unleashed. From a Mac: "
             "Finder > your %@ > Files > JSRF Unleashed. From Windows: the Apple Devices app.\n"
             "3. Come back here.\n\n"
             "Saves are kept inside the game's folder (in UDATA), and each run's log in Logs.",
            device, device];
    }
}

static int unpack_progress(void *ctx, uint64_t done, uint64_t total)
{
    NvGameController *vc = (__bridge NvGameController *)ctx;
    float f = total ? (float)((double)done / (double)total) : 0.0f;
    dispatch_async(dispatch_get_main_queue(), ^{ [vc.progress setProgress:f animated:NO]; });
    return 0;
}

- (void)unpack
{
    NSString *iso = self.iso, *docs = docs_dir();
    NSString *name = iso.lastPathComponent.stringByDeletingPathExtension;
    NSString *tmp, *dest;
    if (!iso) return;
    if ([name.pathExtension.lowercaseString isEqualToString:@"xiso"]) name = name.stringByDeletingPathExtension;
    if (!name.length) name = @"Game";
    dest = [docs stringByAppendingPathComponent:name];
    for (int i = 2; [[NSFileManager defaultManager] fileExistsAtPath:dest]; i++)
        dest = [docs stringByAppendingPathComponent:[NSString stringWithFormat:@"%@ %d", name, i]];
    tmp = [dest stringByAppendingString:@".unpacking"];
    [[NSFileManager defaultManager] removeItemAtPath:tmp error:nil];

    self.unpacking = YES;
    self.unpackButton.enabled = self.checkButton.enabled = NO;
    self.progress.hidden = NO;
    self.progress.progress = 0;
    self.setupText.text = [NSString stringWithFormat:@"Unpacking %@ into the folder \"%@\"... "
                           "Keep the app open until it is done.", iso.lastPathComponent, dest.lastPathComponent];
    [UIApplication sharedApplication].idleTimerDisabled = YES;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        char err[1200], what[640];
        int rc, wrong;
        /* What the file is, before minutes and gigabytes go into it: another
         * game's disc, another build of the game (port/xbe_check.c). */
        int r = jsrf_xbe_check_iso(iso.fileSystemRepresentation, what, sizeof what);
        wrong = r != JSRF_XBE_OK;
        if (wrong) {
            rc = -1;
            snprintf(err, sizeof err, "%s%s%s", what, r == JSRF_XBE_OTHER ? "\n\n" : "",
                     r == JSRF_XBE_OTHER ? JSRF_XBE_OTHER_ADVICE : "");
        } else {
            rc = xiso_extract(iso.fileSystemRepresentation, tmp.fileSystemRepresentation,
                              unpack_progress, (__bridge void *)self, err, sizeof err);
        }
        NSError *e = nil;
        if (!rc && !has_xbe(tmp)) { rc = -1; snprintf(err, sizeof err, "The image unpacked, but it "
                                    "has no default.xbe: it is not a game disc."); }
        if (!rc && ![[NSFileManager defaultManager] moveItemAtPath:tmp toPath:dest error:&e]) {
            rc = -1; snprintf(err, sizeof err, "%s", e.localizedDescription.UTF8String ?: "rename failed");
        }
        if (rc) [[NSFileManager defaultManager] removeItemAtPath:tmp error:nil];
        fprintf(stderr, "[IOS] unpack %s: %s\n", iso.lastPathComponent.UTF8String, rc ? err : "done");
        fflush(stderr);
        NSString *why = rc ? @(err) : nil;      /* a block cannot take the array */
        dispatch_async(dispatch_get_main_queue(), ^{
            self.unpacking = NO;
            self.unpackButton.enabled = self.checkButton.enabled = YES;
            self.progress.hidden = YES;
            [UIApplication sharedApplication].idleTimerDisabled = NO;
            if (why) {
                [self alert:wrong ? @JSRF_XBE_HEADING : @"Could not unpack the image" text:why];
                [self showSetup];
                return;
            }
            [self offerToDelete:iso];
        });
    });
}

- (void)offerToDelete:(NSString *)iso
{
    UIAlertController *a = [UIAlertController alertControllerWithTitle:@"Unpacked"
        message:[NSString stringWithFormat:@"The game runs from the unpacked folder now. Delete %@ "
                 "to free its space?", iso.lastPathComponent]
        preferredStyle:UIAlertControllerStyleAlert];
    [a addAction:[UIAlertAction actionWithTitle:@"Delete the Image" style:UIAlertActionStyleDestructive
        handler:^(UIAlertAction *x) { (void)x;
            [[NSFileManager defaultManager] removeItemAtPath:iso error:nil];
            [self tryStart]; }]];
    [a addAction:[UIAlertAction actionWithTitle:@"Keep It" style:UIAlertActionStyleCancel
        handler:^(UIAlertAction *x) { (void)x; [self tryStart]; }]];
    [self presentViewController:a animated:YES completion:nil];
}

- (void)alert:(NSString *)title text:(NSString *)text
{
    UIAlertController *a = [UIAlertController alertControllerWithTitle:title message:text
                                                        preferredStyle:UIAlertControllerStyleAlert];
    [a addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
    if (self.presentedViewController) [self dismissViewControllerAnimated:NO completion:nil];
    [self presentViewController:a animated:YES completion:nil];
}

/* ---- touch controls ---- */

/* Start and Back are held at least 0.1 s: a quick tap can come and go
 * between two reads of the pad. */
static void pill_release(unsigned short bit, CFTimeInterval down)
{
    CFTimeInterval held = CACurrentMediaTime() - down;
    if (held >= 0.1) { g_pill_buttons &= (unsigned short)~bit; return; }
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)((0.1 - held) * 1e9)),
                   dispatch_get_main_queue(), ^{ g_pill_buttons &= (unsigned short)~bit; });
}
- (void)startDownAction { self.startDown = CACurrentMediaTime(); g_pill_buttons |= KB_START; }
- (void)startUpAction   { pill_release(KB_START, self.startDown); }
- (void)backDownAction  { self.backDown = CACurrentMediaTime(); g_pill_buttons |= KB_BACK; }
- (void)backUpAction    { pill_release(KB_BACK, self.backDown); }

static BOOL touch_pref(void)
{
    NSUserDefaults *d = [NSUserDefaults standardUserDefaults];
    return [d objectForKey:@"touchControls"] ? [d boolForKey:@"touchControls"] : YES;
}

/* The on-screen controls while no controller is connected (and the player
 * has not turned them off). */
- (void)updateControls
{
    BOOL physical = NO, want;
    for (GCController *c in [GCController controllers])
        if (c.extendedGamepad) physical = YES;
    want = self.started && !physical && touch_pref() && !getenv("JSRF_IOS_NO_TOUCH");
    if (!want) [(NvTouchPad *)self.touchPad reset];
    self.touchPad.hidden = !want;
    g_touch_on = want;
    self.startButton.hidden = self.backButton.hidden = !want;
    fprintf(stderr, "[IOS] controls: %s\n", physical ? "a controller" : want ? "on screen" : "none shown");
    fflush(stderr);
}

/* ---- settings ---- */

- (void)showSettings
{
    NSUserDefaults *d = [NSUserDefaults standardUserDefaults];
    NSString *ver = [NSBundle mainBundle].infoDictionary[@"CFBundleShortVersionString"] ?: @"";
    UIAlertController *a = [UIAlertController alertControllerWithTitle:
                               [NSString stringWithFormat:@"JSRF Unleashed %@", ver]
                               message:nil preferredStyle:UIAlertControllerStyleAlert];
    [self releaseKeys];
    [a addAction:[UIAlertAction actionWithTitle:[NSString stringWithFormat:@"Picture: %@",
                                                 k_picture[picture_mode()]]
        style:UIAlertActionStyleDefault handler:^(UIAlertAction *x) { (void)x; [self pickPicture]; }]];
    [a addAction:[UIAlertAction actionWithTitle:touch_pref() ? @"On-screen controls: shown" : @"On-screen controls: hidden"
        style:UIAlertActionStyleDefault handler:^(UIAlertAction *x) { (void)x;
            [d setBool:!touch_pref() forKey:@"touchControls"];
            [self updateControls]; }]];
    [a addAction:[UIAlertAction actionWithTitle:[NSString stringWithFormat:@"Resolution: %dx", nv_window_pref_scale()]
        style:UIAlertActionStyleDefault handler:^(UIAlertAction *x) { (void)x; [self pickScale]; }]];
    [a addAction:[UIAlertAction actionWithTitle:[NSString stringWithFormat:@"Anti-aliasing: %@",
                                                 [self aaName]]
        style:UIAlertActionStyleDefault handler:^(UIAlertAction *x) { (void)x; [self pickAA]; }]];
    [a addAction:[UIAlertAction actionWithTitle:[NSString stringWithFormat:@"Camera on the right stick: %@",
                                                 k_rcam[nv_window_pref_rstick_camera()]]
        style:UIAlertActionStyleDefault handler:^(UIAlertAction *x) { (void)x; [self pickRstickCamera]; }]];
    [a addAction:[UIAlertAction actionWithTitle:@"Photo mode" style:UIAlertActionStyleDefault
        handler:^(UIAlertAction *x) { (void)x; nv_window_request_photo(); }]];
    [a addAction:[UIAlertAction actionWithTitle:@"Controls" style:UIAlertActionStyleDefault
        handler:^(UIAlertAction *x) { (void)x;
            [self alert:@"Controls" text:
             @"A controller works as on the console (Xbox, PlayStation, MFi): A jump, R trigger "
              "talk and spray, Menu/Options Start, View/Create Back, L1 White, R1 Black.\n\n"
              "On screen: put your left thumb anywhere on the left half to steer; A B X Y are the pad's, "
              "R and L its triggers, the hollow and filled circles White and Black; Start and Back are "
              "at the top.\n\n"
              "With the camera on the right stick (the settings), a controller's right stick turns the "
              "camera around you and it eases back behind you when let go; L trigger puts it straight "
              "behind at once. Set to tracking, it only looks around: you keep going the way you "
              "were.\n\n"
              "Photo mode (the settings, or hold Back with a controller): move the camera freely with the sticks, the triggers lower and raise it, Y hides the HUD, A the help, B goes back to the game. Take the picture with the side and volume-up buttons.\n\n"
              "A keyboard: WASD (by position) moves, arrows the camera, Space jump, E spray, "
              "Return Start, Esc Back."]; }]];
    [a addAction:[UIAlertAction actionWithTitle:@"Close" style:UIAlertActionStyleCancel handler:nil]];
    [self presentViewController:a animated:YES completion:nil];
}

/* The picture's shape: the console's 4:3, 16:9, or the whole screen. */
- (void)pickPicture
{
    UIAlertController *a = [UIAlertController alertControllerWithTitle:@"Picture"
        message:@"Wider shows more of the city to the sides; menus and the HUD keep their shape."
        preferredStyle:UIAlertControllerStyleAlert];
    int cur = picture_mode();
    for (int m = 0; m < 3; m++)
        [a addAction:[UIAlertAction actionWithTitle:[NSString stringWithFormat:@"%@%@", k_picture[m],
                                                     m == cur ? @"  ✓" : @""]
            style:UIAlertActionStyleDefault handler:^(UIAlertAction *x) { (void)x;
                [[NSUserDefaults standardUserDefaults] setInteger:m forKey:@"picture"];
                nv2a_set_widescreen(m != 0);
                apply_wide_shape(self.view.bounds.size);
                fprintf(stderr, "[IOS] picture: %s\n", m == 0 ? "4:3" : m == 1 ? "16:9" : "fill the screen");
                fflush(stderr); }]];
    [a addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [self presentViewController:a animated:YES completion:nil];
}

/* The camera on the right stick: on (the skater goes where it looks),
 * tracking (it only looks around, the skater keeps his heading), or off (the
 * game's own camera). */
- (void)pickRstickCamera
{
    UIAlertController *a = [UIAlertController alertControllerWithTitle:@"Camera on the right stick"
        message:@"On: the skater goes where the camera looks. Tracking: the camera only looks around, "
                 "the skater keeps going the way he was."
        preferredStyle:UIAlertControllerStyleAlert];
    int cur = nv_window_pref_rstick_camera();
    static const int order[3] = { 1, 2, 0 };
    for (int i = 0; i < 3; i++) {
        int m = order[i];
        [a addAction:[UIAlertAction actionWithTitle:[NSString stringWithFormat:@"%@%@",
                                                     m == 0 ? @"Off (the game's camera)" : m == 1 ? @"On" : @"Tracking",
                                                     m == cur ? @"  ✓" : @""]
            style:UIAlertActionStyleDefault handler:^(UIAlertAction *x) { (void)x;
                NSUserDefaults *d = [NSUserDefaults standardUserDefaults];
                g_rstick_cam = m;
                [d setBool:(m ? YES : NO) forKey:@"rstickCamera"];
                [d setBool:(m == 2 ? YES : NO) forKey:@"rstickTracking"];
                fprintf(stderr, "[IOS] the camera on the right stick: %s\n", m == 2 ? "tracking" : m ? "on" : "off");
                fflush(stderr); }]];
    }
    [a addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [self presentViewController:a animated:YES completion:nil];
}

/* What the anti-aliasing is set to, in a few words. */
- (NSString *)aaName
{
    int sm = nv2a_smaa(), ms = msaa_pref();
    if (!sm && !ms) return @"off";
    if (sm && ms) return [NSString stringWithFormat:@"SMAA, MSAA %dx", ms];
    return sm ? @"SMAA" : [NSString stringWithFormat:@"MSAA %dx", ms];
}

/* SMAA, on or off at once; or multisampling, from the next start. */
- (void)pickAA
{
    UIAlertController *a = [UIAlertController alertControllerWithTitle:@"Anti-aliasing"
        message:@"SMAA smooths jagged edges in each finished frame, at a small cost. MSAA draws "
                 "every edge with more samples a pixel: cleaner, and more work for the phone; "
                 "a new MSAA is used the next time the app starts. The two can be on together."
        preferredStyle:UIAlertControllerStyleAlert];
    int sm = nv2a_smaa(), ms = msaa_pref(), max = nv2a_msaa_max();
    [a addAction:[UIAlertAction actionWithTitle:sm ? @"SMAA: on  ✓" : @"SMAA: off"
        style:UIAlertActionStyleDefault handler:^(UIAlertAction *x) { (void)x;
            nv2a_set_smaa(!sm);
            [[NSUserDefaults standardUserDefaults] setBool:(!sm ? YES : NO) forKey:@"smaa"];
            fprintf(stderr, "[IOS] SMAA %s\n", !sm ? "on" : "off");
            fflush(stderr); }]];
    for (int n = 1; n <= 4; n *= 2) {
        if (max && n > max) break;
        [a addAction:[UIAlertAction actionWithTitle:[NSString stringWithFormat:@"%@%@",
                                                     n > 1 ? [NSString stringWithFormat:@"MSAA %dx", n]
                                                           : @"MSAA off",
                                                     (n > 1 ? n : 0) == ms ? @"  ✓" : @""]
            style:UIAlertActionStyleDefault handler:^(UIAlertAction *x) { (void)x;
                [[NSUserDefaults standardUserDefaults] setInteger:(n > 1 ? n : 0) forKey:@"msaa"];
                fprintf(stderr, "[IOS] MSAA %dx from the next start\n", n);
                fflush(stderr); }]];
    }
    [a addAction:[UIAlertAction actionWithTitle:@"Done" style:UIAlertActionStyleCancel handler:nil]];
    [self presentViewController:a animated:YES completion:nil];
}

- (void)pickScale
{
    UIAlertController *a = [UIAlertController alertControllerWithTitle:@"Resolution"
        message:@"How sharp the picture is. Used the next time the app starts."
        preferredStyle:UIAlertControllerStyleAlert];
    for (int s = 1; s <= 4; s++)
        [a addAction:[UIAlertAction actionWithTitle:[NSString stringWithFormat:@"%dx (%d x %d)%@", s,
                                                     640 * s, 480 * s, s == nv_window_pref_scale() ? @"  ✓" : @""]
            style:UIAlertActionStyleDefault handler:^(UIAlertAction *x) { (void)x;
                [[NSUserDefaults standardUserDefaults] setInteger:s forKey:@"glScale"]; }]];
    [a addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [self presentViewController:a animated:YES completion:nil];
}

/* ---- a hardware keyboard, by key position (HID usages), as on the Mac ---- */

static void key_apply(long usage, int down)
{
    unsigned short bit = 0;
    int analog = -1;
    switch (usage) {
    case 0x1A: bit = KB_UP; break;          /* W */
    case 0x16: bit = KB_DOWN; break;        /* S */
    case 0x04: bit = KB_LEFT; break;        /* A */
    case 0x07: bit = KB_RIGHT; break;       /* D */
    case 0x52: bit = KB_R_UP; break;        /* arrows: the camera */
    case 0x51: bit = KB_R_DOWN; break;
    case 0x50: bit = KB_R_LEFT; break;
    case 0x4F: bit = KB_R_RIGHT; break;
    case 0x28: case 0x58: bit = KB_START; break;   /* Return, keypad Enter */
    case 0x29: bit = KB_BACK; break;        /* Escape */
    case 0x06: bit = KB_LTHUMB; break;      /* C */
    case 0x19: bit = KB_RTHUMB; break;      /* V */
    case 0x2C: case 0x0D: analog = 0; break;    /* Space, J: A */
    case 0x0E: analog = 1; break;           /* K: B */
    case 0x0B: analog = 2; break;           /* H: X */
    case 0x0F: analog = 3; break;           /* L: Y */
    case 0x15: analog = 4; break;           /* R: Black */
    case 0x09: analog = 5; break;           /* F: White */
    case 0x14: analog = 6; break;           /* Q: L trigger */
    case 0x08: analog = 7; break;           /* E: R trigger */
    default: return;
    }
    if (bit) {
        if (down) g_kb_buttons |= bit;
        else      g_kb_buttons &= (unsigned short)~bit;
    }
    if (analog >= 0) g_kb_analog[analog] = down ? 255 : 0;
}

- (BOOL)pressesHandled:(NSSet<UIPress *> *)presses down:(int)down
{
    BOOL any = NO;
    if (@available(iOS 13.4, *)) {
        for (UIPress *p in presses) {
            if (!p.key || (p.key.modifierFlags & UIKeyModifierCommand)) continue;
            key_apply((long)p.key.keyCode, down);
            any = YES;
        }
    }
    return any;
}
- (void)pressesBegan:(NSSet<UIPress *> *)presses withEvent:(UIPressesEvent *)event
{ if (![self pressesHandled:presses down:1]) [super pressesBegan:presses withEvent:event]; }
- (void)pressesEnded:(NSSet<UIPress *> *)presses withEvent:(UIPressesEvent *)event
{ if (![self pressesHandled:presses down:0]) [super pressesEnded:presses withEvent:event]; }
- (void)pressesCancelled:(NSSet<UIPress *> *)presses withEvent:(UIPressesEvent *)event
{ if (![self pressesHandled:presses down:0]) [super pressesCancelled:presses withEvent:event]; }

- (void)releaseKeys
{
    int i;
    g_kb_buttons = 0;
    g_pill_buttons = 0;
    for (i = 0; i < 8; i++) g_kb_analog[i] = 0;
}
@end

/* ---- the scene and the app ---------------------------------------------- */

@interface NvSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property (nonatomic, strong) UIWindow *window;
@end

@implementation NvSceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session
      options:(UISceneConnectionOptions *)options
{
    (void)session; (void)options;
    if (![scene isKindOfClass:[UIWindowScene class]]) return;
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    self.window.rootViewController = [[NvGameController alloc] init];
    self.window.backgroundColor = [UIColor blackColor];
    [self.window makeKeyAndVisible];
}
- (void)sceneDidBecomeActive:(UIScene *)scene
{
    (void)scene;
    set_visible(1);
    [[AVAudioSession sharedInstance] setActive:YES error:nil];
    xa2_resume();
    [g_vc tryStart];          /* back from the Files app with the game copied in */
}
- (void)sceneWillResignActive:(UIScene *)scene { (void)scene; [g_vc releaseKeys]; }
- (void)sceneDidEnterBackground:(UIScene *)scene { (void)scene; set_visible(0); }
- (void)sceneWillEnterForeground:(UIScene *)scene { (void)scene; set_visible(1); }
@end

@interface NvAppDelegate : UIResponder <UIApplicationDelegate>
@end

@implementation NvAppDelegate
- (BOOL)application:(UIApplication *)app didFinishLaunchingWithOptions:(NSDictionary *)opts
{
    AVAudioSession *s = [AVAudioSession sharedInstance];
    (void)app; (void)opts;
    /* The game's own soundtrack: other audio stops, the silent switch
     * silences it, as for any game. */
    [s setCategory:AVAudioSessionCategorySoloAmbient error:nil];
    [s setActive:YES error:nil];
    [[NSNotificationCenter defaultCenter] addObserverForName:AVAudioSessionInterruptionNotification
        object:s queue:[NSOperationQueue mainQueue] usingBlock:^(NSNotification *n) {
            NSNumber *type = n.userInfo[AVAudioSessionInterruptionTypeKey];
            if (type.unsignedIntegerValue == AVAudioSessionInterruptionTypeEnded) {
                [[AVAudioSession sharedInstance] setActive:YES error:nil];
                xa2_resume();
            }
        }];
    return YES;
}
- (UISceneConfiguration *)application:(UIApplication *)app
        configurationForConnectingSceneSession:(UISceneSession *)session
        options:(UISceneConnectionOptions *)options
{
    UISceneConfiguration *c = [[UISceneConfiguration alloc] initWithName:@"Default"
                                                              sessionRole:session.role];
    (void)app; (void)options;
    c.delegateClass = [NvSceneDelegate class];
    return c;
}
@end

/* ---- main ---------------------------------------------------------------- */

/* What the Mac launcher sets (launcher/JSRFLauncher.swift, Game.environment),
 * plus the Metal renderer and the window the preferences belong to. Anything
 * already in the environment wins: that is how a test run (xcrun simctl
 * launch, SIMCTL_CHILD_*) steers it. */
static const char *const k_env[][2] = {
    { "RECOMP_USB", "1" }, { "RECOMP_GL", "1" }, { "RECOMP_PB_EXEC", "1" },
    { "RECOMP_VBLANK", "1" }, { "RECOMP_AC97_READY_NOTRAP", "1" },
    { "RECOMP_TRACE_BUDGET", "0" }, { "RECOMP_GL_READBACK", "0" }, { "RECOMP_FPS", "60" },
    { "RECOMP_ABI_RESTORE", "1" }, { "RECOMP_LOCK_IDLE", "1" },
    { "RECOMP_GL_CURTAIN", "skip" }, { "RECOMP_MUTE", "0" },
    { "RECOMP_WINDOW", "1" }, { "RECOMP_BACKEND", "metal" },
};

/* Set when the app is loaded, before the runtime's own constructors: some
 * of them read these -- RECOMP_ABI_RESTORE, the repair of the registers a
 * lifted function fails to preserve, is read by one -- and on the Mac the
 * launcher has set them before the engine starts. Set in main, as they were,
 * that one came too late and the iPhone ran without the repair. Darwin runs
 * initialisers in link order, and the app's own files are linked before the
 * runtime's libraries; nv_ios_main checks anyway. Anything already set (a
 * test run's SIMCTL_CHILD_*) wins. */
__attribute__((constructor(101))) static void ios_env(void)
{
    size_t i;
    for (i = 0; i < sizeof k_env / sizeof k_env[0]; i++)
        setenv(k_env[i][0], k_env[i][1], 0);
}
extern int g_recomp_abi_restore, g_recomp_abi_check;

/* Logs/last-run.log, the one before it kept as previous-run.log. Both
 * streams append: two writers each at their own offset would write over
 * each other. */
static void open_log(void)
{
    NSString *logs = [docs_dir() stringByAppendingPathComponent:@"Logs"];
    NSString *last = [logs stringByAppendingPathComponent:@"last-run.log"];
    NSString *prev = [logs stringByAppendingPathComponent:@"previous-run.log"];
    NSFileManager *fm = [NSFileManager defaultManager];
    FILE *f;
    if (getenv("JSRF_IOS_LOG_STDERR")) return;        /* tests: simctl --console */
    [fm createDirectoryAtPath:logs withIntermediateDirectories:YES attributes:nil error:nil];
    [fm removeItemAtPath:prev error:nil];
    [fm moveItemAtPath:last toPath:prev error:nil];
    f = fopen(last.fileSystemRepresentation, "w");
    if (f) fclose(f);
    freopen(last.fileSystemRepresentation, "a", stdout);
    freopen(last.fileSystemRepresentation, "a", stderr);
}

int nv_ios_main(int argc, char **argv, int (*host)(void));
int nv_ios_main(int argc, char **argv, int (*host)(void))
{
    size_t i;
    g_host_main = host;
    @autoreleasepool {
        open_log();
        for (i = 0; i < sizeof k_env / sizeof k_env[0]; i++)
            setenv(k_env[i][0], k_env[i][1], 0);
        /* In case a runtime constructor ran before ios_env after all: the
         * repair is only consulted once the game runs, which is later. */
        if (getenv("RECOMP_ABI_RESTORE") && !g_recomp_abi_restore) {
            g_recomp_abi_restore = 1;
            g_recomp_abi_check = 1;
            fprintf(stderr, "[IOS] ABI repair switched on in main (a constructor ran first)\n");
        }
        /* The renderer reads its resolution from the environment (main.c
         * does this on the Mac, before the window opens). */
        if (!getenv("RECOMP_GL_SCALE")) {
            char b[8];
            snprintf(b, sizeof b, "%d", nv_window_pref_scale());
            setenv("RECOMP_GL_SCALE", b, 1);
        }
        chdir(docs_dir().fileSystemRepresentation);
        fprintf(stderr, "[IOS] JSRF Unleashed %s, iOS %s, %s; ABI repair %s; picture %s\n",
                [[NSBundle mainBundle].infoDictionary[@"CFBundleShortVersionString"] UTF8String] ?: "?",
                UIDevice.currentDevice.systemVersion.UTF8String,
                UIDevice.currentDevice.model.UTF8String,
                g_recomp_abi_restore ? "on" : "OFF",
                k_picture[picture_mode()].UTF8String);
        fflush(stderr);
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([NvAppDelegate class]));
    }
}
