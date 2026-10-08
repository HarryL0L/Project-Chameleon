// SPDX-License-Identifier: GPL-2.0-or-later
package io.github.harryl0l.chameleon;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Rect;
import android.os.Handler;
import android.os.Looper;
import android.util.TypedValue;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewConfiguration;
import android.view.ViewGroup;
import android.widget.ImageButton;
import android.widget.LinearLayout;

import java.util.ArrayList;
import java.util.List;

/**
 * The app's buttons over the desktop, folded into a translucent Chameleon
 * icon: a tap unfolds the keyboard and settings buttons beside it, and after
 * a few seconds untouched it folds again. The icon can be dragged anywhere;
 * it then sticks to the nearest side edge, at the height it was left, and
 * keeps that place (as a fraction of the height) across rotations and
 * restarts. It goes right up to the edges, stepping aside only where it
 * would cover the camera cutout.
 *
 * It is moved with layout margins, not a translation: the window lies over
 * the desktop's SurfaceView, and the area of the window the system shows is
 * worked out from laid-out positions, so a translated menu is cut off where
 * it was last laid out.
 */
final class FloatingMenu extends LinearLayout {
    private static final long FOLD_MS = 4000;
    private static final float FOLDED_ALPHA = 0.7f, UNFOLDED_ALPHA = 0.9f;
    private static final String PREF_RIGHT = "menu_right", PREF_Y = "menu_y";

    private final SharedPreferences mPrefs;
    private final ImageButton mIcon;
    private final View[] mButtons;
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private final Runnable mFold = () -> setUnfolded(false);
    private final int mMargin, mSlop;
    private int mBottom; // the keyboard and its key bar, from the activity
    private final List<Rect> mCutouts = new ArrayList<>(); // camera holes, in window coordinates
    private boolean mRight;
    private float mY; // top of the menu, as a fraction of the free height
    private boolean mUnfolded;

    // drag state
    private float mDownRawX, mDownRawY, mDownX, mDownY;
    private boolean mDragging;

    FloatingMenu(Context context, SharedPreferences prefs, View... buttons) {
        super(context);
        mPrefs = prefs;
        mButtons = buttons;
        mMargin = dp(8);
        mSlop = ViewConfiguration.get(context).getScaledTouchSlop();
        mRight = prefs.getBoolean(PREF_RIGHT, true);
        mY = prefs.getFloat(PREF_Y, 0f);
        setOrientation(HORIZONTAL);

        mIcon = new ImageButton(context);
        mIcon.setImageResource(R.drawable.ic_launcher_monochrome);
        mIcon.setScaleType(ImageButton.ScaleType.FIT_CENTER);
        mIcon.setPadding(0, 0, 0, 0);
        mIcon.setBackgroundResource(R.drawable.toolbar_button);
        mIcon.setContentDescription(context.getString(R.string.menu));
        mIcon.setFocusable(false);
        mIcon.setOnTouchListener(this::onIconTouch);
        for (View b : buttons) {
            b.setVisibility(GONE);
            b.setOnTouchListener((v, e) -> { // a button used: fold again later
                if (e.getActionMasked() == MotionEvent.ACTION_DOWN)
                    scheduleFold();
                return false;
            });
        }
        arrange();
        setAlpha(FOLDED_ALPHA);
        // Not from inside the layout pass: place() changes the layout.
        addOnLayoutChangeListener((v, l, t, r, b, ol, ot, or, ob) -> post(this::place));
    }

