package io.github.harryl0l.chameleon;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.graphics.Insets;
import android.hardware.display.DisplayManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.TypedValue;
import android.view.Display;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.view.inputmethod.InputMethodManager;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.FrameLayout;
import android.widget.HorizontalScrollView;
import android.widget.ImageButton;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.TextView;

/**
 * Full-screen SurfaceView. The native presenter attaches an ASurfaceControl to
 * it and shows AHardwareBuffers sent by a Termux process over
 * $PREFIX/tmp/chameleon-0 without copying them.
 *
 * Input goes back over the same socket: touches (direct or as a trackpad),
 * mouse, hardware keys and the Android keyboard. A small toolbar in the top
 * corner opens the keyboard and the settings.
 */
public class PresenterActivity extends Activity
        implements SurfaceHolder.Callback, DisplayManager.DisplayListener {
    static {
        System.loadLibrary("chameleon_presenter");
    }

    private static native void nativeStart(String socketPath);
    private static native void nativeSurfaceCreated(Object surface);
    private static native void nativeSurfaceChanged(int width, int height, int refreshMilliHz);
    private static native void nativeSurfaceDestroyed();
    private static native void nativeSetFrameRateVote(float hz);
    private static native String nativeStatus();

    private static final String PREFS = "chameleon";
    private static final String PREF_MODE = "input_mode";
    private static final String PREF_SPEED = "pointer_speed";
    private static final String PREF_TAP = "tap_to_click";
    private static final String PREF_BACK = "back_key";

    private static boolean sStarted;
    private int mWidth, mHeight;
    private TextView mStatus;
    private SharedPreferences mPrefs;
    private TouchInput mTouch;
    private final KeyInput mKeys = new KeyInput();
    private ImeView mImeView;
    private LinearLayout mToolbar;
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
            boolean showing = "showing frames".equals(status);
            mStatus.setVisibility(showing ? View.GONE : View.VISIBLE);
            if (!showing)
                mStatus.setText("Chameleon: " + status);
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
            // window is not resized (decor does not fit system windows), so
            // KWin's screen keeps its size while the keyboard is up.
            getWindow().clearFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN);
            getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE);
        }

        requestHighestRefreshRate();

        mPrefs = getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        mTouch = new TouchInput(getResources().getDisplayMetrics().density);
        applyInputPrefs();

        SurfaceView view = new SurfaceView(this);
        view.getHolder().addCallback(this);
        view.setOnTouchListener(mTouch);
        view.setOnGenericMotionListener(mTouch);
        mStatus = new TextView(this);
        mStatus.setTextColor(Color.LTGRAY);
        mStatus.setTextSize(12);
        mStatus.setPadding(32, 96, 32, 32);
        mImeView = new ImeView(this, mKeys);
        FrameLayout root = new FrameLayout(this);
        root.addView(view);
        root.addView(mStatus, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT,
                FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.TOP | Gravity.START));
        root.addView(mImeView, new FrameLayout.LayoutParams(1, 1));
        root.addView(buildToolbar(), new FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT,
                FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.TOP | Gravity.END));
        root.addView(buildExtraKeys(), new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.BOTTOM));
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
            // Same path as CHAM_SOCKET_PATH; we run as the Termux user.
            nativeStart("/data/data/com.termux/files/usr/tmp/chameleon-0");
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
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(dp(40), dp(40));
        lp.setMarginStart(dp(8));
        b.setLayoutParams(lp);
        return b;
    }

    private View buildToolbar() {
        mToolbar = new LinearLayout(this);
        mToolbar.setOrientation(LinearLayout.HORIZONTAL);
        mToolbar.setPadding(dp(8), dp(8), dp(8), dp(8));
        mToolbar.setAlpha(0.6f);
        mToolbar.addView(toolbarButton(R.drawable.ic_keyboard, R.string.keyboard, v -> toggleKeyboard()));
        mToolbar.addView(toolbarButton(R.drawable.ic_tune, R.string.settings, v -> showSettings()));
        return mToolbar;
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
        mExtraKeys.setVisibility(shown ? View.VISIBLE : View.GONE);
    }

    /** Keeps the toolbar clear of the cutout and the extra keys above the IME. */
    private void onInsets(WindowInsets insets) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R)
            return;
        Insets cutout = insets.getInsets(WindowInsets.Type.displayCutout());
        mToolbar.setPadding(dp(8), dp(8) + cutout.top, dp(8) + cutout.right, dp(8));
        boolean imeVisible = insets.isVisible(WindowInsets.Type.ime());
        int imeBottom = insets.getInsets(WindowInsets.Type.ime()).bottom;
        FrameLayout.LayoutParams lp = (FrameLayout.LayoutParams) mExtraKeys.getLayoutParams();
        lp.bottomMargin = imeBottom;
        mExtraKeys.setLayoutParams(lp);
        if (imeVisible != mKeyboardShown)
            setKeyboardShown(imeVisible); // e.g. closed with the keyboard's own button
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
                if (!"leave".equals(mPrefs.getString(PREF_BACK, "escape"))) {
                    if (event.getRepeatCount() == 0)
                        InputSender.keysym(KeyInput.XK_ESCAPE, event.getAction() == KeyEvent.ACTION_DOWN);
                } else if (event.getAction() == KeyEvent.ACTION_UP) {
                    moveTaskToBack(true);
                }
                return true;
        }
        return mKeys.onKeyEvent(event) || super.dispatchKeyEvent(event);
    }

    // ---- settings ----

    private void applyInputPrefs() {
        TouchInput.Mode mode = "trackpad".equals(mPrefs.getString(PREF_MODE, "direct"))
                ? TouchInput.Mode.TRACKPAD : TouchInput.Mode.DIRECT;
        mTouch.configure(mode, mPrefs.getFloat(PREF_SPEED, 1.5f), mPrefs.getBoolean(PREF_TAP, true));
    }

    private TextView settingsLabel(String text, boolean heading) {
        TextView t = new TextView(this);
        t.setText(text);
        t.setTextSize(heading ? 16 : 13);
        t.setPadding(0, heading ? dp(16) : dp(2), 0, dp(4));
        if (!heading)
            t.setAlpha(0.7f);
        return t;
    }

    private void showSettings() {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(24), dp(8), dp(24), dp(8));

        box.addView(settingsLabel(getString(R.string.touch_input), true));
        RadioGroup modes = new RadioGroup(this);
        RadioButton direct = new RadioButton(this);
        direct.setText(R.string.mode_direct);
        direct.setId(View.generateViewId());
        RadioButton trackpad = new RadioButton(this);
        trackpad.setText(R.string.mode_trackpad);
        trackpad.setId(View.generateViewId());
        modes.addView(direct);
        modes.addView(trackpad);
        boolean isTrackpad = "trackpad".equals(mPrefs.getString(PREF_MODE, "direct"));
        modes.check(isTrackpad ? trackpad.getId() : direct.getId());
        box.addView(modes);
        TextView modeHint = settingsLabel(getString(isTrackpad ? R.string.mode_trackpad_hint : R.string.mode_direct_hint), false);
        box.addView(modeHint);

        TextView speedLabel = settingsLabel("", true);
        SeekBar speed = new SeekBar(this);
        speed.setMax(275); // 0.25x .. 3x
        float current = mPrefs.getFloat(PREF_SPEED, 1.5f);
        speed.setProgress(Math.round(current * 100) - 25);
        speedLabel.setText(getString(R.string.pointer_speed, current));
        speed.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar s, int progress, boolean fromUser) {
                speedLabel.setText(getString(R.string.pointer_speed, (progress + 25) / 100f));
            }

            @Override
            public void onStartTrackingTouch(SeekBar s) {}

            @Override
            public void onStopTrackingTouch(SeekBar s) {}
        });
        CheckBox tap = new CheckBox(this);
        tap.setText(R.string.tap_to_click);
        tap.setChecked(mPrefs.getBoolean(PREF_TAP, true));
        LinearLayout trackpadOptions = new LinearLayout(this);
        trackpadOptions.setOrientation(LinearLayout.VERTICAL);
        trackpadOptions.addView(speedLabel);
        trackpadOptions.addView(speed);
        trackpadOptions.addView(tap);
        trackpadOptions.setVisibility(isTrackpad ? View.VISIBLE : View.GONE);
        box.addView(trackpadOptions);
        modes.setOnCheckedChangeListener((group, id) -> {
            boolean t = id == trackpad.getId();
            modeHint.setText(t ? R.string.mode_trackpad_hint : R.string.mode_direct_hint);
            trackpadOptions.setVisibility(t ? View.VISIBLE : View.GONE);
        });

        box.addView(settingsLabel(getString(R.string.back_button), true));
        RadioGroup back = new RadioGroup(this);
        RadioButton backEsc = new RadioButton(this);
        backEsc.setText(R.string.back_escape);
        backEsc.setId(View.generateViewId());
        RadioButton backLeave = new RadioButton(this);
        backLeave.setText(R.string.back_leave);
        backLeave.setId(View.generateViewId());
        back.addView(backEsc);
        back.addView(backLeave);
        back.check("leave".equals(mPrefs.getString(PREF_BACK, "escape")) ? backLeave.getId() : backEsc.getId());
        box.addView(back);

        box.addView(settingsLabel(getString(R.string.mouse_hint), false));

        ScrollView scroll = new ScrollView(this);
        scroll.addView(box);
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(R.string.settings_title)
                .setView(scroll)
                .setPositiveButton(R.string.done, (d, w) -> {
                    mPrefs.edit()
                            .putString(PREF_MODE, modes.getCheckedRadioButtonId() == trackpad.getId() ? "trackpad" : "direct")
                            .putFloat(PREF_SPEED, (speed.getProgress() + 25) / 100f)
                            .putBoolean(PREF_TAP, tap.isChecked())
                            .putString(PREF_BACK, back.getCheckedRadioButtonId() == backLeave.getId() ? "leave" : "escape")
                            .apply();
                    applyInputPrefs();
                })
                .setOnDismissListener(d -> hideSystemBars())
                .show();
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
        if (hasFocus)
            hideSystemBars();
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
