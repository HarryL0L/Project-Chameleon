// SPDX-License-Identifier: GPL-2.0-or-later
package io.github.harryl0l.chameleon;

import android.view.InputDevice;
import android.view.KeyCharacterMap;
import android.view.KeyEvent;

import java.util.HashSet;
import java.util.Set;

/**
 * Keyboard input. Keys go by their Linux key codes to the evdev keyboard,
 * as from a real keyboard: a hardware keyboard's by Android's scan codes (the
 * desktop's own layout applies), the extra keys and the soft keyboard's
 * non-text keys (Enter, Backspace, arrows...) by a table. Typed text goes as
 * X keysyms: KWin's fake-input keyboard_keysym looks the keysym up in its
 * keymap (adding Shift as needed) or maps it to a spare key, so any Unicode
 * text works whatever the layout.
 *
 * Ctrl and Alt can be latched from the extra-keys row; a latched modifier
 * applies to the next key or character and is then released. It goes the
 * same way as that key: by key code, or as a keysym with typed text.
 */
final class KeyInput {
    static final int XK_BACKSPACE = 0xff08;
    static final int XK_TAB = 0xff09;
    static final int XK_RETURN = 0xff0d;
    static final int XK_ESCAPE = 0xff1b;
    static final int XK_HOME = 0xff50;
    static final int XK_LEFT = 0xff51;
    static final int XK_UP = 0xff52;
    static final int XK_RIGHT = 0xff53;
    static final int XK_DOWN = 0xff54;
    static final int XK_PAGE_UP = 0xff55;
    static final int XK_PAGE_DOWN = 0xff56;
    static final int XK_END = 0xff57;
    static final int XK_DELETE = 0xffff;
    static final int XK_CONTROL_L = 0xffe3;
    static final int XK_ALT_L = 0xffe9;

    // Linux key codes (linux/input-event-codes.h)
    static final int KEY_ESC = 1;
    static final int KEY_BACKSPACE = 14;
    static final int KEY_TAB = 15;
    static final int KEY_ENTER = 28;
    static final int KEY_LEFTCTRL = 29;
    static final int KEY_LEFTALT = 56;
    static final int KEY_SPACE = 57;
    static final int KEY_F1 = 59;
    static final int KEY_F11 = 87;
    static final int KEY_F12 = 88;
    static final int KEY_KPENTER = 96;
    static final int KEY_HOME = 102;
    static final int KEY_UP = 103;
    static final int KEY_PAGEUP = 104;
    static final int KEY_LEFT = 105;
    static final int KEY_RIGHT = 106;
    static final int KEY_END = 107;
    static final int KEY_DOWN = 108;
    static final int KEY_PAGEDOWN = 109;
    static final int KEY_INSERT = 110;
    static final int KEY_DELETE = 111;

    interface LatchListener {
        void onLatchChanged(boolean ctrl, boolean alt);
    }

    private boolean mCtrl, mAlt;
    private LatchListener mListener;
    /** Hardware keys sent as a whole on press (because of a latch). */
    private final Set<Integer> mTapped = new HashSet<>();

    void setLatchListener(LatchListener listener) {
        mListener = listener;
    }

    void toggleCtrl() {
        mCtrl = !mCtrl;
        notifyLatch();
    }

    void toggleAlt() {
        mAlt = !mAlt;
        notifyLatch();
    }

    private void notifyLatch() {
        if (mListener != null)
            mListener.onLatchChanged(mCtrl, mAlt);
    }

    /** Press and release {@code keysym}, with any latched modifiers held. */
    void tap(int keysym) {
        boolean ctrl = mCtrl, alt = mAlt;
        if (ctrl)
            InputSender.keysym(XK_CONTROL_L, true);
        if (alt)
            InputSender.keysym(XK_ALT_L, true);
        InputSender.keysym(keysym, true);
        InputSender.keysym(keysym, false);
        if (alt)
            InputSender.keysym(XK_ALT_L, false);
        if (ctrl)
            InputSender.keysym(XK_CONTROL_L, false);
        clearLatch();
    }

    /**
     * Press and release the key with Linux key code {@code code}, with any
     * latched modifiers held, at Android time {@code nanos} (0 = now).
     */
    void tapKey(int code, long nanos) {
        boolean ctrl = mCtrl, alt = mAlt;
        InputSender.time(nanos);
        if (ctrl)
            InputSender.key(KEY_LEFTCTRL, true);
        if (alt)
            InputSender.key(KEY_LEFTALT, true);
        InputSender.key(code, true);
        InputSender.key(code, false);
        if (alt)
            InputSender.key(KEY_LEFTALT, false);
        if (ctrl)
            InputSender.key(KEY_LEFTCTRL, false);
        clearLatch();
    }

