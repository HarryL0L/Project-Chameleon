// SPDX-License-Identifier: GPL-2.0-or-later
package io.github.harryl0l.chameleon;

/**
 * A second window of the app: the second screen of the desktop's card, a
 * monitor that is plugged in while this window exists. It runs in a process
 * of its own (see the manifest), with its own copy of the native presenter,
 * its own Termux broker and its own connection from the desktop's service.
 */
public final class ScreenActivity extends PresenterActivity {
    @Override
    int screen() {
        return 1;
    }

    /**
     * Closed for good (swiped out of the recent apps), the monitor is pulled
     * out: the process ends, and with it its connection from the desktop's
     * service. Only this window lives in the process.
     */
    @Override
    protected void onDestroy() {
        boolean closed = isFinishing();
        super.onDestroy();
        if (closed)
            android.os.Process.killProcess(android.os.Process.myPid());
    }
}
