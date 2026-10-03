package io.github.harryl0l.chameleon;

import android.content.Intent;
import android.net.LocalServerSocket;
import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Parcel;
import android.os.Process;
import android.os.RemoteException;
import android.system.Os;
import android.system.OsConstants;

import java.io.File;
import java.lang.reflect.Method;

/**
 * Runs in Termux, as the Termux user, started by the `chameleon` launcher:
 *
 *   CLASSPATH=<apk> app_process / io.github.harryl0l.chameleon.Broker <pid> <socket>
 *
 * The app can't listen in $PREFIX/tmp, so this process does while the app is
 * registered, and hands each connection from KWin's shim to the app over
 * Binder. The app finds it through a broadcast carrying this Binder, repeated
 * until it registers. Exits when <pid> (the session) does.
 */
public final class Broker extends Binder {
    static final String ACTION = "io.github.harryl0l.chameleon.action.BROKER";
    static final String EXTRA = "io.github.harryl0l.chameleon.extra.BROKER";
    static final String KEY_BINDER = "binder";
    /** app -> broker: data = the app's connection Binder. */
    static final int REGISTER = FIRST_CALL_TRANSACTION;
    /** broker -> app (oneway): data = a connected socket from the shim. */
    static final int CONNECTION = FIRST_CALL_TRANSACTION;

    private static final String APP = "io.github.harryl0l.chameleon";

    private final String mPath;
    private IBinder mApp;          // guarded by this
    private LocalSocket mSocket;   // guarded by this; listening while mApp is set
    private LocalServerSocket mServer;
    private long mInode;           // of mPath while it's our socket

    private Broker(String path) {
        mPath = path;
    }

    public static void main(String[] args) {
        if (args.length != 2) {
            System.err.println("usage: Broker <pid to follow> <socket path>");
            System.exit(2);
        }
        File session = new File("/proc/" + args[0]);
        Broker broker = new Broker(args[1]);
        System.out.println("chameleon-broker: waiting for the Chameleon app");
        try {
            while (session.exists()) {
                if (!broker.registered())
                    broker.announce();
                Thread.sleep(1000);
            }
        } catch (InterruptedException ignored) {
        }
        broker.stopListening();
        System.exit(0);
    }

    private synchronized boolean registered() {
        return mApp != null;
    }

    /** Tells the app where we are: an explicit broadcast carrying this Binder. */
    private void announce() {
        Bundle extra = new Bundle();
        extra.putBinder(KEY_BINDER, this);
        Intent intent = new Intent(ACTION)
                .setPackage(APP)
                .putExtra(EXTRA, extra);
        try {
            broadcast(intent);
        } catch (Exception e) {
            System.err.println("chameleon-broker: broadcast failed: " + e);
        }
    }

    /**
     * There's no Context in an app_process program, so this goes to the
     * activity manager directly, the way `am broadcast` does. The method's
     * parameters vary between Android versions; they're filled in by type.
     */
    private static void broadcast(Intent intent) throws Exception {
        Object am = Class.forName("android.app.ActivityManager").getMethod("getService").invoke(null);
        Method send = null;
        for (Method m : am.getClass().getMethods()) {
            if (m.getName().equals("broadcastIntent")) {
                send = m;
                break;
            }
            if (m.getName().equals("broadcastIntentWithFeature"))
                send = m;
        }
        if (send == null)
            throw new NoSuchMethodException("IActivityManager.broadcastIntent");
        Class<?>[] types = send.getParameterTypes();
        Object[] args = new Object[types.length];
        int ints = 0;
        for (Class<?> t : types)
            if (t == int.class)
                ints++;
        // The int parameters are, in order: resultCode, appOp, userId.
        int seen = 0;
        for (int i = 0; i < types.length; i++) {
            if (types[i] == Intent.class) {
                args[i] = intent;
            } else if (types[i] == int.class) {
                seen++;
                args[i] = seen == 1 ? 0 : seen == ints ? Process.myUid() / 100000 : -1 /* no app op */;
            } else if (types[i] == boolean.class) {
                args[i] = false;
            }
        }
        send.invoke(am, args);
    }

    @Override
    protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) throws RemoteException {
        if (code != REGISTER)
            return super.onTransact(code, data, reply, flags);
        IBinder app = data.readStrongBinder();
        if (app == null)
            return true;
        synchronized (this) {
            if (app.equals(mApp))
                return true; // already registered (a late announcement)
        }
        try {
            app.linkToDeath(() -> appDied(app), 0);
        } catch (RemoteException e) {
            return true; // already gone
        }
        synchronized (this) {
            mApp = app;
        }
        System.out.println("chameleon-broker: the app registered (uid " + getCallingUid() + ")");
        startListening();
        return true;
    }

    private void appDied(IBinder app) {
        synchronized (this) {
            if (mApp != app)
                return;
            mApp = null;
            closeLocked(); // in the same step: a new registration may follow at once
        }
        System.out.println("chameleon-broker: the app went away");
    }

    /** Opens the socket the shim connects to, if it isn't open yet. */
    private void startListening() {
        LocalServerSocket server;
        synchronized (this) {
            if (mServer != null)
                return;
            try {
                new File(mPath).delete();
                mSocket = new LocalSocket(LocalSocket.SOCKET_SEQPACKET);
                mSocket.bind(new LocalSocketAddress(mPath, LocalSocketAddress.Namespace.FILESYSTEM));
                Os.chmod(mPath, 0600);
                mInode = Os.stat(mPath).st_ino;
                mServer = new LocalServerSocket(mSocket.getFileDescriptor());
            } catch (Exception e) {
                System.err.println("chameleon-broker: cannot listen on " + mPath + ": " + e);
                closeLocked();
                return;
            }
            server = mServer;
        }
        System.out.println("chameleon-broker: listening on " + mPath);
        Thread t = new Thread(() -> acceptLoop(server), "chameleon-accept");
        t.setDaemon(true);
        t.start();
    }

    private void acceptLoop(LocalServerSocket server) {
        for (;;) {
            LocalSocket client;
            try {
                client = server.accept();
            } catch (Exception e) {
                return; // socket closed (closeLocked)
            }
            IBinder app;
            synchronized (this) {
                app = mApp;
            }
            Parcel data = Parcel.obtain();
            try {
                if (app != null) {
                    data.writeFileDescriptor(client.getFileDescriptor()); // the app gets its own copy
                    app.transact(CONNECTION, data, null, FLAG_ONEWAY);
                    System.out.println("chameleon-broker: passed a connection to the app");
                }
            } catch (Exception e) {
                System.err.println("chameleon-broker: cannot pass a connection to the app: " + e);
            } finally {
                data.recycle();
                try {
                    client.close();
                } catch (Exception ignored) {
                }
            }
        }
    }

    /** Without the app, no socket: the shim waits as when the app is closed. */
    private void stopListening() {
        synchronized (this) {
            closeLocked();
        }
    }

    private void closeLocked() {
        if (mServer != null) {
            try {
                // Wakes the accept loop; closing alone doesn't.
                Os.shutdown(mServer.getFileDescriptor(), OsConstants.SHUT_RDWR);
            } catch (Exception ignored) {
            }
            try {
                mServer.close();
            } catch (Exception ignored) {
            }
            mServer = null;
        }
        if (mSocket != null) {
            try {
                mSocket.close();
            } catch (Exception ignored) {
            }
            mSocket = null;
        }
        // Only if it's still ours: a newer session may have taken the path.
        try {
            if (mInode != 0 && Os.stat(mPath).st_ino == mInode)
                Os.remove(mPath);
        } catch (Exception ignored) {
        }
        mInode = 0;
    }
}
