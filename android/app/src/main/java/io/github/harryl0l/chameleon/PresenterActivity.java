// SPDX-License-Identifier: GPL-2.0-or-later
package io.github.harryl0l.chameleon;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.SharedPreferences;
import android.content.pm.ActivityInfo;
import android.content.res.ColorStateList;
import android.graphics.Color;
import android.graphics.Rect;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.InsetDrawable;
import android.graphics.drawable.LayerDrawable;
import android.graphics.drawable.StateListDrawable;
import android.hardware.display.DisplayManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.TypedValue;
import android.view.Display;
import android.view.DisplayCutout;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.PointerIcon;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.view.inputmethod.InputMethodManager;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.HorizontalScrollView;
import android.widget.ImageButton;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Switch;
import android.widget.TextView;

import java.util.Collections;
import java.util.List;

/**
 * Full-screen SurfaceView. The native presenter attaches an ASurfaceControl to
 * it and shows AHardwareBuffers from a Termux process (for KWin, after one GPU
 * copy; see presenter.cpp), over a socket the Termux broker hands over
 * (BrokerLink).
 *
 * Input goes back over the same socket: touches (direct or as a trackpad),
 * mouse, hardware keys and the Android keyboard. A small toolbar in the top
 * corner opens the keyboard and the settings.
 *
 * The surface's size goes to the producer too; the KWin shim makes KWin's
 * screen follow it, so rotating the phone (and, if enabled, opening the
 * keyboard, which shrinks the surface) resizes the desktop.
 */
