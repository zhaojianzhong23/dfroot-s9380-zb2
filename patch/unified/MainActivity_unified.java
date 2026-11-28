package df.root;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Menu;
import android.view.MenuItem;
import android.view.View;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.Executor;
import java.util.concurrent.Executors;

public class MainActivity extends Activity implements IReporter {

    private static final String TAG = "dfroot";

    private Button btnRun;
    private ScrollView outputScroll;
    private TextView outputView;
    private Spinner spinnerFlavor;
    private Context mDeCtx;
    private final Handler mMain = new Handler(Looper.getMainLooper());
    private final Executor mExec = Executors.newSingleThreadExecutor();
    private int mValidFlavorPos = 0;

    /* flavor label -> (id, manager package). Order matches the spinner entries. */
    private static final String[] FLAVOR_IDS    = { null, "v9", "bakasu", "finasu" };
    private static final String[] FLAVOR_LABELS = {
            "Select a root flavor",
            "v9  - Samsung KernelSU (UAPI 5, stable)",
            "BakaSU  - UAPI 6 (Samsung-patched ko)",
            "FinaSU  - full features (Samsung-patched ko)"
    };
    private static final String[] FLAVOR_MGRS   = {
            null, "me.weishu.kernelsu", "com.resukisu.resukisu", "me.yuki.fina"
    };

    @Override
    public void report(String msg) {
        Log.i(TAG, msg.trim());
        mMain.post(() -> {
            outputView.append(msg);
            outputScroll.post(() -> outputScroll.fullScroll(View.FOCUS_DOWN));
        });
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        mDeCtx = createDeviceProtectedStorageContext();
        setContentView(R.layout.activity_main);

        getActionBar().setSubtitle("unified - v9 / BakaSU / FinaSU");

        btnRun = findViewById(R.id.btnRun);
        outputScroll = findViewById(R.id.outputScroll);
        outputView = findViewById(R.id.outputView);
        spinnerFlavor = findViewById(R.id.spinnerFlavor);

        List<String> entries = new ArrayList<>();
        for (String s : FLAVOR_LABELS) entries.add(s);
        ArrayAdapter<String> adapter = new ArrayAdapter<String>(this,
                android.R.layout.simple_spinner_item, entries) {
            @Override
            public boolean isEnabled(int pos) { return pos != 0; }
        };
        adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        spinnerFlavor.setAdapter(adapter);

        /* Always start at "Select a root flavor" - user must pick every time. */
        spinnerFlavor.setSelection(0);

        spinnerFlavor.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(AdapterView<?> parent, View view, int pos, long id) {
                if (pos == 0) {
                    mValidFlavorPos = 0;
                    return;
                }
                mValidFlavorPos = pos;
                SharedPreferences prefs = mDeCtx.getSharedPreferences(
                        ExploitRunner.PREFS_NAME, Context.MODE_PRIVATE);
                prefs.edit()
                     .putString(ExploitRunner.PREF_FLAVOR, FLAVOR_IDS[pos])
                     .putString(ExploitRunner.PREF_SU_MANAGER, FLAVOR_MGRS[pos])
                     .apply();
                updateRunButton();
            }
            @Override
            public void onNothingSelected(AdapterView<?> parent) {}
        });

        updateRunButton();

        btnRun.setOnClickListener(v -> {
            btnRun.setEnabled(false);
            outputView.setText("");
            mExec.execute(this::runExploit);
        });
    }

    @Override
    public boolean onCreateOptionsMenu(Menu menu) {
        getMenuInflater().inflate(R.menu.main_menu, menu);
        return true;
    }

    @Override
    public boolean onOptionsItemSelected(MenuItem item) {
        if (item.getItemId() == R.id.action_settings) {
            startActivity(new Intent(this, SettingsActivity.class));
            return true;
        }
        return super.onOptionsItemSelected(item);
    }

    private void updateRunButton() {
        btnRun.setEnabled(mValidFlavorPos >= 1 && !new File("/dev/df").exists());
    }

    private void runExploit() {
        try {
            int rc = ExploitRunner.run(mDeCtx, this);
            String msg = rc == 0 ? "DFRoot: SUCCESS"
                       : rc == 1 ? "DFRoot: Error - ksud nonzero exit"
                       : rc == 2 ? "DFRoot: Error - check output above"
                       : "DFRoot: Error - failed to patch files";
            mMain.post(() -> Toast.makeText(this, msg, Toast.LENGTH_LONG).show());
        } catch (Exception e) {
            Log.e(TAG, "exploit exception", e);
            report("\nexception: " + e + "\n");
        } finally {
            mMain.post(this::updateRunButton);
        }
    }
}
