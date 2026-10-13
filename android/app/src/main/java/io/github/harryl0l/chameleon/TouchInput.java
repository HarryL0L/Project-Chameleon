// SPDX-License-Identifier: GPL-2.0-or-later
package io.github.harryl0l.chameleon;

import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.view.HapticFeedbackConstants;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;

/**
 * Turns touches and mouse events on the surface into input for KWin.
 *
 * Direct touch: every finger is a touch point where it lands.
 * Trackpad: one finger moves the pointer, a tap clicks, tap then drag or a
 * long press holds the button (moving windows, drag and drop, selection),
 * two-finger tap = right click,
 * three-finger tap = middle click, two-finger drag scrolls.
 * A mouse always drives the pointer directly, in either mode: the first
 * mouse event captures it (pointer capture, as games do), so the desktop gets
 * its own counts and buttons as from a real mouse, without Android's pointer
 * handling in between. Android gives the mouse back when the app loses focus;
 * the next mouse event captures it again.
 */
final class TouchInput implements View.OnTouchListener, View.OnGenericMotionListener,
        View.OnCapturedPointerListener {
    enum Mode { DIRECT, TRACKPAD }

    private static final long TAP_MS = 250;
    private static final long DOUBLE_TAP_MS = 300;
    private static final long LONG_PRESS_MS = 500;

    private Mode mMode = Mode.DIRECT;
    private float mSpeed = 1f;
    private boolean mTapToClick = true;
    private final float mTouchSlop;

    // trackpad state
    private int mMaxPointers;
    private long mDownTime, mLastTapUp;
    private float mLastX, mLastY, mTravel;
    private boolean mDragging;
    private float mScrollX, mScrollY;
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private View mView;
    private final Runnable mLongPress = this::longPress;

    private int mMouseButtons;
    private float mMouseRestX, mMouseRestY; // captured movement short of a whole count

    TouchInput(float density) {
        mTouchSlop = 8 * density;
    }

    void configure(Mode mode, float speed, boolean tapToClick) {
        if (mode != mMode)
            reset();
        mMode = mode;
        mSpeed = speed;
        mTapToClick = tapToClick;
    }

    /** One finger held still: press the button, as tap-then-drag does. */
    private void longPress() {
        if (mDragging || mMaxPointers != 1 || mTravel >= mTouchSlop)
            return;
        mDragging = true;
        InputSender.button(InputSender.BTN_LEFT, true);
        if (mView != null)
            mView.performHapticFeedback(HapticFeedbackConstants.LONG_PRESS);
    }

    private void reset() {
        mHandler.removeCallbacks(mLongPress);
        if (mDragging)
            InputSender.button(InputSender.BTN_LEFT, false);
        mDragging = false;
        mMaxPointers = 0;
        mLastTapUp = 0;
    }

    private static boolean isMouse(MotionEvent e) {
        return e.isFromSource(InputDevice.SOURCE_MOUSE) && e.getToolType(0) == MotionEvent.TOOL_TYPE_MOUSE;
    }

    @Override
    public boolean onTouch(View v, MotionEvent e) {
        if (isMouse(e))
            return mouse(v, e);
        if (mMode == Mode.TRACKPAD)
            trackpad(v, e);
        else
            direct(v, e);
        return true;
    }

    @Override
    public boolean onGenericMotion(View v, MotionEvent e) {
        return isMouse(e) && mouse(v, e);
    }

    // ---- direct touch ----

    private void direct(View v, MotionEvent e) {
        int w = v.getWidth(), h = v.getHeight();
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                int i = e.getActionIndex();
                InputSender.touch(InputSender.TOUCH_DOWN, e.getPointerId(i),
                        InputSender.pos(e.getX(i), e.getY(i), w, h));
                InputSender.touchFrame();
                break;
            }
            case MotionEvent.ACTION_MOVE:
                for (int i = 0; i < e.getPointerCount(); i++)
                    InputSender.touch(InputSender.TOUCH_MOTION, e.getPointerId(i),
                            InputSender.pos(e.getX(i), e.getY(i), w, h));
                InputSender.touchFrame();
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
                InputSender.touch(InputSender.TOUCH_UP, e.getPointerId(e.getActionIndex()), 0);
                InputSender.touchFrame();
                break;
            case MotionEvent.ACTION_CANCEL:
                InputSender.touchCancel();
                break;
        }
    }

    // ---- trackpad ----

    /** Centroid of the fingers still down, leaving out {@code skip}. */
    private void centroid(MotionEvent e, int skip) {
        float x = 0, y = 0;
        int n = 0;
        for (int i = 0; i < e.getPointerCount(); i++) {
            if (i == skip)
                continue;
            x += e.getX(i);
            y += e.getY(i);
            n++;
        }
        if (n > 0) {
            mLastX = x / n;
            mLastY = y / n;
        }
    }

    private void trackpad(View v, MotionEvent e) {
        int w = v.getWidth(), h = v.getHeight();
        long now = SystemClock.uptimeMillis();
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                mMaxPointers = 1;
                mDownTime = now;
                mTravel = 0;
                mScrollX = mScrollY = 0;
                centroid(e, -1);
                if (mTapToClick && now - mLastTapUp < DOUBLE_TAP_MS) {
                    mDragging = true; // tap, then touch again: hold the button
                    InputSender.button(InputSender.BTN_LEFT, true);
                } else {
                    mView = v;
                    mHandler.postDelayed(mLongPress, LONG_PRESS_MS);
                }
                break;
            case MotionEvent.ACTION_POINTER_DOWN:
                mHandler.removeCallbacks(mLongPress);
                mMaxPointers = Math.max(mMaxPointers, e.getPointerCount());
                centroid(e, -1);
                break;
            case MotionEvent.ACTION_POINTER_UP:
                centroid(e, e.getActionIndex());
                break;
            case MotionEvent.ACTION_MOVE: {
                float x = mLastX, y = mLastY;
                centroid(e, -1);
                float dx = mLastX - x, dy = mLastY - y;
                mTravel += Math.abs(dx) + Math.abs(dy);
                if (mTravel >= mTouchSlop)
                    mHandler.removeCallbacks(mLongPress);
                if (e.getPointerCount() == 1) {
                    if (dx != 0 || dy != 0)
                        InputSender.motion(InputSender.delta(dx * mSpeed, dy * mSpeed, w, h));
                } else if (e.getPointerCount() == 2 && !mDragging) {
                    // natural scrolling: content follows the fingers
                    mScrollX -= dx;
                    mScrollY -= dy;
                    if (Math.abs(mScrollY) >= 1) {
                        InputSender.axis(InputSender.AXIS_VERTICAL, mScrollY);
                        mScrollY = 0;
                    }
                    if (Math.abs(mScrollX) >= 1) {
                        InputSender.axis(InputSender.AXIS_HORIZONTAL, mScrollX);
                        mScrollX = 0;
                    }
                }
                break;
            }
            case MotionEvent.ACTION_UP: {
                mHandler.removeCallbacks(mLongPress);
                boolean tap = mTapToClick && mTravel < mTouchSlop && now - mDownTime < TAP_MS;
                if (mDragging) {
                    InputSender.button(InputSender.BTN_LEFT, false); // end of the drag (or a double click)
                    mDragging = false;
                    mLastTapUp = 0;
                } else if (tap) {
                    int button = mMaxPointers == 1 ? InputSender.BTN_LEFT
                            : mMaxPointers == 2 ? InputSender.BTN_RIGHT : InputSender.BTN_MIDDLE;
                    InputSender.click(button);
                    mLastTapUp = mMaxPointers == 1 ? now : 0;
                } else {
                    mLastTapUp = 0;
                }
                break;
            }
            case MotionEvent.ACTION_CANCEL:
                reset();
                break;
        }
    }

    // ---- mouse ----

    /** The mouse's Back button is left out: see mouseBackKey(). */
    private static int evdevButton(int androidButton) {
        switch (androidButton) {
            case MotionEvent.BUTTON_PRIMARY:
                return InputSender.BTN_LEFT;
            case MotionEvent.BUTTON_SECONDARY:
                return InputSender.BTN_RIGHT;
            case MotionEvent.BUTTON_TERTIARY:
                return InputSender.BTN_MIDDLE;
            case MotionEvent.BUTTON_FORWARD:
                return InputSender.BTN_EXTRA;
        }
        return 0;
    }

    /** A mouse the app hasn't captured yet: absolute, then capture it. */
    private boolean mouse(View v, MotionEvent e) {
        if (!wheel(e)) {
            InputSender.moveTo(InputSender.pos(e.getX(), e.getY(), v.getWidth(), v.getHeight()));
            buttons(e.getButtonState());
        }
        if (v.hasWindowFocus() && !v.hasPointerCapture())
            v.requestPointerCapture();
        return true;
    }

    /**
     * A Back key from a mouse is its right button: many phones (realme,
     * Xiaomi, Asus, older Android) report the right button as Back, and send
     * a Back key for it, to every app. A real side Back button acts the same.
     */
    static boolean mouseBackKey(KeyEvent e) {
        if (e.getKeyCode() != KeyEvent.KEYCODE_BACK
                || !(e.isFromSource(InputDevice.SOURCE_MOUSE) || e.isFromSource(InputDevice.SOURCE_MOUSE_RELATIVE)))
            return false;
        if (e.getRepeatCount() == 0 && e.getAction() != KeyEvent.ACTION_MULTIPLE)
            InputSender.mouseButton(InputSender.BTN_RIGHT, e.getAction() == KeyEvent.ACTION_DOWN);
        return true;
    }

    @Override
    public boolean onCapturedPointer(View v, MotionEvent e) {
        if (!e.isFromSource(InputDevice.SOURCE_MOUSE_RELATIVE))
            return false;
        if (wheel(e))
            return true;
        float dx = mMouseRestX + e.getX(), dy = mMouseRestY + e.getY();
        for (int i = 0; i < e.getHistorySize(); i++) {
            dx += e.getHistoricalX(i);
            dy += e.getHistoricalY(i);
        }
        int ix = (int) dx, iy = (int) dy;
        mMouseRestX = dx - ix;
        mMouseRestY = dy - iy;
        if (ix != 0 || iy != 0)
            InputSender.mouseMotion(ix, iy);
        buttons(e.getButtonState());
        return true;
    }

    private static boolean wheel(MotionEvent e) {
        if (e.getActionMasked() != MotionEvent.ACTION_SCROLL)
            return false;
        float vs = e.getAxisValue(MotionEvent.AXIS_VSCROLL);
        float hs = e.getAxisValue(MotionEvent.AXIS_HSCROLL);
        if (vs != 0)
            InputSender.mouseWheel(InputSender.AXIS_VERTICAL, vs);
        if (hs != 0)
            InputSender.mouseWheel(InputSender.AXIS_HORIZONTAL, hs);
        return true;
    }

    private void buttons(int buttons) {
        int changed = buttons ^ mMouseButtons;
        for (int bit = 1; changed != 0 && bit != 0; bit <<= 1) {
            if ((changed & bit) == 0)
                continue;
            changed &= ~bit;
            int button = evdevButton(bit);
            if (button != 0)
                InputSender.mouseButton(button, (buttons & bit) != 0);
        }
        mMouseButtons = buttons;
    }
}
