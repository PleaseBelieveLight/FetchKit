package com.luofelix.fetchkit.sizehost;

import android.app.Activity;
import android.widget.TextView;
import com.luofelix.fetchkit.*;
import java.io.File;
import java.security.MessageDigest;
import java.util.Collections;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import kotlin.Unit;
import org.json.JSONObject;

/** Standalone Maven consumer with R8, a fresh provider, events and full lease verification. */
final class Integration {
    static String label() { return "FetchKit production host " + FetchKit.INSTANCE.version(); }
    static void launch(Activity activity, TextView text) {
        String url = activity.getIntent().getStringExtra("fetchkit_url");
        String sha = activity.getIntent().getStringExtra("fetchkit_sha256");
        String runId = activity.getIntent().getStringExtra("fetchkit_run_id");
        if (url == null || sha == null || runId == null) return;
        EngineConfig config = new EngineConfig(new File(activity.getNoBackupFilesDir(), "fetchkit"),
            67108864L, 134217728L, 86400000L, true, 3, 1024, 0, 5000, 15000, 10000, 5, 4096, 4194304L, 1000, 1000, 30000, null);
        AtomicInteger events = new AtomicInteger();
        FetchKit.INSTANCE.open(config).whenComplete((engine, openError) -> {
            if (openError != null) { report(activity, text, runId, false, 0, events.get()); return; }
            DownloadRequest request = new DownloadRequest(new ResourceIdentity("release", "resource", runId, ""),
                new RequestInfo(url, Collections.emptyMap()), "public-test-resource", null, sha, true, 0L, true,
                context -> new RequestInfo(context.getProposedUrl(), Collections.emptyMap()));
            engine.enqueue(request).thenCompose(sub -> {
                sub.observe(Runnable::run, event -> { events.incrementAndGet(); return Unit.INSTANCE; });
                return sub.awaitStopped();
            }).thenCompose(snapshot -> CompletableFuture.supplyAsync(() -> {
                ResourceLease lease = null;
                long bytes = 0;
                try {
                    if (snapshot.getState() != TaskState.Completed || snapshot.getIntegrity() != Integrity.DigestVerified) throw new IllegalStateException();
                    lease = engine.acquire(snapshot.getPersistentId()).get(30, TimeUnit.SECONDS);
                    MessageDigest digest = MessageDigest.getInstance("SHA-256");
                    while (true) {
                        byte[] block = lease.read(bytes, 65536).get(30, TimeUnit.SECONDS);
                        if (block.length == 0) break;
                        digest.update(block); bytes += block.length;
                    }
                    StringBuilder actual = new StringBuilder();
                    for (byte value : digest.digest()) actual.append(String.format(java.util.Locale.ROOT, "%02x", value & 255));
                    if (!sha.equals(actual.toString())) throw new IllegalStateException();
                    return bytes;
                } catch (Exception error) { throw new java.util.concurrent.CompletionException(error); }
                finally { if (lease != null) lease.releaseAsync().join(); }
            })).whenComplete((bytes, error) -> engine.closeAsync().whenComplete((ignored, closeError) ->
                report(activity, text, runId, error == null && closeError == null, bytes == null ? 0 : bytes, events.get())));
        });
    }
    private static void report(Activity activity, TextView text, String runId, boolean passed, long bytes, int events) {
        try {
            JSONObject value = new JSONObject().put("run_id", runId).put("passed", passed).put("bytes", bytes)
                .put("events", events).put("version", FetchKit.INSTANCE.version());
            File file = new File(activity.getExternalFilesDir(null), "m4-release.json");
            try (java.io.FileOutputStream stream = new java.io.FileOutputStream(file)) {
                stream.write(value.toString().getBytes(java.nio.charset.StandardCharsets.UTF_8));
            }
            activity.runOnUiThread(() -> text.setText(passed ? "Release download verified" : "Release validation failed"));
        } catch (Exception ignored) { activity.runOnUiThread(() -> text.setText("Unable to write release result")); }
    }
}
