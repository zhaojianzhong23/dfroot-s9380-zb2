package df.root;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.util.Log;

/**
 * Unified build: privilege escalation always requires an explicit flavor
 * selection in the app UI. Boot-time auto-start is intentionally disabled so
 * a previously selected v9/BakaSU flavor can never be reused silently.
 */
public class BootReceiver extends BroadcastReceiver {
    private static final String TAG = "dfroot";

    @Override
    public void onReceive(Context context, Intent intent) {
        Log.i(TAG, "boot: unified build requires manual flavor selection; skipping");
    }
}