public class PresenterActivity extends Activity
        implements SurfaceHolder.Callback, DisplayManager.DisplayListener {
    static {
        System.loadLibrary("chameleon_presenter");
    }

    private static native void nativeStart();
    private static native void nativeSurfaceCreated(Object surface);
    private static native void nativeSurfaceChanged(int width, int height, int refreshMilliHz);
    private static native void nativeSurfaceDestroyed();
    private static native void nativeSetFrameRateVote(float hz);
    private static native String nativeStatus();
    /** A connection from KWin's shim, passed on by the Termux broker (BrokerLink). */
    static native void nativeConnection(int fd);
    private static native boolean nativeScreenOff();

    private static final String PREFS = "chameleon";
    private static final String PREF_MODE = "input_mode";
    private static final String PREF_SPEED = "pointer_speed";
    private static final String PREF_TAP = "tap_to_click";
    private static final String PREF_BACK = "back_key";
    private static final String PREF_ORIENTATION = "orientation";
    private static final String PREF_KEYBOARD_RESIZE = "keyboard_resize";
    private static final String PREF_EXTRA_KEYS = "extra_keys";
    private static final String PREF_CLIPBOARD = "clipboard_sharing";
    private static final float DEFAULT_SPEED = 1.5f;

    private static boolean sStarted;
    private int mWidth, mHeight;
    private TextView mStatus;
    private TextView mWakeHint;
    private SurfaceView mSurface;
    private int mImeBottom;
    private List<Rect> mCutouts = Collections.emptyList();
    private SharedPreferences mPrefs;
    private ClipboardShare mClipboard;
    private TouchInput mTouch;
    private final KeyInput mKeys = new KeyInput();
    private ImeView mImeView;
    private FloatingMenu mMenu;
    private HorizontalScrollView mExtraKeys;
    private Button mCtrlKey, mAltKey;
    private boolean mKeyboardShown;
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    // Shows the connection state until frames arrive, so problems are
    // visible without logcat.
    private final Runnable mStatusPoll = new Runnable() {
        @Override
        public void run() {
            String status = nativeStatus();
            boolean off = nativeScreenOff();
            boolean showing = off || "showing frames".equals(status);
            mStatus.setVisibility(showing ? View.GONE : View.VISIBLE);
            if (!showing)
                mStatus.setText("Chameleon: " + status);
            mWakeHint.setVisibility(off ? View.VISIBLE : View.GONE);
            mHandler.postDelayed(this, 500);
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        // Let content use the notch area instead of leaving a black strip when
        // the bars are hidden (minSdk 29, so SHORT_EDGES always exists).
        WindowManager.LayoutParams attrs = getWindow().getAttributes();
        attrs.layoutInDisplayCutoutMode = Build.VERSION.SDK_INT >= Build.VERSION_CODES.R
                ? WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS
                : WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        getWindow().setAttributes(attrs);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            getWindow().setDecorFitsSystemWindows(false);
            // Bars are hidden through WindowInsetsController instead, and
            // without FLAG_FULLSCREEN the keyboard's insets are reported. The
            // window itself is not resized (decor does not fit system
            // windows); updateSurfaceArea() decides what the keyboard does.
            getWindow().clearFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN);
            getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE);
        }

        requestHighestRefreshRate();

        mPrefs = getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        mTouch = new TouchInput(getResources().getDisplayMetrics().density);
        mClipboard = ClipboardShare.get(this);
        applyInputPrefs();
        applyOrientation();

        SurfaceView view = new SurfaceView(this);
        mSurface = view;
        view.getHolder().addCallback(this);
        view.setOnTouchListener(mTouch);
        view.setOnGenericMotionListener(mTouch);
        // KWin draws its own cursor in the frames; Android's would be a second one.
        view.setPointerIcon(PointerIcon.getSystemIcon(this, PointerIcon.TYPE_NULL));
        mStatus = new TextView(this);
        mStatus.setTextColor(Color.LTGRAY);
        mStatus.setTextSize(12);
        mStatus.setPadding(32, 96, 32, 32);
        // KWin turned its screen off (idle): a tap wakes it. The tap is ours,
        // so it can't click whatever is under the finger.
        mWakeHint = new TextView(this);
        mWakeHint.setText(R.string.screen_off);
        mWakeHint.setTextColor(Color.LTGRAY);
        mWakeHint.setTextSize(16);
        mWakeHint.setGravity(Gravity.CENTER);
        mWakeHint.setBackgroundColor(Color.BLACK);
        mWakeHint.setVisibility(View.GONE);
        mWakeHint.setOnClickListener(v -> wakeScreen());
        mWakeHint.setOnGenericMotionListener((v, e) -> { // a mouse
            wakeScreen();
            return true;
        });
        mImeView = new ImeView(this, mKeys);
        FrameLayout root = new FrameLayout(this);
        root.addView(view, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT));
        root.addView(mStatus, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT,
                FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.TOP | Gravity.START));
        root.addView(mWakeHint, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT));
        root.addView(mImeView, new FrameLayout.LayoutParams(1, 1));
        mMenu = new FloatingMenu(this, mPrefs,
                toolbarButton(R.drawable.ic_keyboard, R.string.keyboard, v -> toggleKeyboard()),
                toolbarButton(R.drawable.ic_tune, R.string.settings, v -> showSettings()));
        root.addView(buildExtraKeys(), new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.BOTTOM));
        // Last, so it stays above the extra keys.
        root.addView(mMenu, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT,
                FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.TOP | Gravity.START));
        // Not from inside the layout pass: the surface's size follows the row's.
        mExtraKeys.addOnLayoutChangeListener((v, l, t, r, b, ol, ot, or, ob) -> v.post(this::updateSurfaceArea));
        root.addOnLayoutChangeListener((v, l, t, r, b, ol, ot, or, ob) -> v.post(this::updateMenuInsets));
        setContentView(root);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            root.setOnApplyWindowInsetsListener((v, insets) -> {
                onInsets(insets);
                return insets;
            });
        }
        mKeys.setLatchListener((ctrl, alt) -> {
            mCtrlKey.setActivated(ctrl);
            mAltKey.setActivated(alt);
        });

        if (!sStarted) {
            nativeStart(); // serves the connections the Termux broker passes on
            BrokerLink.listen(this);
            sStarted = true;
        }
    }

    // ---- toolbar, keyboard and extra keys ----

    private int dp(float v) {
        return Math.round(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics()));
    }

    private ImageButton toolbarButton(int icon, int description, View.OnClickListener onClick) {
        ImageButton b = new ImageButton(this);
        b.setImageResource(icon);
        b.setBackgroundResource(R.drawable.toolbar_button);
        b.setContentDescription(getString(description));
        b.setFocusable(false);
        b.setOnClickListener(onClick);
        return b;
    }

    private Button extraKey(LinearLayout row, String label, Runnable action) {
        Button b = new Button(this);
        b.setText(label);
        b.setAllCaps(false);
        b.setTextColor(Color.WHITE);
        b.setTextSize(14);
        b.setBackgroundResource(R.drawable.extra_key);
        b.setFocusable(false);
        b.setMinWidth(dp(48));
        b.setMinimumWidth(dp(48));
        b.setMinHeight(dp(40));
        b.setMinimumHeight(dp(40));
        b.setPadding(dp(8), 0, dp(8), 0);
        b.setOnClickListener(v -> action.run());
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, dp(40));
        lp.setMargins(dp(3), dp(4), dp(3), dp(4));
        row.addView(b, lp);
        return b;
    }

    private View buildExtraKeys() {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setPadding(dp(4), 0, dp(4), 0);
        extraKey(row, "Esc", () -> mKeys.tap(KeyInput.XK_ESCAPE));
        extraKey(row, "Tab", () -> mKeys.tap(KeyInput.XK_TAB));
        mCtrlKey = extraKey(row, "Ctrl", mKeys::toggleCtrl);
        mAltKey = extraKey(row, "Alt", mKeys::toggleAlt);
        extraKey(row, "\u2190", () -> mKeys.tap(KeyInput.XK_LEFT));
        extraKey(row, "\u2193", () -> mKeys.tap(KeyInput.XK_DOWN));
        extraKey(row, "\u2191", () -> mKeys.tap(KeyInput.XK_UP));
        extraKey(row, "\u2192", () -> mKeys.tap(KeyInput.XK_RIGHT));
        extraKey(row, "Home", () -> mKeys.tap(KeyInput.XK_HOME));
        extraKey(row, "End", () -> mKeys.tap(KeyInput.XK_END));
        extraKey(row, "PgUp", () -> mKeys.tap(KeyInput.XK_PAGE_UP));
        extraKey(row, "PgDn", () -> mKeys.tap(KeyInput.XK_PAGE_DOWN));
        mExtraKeys = new HorizontalScrollView(this);
        mExtraKeys.setBackgroundColor(0xE6202124);
        mExtraKeys.setHorizontalScrollBarEnabled(false);
        mExtraKeys.addView(row);
        mExtraKeys.setVisibility(View.GONE);
        return mExtraKeys;
    }

    private void toggleKeyboard() {
        InputMethodManager imm = getSystemService(InputMethodManager.class);
        if (mKeyboardShown) {
            imm.hideSoftInputFromWindow(mImeView.getWindowToken(), 0);
            setKeyboardShown(false);
        } else {
            mImeView.requestFocus();
            imm.showSoftInput(mImeView, InputMethodManager.SHOW_IMPLICIT);
            setKeyboardShown(true);
        }
    }

    private void setKeyboardShown(boolean shown) {
        mKeyboardShown = shown;
        boolean extraKeys = shown && mPrefs.getBoolean(PREF_EXTRA_KEYS, true);
        mExtraKeys.setVisibility(extraKeys ? View.VISIBLE : View.GONE);
        updateSurfaceArea();
    }

    /**
     * With "shrink the desktop above the keyboard", the surface ends above the
     * keyboard and the extra keys; its new size reaches KWin as a new screen
     * size. Otherwise the keyboard covers the bottom of the desktop.
     */
    private void updateSurfaceArea() {
        int bottom = 0;
        if (mKeyboardShown && mPrefs.getBoolean(PREF_KEYBOARD_RESIZE, true)) {
            bottom = mImeBottom;
            if (mExtraKeys.getVisibility() == View.VISIBLE)
                bottom += mExtraKeys.getHeight();
            // A full-size keyboard in landscape can leave a strip a couple of
            // hundred pixels tall, shorter than Plasma's panel. Below 40% of
            // the window the keyboard covers the desktop instead.
            View root = (View) mSurface.getParent();
            int height = root != null ? root.getHeight() : 0;
            if (height > 0 && height - bottom < height * 2 / 5)
                bottom = 0;
        }
        FrameLayout.LayoutParams lp = (FrameLayout.LayoutParams) mSurface.getLayoutParams();
        if (lp == null || lp.bottomMargin == bottom)
            return;
        lp.bottomMargin = bottom;
        mSurface.setLayoutParams(lp);
    }

    private void applyOrientation() {
        switch (mPrefs.getString(PREF_ORIENTATION, "auto")) {
            case "portrait":
                setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_USER_PORTRAIT);
                break;
            case "landscape":
                setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_USER_LANDSCAPE);
                break;
            default:
                // Follows the sensor unless the user locked rotation.
                setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_FULL_USER);
                break;
        }
    }

    /** Keeps the menu clear of the camera cutouts, the keyboard and the extra keys. */
    private void updateMenuInsets() {
        int bottom = mImeBottom + (mExtraKeys.getVisibility() == View.VISIBLE ? mExtraKeys.getHeight() : 0);
        mMenu.setObstacles(bottom, mCutouts);
    }

    /** Keeps the menu clear of the cutout and the extra keys above the IME. */
    private void onInsets(WindowInsets insets) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R)
            return;
        DisplayCutout cutout = insets.getDisplayCutout();
        mCutouts = cutout != null ? cutout.getBoundingRects() : Collections.emptyList();
        boolean imeVisible = insets.isVisible(WindowInsets.Type.ime());
        int imeBottom = insets.getInsets(WindowInsets.Type.ime()).bottom;
        FrameLayout.LayoutParams lp = (FrameLayout.LayoutParams) mExtraKeys.getLayoutParams();
        if (lp.bottomMargin != imeBottom) {
            lp.bottomMargin = imeBottom;
            mExtraKeys.setLayoutParams(lp);
        }
        mImeBottom = imeBottom;
        updateMenuInsets();
        if (imeVisible != mKeyboardShown)
            setKeyboardShown(imeVisible); // e.g. closed with the keyboard's own button
        else
            updateSurfaceArea();
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        int code = event.getKeyCode();
        switch (code) {
            case KeyEvent.KEYCODE_VOLUME_UP:
            case KeyEvent.KEYCODE_VOLUME_DOWN:
            case KeyEvent.KEYCODE_VOLUME_MUTE:
            case KeyEvent.KEYCODE_POWER:
            case KeyEvent.KEYCODE_APP_SWITCH:
            case KeyEvent.KEYCODE_HOME:
                return super.dispatchKeyEvent(event);
            case KeyEvent.KEYCODE_BACK:
                if (!backLeavesApp()) {
                    if (event.getRepeatCount() == 0)
                        InputSender.keysym(KeyInput.XK_ESCAPE, event.getAction() == KeyEvent.ACTION_DOWN);
                } else if (event.getAction() == KeyEvent.ACTION_UP) {
                    moveTaskToBack(true);
                }
                return true;
        }
        return mKeys.onKeyEvent(event) || super.dispatchKeyEvent(event);
    }

    /** Wakes KWin's screen like a mouse would: the pointer a pixel right and back. */
    private void wakeScreen() {
        int w = mSurface.getWidth(), h = mSurface.getHeight();
        InputSender.motion(InputSender.delta(1, 0, w, h));
        InputSender.motion(InputSender.delta(-1, 0, w, h));
        mWakeHint.setVisibility(View.GONE); // back if the screen stays off
    }

    // ---- settings ----

    private void applyInputPrefs() {
        mTouch.configure(isTrackpad() ? TouchInput.Mode.TRACKPAD : TouchInput.Mode.DIRECT,
                mPrefs.getFloat(PREF_SPEED, DEFAULT_SPEED), mPrefs.getBoolean(PREF_TAP, true));
        mClipboard.setEnabled(mPrefs.getBoolean(PREF_CLIPBOARD, true));
    }

    private boolean isTrackpad() {
        return "trackpad".equals(mPrefs.getString(PREF_MODE, "direct"));
    }

    private boolean backLeavesApp() {
        return "leave".equals(mPrefs.getString(PREF_BACK, "escape"));
    }

    // Pointer speed 0.25x..3x on a SeekBar of 0..275.
    private static int speedToProgress(float speed) {
        return Math.round(speed * 100) - 25;
    }

    private static float progressToSpeed(int progress) {
        return (progress + 25) / 100f;
    }

    /** A rounded card with a heading, added to box; the section's rows go into it. */
    private LinearLayout settingsCard(LinearLayout box, int heading) {
        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(dp(16), dp(12), dp(16), dp(8));
        GradientDrawable bg = new GradientDrawable();
        bg.setCornerRadius(dp(20));
        bg.setColor(accentColor() & 0x00ffffff | 0x1f000000);
        card.setBackground(bg);
        TextView t = new TextView(this);
        t.setText(heading);
        t.setAllCaps(true);
        t.setTextSize(12);
        t.setTextColor(accentColor());
        t.setPadding(0, 0, 0, dp(4));
        card.addView(t);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(12);
        box.addView(card, lp);
        return card;
    }

    private TextView settingsHint(int text) {
        TextView t = new TextView(this);
        t.setText(text);
        t.setTextSize(13);
        t.setAlpha(0.7f);
        t.setPadding(0, 0, 0, dp(6));
        return t;
    }

    /** A label on the left, its control on the right. */
    private LinearLayout settingsRow(TextView label, View control) {
        LinearLayout row = new LinearLayout(this);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setMinimumHeight(dp(48));
        row.addView(label, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        row.addView(control);
        return row;
    }

    private LinearLayout settingsRow(int label, View control) {
        TextView t = new TextView(this);
        t.setText(label);
        t.setTextSize(16);
        return settingsRow(t, control);
    }

    /** Saved and applied at once. Drawn like a Material 3 switch: an outlined
     *  track with a small dot when off, a filled one with a checked dot when on. */
    private Switch settingsSwitch(String pref) {
        Switch s = new Switch(this);
        int accent = accentColor();
        int outline = new TextView(this).getCurrentTextColor() & 0x00ffffff | 0x99000000;

        GradientDrawable trackOn = new GradientDrawable();
        trackOn.setCornerRadius(dp(16));
        trackOn.setSize(dp(52), dp(32));
        trackOn.setColor(accent);
        GradientDrawable trackOff = new GradientDrawable();
        trackOff.setCornerRadius(dp(16));
        trackOff.setSize(dp(52), dp(32));
        trackOff.setStroke(dp(2), outline);
        StateListDrawable track = new StateListDrawable();
        track.addState(new int[] {android.R.attr.state_checked}, trackOn);
        track.addState(new int[0], trackOff);

        GradientDrawable dotOn = new GradientDrawable();
        dotOn.setShape(GradientDrawable.OVAL);
        dotOn.setSize(dp(24), dp(24));
        dotOn.setColor(onColor(accent));
        Drawable check = getDrawable(R.drawable.ic_check).mutate();
        check.setTint(accent);
        LayerDrawable thumbOn = new LayerDrawable(new Drawable[] {dotOn, check});
        // The thumb is 26 dp wide, as a switch is at least twice its thumb's
        // width: the dots sit off-centre in it to land where Material 3 puts them.
        thumbOn.setLayerInset(0, 0, dp(4), dp(2), dp(4));
        thumbOn.setLayerInset(1, dp(4), dp(8), dp(6), dp(8));
        GradientDrawable dotOff = new GradientDrawable();
        dotOff.setShape(GradientDrawable.OVAL);
        dotOff.setSize(dp(16), dp(16));
        dotOff.setColor(outline);
        LayerDrawable thumbOff = new LayerDrawable(new Drawable[] {dotOff});
        thumbOff.setLayerInset(0, dp(8), dp(8), dp(2), dp(8));
        StateListDrawable thumb = new StateListDrawable();
        thumb.addState(new int[] {android.R.attr.state_checked}, thumbOn);
        thumb.addState(new int[0], thumbOff);

        s.setTrackDrawable(track);
        s.setThumbDrawable(thumb);
        s.setSwitchMinWidth(dp(52));
        s.setChecked(mPrefs.getBoolean(pref, true));
        s.setOnCheckedChangeListener((v, on) -> {
            mPrefs.edit().putBoolean(pref, on).apply();
            applySettings();
        });
        return s;
    }

    /** Two or three side-by-side buttons, one of them selected; returns the group. */
    private RadioGroup settingsChoice(int[] labels, String[] values, String current, OnChoice onChoice) {
        RadioGroup group = new RadioGroup(this);
        group.setOrientation(LinearLayout.HORIZONTAL);
        int accent = accentColor();
        ColorStateList text = new ColorStateList(
                new int[][] {{android.R.attr.state_checked}, {}},
                new int[] {onColor(accent), new TextView(this).getCurrentTextColor()});
        for (int i = 0; i < labels.length; i++) {
            RadioButton b = new RadioButton(this);
            b.setText(labels[i]);
            b.setId(View.generateViewId());
            b.setTag(values[i]);
            b.setButtonDrawable(null);
            b.setTextColor(text);
            b.setGravity(Gravity.CENTER);
            b.setPadding(dp(12), dp(6), dp(12), dp(6));
            GradientDrawable on = new GradientDrawable();
            on.setCornerRadius(dp(16));
            on.setColor(accent);
            GradientDrawable off = new GradientDrawable();
            off.setCornerRadius(dp(16));
            off.setStroke(dp(1), accent);
            StateListDrawable bg = new StateListDrawable();
            bg.addState(new int[] {android.R.attr.state_checked}, on);
            bg.addState(new int[0], off);
            b.setBackground(bg);
            RadioGroup.LayoutParams lp = new RadioGroup.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT);
            if (i > 0)
                lp.setMarginStart(dp(6));
            group.addView(b, lp);
            if (values[i].equals(current))
                group.check(b.getId());
        }
        group.setOnCheckedChangeListener((g, id) -> onChoice.chosen((String) g.findViewById(id).getTag()));
        return group;
    }

    private interface OnChoice {
        void chosen(String value);
    }

    private int accentColor() {
        TypedValue v = new TypedValue();
        return getTheme().resolveAttribute(android.R.attr.colorAccent, v, true) ? v.data : 0xff4f8ef7;
    }

    /** Black or white, whichever reads better on color. */
    private static int onColor(int color) {
        double l = 0.299 * Color.red(color) + 0.587 * Color.green(color) + 0.114 * Color.blue(color);
        return l > 150 ? 0xff1b1b1f : Color.WHITE;
    }

    private void applySettings() {
        applyInputPrefs();
        applyOrientation();
        setKeyboardShown(mKeyboardShown); // the extra keys, and the surface's area
    }

    /** Every change is saved and applied as it is made. */
    private void showSettings() {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(16), 0, dp(16), dp(8));

        LinearLayout touch = settingsCard(box, R.string.touch_input);
        TextView modeHint = settingsHint(isTrackpad() ? R.string.mode_trackpad_hint : R.string.mode_direct_hint);
        LinearLayout trackpadOptions = new LinearLayout(this);
        trackpadOptions.setOrientation(LinearLayout.VERTICAL);
        trackpadOptions.setVisibility(isTrackpad() ? View.VISIBLE : View.GONE);
        touch.addView(settingsRow(R.string.input_mode, settingsChoice(
                new int[] {R.string.mode_direct, R.string.mode_trackpad}, new String[] {"direct", "trackpad"},
                isTrackpad() ? "trackpad" : "direct", value -> {
                    mPrefs.edit().putString(PREF_MODE, value).apply();
                    applySettings();
                    boolean t = isTrackpad();
                    modeHint.setText(t ? R.string.mode_trackpad_hint : R.string.mode_direct_hint);
                    trackpadOptions.setVisibility(t ? View.VISIBLE : View.GONE);
                })));
        touch.addView(modeHint);

        TextView speedLabel = new TextView(this);
        speedLabel.setTextSize(16);
        float current = mPrefs.getFloat(PREF_SPEED, DEFAULT_SPEED);
        speedLabel.setText(getString(R.string.pointer_speed, current));
        SeekBar speed = new SeekBar(this);
        speed.setMax(speedToProgress(3f));
        speed.setProgress(speedToProgress(current));
        speed.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar s, int progress, boolean fromUser) {
                speedLabel.setText(getString(R.string.pointer_speed, progressToSpeed(progress)));
            }

            @Override
            public void onStartTrackingTouch(SeekBar s) {}

            @Override
            public void onStopTrackingTouch(SeekBar s) {
                mPrefs.edit().putFloat(PREF_SPEED, progressToSpeed(s.getProgress())).apply();
                applySettings();
            }
        });
        trackpadOptions.addView(speedLabel, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));
        trackpadOptions.addView(speed);
        trackpadOptions.addView(settingsRow(R.string.tap_to_click, settingsSwitch(PREF_TAP)));
        touch.addView(trackpadOptions);
        touch.addView(settingsHint(R.string.mouse_hint));

        LinearLayout keys = settingsCard(box, R.string.keys);
        keys.addView(settingsRow(R.string.back_button, settingsChoice(
                new int[] {R.string.back_escape, R.string.back_leave}, new String[] {"escape", "leave"},
                backLeavesApp() ? "leave" : "escape", value -> {
                    mPrefs.edit().putString(PREF_BACK, value).apply();
                    applySettings();
                })));
        keys.addView(settingsRow(R.string.extra_keys, settingsSwitch(PREF_EXTRA_KEYS)));

        LinearLayout screen = settingsCard(box, R.string.screen);
        // Three choices don't fit beside the label: they go below it.
        TextView orientationLabel = new TextView(this);
        orientationLabel.setText(R.string.orientation);
        orientationLabel.setTextSize(16);
        orientationLabel.setPadding(0, dp(12), 0, dp(8));
        screen.addView(orientationLabel);
        screen.addView(settingsChoice(
                new int[] {R.string.orientation_auto, R.string.orientation_portrait, R.string.orientation_landscape},
                new String[] {"auto", "portrait", "landscape"}, mPrefs.getString(PREF_ORIENTATION, "auto"), value -> {
                    mPrefs.edit().putString(PREF_ORIENTATION, value).apply();
                    applySettings();
                }));
        screen.addView(settingsRow(R.string.keyboard_resize, settingsSwitch(PREF_KEYBOARD_RESIZE)));
        screen.addView(settingsHint(R.string.screen_hint));

        LinearLayout clipboard = settingsCard(box, R.string.clipboard);
        clipboard.addView(settingsRow(R.string.clipboard_sharing, settingsSwitch(PREF_CLIPBOARD)));
        clipboard.addView(settingsHint(R.string.clipboard_hint));

        ScrollView scroll = new ScrollView(this);
        scroll.addView(box);
        // Close below the scrolling cards, shaped like the choice buttons
        // (the dialog's own button bar can't take that shape).
        Button close = new Button(this);
        close.setText(R.string.close);
        close.setAllCaps(false);
        close.setTextSize(TypedValue.COMPLEX_UNIT_PX, new RadioButton(this).getTextSize());
        close.setTextColor(onColor(accentColor()));
        close.setStateListAnimator(null); // no shadow
        close.setMinHeight(0);
        close.setMinimumHeight(0);
        close.setPadding(dp(20), 0, dp(20), 0);
        GradientDrawable pill = new GradientDrawable();
        pill.setCornerRadius(dp(20));
        pill.setColor(accentColor());
        close.setBackground(pill);
        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        // Between the title and the cards scrolling under it.
        View separator = new View(this);
        separator.setBackgroundColor(new TextView(this).getCurrentTextColor());
        separator.setAlpha(0.2f);
        LinearLayout.LayoutParams sp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(1));
        sp.setMargins(dp(24), dp(12), dp(24), 0);
        content.addView(separator, sp);
        content.addView(scroll, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT, 1)); // shrinks to fit, the button stays
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, dp(40));
        lp.gravity = Gravity.END;
        lp.setMargins(0, dp(12), dp(16), dp(16));
        content.addView(close, lp);

        AlertDialog dialog = new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(R.string.settings_title)
                .setView(content)
                .setOnDismissListener(d -> hideSystemBars())
                .show();
        close.setOnClickListener(v -> dialog.dismiss());
        // Rounder than the default dialog.
        TypedValue v = new TypedValue();
        GradientDrawable bg = new GradientDrawable();
        bg.setCornerRadius(dp(28));
        bg.setColor(dialog.getContext().getTheme().resolveAttribute(android.R.attr.colorBackground, v, true)
                ? v.data : 0xff202124);
        dialog.getWindow().setBackgroundDrawable(new InsetDrawable(bg, dp(16)));
    }

    private Display display() {
        return Build.VERSION.SDK_INT >= 30 ? getDisplay() : getWindowManager().getDefaultDisplay();
    }

    /** Ask for the fastest mode at the current resolution (e.g. 120 Hz). */
    private void requestHighestRefreshRate() {
        Display display = display();
        if (display == null)
            return;
        Display.Mode current = display.getMode();
        Display.Mode best = current;
        for (Display.Mode mode : display.getSupportedModes()) {
            if (mode.getPhysicalWidth() == current.getPhysicalWidth()
                    && mode.getPhysicalHeight() == current.getPhysicalHeight()
                    && mode.getRefreshRate() > best.getRefreshRate())
                best = mode;
        }
        WindowManager.LayoutParams lp = getWindow().getAttributes();
        lp.preferredDisplayModeId = best.getModeId();
        getWindow().setAttributes(lp);
        nativeSetFrameRateVote(best.getRefreshRate());
    }

    private int refreshMilliHz() {
        Display display = display();
        return Math.round((display != null ? display.getRefreshRate() : 60f) * 1000f);
    }

    @Override
    protected void onResume() {
        super.onResume();
        getSystemService(DisplayManager.class).registerDisplayListener(this, null);
        mHandler.post(mStatusPoll);
    }

    @Override
    protected void onPause() {
        mHandler.removeCallbacks(mStatusPoll);
        getSystemService(DisplayManager.class).unregisterDisplayListener(this);
        super.onPause();
    }

    // The panel switches modes after our request lands; tell the producer.
    @Override
    public void onDisplayChanged(int displayId) {
        Display display = display();
        if (display != null && display.getDisplayId() == displayId && mWidth > 0)
            nativeSurfaceChanged(mWidth, mHeight, refreshMilliHz());
    }

    @Override
    public void onDisplayAdded(int displayId) {}

    @Override
    public void onDisplayRemoved(int displayId) {}

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            hideSystemBars();
            mClipboard.onPrimaryClipChanged(); // copied in another app meanwhile?
        }
    }

    private void hideSystemBars() {
        WindowInsetsController controller =
                Build.VERSION.SDK_INT >= 30 ? getWindow().getInsetsController() : null;
        if (controller != null) {
            controller.hide(WindowInsets.Type.systemBars());
            controller.setSystemBarsBehavior(
                    WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        } else {
            getWindow().getDecorView().setSystemUiVisibility(
                    View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_FULLSCREEN
                            | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION);
        }
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        nativeSurfaceCreated(holder.getSurface());
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        mWidth = width;
        mHeight = height;
        nativeSurfaceChanged(width, height, refreshMilliHz());
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        mWidth = mHeight = 0;
        nativeSurfaceDestroyed();
    }
}
