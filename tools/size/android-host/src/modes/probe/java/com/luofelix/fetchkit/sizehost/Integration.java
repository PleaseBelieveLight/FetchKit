package com.luofelix.fetchkit.sizehost;

import com.luofelix.fetchkit.FetchKit;
import com.luofelix.fetchkit.probe.PlatformProbe;

final class Integration {
    static String label() {
        return "FetchKit size host " + FetchKit.INSTANCE.version() + " " + PlatformProbe.INSTANCE.backendName();
    }
}
