// SPDX-License-Identifier: GPL-2.0-or-later
package io.github.harryl0l.chameleon;

import android.accessibilityservice.AccessibilityService;
import android.view.KeyEvent;
import android.view.accessibility.AccessibilityEvent;

import java.util.HashSet;
import java.util.Set;

/**
 * Optional, turned on in Android's accessibility settings: Android keeps
 * some hardware keyboard shortcuts (Alt+Tab, Meta...) for itself and never
 * delivers them to apps. An accessibility service that filters keys sees
 * them first, so while Chameleon's window has focus its keys go to the
 * desktop, as on a PC. Everything else passes through untouched.
 */
public class KeyService extends AccessibilityService {
    /* Keys whose press Chameleon took, and the activity that took them: their
     * releases go there too, even once focus has moved (a key held while
     * switching apps), or the desktop would see them held down. */
    private final Set<Integer> mHeld = new HashSet<>();
    private PresenterActivity mHolder;

    @Override
    protected boolean onKeyEvent(KeyEvent event) {
        int code = event.getKeyCode();
        if (event.getAction() == KeyEvent.ACTION_UP && mHeld.remove(code)) {
            mHolder.onShortcutKey(event);
            if (mHeld.isEmpty())
                mHolder = null;
            return true;
        }
        PresenterActivity activity = PresenterActivity.sFocused;
        if (activity == null || !activity.onShortcutKey(event))
            return false;
        if (event.getAction() == KeyEvent.ACTION_DOWN) {
            mHeld.add(code);
            mHolder = activity;
        }
        return true;
    }

    @Override
    public void onAccessibilityEvent(AccessibilityEvent event) {}

    @Override
    public void onInterrupt() {}
}
