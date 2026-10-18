// Small library the launcher loads to label imported BIOS files and identify discs, without pulling in SDL or the
// emulator.

#include "disc_hash.hpp"
#include "ipl_select.hpp"

#include <jni.h>
#include <unistd.h>

#include <cinttypes>
#include <cstdio>
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

/// "crc32|size" of a CHD's first track as Redump dumps it, or null. Reads from a dup of fd and leaves fd open.
extern "C" JNIEXPORT jstring JNICALL
Java_com_izzy2lost_neonsaturn_DiscIdentifier_nativeHashChdTrack1(JNIEnv *env, jclass, jint fd) {
    const int ownFd = dup(fd);
    if (ownFd < 0) {
        return nullptr;
    }
    FILE *file = fdopen(ownFd, "rb");
    if (file == nullptr) {
        close(ownFd);
        return nullptr;
    }
    const auto hash = neonsaturn::android::HashChdTrack1(file);
    std::fclose(file);
    if (!hash) {
        return nullptr;
    }

    char result[32];
    std::snprintf(result, sizeof(result), "%08" PRIx32 "|%" PRIu64, hash->crc32, hash->size);
    return env->NewStringUTF(result);
}
