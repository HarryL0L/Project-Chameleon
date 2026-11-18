// SPDX-License-Identifier: GPL-2.0-or-later
package io.github.harryl0l.chameleon;

import android.content.Context;
import android.icu.text.BreakIterator;
import android.text.InputType;
import android.view.KeyEvent;
import android.view.View;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;

/**
 * Invisible text editor the Android keyboard types into. Its text, the word
 * being composed and deletions go to the shim as they arrive, which gives
 * them to the desktop app's text field like a Linux on-screen keyboard (or
 * types them as keys). Deletions become key presses there, so they are
 * counted in characters as the user sees them, from the text this editor
 * was given; keyboards count in UTF-16 units or code points.
 */
final class ImeView extends View {
    private final KeyInput mKeys;

    ImeView(Context context, KeyInput keys) {
        super(context);
        mKeys = keys;
        setFocusable(true);
        setFocusableInTouchMode(true);
    }

    @Override
    public boolean onCheckIsTextEditor() {
        return true;
    }

    @Override
    public InputConnection onCreateInputConnection(EditorInfo outAttrs) {
        // A plain text field: the keyboard composes, suggests and offers emoji
        // (the shim handles composing text); Enter stays a key. Multi-line, so
        // keyboards keep the line breaks of pasted text.
        outAttrs.inputType = InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_MULTI_LINE;
        outAttrs.imeOptions = EditorInfo.IME_FLAG_NO_FULLSCREEN | EditorInfo.IME_FLAG_NO_EXTRACT_UI
                | EditorInfo.IME_ACTION_NONE | EditorInfo.IME_FLAG_NO_ENTER_ACTION;
        return new BaseInputConnection(this, false) {
            private String mComposing = "";
            private final StringBuilder mBefore = new StringBuilder(); // committed text before the cursor

            private void committed(CharSequence text) {
                mBefore.append(text);
                if (mBefore.length() > 4096)
                    mBefore.delete(0, mBefore.length() - 1024);
            }

            /* The characters the last `units` UTF-16 units before the cursor
             * make up, and forgets them; text it wasn't given (from before)
             * counts one per unit. */
            private int charactersBefore(int units) {
                int known = Math.min(units, mBefore.length()), start = mBefore.length() - known, count = 0;
                BreakIterator it = BreakIterator.getCharacterInstance();
                it.setText(mBefore.substring(start));
                for (int b = it.next(); b != BreakIterator.DONE; b = it.next())
                    count++;
                mBefore.setLength(start);
                return count + units - known;
            }

            @Override
            public boolean commitText(CharSequence text, int newCursorPosition) {
                mKeys.sendText(text);
                committed(text);
                mComposing = "";
                return true;
            }

            @Override
            public boolean setComposingText(CharSequence text, int newCursorPosition) {
                mComposing = text.toString();
                InputSender.text(mComposing, false);
                return true;
            }

            @Override
            public boolean finishComposingText() {
                if (!mComposing.isEmpty())
                    InputSender.text(mComposing, true); /* keeps the word as it is */
                committed(mComposing);
                mComposing = "";
                return true;
            }

            /* The keyboard's Backspace as a key (the desktop gets it from
             * the activity): one character less before the cursor here too. */
            @Override
            public boolean sendKeyEvent(KeyEvent event) {
                if (event.getAction() == KeyEvent.ACTION_DOWN && event.getKeyCode() == KeyEvent.KEYCODE_DEL
                        && mBefore.length() > 0) {
                    BreakIterator it = BreakIterator.getCharacterInstance();
                    it.setText(mBefore.toString());
                    int start = it.preceding(mBefore.length());
                    mBefore.setLength(start == BreakIterator.DONE ? 0 : start);
                }
                return super.sendKeyEvent(event);
            }

            /* Lengths in UTF-16 units; the text after the cursor is unknown, one character per unit. */
            @Override
            public boolean deleteSurroundingText(int beforeLength, int afterLength) {
                InputSender.deleteText(charactersBefore(Math.max(beforeLength, 0)), Math.max(afterLength, 0));
                return true;
            }

            @Override
            public boolean deleteSurroundingTextInCodePoints(int beforeLength, int afterLength) {
                int units = 0;
                for (int i = mBefore.length(); beforeLength > 0; beforeLength--) {
                    if (i == 0) { /* text it wasn't given: one unit each */
                        units = (int) Math.min((long) units + beforeLength, Integer.MAX_VALUE);
                        break;
                    }
                    int n = Character.charCount(mBefore.codePointBefore(i));
                    units += n;
                    i -= n;
                }
                InputSender.deleteText(charactersBefore(units), Math.max(afterLength, 0));
                return true;
            }
        };
    }
}
