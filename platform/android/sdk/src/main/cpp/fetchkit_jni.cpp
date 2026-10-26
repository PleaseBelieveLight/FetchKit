#include "fetchkit/version.h"

#include <jni.h>

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
    return result == JNI_OK ? JNI_VERSION_1_6 : JNI_ERR;
}
