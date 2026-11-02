/*
 * jsrf_android.c -- the engine inside the Android app.
 *
 * SDL's Java side (launcher/android: JsrfActivity, an SDLActivity) loads
 * libmain.so and calls its SDL_main (main.c), which comes here with
 *   argv[1]  the game folder (default.xbe and Media/), in the app's storage
 *   argv[2]  the app's own folder (getExternalFilesDir): Logs/, the settings,
 *            save/ (the cache and the disk images; the game's saves are in the
 *            game folder, under UDATA, as on every platform)
 *   argv[3]  its cache folder, for temporary files
 *
 * What the Linux launcher and the iPhone app do before the engine starts is
 * done here: the environment (as the library is loaded), the log
 * (Logs/last-run.log, the one before kept as previous-run.log), the folders.
 * Then host_main runs on a thread with a 16 MB stack -- SDL's own is a Java
 * thread's.
 *
 * The setup screen (SetupActivity) unpacks a disc image through
 * nativeProbe / nativeUnpack below, over port/ios/xiso.c.
 */
#include <jni.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <android/log.h>
#include "xiso.h"
#include "../xbe_check.h"

#ifndef JSRF_VERSION
#define JSRF_VERSION "dev"
#endif

#define TAG "JSRF"

static void mkdir_p(const char *path)
{
    char buf[1024];
    char *p;
    snprintf(buf, sizeof buf, "%s", path);
    for (p = buf + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(buf, 0755); *p = '/'; }
    mkdir(buf, 0755);
}

/* Lines from the Java side (JsrfActivity: which input devices it counts as
 * controllers). They come before the log is open -- the activity starts
 * before the engine -- so they wait here until jlog_start prints them. */
static pthread_mutex_t g_jlog_mu = PTHREAD_MUTEX_INITIALIZER;
static char g_jlog[4096];
static int  g_log_open;

/* Logs/last-run.log, the one before it kept as previous-run.log. Both
 * streams append to the same file, unbuffered. */
static void open_log(const char *data)
{
    char logs[1024], last[1100], prev[1100];
    FILE *f;
    snprintf(logs, sizeof logs, "%s/Logs", data);
    mkdir_p(logs);
    snprintf(last, sizeof last, "%s/last-run.log", logs);
    snprintf(prev, sizeof prev, "%s/previous-run.log", logs);
    rename(last, prev);
    if ((f = fopen(last, "w")) != NULL) fclose(f);
    if (!freopen(last, "a", stdout) || !freopen(last, "a", stderr))
        __android_log_print(ANDROID_LOG_ERROR, TAG, "cannot open %s: %s", last, strerror(errno));
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
}

/* From here on the Java side's lines go straight into the log; the ones that
 * waited go in now -- after the version line, which stays the log's first. */
static void jlog_start(void)
{
    pthread_mutex_lock(&g_jlog_mu);
    g_log_open = 1;
    fputs(g_jlog, stderr);
    g_jlog[0] = 0;
    pthread_mutex_unlock(&g_jlog_mu);
}

/* key=value lines (the file the engine's F9/F10 keys write to as well). */
static int cfg_get_int(const char *file, const char *key, int def)
{
    char line[512];
    size_t kl = strlen(key);
    int v = def;
    FILE *f = fopen(file, "r");
    if (!f) return def;
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, key, kl) && line[kl] == '=') v = atoi(line + kl + 1);
    fclose(f);
    return v;
}

/* The same settings as the Linux and Windows launchers'; full screen always.
 * Then SDL's: Back is the game's (the Xbox Back button, nv2a_gl.c) rather than
 * a way out of it, the phone's motion sensor is not a controller, and neither
 * is a device without a stick or a d-pad. Phones list some of their own parts
 * as gamepads -- the fingerprint reader's or the side buttons' device declares
 * gamepad buttons -- and SDL took one for a controller: it was opened first,
 * into player 1's place, and a real controller paired after it went into
 * player 2's, which the game does not read. SDL_TV_REMOTE_AS_JOYSTICK=0 is
 * SDL's own rule for this (fewer than two axes and no hat: not a joystick). */
