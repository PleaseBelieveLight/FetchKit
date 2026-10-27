# JNI_OnLoad resolves this class and its native method by name.
-keep class com.luofelix.fetchkit.FetchKit {
    native <methods>;
}

# Registered entry points and methods invoked from attached C++ workers.
-keep class com.luofelix.fetchkit.NativeBridge { *; }
-keep class com.luofelix.fetchkit.NativeProvider { *; }
-keep class com.luofelix.fetchkit.AndroidTransport { *; }

# The class is present only in builds explicitly enabling the M0 experiment.
-keep class com.luofelix.fetchkit.probe.PlatformProbe {
    native <methods>;
}
