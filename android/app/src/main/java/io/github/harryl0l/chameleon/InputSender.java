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
    static final int POINTER_ABS = 7;
    static final int KEYSYM = 10;
    static final int KEY = 11;
    static final int MOUSE_BUTTON = 12;
    static final int MOUSE_WHEEL = 13;
    static final int MOUSE_MOTION = 14;
    static final int PAD_DOWN = 15; // .. PAD_FRAME = 19
    static final int TIME = 20;
    static final int TEXT_CHAR = 21;
    static final int TEXT_COMMIT = 22;
    static final int TEXT_PREEDIT = 23;
    static final int TEXT_DELETE = 24;

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

    /**
     * When the events that follow happened: Android's event time in ns
     * (SystemClock.uptimeMillis()'s clock, CLOCK_MONOTONIC), so the desktop
     * sees the real time between them, not when they arrived in a batch.
     */
    static void time(long nanos) {
        nativeSendInput(TIME, 0, nanos);
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

    /** A key of the evdev keyboard, by its Linux key code. */
    static void key(int code, boolean pressed) {
        nativeSendInput(KEY, code, pressed ? 1 : 0);
    }

    /**
     * Text from the Android keyboard: committed, or the word being composed
     * (shown as such where the desktop app's text field takes text).
     */
    static void text(CharSequence text, boolean commit) {
        time(0);
        String s = text.toString();
        for (int i = 0; i < s.length(); ) {
            int cp = s.codePointAt(i);
            i += Character.charCount(cp);
            nativeSendInput(TEXT_CHAR, cp, 0);
        }
        nativeSendInput(commit ? TEXT_COMMIT : TEXT_PREEDIT, 0, 0);
    }

    /** Deletes code points before / after the cursor. */
    static void deleteText(int before, int after) {
        time(0);
        nativeSendInput(TEXT_DELETE, before, after);
    }

    static void keysym(int keysym, boolean pressed) {
        nativeSendInput(KEYSYM, keysym & 0xffffffffL, pressed ? 1 : 0);
    }
}
