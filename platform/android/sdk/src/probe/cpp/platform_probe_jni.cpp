#include <jni.h>

#include <array>
#include <cstdint>

namespace {

jlong nativeFingerprint(JNIEnv* env, jobject, jbyteArray bytes, jint count, jlong seed) noexcept {
    if (bytes == nullptr || count < 0 || count > 65536 || count > env->GetArrayLength(bytes)) {
        const auto exception = env->FindClass("java/lang/IllegalArgumentException");
        if (exception != nullptr) {
            env->ThrowNew(exception, "Invalid probe buffer length");
            env->DeleteLocalRef(exception);
        }
        return seed;
    }
    std::array<jbyte, 65536> buffer{};
    env->GetByteArrayRegion(bytes, 0, count, buffer.data());
    if (env->ExceptionCheck()) return seed;
    auto fingerprint = static_cast<std::uint64_t>(seed);
    for (jint i = 0; i < count; ++i) {
        fingerprint ^= static_cast<std::uint8_t>(buffer[i]);
        fingerprint *= 1099511628211ULL;
    }
    // No pointers, Java references or data survive this synchronous batch call.
    return static_cast<jlong>(fingerprint);
}

}

bool registerPlatformProbe(JNIEnv* env) noexcept {
    const auto probe_class = env->FindClass("com/luofelix/fetchkit/probe/PlatformProbe");
    if (probe_class == nullptr) return false;
    const JNINativeMethod methods[] = {{
        const_cast<char*>("nativeFingerprint"), const_cast<char*>("([BIJ)J"),
        reinterpret_cast<void*>(nativeFingerprint)
    }};
    const auto result = env->RegisterNatives(probe_class, methods, 1);
    env->DeleteLocalRef(probe_class);
    return result == JNI_OK;
}
