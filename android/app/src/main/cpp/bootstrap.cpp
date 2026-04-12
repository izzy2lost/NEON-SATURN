#include "bootstrap.hpp"

#include <jni.h>

#include <mutex>
#include <utility>

namespace neonsaturn::android {

namespace {
std::mutex g_bootstrapMutex;
BootstrapConfig g_bootstrapConfig{};

std::string JStringToString(JNIEnv *env, jstring value) {
    if (value == nullptr) {
        return {};
    }

    const char *chars = env->GetStringUTFChars(value, nullptr);
    if (chars == nullptr) {
        return {};
    }

    std::string result{chars};
    env->ReleaseStringUTFChars(value, chars);
    return result;
}
} // namespace

void SetBootstrapConfig(BootstrapConfig config) {
    std::lock_guard lock{g_bootstrapMutex};
    g_bootstrapConfig = std::move(config);
}

BootstrapConfig GetBootstrapConfig() {
    std::lock_guard lock{g_bootstrapMutex};
    return g_bootstrapConfig;
}

extern "C" JNIEXPORT void JNICALL
Java_com_izzy2lost_neonsaturn_NativeBootstrap_nativeConfigure(JNIEnv *env, jclass, jstring iplPath, jstring cdbPath,
                                                              jstring discPath, jstring dataRoot) {
    SetBootstrapConfig({
        .iplPath = JStringToString(env, iplPath),
        .cdbPath = JStringToString(env, cdbPath),
        .discPath = JStringToString(env, discPath),
        .dataRoot = JStringToString(env, dataRoot),
    });
}

} // namespace neonsaturn::android
