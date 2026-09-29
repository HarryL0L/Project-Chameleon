package io.github.harryl0l.chameleon;

import android.app.Activity;
import android.hardware.display.DisplayManager;
import android.os.Build;
import android.os.Bundle;
import android.view.Display;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;

/**
 * Full-screen SurfaceView. The native presenter attaches an ASurfaceControl to
 * it and shows AHardwareBuffers sent by a Termux process over
 * $PREFIX/tmp/chameleon-0 without copying them.
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

    private static boolean sStarted;
    private int mWidth, mHeight;

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
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R)
            getWindow().setDecorFitsSystemWindows(false);

        requestHighestRefreshRate();

        SurfaceView view = new SurfaceView(this);
        view.getHolder().addCallback(this);
        setContentView(view);

        if (!sStarted) {
            // Same path as CHAM_SOCKET_PATH; we run as the Termux user.
            nativeStart("/data/data/com.termux/files/usr/tmp/chameleon-0");
            sStarted = true;
        }
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
    }

    @Override
    protected void onPause() {
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
        if (!hasFocus)
            return;
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
