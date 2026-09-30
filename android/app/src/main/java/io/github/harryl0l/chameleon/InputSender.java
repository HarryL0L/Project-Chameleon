// SPDX-License-Identifier: GPL-2.0-or-later
package io.github.harryl0l.chameleon;

/**
 * Sends CHAM_INPUT messages (common/chameleon_proto.h) to the connected
 * producer; the KWin shim turns them into org_kde_kwin_fake_input requests.
 */
final class InputSender {
    static {
        System.loadLibrary("chameleon_presenter");
    }

    static final int TOUCH_DOWN = 1;
    static final int TOUCH_MOTION = 2;
    static final int TOUCH_UP = 3;
    static final int TOUCH_CANCEL = 4;
    static final int TOUCH_FRAME = 5;
    static final int POINTER_MOTION = 6;
    static final int POINTER_ABS = 7;
    static final int BUTTON = 8;
    static final int AXIS = 9;
    static final int KEYSYM = 10;

    // evdev button codes (linux/input-event-codes.h)
    static final int BTN_LEFT = 0x110;
    static final int BTN_RIGHT = 0x111;
    static final int BTN_MIDDLE = 0x112;
    static final int BTN_SIDE = 0x113;
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

    static void touch(int kind, int id, long pos) {
        nativeSendInput(kind, id, pos);
    }

    static void touchFrame() {
        nativeSendInput(TOUCH_FRAME, 0, 0);
    }

    static void touchCancel() {
        nativeSendInput(TOUCH_CANCEL, 0, 0);
        nativeSendInput(TOUCH_FRAME, 0, 0);
    }

    static void motion(long delta) {
        nativeSendInput(POINTER_MOTION, 0, delta);
    }

    static void moveTo(long pos) {
        nativeSendInput(POINTER_ABS, 0, pos);
    }

    static void button(int button, boolean pressed) {
        nativeSendInput(BUTTON, button, pressed ? 1 : 0);
    }

    static void click(int button) {
        button(button, true);
        button(button, false);
    }

    /** Scroll by {@code value} (wl_pointer axis units, ~15 per wheel notch). */
    static void axis(int axis, float value) {
        nativeSendInput(AXIS, axis, (long) (value * 256f));
    }

    static void keysym(int keysym, boolean pressed) {
        nativeSendInput(KEYSYM, keysym & 0xffffffffL, pressed ? 1 : 0);
    }
}
