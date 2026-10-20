// SPDX-License-Identifier: GPL-2.0-or-later
package io.github.harryl0l.chameleon;

import android.content.Context;
import android.text.InputType;
import android.view.View;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;

/**
 * Invisible text editor the Android keyboard types into. Its text, the word
 * being composed and deletions go to the shim as they arrive, which gives
 * them to the desktop app's text field like a Linux on-screen keyboard (or
 * types them as keys).
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
        // (the shim handles composing text); Enter stays a key.
        outAttrs.inputType = InputType.TYPE_CLASS_TEXT;
        outAttrs.imeOptions = EditorInfo.IME_FLAG_NO_FULLSCREEN | EditorInfo.IME_FLAG_NO_EXTRACT_UI
                | EditorInfo.IME_ACTION_NONE | EditorInfo.IME_FLAG_NO_ENTER_ACTION;
        return new BaseInputConnection(this, false) {
            private String mComposing = "";

            @Override
            public boolean commitText(CharSequence text, int newCursorPosition) {
                mKeys.sendText(text);
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
                mComposing = "";
                return true;
            }

            @Override
            public boolean deleteSurroundingText(int beforeLength, int afterLength) {
                InputSender.deleteText(beforeLength, afterLength);
                return true;
            }

            @Override
            public boolean deleteSurroundingTextInCodePoints(int beforeLength, int afterLength) {
                return deleteSurroundingText(beforeLength, afterLength);
            }
        };
    }
}