    private int dp(float v) {
        return Math.round(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics()));
    }

    /**
     * What the menu must stay clear of: the keyboard and key bar below, and
     * the camera cutouts. Called on every layout of the window, so the menu
     * is placed again whenever the window changes, even when this doesn't.
     */
    void setObstacles(int bottom, List<Rect> cutouts) {
        mBottom = bottom;
        mCutouts.clear();
        mCutouts.addAll(cutouts);
        place();
    }

    /** The icon on the outer side, the buttons towards the middle of the screen. */
    private void arrange() {
        removeAllViews();
        LayoutParams icon = new LayoutParams(dp(40), dp(40));
        if (!mRight)
            addView(mIcon, icon);
        for (View b : mButtons) {
            LayoutParams lp = new LayoutParams(dp(40), dp(40));
            lp.setMarginStart(dp(8));
            if (mRight) {
                lp.setMarginStart(0);
                lp.setMarginEnd(dp(8));
            }
            addView(b, lp);
        }
        if (mRight)
            addView(mIcon, icon);
    }

    private void setUnfolded(boolean unfolded) {
        mHandler.removeCallbacks(mFold);
        mUnfolded = unfolded;
        for (View b : mButtons)
            b.setVisibility(unfolded ? VISIBLE : GONE);
        setAlpha(unfolded ? UNFOLDED_ALPHA : FOLDED_ALPHA);
        if (unfolded)
            scheduleFold();
    }

    private void scheduleFold() {
        mHandler.removeCallbacks(mFold);
        mHandler.postDelayed(mFold, FOLD_MS);
    }

    private int freeHeight() {
        ViewGroup parent = (ViewGroup) getParent();
        return parent == null ? 0 : parent.getHeight() - mBottom - getHeight() - 2 * mMargin;
    }

    /** Puts the menu at its edge and height, inside the free area. */
    private void place() {
        ViewGroup parent = (ViewGroup) getParent();
        if (parent == null || mDragging)
            return;
        if (getWidth() == 0) {
            // Squeezed to nothing by a margin from a wider window (rotated
            // to portrait on the right edge): back to the corner to be
            // measured again, then placed by the next layout.
            moveTo(0, 0);
            return;
        }
        int x = mRight ? parent.getWidth() - mMargin - getWidth() : mMargin;
        int y = mMargin + Math.round(Math.max(0, freeHeight()) * mY);
        y = Math.max(0, Math.min(y, parent.getHeight() - getHeight()));
        // Away from the edge, past any camera hole it would cover.
        for (Rect c : mCutouts) {
            if (Rect.intersects(c, new Rect(x - mMargin, y - mMargin, x + getWidth() + mMargin, y + getHeight() + mMargin)))
                x = mRight ? Math.min(x, c.left - mMargin - getWidth()) : Math.max(x, c.right + mMargin);
        }
        moveTo(Math.max(0, Math.min(x, parent.getWidth() - getWidth())), y);
    }

    private void moveTo(float x, float y) {
        MarginLayoutParams lp = (MarginLayoutParams) getLayoutParams();
        int left = Math.round(x), top = Math.round(y);
        if (lp == null || (lp.leftMargin == left && lp.topMargin == top))
            return;
        lp.leftMargin = left;
        lp.topMargin = top;
        setLayoutParams(lp);
    }

    private float left() {
        return ((MarginLayoutParams) getLayoutParams()).leftMargin;
    }

    private float top() {
        return ((MarginLayoutParams) getLayoutParams()).topMargin;
    }

    private boolean onIconTouch(View v, MotionEvent e) {
        ViewGroup parent = (ViewGroup) getParent();
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                mDownRawX = e.getRawX();
                mDownRawY = e.getRawY();
                mDownX = left();
                mDownY = top();
                mDragging = false;
                mHandler.removeCallbacks(mFold);
                return true;
            case MotionEvent.ACTION_MOVE: {
                float dx = e.getRawX() - mDownRawX, dy = e.getRawY() - mDownRawY;
                if (!mDragging && Math.hypot(dx, dy) > mSlop) {
                    mDragging = true;
                    setAlpha(1f); // easy to follow under the finger
                }
                if (mDragging && parent != null) {
                    moveTo(clamp(mDownX + dx, 0, parent.getWidth() - getWidth()),
                            clamp(mDownY + dy, 0, parent.getHeight() - getHeight()));
                }
                return true;
            }
            case MotionEvent.ACTION_UP:
                if (mDragging) {
                    mDragging = false;
                    setAlpha(mUnfolded ? UNFOLDED_ALPHA : FOLDED_ALPHA);
                    stick();
                    if (mUnfolded)
                        scheduleFold();
                } else {
                    setUnfolded(!mUnfolded);
                }
                return true;
            case MotionEvent.ACTION_CANCEL:
                mDragging = false;
                setAlpha(mUnfolded ? UNFOLDED_ALPHA : FOLDED_ALPHA);
                place();
                return true;
        }
        return false;
    }

    /** After a drag: the nearest side edge, and the height it was left at. */
    private void stick() {
        ViewGroup parent = (ViewGroup) getParent();
        if (parent == null)
            return;
        boolean right = left() + getWidth() / 2f > parent.getWidth() / 2f;
        int free = freeHeight();
        mY = free > 0 ? clamp((top() - mMargin) / free, 0, 1) : 0;
        if (right != mRight) {
            mRight = right;
            arrange(); // relayout places it
        } else {
            place();
        }
        mPrefs.edit().putBoolean(PREF_RIGHT, mRight).putFloat(PREF_Y, mY).apply();
    }

    private static float clamp(float v, float min, float max) {
        return Math.max(min, Math.min(max, v));
    }
}
