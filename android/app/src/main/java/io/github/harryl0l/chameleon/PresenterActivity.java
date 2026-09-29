package io.github.harryl0l.chameleon;

import android.app.Activity;
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
public class PresenterActivity extends Activity implements SurfaceHolder.Callback {
    static {
        System.loadLibrary("chameleon_presenter");
    }

    private static native void nativeStart(String socketPath);
    private static native void nativeSurfaceCreated(Object surface);
    private static native void nativeSurfaceChanged(int width, int height, int refreshMilliHz);
    private static native void nativeSurfaceDestroyed();

    private static boolean sStarted;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        getWindow().getAttributes().layoutInDisplayCutoutMode =
                WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;

        SurfaceView view = new SurfaceView(this);
        view.getHolder().addCallback(this);
        setContentView(view);

        if (!sStarted) {
            // Same path as CHAM_SOCKET_PATH; we run as the Termux user.
            nativeStart("/data/data/com.termux/files/usr/tmp/chameleon-0");
            sStarted = true;
        }
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (!hasFocus)
            return;
        WindowInsetsController controller = getWindow().getInsetsController();
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
        Display display = getDisplay();
        float hz = display != null ? display.getRefreshRate() : 60f;
        nativeSurfaceChanged(width, height, Math.round(hz * 1000f));
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        nativeSurfaceDestroyed();
    }
}
