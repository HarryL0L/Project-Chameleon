// SPDX-License-Identifier: GPL-2.0-or-later
package io.github.harryl0l.chameleon;

import android.content.ClipData;
import android.content.ClipDescription;
import android.content.ClipboardManager;
import android.content.Context;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;
import android.system.StructPollfd;
import android.util.Log;

import java.io.ByteArrayOutputStream;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;

/**
 * Clipboard sharing with the desktop (the "Clipboard sharing" setting; the
 * KWin shim's side is shim/core/clipboard.c). Text only, passed in pipes:
 * the desktop's copies arrive as CHAM_CLIPBOARD_DATA and go onto Android's
 * clipboard; Android's new text is announced with CHAM_CLIPBOARD_OFFER and
 * written into each CHAM_CLIPBOARD_REQUEST pipe when a desktop app pastes.
 * Android lets only the app in front read its clipboard, so new text is
 * looked for when this app gets the focus and while it has it.
 */
final class ClipboardShare implements ClipboardManager.OnPrimaryClipChangedListener {
    static {
        System.loadLibrary("chameleon_presenter");
    }

    private static final String TAG = "Chameleon";
    // common/chameleon_proto.h
    private static final int DATA = 6;
    private static final int REQUEST = 7;
    // Android's clipboard goes through Binder, which refuses much more.
    private static final int MAX_BYTES = 1 << 20;
    // A desktop app that stops writing mid-copy must not stall this thread.
    private static final int READ_TIMEOUT_MS = 5000;

    private static native long nativeClipboardWait();
    private static native void nativeClipboardOffer();

    private static ClipboardShare sInstance;

    private final Context mContext;
    private final ClipboardManager mClipboard;
    private final Handler mMain = new Handler(Looper.getMainLooper());
    private volatile boolean mEnabled;
    // The text both sides have: Android's last offered text or the desktop's
    // last copy, so neither is sent back where it came from.
    private volatile String mText;
    // Android's clip last looked at; reading the clip itself makes Android 12+
    // show "pasted from your clipboard", so it is only read when it changed.
    private long mSeen = -1;

    static ClipboardShare get(Context context) {
        if (sInstance == null)
            sInstance = new ClipboardShare(context.getApplicationContext());
        return sInstance;
    }

    private ClipboardShare(Context context) {
        mContext = context;
        mClipboard = context.getSystemService(ClipboardManager.class);
        mClipboard.addPrimaryClipChangedListener(this);
        Thread thread = new Thread(this::serve, "chameleon-clipboard");
        thread.setDaemon(true);
        thread.start();
    }

    void setEnabled(boolean enabled) {
        mEnabled = enabled;
    }

    /** Main thread: Android's clipboard may have new text. */
    @Override
    public void onPrimaryClipChanged() {
        if (!mEnabled)
            return;
        ClipDescription description = mClipboard.getPrimaryClipDescription();
        if (description == null || description.getTimestamp() == mSeen)
            return;
        ClipData clip = mClipboard.getPrimaryClip(); // null without the focus
        if (clip == null || clip.getItemCount() == 0)
            return;
        mSeen = description.getTimestamp();
        CharSequence text = clip.getItemAt(0).coerceToText(mContext);
        if (text == null || text.length() == 0 || text.toString().equals(mText))
            return;
        mText = text.toString();
        nativeClipboardOffer();
    }

    private void serve() {
        for (;;) {
            long next = nativeClipboardWait();
            int type = (int) (next >>> 32);
            try (ParcelFileDescriptor pipe = ParcelFileDescriptor.adoptFd((int) next)) {
                if (!mEnabled)
                    continue;
                if (type == DATA)
                    receive(pipe);
                else if (type == REQUEST)
                    send(pipe);
            } catch (IOException e) {
                Log.w(TAG, "clipboard: " + e);
            }
        }
    }

    /** The desktop copied text: put it on Android's clipboard. */
    private void receive(ParcelFileDescriptor pipe) throws IOException {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        byte[] chunk = new byte[16384];
        StructPollfd readable = new StructPollfd();
        readable.fd = pipe.getFileDescriptor();
        readable.events = (short) OsConstants.POLLIN;
        try (FileInputStream in = new FileInputStream(pipe.getFileDescriptor())) {
            for (;;) {
                try {
                    if (Os.poll(new StructPollfd[] {readable}, READ_TIMEOUT_MS) == 0) {
                        Log.w(TAG, "clipboard: the desktop app stopped sending its text; not copied to Android");
                        return;
                    }
                } catch (ErrnoException e) {
                    throw new IOException(e);
                }
                int n = in.read(chunk);
                if (n <= 0)
                    break;
                bytes.write(chunk, 0, n);
                if (bytes.size() > MAX_BYTES) {
                    Log.w(TAG, "clipboard: the desktop's text is over 1 MiB; not copied to Android");
                    return;
                }
            }
        }
        if (bytes.size() == 0)
            return;
        String text = new String(bytes.toByteArray(), StandardCharsets.UTF_8);
        mText = text;
        mMain.post(() -> {
            try {
                mClipboard.setPrimaryClip(ClipData.newPlainText("Chameleon", text));
            } catch (RuntimeException e) {
                Log.w(TAG, "clipboard: " + e);
            }
        });
    }

    /** A desktop app pastes Android's text. */
    private void send(ParcelFileDescriptor pipe) throws IOException {
        String text = mText;
        if (text == null)
            return;
        try (FileOutputStream out = new FileOutputStream(pipe.getFileDescriptor())) {
            out.write(text.getBytes(StandardCharsets.UTF_8));
        }
    }
}
