package com.jumatt.jsrfunleashed;

import android.app.AlertDialog;
import android.content.pm.ActivityInfo;
import android.hardware.input.InputManager;
import android.os.Bundle;
import android.util.Log;
import android.view.InputDevice;
import android.view.MotionEvent;
import android.view.ViewGroup;
import android.widget.RelativeLayout;
import android.widget.Toast;

import java.io.File;

import org.libsdl.app.SDLActivity;

/**
 * The game: SDL's activity, which loads libSDL2.so and libmain.so (the
 * engine) and calls its SDL_main with the folders below
 * (port/android/jsrf_android.c reads them). On top of SDL's surface, the
 * on-screen controls (TouchPadView) while no controller is connected, and
 * the settings button.
 */
public class JsrfActivity extends SDLActivity
        implements TouchPadView.Listener, InputManager.InputDeviceListener {

    /* port/android/jsrf_android.c */
    static native void nativeTouch(int buttons, int analog, int x, int y, boolean stick, boolean shown);
    static native boolean nativeWidescreen();
    static native void nativeSetWidescreen(boolean on);
    static native int nativeRstickCamera();            /* 0 off, 1 the camera, 2 tracking */
    static native void nativeSetRstickCamera(int mode);
    static native void nativePhotoMode();
    static native String[] nativeMapNames();          /* the map selector (jsrf_maps.h) */
    static native void nativeGoToMap(int index);
    static native String nativeVersion();
    static native void nativeLog(String line);

    private TouchPadView mPad;
    private InputManager mInput;
    private String mPadsSaid = "";

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "main" };
    }

    @Override
    protected String[] getArguments() {
        File data = getExternalFilesDir(null);
        return new String[] {
            new File(data, "game").getAbsolutePath(),   /* default.xbe and Media/ */
            data.getAbsolutePath(),                      /* Logs/, settings, save/ */
            getCacheDir().getAbsolutePath(),             /* temporary files */
        };
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        if (mLayout == null) return;        /* SDL could not load the engine: it says so */
        mPad = new TouchPadView(this, this);
        mLayout.addView(mPad, new RelativeLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        mInput = (InputManager) getSystemService(INPUT_SERVICE);
        if (mInput != null) mInput.registerInputDeviceListener(this, null);
        updateControls();
    }

    @Override
    protected void onPause() {
        if (mPad != null) mPad.reset();
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        if (mInput != null) mInput.unregisterInputDeviceListener(this);
        super.onDestroy();
    }

    /** Landscape, either way up. SDL would let a resizable window turn with
     *  the phone, into portrait too. */
    @Override
    public void setOrientationBis(int w, int h, boolean resizable, String hint) {
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
    }

    /* ---- controllers and the on-screen controls ---- */

    @Override public void onInputDeviceAdded(int id)   { updateControls(); }
    @Override public void onInputDeviceRemoved(int id) { updateControls(); }
    @Override public void onInputDeviceChanged(int id) { updateControls(); }

    /** Android calls it a gamepad or a joystick. */
    private static boolean looksLikePad(InputDevice d) {
        if (d == null || d.isVirtual()) return false;
        int s = d.getSources();
        return (s & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
            || (s & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
    }

    /** Its sticks' and triggers' axes, and its d-pad's when it reports one as
     *  axes: {axes, hat axes}, counted as SDL counts them. */
    private static int[] axesOf(InputDevice d) {
        int axes = 0, hats = 0;
        for (InputDevice.MotionRange r : d.getMotionRanges()) {
            if ((r.getSource() & InputDevice.SOURCE_CLASS_JOYSTICK) == 0) continue;
            if (r.getAxis() == MotionEvent.AXIS_HAT_X || r.getAxis() == MotionEvent.AXIS_HAT_Y) hats++;
            else axes++;
        }
        return new int[] { axes, hats };
    }

    /** A controller: a gamepad with a stick or a d-pad that reports as axes.
     *
     *  Being called a gamepad is not enough. Phones list some of their own
     *  parts as gamepads -- the fingerprint reader's or the side buttons'
     *  device declares gamepad buttons -- and counting those hid the on-screen
     *  controls on such a phone with nothing connected. They have no axes.
     *  SDL is told the same (SDL_TV_REMOTE_AS_JOYSTICK in
     *  port/android/jsrf_android.c), so the engine does not take one for
     *  player 1's controller either. */
    private static boolean isController(InputDevice d) {
        if (!looksLikePad(d)) return false;
        int[] a = axesOf(d);
        return a[0] >= 2 || a[1] >= 2;
    }

    private boolean touchPref() {
        return Settings.get(this, "touch", 1) != 0;
    }

    /** The on-screen controls while no controller is connected (and the
     *  player has not turned them off). */
    private void updateControls() {
        if (mPad == null) return;
        boolean pad = false;
        StringBuilder said = new StringBuilder();
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice d = InputDevice.getDevice(id);
            if (!looksLikePad(d)) continue;
            boolean c = isController(d);
            int[] a = axesOf(d);
            if (c) pad = true;
            /* Into the log: what each device Android calls a gamepad was
             * taken for. Its name and counts only -- a descriptor can carry
             * a Bluetooth address, and logs end up in public issues. */
            said.append(String.format(java.util.Locale.ROOT, "[PAD] Android: %s: %d axes, %d hat axes -- %s\n",
                    d.getName(), a[0], a[1], c ? "a controller" : "no stick or d-pad, not a controller"));
        }
        boolean want = !pad && touchPref();
        mPad.setControls(want);
        said.append("[PAD] Android: on-screen controls ").append(pad ? "hidden (a controller)" : want ? "shown" : "hidden (the setting)");
        String line = said.toString();
        if (!line.equals(mPadsSaid)) {
            mPadsSaid = line;
            Log.i("JSRF", line);
            for (String l : line.split("\n")) nativeLog(l);
        }
    }

    /* ---- settings ---- */

    private AlertDialog.Builder dialog() {
        return new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert);
    }

    @Override
    public void onSettings() {
        final boolean wide = nativeWidescreen();
        final boolean touch = touchPref();
        final int rcam = nativeRstickCamera();
        final int scale = Settings.get(this, "scale", 2);
        String[] items = {
            "Widescreen: " + (wide ? "on" : "off"),
            "On-screen controls: " + (touch ? "shown" : "hidden"),
            "Resolution: " + scale + "x",
            "Camera on the right stick: " + (rcam == 2 ? "tracking" : rcam != 0 ? "on" : "off"),
            "Photo mode",
            "Go to a map",
            "Controls",
        };
        mPad.reset();
        dialog().setTitle("JSRF Unleashed " + nativeVersion())
            .setItems(items, (d, which) -> {
                switch (which) {
                case 0:
                    nativeSetWidescreen(!wide);
                    break;
                case 1:
                    Settings.set(this, "touch", touch ? 0 : 1);
                    updateControls();
                    break;
                case 2:
                    pickScale(scale);
                    break;
                case 3:
                    pickRstickCamera(rcam);
                    break;
                case 4:
                    nativePhotoMode();
                    break;
                case 5:
                    pickMap();
                    break;
                default:
                    showControls();
                    break;
                }
            })
            .setNegativeButton("Close", null)
            .show();
    }

    /* The map selector: straight to a map, to skate it freely. */
    private void pickMap() {
        String[] items = nativeMapNames();
        if (items == null) return;
        dialog().setTitle("Go to a map")
            .setItems(items, (d, which) -> nativeGoToMap(which))
            .setNegativeButton("Cancel", null)
            .show();
    }

    /* The camera on the right stick: on (the skater goes where it looks),
     * tracking (it only looks around: the skater keeps his heading), or off
     * (the game's own camera). */
    private void pickRstickCamera(int now) {
        String[] names = { "Off (the game's camera)", "On", "Tracking (look around, keep going)" };
        String[] items = new String[3];
        int[] modes = { 1, 2, 0 };
        for (int i = 0; i < 3; i++)
            items[i] = names[modes[i]] + (modes[i] == now ? "  ✓" : "");
        dialog().setTitle("Camera on the right stick")
            .setItems(items, (d, which) -> nativeSetRstickCamera(modes[which]))
            .setNegativeButton("Cancel", null)
            .show();
    }

    private void pickScale(int now) {
        String[] items = new String[4];
        for (int s = 1; s <= 4; s++)
            items[s - 1] = s + "x (" + 640 * s + " x " + 480 * s + ")" + (s == now ? "  ✓" : "");
        dialog().setTitle("Resolution")
            .setItems(items, (d, which) -> {
                Settings.set(this, "scale", which + 1);
                if (which + 1 != now)
                    Toast.makeText(this, "Used the next time the game starts.", Toast.LENGTH_LONG).show();
            })
            .setNegativeButton("Cancel", null)
            .show();
    }

    private void showControls() {
        dialog().setTitle("Controls")
            .setMessage("A controller works as on the console (Xbox, PlayStation, Switch Pro and "
                + "others, over Bluetooth or USB): A jump, R trigger talk and spray, Start, "
                + "Back/Select Back.\n\n"
                + "On screen: put your left thumb anywhere on the left half to steer; A B X Y are "
                + "the pad's, R and L its triggers, the hollow and filled circles White and Black; "
                + "Start and Back are at the top.\n\n"
                + "With the camera on the right stick (the settings), a controller's right stick "
                + "turns the camera around you and it eases back behind you when let go; "
                + "L trigger puts it straight behind at once. Set to tracking, it only looks "
                + "around: you keep going the way you were.\n\n"
                + "Photo mode (the settings, or hold Back on a controller): move the camera freely with "
                + "the sticks, the triggers lower and raise it, Y hides the HUD, A the help, B goes back "
                + "to the game.\n\n"
                + "A keyboard: WASD (by position) moves, arrows the camera, Space jump, E spray, "
                + "Enter Start, Esc Back.\n\n"
                + "A new resolution is used the next time the game starts.")
            .setPositiveButton("OK", null)
            .show();
    }
}
