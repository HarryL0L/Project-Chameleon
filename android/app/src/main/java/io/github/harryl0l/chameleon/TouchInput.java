// SPDX-License-Identifier: GPL-2.0-or-later
package io.github.harryl0l.chameleon;

import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.view.HapticFeedbackConstants;
import android.view.InputDevice;
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
 * A mouse always drives the pointer directly, in either mode.
 */
final class TouchInput implements View.OnTouchListener, View.OnGenericMotionListener {
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

    private static int evdevButton(int androidButton) {
        switch (androidButton) {
            case MotionEvent.BUTTON_PRIMARY:
                return InputSender.BTN_LEFT;
            case MotionEvent.BUTTON_SECONDARY:
                return InputSender.BTN_RIGHT;
            case MotionEvent.BUTTON_TERTIARY:
                return InputSender.BTN_MIDDLE;
            case MotionEvent.BUTTON_BACK:
                return InputSender.BTN_SIDE;
            case MotionEvent.BUTTON_FORWARD:
                return InputSender.BTN_EXTRA;
        }
        return 0;
    }

    private boolean mouse(View v, MotionEvent e) {
        int w = v.getWidth(), h = v.getHeight();
        if (e.getActionMasked() == MotionEvent.ACTION_SCROLL) {
            float vs = e.getAxisValue(MotionEvent.AXIS_VSCROLL);
            float hs = e.getAxisValue(MotionEvent.AXIS_HSCROLL);
            if (vs != 0)
                InputSender.axis(InputSender.AXIS_VERTICAL, -vs * 15f);
            if (hs != 0)
                InputSender.axis(InputSender.AXIS_HORIZONTAL, hs * 15f);
            return true;
        }
        InputSender.moveTo(InputSender.pos(e.getX(), e.getY(), w, h));
        int buttons = e.getButtonState();
        int changed = buttons ^ mMouseButtons;
        for (int bit = 1; changed != 0 && bit != 0; bit <<= 1) {
            if ((changed & bit) == 0)
                continue;
            changed &= ~bit;
            int button = evdevButton(bit);
            if (button != 0)
                InputSender.button(button, (buttons & bit) != 0);
        }
        mMouseButtons = buttons;
        return true;
    }
}
