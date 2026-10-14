// Small library the launcher loads to label imported BIOS files, without pulling in SDL or the emulator.

#include "ipl_select.hpp"

#include <jni.h>

#include <string>

namespace {

const char *RegionName(ymir::db::SystemRegion region) {
    switch (region) {
    case ymir::db::SystemRegion::US_EU: return "US_EU";
    case ymir::db::SystemRegion::JP: return "JP";
    case ymir::db::SystemRegion::KR: return "KR";
    default: return "NONE";
    }
}

const char *VariantName(ymir::db::SystemVariant variant) {
    switch (variant) {
    case ymir::db::SystemVariant::Saturn: return "SATURN";
    case ymir::db::SystemVariant::HiSaturn: return "HISATURN";
    case ymir::db::SystemVariant::VSaturn: return "VSATURN";
    case ymir::db::SystemVariant::DevKit: return "DEVKIT";
    default: return "NONE";
    }
}

} // namespace

/// "version|region|regionFree|variant" for a known IPL ROM, or null.
extern "C" JNIEXPORT jstring JNICALL
Java_com_izzy2lost_neonsaturn_BiosIdentifier_nativeIdentify(JNIEnv *env, jclass, jstring path) {
    const char *chars = env->GetStringUTFChars(path, nullptr);
    if (chars == nullptr) {
        return nullptr;
    }
    const auto *info = neonsaturn::android::IdentifyIPL(chars);
    env->ReleaseStringUTFChars(path, chars);
    if (info == nullptr) {
        return nullptr;
    }

    const std::string result = std::string{info->version} + '|' + RegionName(info->region) + '|' +
                               (info->regionFree ? "1" : "0") + '|' + VariantName(info->variant);
    return env->NewStringUTF(result.c_str());
}