static const char *const k_env[][2] = {
    { "RECOMP_USB", "1" }, { "RECOMP_GL", "1" }, { "RECOMP_PB_EXEC", "1" },
    { "RECOMP_VBLANK", "1" }, { "RECOMP_AC97_READY_NOTRAP", "1" },
    { "RECOMP_TRACE_BUDGET", "0" }, { "RECOMP_GL_READBACK", "0" }, { "RECOMP_FPS", "60" },
    { "RECOMP_ABI_RESTORE", "1" }, { "RECOMP_LOCK_IDLE", "1" },
    { "RECOMP_GL_CURTAIN", "skip" }, { "RECOMP_MUTE", "0" }, { "RECOMP_WINDOW", "1" },
    { "RECOMP_FULLSCREEN", "1" },
    { "SDL_ANDROID_TRAP_BACK_BUTTON", "1" }, { "SDL_ACCELEROMETER_AS_JOYSTICK", "0" },
    { "SDL_TV_REMOTE_AS_JOYSTICK", "0" },
};

/* Set when the library is loaded, before the runtime's own constructors run
 * (priority 101 comes first): some of them read these, and on a computer the
 * launcher has set them before the engine starts. Anything already set wins. */
__attribute__((constructor(101))) static void android_env(void)
{
    size_t i;
    for (i = 0; i < sizeof k_env / sizeof k_env[0]; i++)
        setenv(k_env[i][0], k_env[i][1], 0);
}

struct host_call { int (*host)(void); int rc; };

static void *host_thread(void *p)
{
    struct host_call *c = (struct host_call *)p;
    c->rc = c->host();
    return NULL;
}

int nv_android_main(int argc, char **argv, int (*host)(void));
int nv_android_main(int argc, char **argv, int (*host)(void))
{
    const char *game  = argc > 1 ? argv[1] : NULL;
    const char *data  = argc > 2 ? argv[2] : NULL;
    const char *cache = argc > 3 ? argv[3] : NULL;
    char save[1024], cfg[1024], scale[8];
    struct host_call call;
    pthread_attr_t a;
    pthread_t t;

    if (!game || !data) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "started without the game and data folders");
        return 1;
    }
    open_log(data);
    snprintf(save, sizeof save, "%s/save", data);
    snprintf(cfg, sizeof cfg, "%s/settings", data);
    mkdir_p(save);
    { int s = cfg_get_int(cfg, "scale", 2);
      snprintf(scale, sizeof scale, "%d", s >= 1 && s <= 4 ? s : 2);
      setenv("RECOMP_GL_SCALE", scale, 0); }
    setenv("JSRF_GAME_DIR", game, 1);
    setenv("JSRF_SAVE_DIR", save, 1);
    setenv("JSRF_SETTINGS_FILE", cfg, 1);
    if (cache) setenv("TMPDIR", cache, 1);
    if (chdir(game) != 0)
        fprintf(stderr, "[ANDROID] cannot enter %s: %s\n", game, strerror(errno));
    fprintf(stderr, "[ANDROID] JSRF Unleashed %s\n[ANDROID] game: %s\n[ANDROID] data: %s\n",
            JSRF_VERSION, game, data);
    jlog_start();

    call.host = host;
    call.rc = 1;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 16u << 20);
    if (pthread_create(&t, &a, host_thread, &call) != 0) {
        fprintf(stderr, "[ANDROID] no thread for the engine\n");
        pthread_attr_destroy(&a);
        return 1;
    }
    pthread_attr_destroy(&a);
    pthread_join(t, NULL);
    fprintf(stderr, "[ANDROID] the engine returned %d\n", call.rc);
    return call.rc;
}

/* ---- the on-screen controls (JsrfActivity's TouchPadView) ---------------- */

/* What the controls hold: the keyboard's bits (Start, Back, and the d-pad
 * the stick gives, which menus read), the buttons as analog values (A B X Y
 * Black White LT RT), the stick, and whether the controls are showing. */
static volatile unsigned short g_touch_buttons;
static volatile unsigned char  g_touch_analog[8];
static volatile short g_touch_x, g_touch_y;
static volatile int   g_touch_stick, g_touch_shown;

