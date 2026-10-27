#include "fetchkit/version.h"

#include <jni.h>

bool registerAndroidBridge(JavaVM* vm, JNIEnv* env) noexcept;

#ifdef FETCHKIT_ENABLE_PLATFORM_PROBE
bool registerPlatformProbe(JNIEnv* env) noexcept;
#endif

namespace {

jstring nativeVersion(JNIEnv* env, jobject) noexcept {
    return env->NewStringUTF(fetchkit::version());
}

}

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    void* raw_env = nullptr;
    if (vm->GetEnv(&raw_env, JNI_VERSION_1_6) != JNI_OK) {
        return JNI_ERR;
    }
    auto* env = static_cast<JNIEnv*>(raw_env);

    jclass sdk_class = env->FindClass("com/luofelix/fetchkit/FetchKit");
    if (sdk_class == nullptr) {
        return JNI_ERR;
    }

    const JNINativeMethod methods[] = {
        {
            const_cast<char*>("nativeVersion"),
            const_cast<char*>("()Ljava/lang/String;"),
            reinterpret_cast<void*>(nativeVersion)
        }
    };
    const jint result = env->RegisterNatives(sdk_class, methods, 1);
    env->DeleteLocalRef(sdk_class);
    if (result != JNI_OK) {
        return JNI_ERR;
    }
    if (!registerAndroidBridge(vm, env)) return JNI_ERR;
#ifdef FETCHKIT_ENABLE_PLATFORM_PROBE
    if (!registerPlatformProbe(env)) {
        return JNI_ERR;
    }
#endif
    return JNI_VERSION_1_6;
}