    private void clearLatch() {
        if (mCtrl || mAlt) {
            mCtrl = mAlt = false;
            notifyLatch();
        }
    }

    /** Types {@code text} (from the soft keyboard). */
    void sendText(CharSequence text) {
        String s = text.toString();
        for (int i = 0; i < s.length(); ) {
            int cp = s.codePointAt(i);
            i += Character.charCount(cp);
            tap(keysymForChar(cp));
        }
    }

    static int keysymForChar(int cp) {
        if (cp == '\n' || cp == '\r')
            return XK_RETURN;
        if (cp == '\t')
            return XK_TAB;
        if (cp == '\b')
            return XK_BACKSPACE;
        if ((cp >= 0x20 && cp <= 0x7e) || (cp >= 0xa0 && cp <= 0xff))
            return cp; // Latin-1 keysyms are the code points
        return 0x01000000 | cp; // Unicode keysym
    }

    static int keysymForKeyCode(int keyCode) {
        switch (keyCode) {
            case KeyEvent.KEYCODE_ENTER:
            case KeyEvent.KEYCODE_NUMPAD_ENTER:
                return XK_RETURN;
            case KeyEvent.KEYCODE_DEL:
                return XK_BACKSPACE;
            case KeyEvent.KEYCODE_FORWARD_DEL:
                return XK_DELETE;
            case KeyEvent.KEYCODE_TAB:
                return XK_TAB;
            case KeyEvent.KEYCODE_ESCAPE:
                return XK_ESCAPE;
            case KeyEvent.KEYCODE_DPAD_LEFT:
                return XK_LEFT;
            case KeyEvent.KEYCODE_DPAD_UP:
                return XK_UP;
            case KeyEvent.KEYCODE_DPAD_RIGHT:
                return XK_RIGHT;
            case KeyEvent.KEYCODE_DPAD_DOWN:
                return XK_DOWN;
            case KeyEvent.KEYCODE_MOVE_HOME:
                return XK_HOME;
            case KeyEvent.KEYCODE_MOVE_END:
                return XK_END;
            case KeyEvent.KEYCODE_PAGE_UP:
                return XK_PAGE_UP;
            case KeyEvent.KEYCODE_PAGE_DOWN:
                return XK_PAGE_DOWN;
            case KeyEvent.KEYCODE_INSERT:
                return 0xff63;
            case KeyEvent.KEYCODE_MENU:
                return 0xff67;
            case KeyEvent.KEYCODE_SYSRQ:
                return 0xff61; // Print
            case KeyEvent.KEYCODE_SCROLL_LOCK:
                return 0xff14;
            case KeyEvent.KEYCODE_NUM_LOCK:
                return 0xff7f;
            case KeyEvent.KEYCODE_CAPS_LOCK:
                return 0xffe5;
            case KeyEvent.KEYCODE_SHIFT_LEFT:
                return 0xffe1;
            case KeyEvent.KEYCODE_SHIFT_RIGHT:
                return 0xffe2;
            case KeyEvent.KEYCODE_CTRL_LEFT:
                return XK_CONTROL_L;
            case KeyEvent.KEYCODE_CTRL_RIGHT:
                return 0xffe4;
            case KeyEvent.KEYCODE_ALT_LEFT:
                return XK_ALT_L;
            case KeyEvent.KEYCODE_ALT_RIGHT:
                return 0xffea;
            case KeyEvent.KEYCODE_META_LEFT:
                return 0xffeb; // Super_L
            case KeyEvent.KEYCODE_META_RIGHT:
                return 0xffec;
            case KeyEvent.KEYCODE_SPACE:
                return ' ';
        }
        if (keyCode >= KeyEvent.KEYCODE_F1 && keyCode <= KeyEvent.KEYCODE_F12)
            return 0xffbe + (keyCode - KeyEvent.KEYCODE_F1);
        return 0;
    }