JNIEXPORT void JNICALL
Java_com_jumatt_jsrfunleashed_JsrfActivity_nativeTouch(JNIEnv *env, jclass cls, jint buttons,
                                                       jint analog, jint x, jint y,
                                                       jboolean stick, jboolean shown)
{
    int i;
    (void)env; (void)cls;
    for (i = 0; i < 8; i++) g_touch_analog[i] = ((analog >> i) & 1) ? 255 : 0;
    g_touch_x = (short)x;
    g_touch_y = (short)y;
    g_touch_stick = stick ? 1 : 0;
    g_touch_buttons = (unsigned short)buttons;
    g_touch_shown = shown ? 1 : 0;
}

/* Merged in by nv2a_gl.c's nv_window_keys; 1 while the controls show. */
int nv_touch_keys(unsigned short *buttons, unsigned char *analog);
int nv_touch_keys(unsigned short *buttons, unsigned char *analog)
{
    int i, any = g_touch_shown || g_touch_buttons;
    *buttons = g_touch_buttons;
    for (i = 0; i < 8; i++) {
        analog[i] = g_touch_analog[i];
        if (analog[i]) any = 1;
    }
    return any;
}

/* xinput_device.c: the on-screen stick moves the left stick, while a thumb
 * is on it (this replaces its weak "none"). */
int nv_window_touch_stick(short *x, short *y);
int nv_window_touch_stick(short *x, short *y)
{
    if (!g_touch_stick) return 0;
    *x = g_touch_x;
    *y = g_touch_y;
    return 1;
}

/* The settings: widescreen now (by the window's thread, as F10 does it). */
int  nv2a_widescreen(void);
void nv_window_request_widescreen(int on);

