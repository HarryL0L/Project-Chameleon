package io.github.harryl0l.chameleon;

import android.content.Context;
import android.text.InputType;
import android.view.View;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;

/**
 * Invisible text editor the Android keyboard types into. Text is forwarded
 * as keysyms as it arrives; suggestions are turned off (as in Termux), and
 * any composing text a keyboard still uses is replaced with backspaces as it
 * changes, so what KWin sees always matches what the keyboard shows.
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
        outAttrs.inputType = InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD
                | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS;
        outAttrs.imeOptions = EditorInfo.IME_FLAG_NO_FULLSCREEN | EditorInfo.IME_FLAG_NO_EXTRACT_UI
                | EditorInfo.IME_ACTION_NONE;
        return new BaseInputConnection(this, false) {
            private String mComposing = "";

            private void replaceComposing(String text) {
                int common = 0;
                int max = Math.min(mComposing.length(), text.length());
                while (common < max && mComposing.charAt(common) == text.charAt(common))
                    common++;
                int erase = mComposing.codePointCount(common, mComposing.length());
                for (int i = 0; i < erase; i++)
                    mKeys.tap(KeyInput.XK_BACKSPACE);
                mKeys.sendText(text.substring(common));
            }

            @Override
            public boolean commitText(CharSequence text, int newCursorPosition) {
                replaceComposing(text.toString());
                mComposing = "";
                return true;
            }

            @Override
            public boolean setComposingText(CharSequence text, int newCursorPosition) {
                replaceComposing(text.toString());
                mComposing = text.toString();
                return true;
            }

            @Override
            public boolean finishComposingText() {
                mComposing = "";
                return true;
            }

            @Override
            public boolean deleteSurroundingText(int beforeLength, int afterLength) {
                for (int i = 0; i < beforeLength; i++)
                    mKeys.tap(KeyInput.XK_BACKSPACE);
                for (int i = 0; i < afterLength; i++)
                    mKeys.tap(KeyInput.XK_DELETE);
                return true;
            }

            @Override
            public boolean deleteSurroundingTextInCodePoints(int beforeLength, int afterLength) {
                return deleteSurroundingText(beforeLength, afterLength);
            }
        };
    }
}