    /** The Linux key code of a soft keyboard's non-text key, else 0. */
    private static int linuxCodeForKeyCode(int keyCode) {
        switch (keyCode) {
            case KeyEvent.KEYCODE_ENTER:
                return KEY_ENTER;
            case KeyEvent.KEYCODE_NUMPAD_ENTER:
                return KEY_KPENTER;
            case KeyEvent.KEYCODE_DEL:
                return KEY_BACKSPACE;
            case KeyEvent.KEYCODE_FORWARD_DEL:
                return KEY_DELETE;
            case KeyEvent.KEYCODE_TAB:
                return KEY_TAB;
            case KeyEvent.KEYCODE_ESCAPE:
                return KEY_ESC;
            case KeyEvent.KEYCODE_DPAD_LEFT:
                return KEY_LEFT;
            case KeyEvent.KEYCODE_DPAD_UP:
                return KEY_UP;
            case KeyEvent.KEYCODE_DPAD_RIGHT:
                return KEY_RIGHT;
            case KeyEvent.KEYCODE_DPAD_DOWN:
                return KEY_DOWN;
            case KeyEvent.KEYCODE_MOVE_HOME:
                return KEY_HOME;
            case KeyEvent.KEYCODE_MOVE_END:
                return KEY_END;
            case KeyEvent.KEYCODE_PAGE_UP:
                return KEY_PAGEUP;
            case KeyEvent.KEYCODE_PAGE_DOWN:
                return KEY_PAGEDOWN;
            case KeyEvent.KEYCODE_INSERT:
                return KEY_INSERT;
            case KeyEvent.KEYCODE_SPACE:
                return KEY_SPACE;
            case KeyEvent.KEYCODE_F11:
                return KEY_F11;
            case KeyEvent.KEYCODE_F12:
                return KEY_F12;
        }
        if (keyCode >= KeyEvent.KEYCODE_F1 && keyCode <= KeyEvent.KEYCODE_F10)
            return KEY_F1 + (keyCode - KeyEvent.KEYCODE_F1);
        return 0;
    }

    /** The Linux key code of a hardware keyboard's key, else 0. */
    private static int scanCode(KeyEvent event) {
        InputDevice device = event.getDevice();
        if (device == null || device.isVirtual() || device.getKeyboardType() != InputDevice.KEYBOARD_TYPE_ALPHABETIC)
            return 0;
        int code = event.getScanCode();
        return code > 0 && code <= 248 ? code : 0; // KEY_ESC .. KEY_MICMUTE
    }

    /** Whether the key comes from a hardware keyboard (it has a Linux key code). */
    static boolean isHardwareKey(KeyEvent event) {
        return scanCode(event) != 0;
    }

    private static boolean isModifier(int keyCode) {
        return KeyEvent.isModifierKey(keyCode) || keyCode == KeyEvent.KEYCODE_CAPS_LOCK;
    }

    /**
     * A key event from a hardware keyboard or from the soft keyboard's
     * sendKeyEvent(). Returns false for keys that are not ours (volume...).
     */
    boolean onKeyEvent(KeyEvent event) {
        int code = event.getKeyCode();
        if (event.getAction() == KeyEvent.ACTION_MULTIPLE) {
            if (code == KeyEvent.KEYCODE_UNKNOWN && event.getCharacters() != null) {
                sendText(event.getCharacters());
                return true;
            }
            return false;
        }
        int keysym = keysymForKeyCode(code);
        if (keysym == 0) {
            int meta = event.getMetaState()
                    & ~(KeyEvent.META_CTRL_MASK | KeyEvent.META_ALT_MASK | KeyEvent.META_META_MASK);
            int ch = event.getUnicodeChar(meta);
            if (ch == 0)
                return false;
            if ((ch & KeyCharacterMap.COMBINING_ACCENT) == 0)
                keysym = keysymForChar(ch);
        }
        boolean down = event.getAction() == KeyEvent.ACTION_DOWN;
        if (down && event.getRepeatCount() > 0)
            return true; // Wayland clients repeat held keys themselves
        int linux = scanCode(event);
        if (linux == 0)
            linux = linuxCodeForKeyCode(code);
        if (linux != 0 && !mTapped.contains(code)) {
            long nanos = event.getEventTime() * 1_000_000L;
            if (down && !isModifier(code) && (mCtrl || mAlt)) {
                tapKey(linux, nanos); // with Ctrl / Alt latched, the key is tapped with them
                mTapped.add(code);
            } else {
                InputSender.time(nanos);
                InputSender.key(linux, down);
            }
            return true;
        }
        if (keysym == 0)
            return true; // a dead key from the soft keyboard: swallow, the IME composes it
        if (isModifier(code)) {
            InputSender.keysym(keysym, down);
            return true;
        }
        if (down && (mCtrl || mAlt)) {
            tap(keysym);
            mTapped.add(code);
            return true;
        }
        if (!down && mTapped.remove(code))
            return true;
        InputSender.keysym(keysym, down);
        return true;
    }
}