JNIEXPORT jboolean JNICALL
Java_com_jumatt_jsrfunleashed_JsrfActivity_nativeWidescreen(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    return nv2a_widescreen() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_jumatt_jsrfunleashed_JsrfActivity_nativeSetWidescreen(JNIEnv *env, jclass cls, jboolean on)
{
    (void)env; (void)cls;
    nv_window_request_widescreen(on ? 1 : 0);
}

/* The camera on the right stick (port/recomp_manual.c), as F7 switches it:
 * 0 off, 1 the camera, 2 tracking. */
int  nv_window_pref_rstick_camera(void);
void nv_window_request_rstick_camera(int mode);

JNIEXPORT jint JNICALL
Java_com_jumatt_jsrfunleashed_JsrfActivity_nativeRstickCamera(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    return (jint)nv_window_pref_rstick_camera();
}

JNIEXPORT void JNICALL
Java_com_jumatt_jsrfunleashed_JsrfActivity_nativeSetRstickCamera(JNIEnv *env, jclass cls, jint mode)
{
    (void)env; (void)cls;
    nv_window_request_rstick_camera((int)mode);
}

/* Photo mode, from the settings (port/recomp_manual.c; the request is
 * nv2a_gl.c's, with SDL). */
void nv_window_request_photo(void);
JNIEXPORT void JNICALL Java_com_jumatt_jsrfunleashed_JsrfActivity_nativePhotoMode(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    nv_window_request_photo();
}

/* The map selector, from the settings (port/recomp_manual.c; the maps are
 * jsrf_maps.h's, the request nv2a_gl.c's). */
#include "../../src/nv2a_gl/jsrf_maps.h"
void nv_window_request_map(int mission);
JNIEXPORT jobjectArray JNICALL Java_com_jumatt_jsrfunleashed_JsrfActivity_nativeMapNames(JNIEnv *env, jclass cls)
{
    jclass str = (*env)->FindClass(env, "java/lang/String");
    jobjectArray a;
    int k;
    (void)cls;
    if (!str) return NULL;
    a = (*env)->NewObjectArray(env, JSRF_MAP_COUNT, str, NULL);
    for (k = 0; a && k < JSRF_MAP_COUNT; k++) {
        jstring n = (*env)->NewStringUTF(env, k_jsrf_maps[k].name);
        (*env)->SetObjectArrayElement(env, a, k, n);
        (*env)->DeleteLocalRef(env, n);
    }
    return a;
}
JNIEXPORT void JNICALL Java_com_jumatt_jsrfunleashed_JsrfActivity_nativeGoToMap(JNIEnv *env, jclass cls, jint index)
{
    (void)env; (void)cls;
    if (index >= 0 && index < JSRF_MAP_COUNT) nv_window_request_map(k_jsrf_maps[index].mission);
}

/* ---- vibration: the phone's own, with the on-screen controls ------------
 * The game's two motors (src/input/xinput_device.c, with no controller on
 * the first port) as one strength for JsrfActivity.rumble: a long one-shot at
 * that amplitude, sent again when it changes much or is about to run out,
 * cancelled at 0. At most ten calls a second. Off with the settings'
 * Vibration (port/recomp_manual.c sends nothing then). */
void *SDL_AndroidGetJNIEnv(void);
void *SDL_AndroidGetActivity(void);
void nv_android_rumble(unsigned lo, unsigned hi);
void nv_android_rumble(unsigned lo, unsigned hi)
{
    static int amp_now;
    static double sent;
    static jclass cls;
    static jmethodID mid;
    static int broken;
    struct timespec ts;
    double now;
    int amp = (int)((lo > hi ? lo : hi) / 257u);
    JNIEnv *env;
    if (!g_touch_shown || amp < 8) amp = 0;
    if (amp > 255) amp = 255;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    now = ts.tv_sec + ts.tv_nsec * 1e-9;
    if (broken) return;
    if (amp == 0 && amp_now == 0) return;
    if (amp && amp_now && abs(amp - amp_now) < 20 && now - sent < 8.0) return;
    if (amp && now - sent < 0.1) return;
    env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    if (!env) return;
    if (!mid) {
        jobject act = (jobject)SDL_AndroidGetActivity();
        jclass c;
        if (!act) return;
        c = (*env)->GetObjectClass(env, act);
        cls = (jclass)(*env)->NewGlobalRef(env, c);
        mid = (*env)->GetStaticMethodID(env, cls, "rumble", "(I)V");
        (*env)->DeleteLocalRef(env, c);
        (*env)->DeleteLocalRef(env, act);
        if (!mid) { (*env)->ExceptionClear(env); broken = 1; return; }
    }
    (*env)->CallStaticVoidMethod(env, cls, mid, (jint)amp);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    amp_now = amp;
    sent = now;
}

/* The settings' Vibration (the file is written by the Java side). */
void nv_window_note_vibration(int on);
JNIEXPORT void JNICALL Java_com_jumatt_jsrfunleashed_JsrfActivity_nativeSetVibration(JNIEnv *env, jclass cls, jboolean on)
{
    (void)env; (void)cls;
    nv_window_note_vibration(on ? 1 : 0);
}

/* A line for the log, from the Java side. */
JNIEXPORT void JNICALL
Java_com_jumatt_jsrfunleashed_JsrfActivity_nativeLog(JNIEnv *env, jclass cls, jstring line)
{
    const char *s = (*env)->GetStringUTFChars(env, line, NULL);
    (void)cls;
    if (!s) return;
    pthread_mutex_lock(&g_jlog_mu);
    if (g_log_open)
        fprintf(stderr, "%s\n", s);
    else if (strlen(g_jlog) + strlen(s) + 2 < sizeof g_jlog) {
        strcat(g_jlog, s);
        strcat(g_jlog, "\n");
    }
    pthread_mutex_unlock(&g_jlog_mu);
    (*env)->ReleaseStringUTFChars(env, line, s);
}

JNIEXPORT jstring JNICALL
Java_com_jumatt_jsrfunleashed_JsrfActivity_nativeVersion(JNIEnv *env, jclass cls)
{
    (void)cls;
    return (*env)->NewStringUTF(env, JSRF_VERSION);
}

/* ---- the setup screen's unpacker (SetupActivity) ------------------------- */

JNIEXPORT jstring JNICALL
Java_com_jumatt_jsrfunleashed_SetupActivity_nativeVersion(JNIEnv *env, jclass cls)
{
    (void)cls;
    return (*env)->NewStringUTF(env, JSRF_VERSION);
}

JNIEXPORT jboolean JNICALL
Java_com_jumatt_jsrfunleashed_SetupActivity_nativeProbe(JNIEnv *env, jclass cls, jstring path)
{
    const char *p = (*env)->GetStringUTFChars(env, path, NULL);
    int r = p ? xiso_probe(p) : 0;
    (void)cls;
    if (p) (*env)->ReleaseStringUTFChars(env, path, p);
    return r ? JNI_TRUE : JNI_FALSE;
}

/* Whether the picked file (a path or "fd:N") is an image of the game's disc,
 * before unpacking it: null if it is, else what to tell the player -- not an
 * Xbox disc image (a zip...), another game's disc, another build of the game. */
JNIEXPORT jstring JNICALL
Java_com_jumatt_jsrfunleashed_SetupActivity_nativeCheckImage(JNIEnv *env, jclass cls, jstring path)
{
    const char *p = (*env)->GetStringUTFChars(env, path, NULL);
    char why[640], text[1300];
    int r = p ? jsrf_xbe_check_iso(p, why, sizeof why) : JSRF_XBE_NOT_IMAGE;
    (void)cls;
    if (p) (*env)->ReleaseStringUTFChars(env, path, p);
    if (r == JSRF_XBE_OK) return NULL;
    if (!p) snprintf(why, sizeof why, "Could not open that file.");
    __android_log_print(ANDROID_LOG_WARN, TAG, "%s", why);
    snprintf(text, sizeof text, "%s.\n\n%s%s%s", JSRF_XBE_HEADING, why,
             r == JSRF_XBE_OTHER ? "\n\n" : "", r == JSRF_XBE_OTHER ? JSRF_XBE_OTHER_ADVICE : "");
    return (*env)->NewStringUTF(env, text);
}

/* Whether <dir>/default.xbe is the standalone US disc's program
 * (port/xbe_check.c): null if it is (or cannot be read -- the engine checks
 * again), else what to tell the player. Another build starts and then
 * crashes a few seconds in. */
JNIEXPORT jstring JNICALL
Java_com_jumatt_jsrfunleashed_SetupActivity_nativeCheckGame(JNIEnv *env, jclass cls, jstring dir)
{
    const char *d = (*env)->GetStringUTFChars(env, dir, NULL);
    char why[512], text[1100];
    int r = d ? jsrf_xbe_check_folder(d, why, sizeof why) : JSRF_XBE_OK;
    (void)cls;
    if (d) (*env)->ReleaseStringUTFChars(env, dir, d);
    if (r != JSRF_XBE_OTHER) return NULL;
    __android_log_print(ANDROID_LOG_WARN, TAG, "%s", why);
    snprintf(text, sizeof text, "%s\n\n%s", why, JSRF_XBE_OTHER_ADVICE);
    return (*env)->NewStringUTF(env, text);
}

struct unpack_ctx { JNIEnv *env; jobject self; jmethodID progress; };

static int unpack_progress(void *ctx, uint64_t done, uint64_t total)
{
    struct unpack_ctx *u = (struct unpack_ctx *)ctx;
    jboolean go = (*u->env)->CallBooleanMethod(u->env, u->self, u->progress,
                                               (jlong)done, (jlong)total);
    if ((*u->env)->ExceptionCheck(u->env)) { (*u->env)->ExceptionClear(u->env); return 0; }
    return go ? 0 : 1;
}

/* Unpacks the image at `iso` (a path, or "fd:N" for a file the picker
 * handed over as a descriptor) into `dest`; progress goes to this.onUnpackProgress(done, total), which
 * returns false to stop. null on success, else a sentence for the player. */
JNIEXPORT jstring JNICALL
Java_com_jumatt_jsrfunleashed_SetupActivity_nativeUnpack(JNIEnv *env, jobject self,
                                                         jstring iso, jstring dest)
{
    const char *i = (*env)->GetStringUTFChars(env, iso, NULL);
    const char *d = (*env)->GetStringUTFChars(env, dest, NULL);
    char err[512] = "";
    struct unpack_ctx u;
    int rc = -1;
    u.env = env;
    u.self = self;
    u.progress = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, self),
                                     "onUnpackProgress", "(JJ)Z");
    if (i && d && u.progress)
        rc = xiso_extract(i, d, unpack_progress, &u, err, sizeof err);
    else
        snprintf(err, sizeof err, "Internal error: the unpacker could not start.");
    if (i) (*env)->ReleaseStringUTFChars(env, iso, i);
    if (d) (*env)->ReleaseStringUTFChars(env, dest, d);
    return rc == 0 ? NULL : (*env)->NewStringUTF(env, err[0] ? err : "The image could not be unpacked.");
}
