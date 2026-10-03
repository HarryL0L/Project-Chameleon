package io.github.harryl0l.chameleon;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageManager;
import android.os.Binder;
import android.os.Build;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Parcel;
import android.os.ParcelFileDescriptor;
import android.util.Log;

/**
 * The app's side of {@link Broker}: the broker announces itself with a
 * broadcast, the app registers its connection Binder with it, and the broker
 * passes each connection from KWin's shim to {@link PresenterActivity#nativeConnection}.
 */
public final class BrokerLink extends BroadcastReceiver {
    private static final String TAG = "Chameleon";
    private static final String TERMUX = "com.termux";

    private static IBinder sBroker;          // the latest broker announced
    private static IBinder sRegistered;      // the broker we registered with
    private static PackageManager sPackages;

    /** Receives the shim's connections; only Termux's user may send them. */
    private static final Binder sConnections = new Binder() {
        @Override
        protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) {
            if (code != Broker.CONNECTION)
                return false;
            int uid = getCallingUid();
            if (!isTermux(uid)) {
                Log.w(TAG, "refusing a connection from uid " + uid + ": not Termux");
                return true;
            }
            ParcelFileDescriptor fd = data.readFileDescriptor();
            if (fd != null)
                PresenterActivity.nativeConnection(fd.detachFd());
            return true;
        }
    };

    @Override
    public void onReceive(Context context, Intent intent) {
        if (!Broker.ACTION.equals(intent.getAction()))
            return;
        Bundle extra = intent.getBundleExtra(Broker.EXTRA);
        IBinder broker = extra == null ? null : extra.getBinder(Broker.KEY_BINDER);
        if (broker == null)
            return;
        synchronized (BrokerLink.class) {
            // Announcements repeat until we register, and can arrive late.
            if (broker.equals(sRegistered) && broker.isBinderAlive())
                return;
            sBroker = broker;
            register();
        }
    }

    /**
     * Called once the presenter runs. Registered at runtime, not in the
     * manifest, so the broker's announcements never launch the app.
     */
    static void listen(Context context) {
        Context app = context.getApplicationContext();
        synchronized (BrokerLink.class) {
            sPackages = app.getPackageManager();
        }
        IntentFilter filter = new IntentFilter(Broker.ACTION);
        // Sent from Termux, so exported (a flag Android 14 requires).
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU)
            app.registerReceiver(new BrokerLink(), filter, Context.RECEIVER_EXPORTED);
        else
            app.registerReceiver(new BrokerLink(), filter);
    }

    private static void register() {
        Parcel data = Parcel.obtain();
        try {
            data.writeStrongBinder(sConnections);
            sBroker.transact(Broker.REGISTER, data, null, IBinder.FLAG_ONEWAY);
            sRegistered = sBroker;
            Log.i(TAG, "registered with the Termux broker");
        } catch (Exception e) {
            Log.w(TAG, "cannot register with the Termux broker: " + e);
            sBroker = null; // gone; it announces itself again if it comes back
        } finally {
            data.recycle();
        }
    }

    private static boolean isTermux(int uid) {
        PackageManager pm;
        synchronized (BrokerLink.class) {
            pm = sPackages;
        }
        String[] packages = pm == null ? null : pm.getPackagesForUid(uid);
        if (packages != null)
            for (String p : packages)
                if (TERMUX.equals(p))
                    return true;
        return false;
    }
}
