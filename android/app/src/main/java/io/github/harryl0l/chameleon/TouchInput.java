// SPDX-License-Identifier: GPL-2.0-or-later
package io.github.harryl0l.chameleon;

import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;

/**
 * Turns touches and mouse events on the surface into input for the desktop.
 *
 * Direct touch: every finger is a touch point where it lands (the shim's
 * touchscreen). Trackpad: the surface is a touchpad (the shim's touchpad),
 * so the desktop's touchpad handling applies: tap to click, two-finger
 * scrolling, gestures.
 * A mouse always drives the pointer directly, in either mode: the first
 * mouse event captures it (pointer capture, as games do), so the desktop gets
 * its own counts and buttons as from a real mouse, without Android's pointer
 * handling in between. Android gives the mouse back when the app loses focus;
 * the next mouse event captures it again.
 */
final class TouchInput implements View.OnTouchListener, View.OnGenericMotionListener,
        View.OnCapturedPointerListener {
    enum Mode { DIRECT, TRACKPAD }

    private Mode mMode = Mode.DIRECT;
    private int mFingersTo = InputSender.TOUCH_DOWN; // the kind of the fingers' messages

    private int mMouseButtons;
    private float mMouseRestX, mMouseRestY; // captured movement short of a whole count

    void configure(Mode mode) {
        if (mode == mMode)
            return;
        InputSender.fingersCancel(mFingersTo); // fingers down now belong to the old device
        mMode = mode;
        mFingersTo = mode == Mode.TRACKPAD ? InputSender.PAD_DOWN : InputSender.TOUCH_DOWN;
    }

    private static boolean isMouse(MotionEvent e) {
        return e.isFromSource(InputDevice.SOURCE_MOUSE) && e.getToolType(0) == MotionEvent.TOOL_TYPE_MOUSE;
    }

    @Override
    public boolean onTouch(View v, MotionEvent e) {
        if (isMouse(e))
            return mouse(v, e);
        fingers(v, e, mFingersTo);
        return true;
    }

    @Override
    public boolean onGenericMotion(View v, MotionEvent e) {
        return isMouse(e) && mouse(v, e);
    }

    // ---- touch: every finger where it is, to the touchscreen or touchpad ----

    private static void fingers(View v, MotionEvent e, int down) {
        int w = v.getWidth(), h = v.getHeight();
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                int i = e.getActionIndex();
                InputSender.finger(down, InputSender.DOWN, e.getPointerId(i),
                        InputSender.pos(e.getX(i), e.getY(i), w, h));
                InputSender.fingersFrame(down);
                break;
            }
            case MotionEvent.ACTION_MOVE:
                for (int i = 0; i < e.getPointerCount(); i++)
                    InputSender.finger(down, InputSender.MOTION, e.getPointerId(i),
                            InputSender.pos(e.getX(i), e.getY(i), w, h));
                InputSender.fingersFrame(down);
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
                InputSender.finger(down, InputSender.UP, e.getPointerId(e.getActionIndex()), 0);
                InputSender.fingersFrame(down);
                break;
            case MotionEvent.ACTION_CANCEL:
                InputSender.fingersCancel(down);
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
