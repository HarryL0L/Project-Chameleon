// SPDX-License-Identifier: GPL-2.0-or-later
package io.github.harryl0l.chameleon;

/**
 * Sends CHAM_INPUT messages (common/chameleon_proto.h) to the connected
 * producer; the KWin shim serves touch, hardware keys and the mouse as evdev
 * devices and turns the rest into org_kde_kwin_fake_input requests.
 */
final class InputSender {
    static {
        System.loadLibrary("chameleon_presenter");
    }

    static final int TOUCH_DOWN = 1; // .. TOUCH_FRAME = 5
    static final int POINTER_MOTION = 6;
    static final int POINTER_ABS = 7;
    static final int KEYSYM = 10;
    static final int KEY = 11;
    static final int MOUSE_BUTTON = 12;
    static final int MOUSE_WHEEL = 13;
    static final int MOUSE_MOTION = 14;
    static final int PAD_DOWN = 15; // .. PAD_FRAME = 19

    // evdev button codes (linux/input-event-codes.h)
    static final int BTN_LEFT = 0x110;
    static final int BTN_RIGHT = 0x111;
    static final int BTN_MIDDLE = 0x112;
    static final int BTN_EXTRA = 0x114;

    static final int AXIS_VERTICAL = 0;
    static final int AXIS_HORIZONTAL = 1;

    private static native void nativeSendInput(int kind, long a, long b);

    private InputSender() {}

    private static long clampFraction(float v, int size) {
        long f = (long) (v / Math.max(size, 1) * 65536f);
        return Math.max(0, Math.min(65535, f));
    }

    /** Position (x, y) in a w x h surface, as 16.16 fractions. */
    static long pos(float x, float y, int w, int h) {
        return clampFraction(x, w) | clampFraction(y, h) << 32;
    }

    /** Movement in surface pixels, as signed 12.20 fractions of the surface. */
    static long delta(float dx, float dy, int w, int h) {
        long ix = (int) (dx / Math.max(w, 1) * 1048576f);
        long iy = (int) (dy / Math.max(h, 1) * 1048576f);
        return (ix & 0xffffffffL) | (iy << 32);
    }

    // A finger message: down is TOUCH_DOWN (the touchscreen) or PAD_DOWN (the
    // touchpad), step one of these offsets from it.
    static final int DOWN = 0, MOTION = 1, UP = 2, CANCEL = 3, FRAME = 4;

    static void finger(int down, int step, int id, long pos) {
        nativeSendInput(down + step, id, pos);
    }

    static void fingersFrame(int down) {
        nativeSendInput(down + FRAME, 0, 0);
    }

    static void fingersCancel(int down) {
        nativeSendInput(down + CANCEL, 0, 0);
        nativeSendInput(down + FRAME, 0, 0);
    }

    static void motion(long delta) {
        nativeSendInput(POINTER_MOTION, 0, delta);
    }

    static void moveTo(long pos) {
        nativeSendInput(POINTER_ABS, 0, pos);
    }

    /** Captured mouse movement, in the mouse's own counts. */
    static void mouseMotion(int dx, int dy) {
        nativeSendInput(MOUSE_MOTION, 0, (dx & 0xffffffffL) | ((long) dy << 32));
    }

    /** A mouse button. */
    static void mouseButton(int button, boolean pressed) {
        nativeSendInput(MOUSE_BUTTON, button, pressed ? 1 : 0);
    }

    /** Mouse wheel movement in notches, + up / right. */
    static void mouseWheel(int axis, float notches) {
        nativeSendInput(MOUSE_WHEEL, axis, Math.round(notches * 120f));
    }

    /** A hardware keyboard's key, by its Linux key code. */
    static void key(int code, boolean pressed) {
        nativeSendInput(KEY, code, pressed ? 1 : 0);
    }

    static void keysym(int keysym, boolean pressed) {
        nativeSendInput(KEYSYM, keysym & 0xffffffffL, pressed ? 1 : 0);
    }
}
