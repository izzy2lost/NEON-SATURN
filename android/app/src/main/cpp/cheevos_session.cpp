#include "cheevos_session.hpp"

#include <ymir/media/disc.hpp>
#include <ymir/sys/memory.hpp>

#include <android/log.h>
#include <jni.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#include "md5.h"

#define TAG "NeonSaturnCheevos"
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

namespace neonsaturn::cheevos {
namespace {

// ── HTTP through Java ─────────────────────────────────────────────────────────

JavaVM *g_vm = nullptr;
jclass g_httpClass = nullptr; // global ref to CheevosHttp
jmethodID g_perform = nullptr;

// A native thread is attached on its first request and detached when it exits.
struct AttachedThread {
    JNIEnv *env = nullptr;
    bool ours = false;
    ~AttachedThread() {
        if (ours && g_vm) {
            g_vm->DetachCurrentThread();
        }
    }
};

JNIEnv *ThreadEnv() {
    thread_local AttachedThread a;
    if (a.env) {
        return a.env;
    }
    if (!g_vm) {
        return nullptr;
    }
    if (g_vm->GetEnv(reinterpret_cast<void **>(&a.env), JNI_VERSION_1_6) == JNI_OK) {
        return a.env;
    }
    JavaVMAttachArgs args{JNI_VERSION_1_6, "NeonSaturn cheevos", nullptr};
    if (g_vm->AttachCurrentThread(&a.env, &args) != JNI_OK) {
        a.env = nullptr;
        return nullptr;
    }
    a.ours = true;
    return a.env;
}

class JniBackend final : public Backend {
public:
    // CheevosHttp.perform returns the status as four big-endian bytes, then the body; status 0 carries the
    // transport's error message as the body.
    Response perform(const Request &req, const char *userAgent) override {
        Response res;
        JNIEnv *env = ThreadEnv();
        if (!env || !g_perform) {
            res.error = "no Java transport";
            return res;
        }
        jstring url = env->NewStringUTF(req.url.c_str());
        jstring body = env->NewStringUTF(req.body.c_str());
        jstring type = env->NewStringUTF(req.content_type.c_str());
        jstring ua = env->NewStringUTF(userAgent ? userAgent : "");
        auto out =
            static_cast<jbyteArray>(env->CallStaticObjectMethod(g_httpClass, g_perform, url, body, type, ua));
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            out = nullptr;
        }
        env->DeleteLocalRef(url);
        env->DeleteLocalRef(body);
        env->DeleteLocalRef(type);
        env->DeleteLocalRef(ua);
        if (!out) {
            res.error = "the request failed";
            return res;
        }
        const jsize n = env->GetArrayLength(out);
        std::string bytes(static_cast<size_t>(n), '\0');
        if (n > 0) {
            env->GetByteArrayRegion(out, 0, n, reinterpret_cast<jbyte *>(bytes.data()));
        }
        env->DeleteLocalRef(out);
        if (bytes.size() < 4) {
            res.error = "the request failed";
            return res;
        }
        const auto b = reinterpret_cast<const unsigned char *>(bytes.data());
        res.status = static_cast<int>((b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]);
        if (res.status == 0) {
            res.error = bytes.substr(4);
            LOGW("no answer from %s: %s", req.url.c_str(), res.error.c_str());
        } else {
            res.body = bytes.substr(4);
        }
        return res;
    }
};

// ── Saturn memory ─────────────────────────────────────────────────────────────

constexpr uint32_t kWRAMLowSize = 0x100000;
constexpr uint32_t kWRAMSize = 0x200000;

// RetroAchievements' Saturn addresses are low work RAM ($000000) then high work RAM ($100000), laid out as Beetle
// Saturn exposes them: native little-endian 16-bit words, so byte A of a set is Saturn byte A ^ 1. Ymir keeps work
// RAM in the Saturn's big-endian order.
uint32_t ReadWorkRAM(const ymir::sys::SystemMemory &mem, uint32_t address, uint8_t *buffer, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t a = address + i;
        if (a >= kWRAMSize) {
            std::memset(buffer + i, 0, n - i);
            return i;
        }
        const uint32_t s = a ^ 1;
        buffer[i] = s < kWRAMLowSize ? mem.WRAMLow[s] : mem.WRAMHigh[s - kWRAMLowSize];
    }
    return n;
}

struct Session {
    std::unique_ptr<Client> client;
};

std::mutex g_sessionLock;
std::unique_ptr<Session> g_session;

bool ReadWholeFile(const std::string &path, std::vector<uint8_t> &out) {
    out.clear();
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) {
        return false;
    }
    uint8_t buf[16 * 1024];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        out.insert(out.end(), buf, buf + n);
    }
    std::fclose(f);
    return true;
}

bool WriteWholeFile(const std::string &path, const std::vector<uint8_t> &data) {
    const std::string tmp = path + ".tmp";
    FILE *f = std::fopen(tmp.c_str(), "wb");
    if (!f) {
        return false;
    }
    const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    std::fclose(f);
    if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

const char *StateName(State state) {
    switch (state) {
    case State::Off: return "off";
    case State::SignedOut: return "signed_out";
    case State::SigningIn: return "signing_in";
    case State::SignedIn: return "signed_in";
    case State::LoadingGame: return "loading";
    case State::Playing: return "playing";
    case State::NoSet: return "no_set";
    case State::EmptySet: return "empty_set";
    }
    return "off";
}

constexpr char kSep = '\x1f';

std::string FromJavaString(JNIEnv *env, jstring s) {
    if (!s) {
        return {};
    }
    const char *chars = env->GetStringUTFChars(s, nullptr);
    if (!chars) {
        return {};
    }
    std::string out{chars};
    env->ReleaseStringUTFChars(s, chars);
    return out;
}

jobjectArray ToJavaStrings(JNIEnv *env, const std::vector<std::string> &items) {
    jclass stringClass = env->FindClass("java/lang/String");
    jobjectArray out = env->NewObjectArray(static_cast<jsize>(items.size()), stringClass, nullptr);
    for (size_t i = 0; i < items.size(); i++) {
        jstring s = env->NewStringUTF(items[i].c_str());
        env->SetObjectArrayElement(out, static_cast<jsize>(i), s);
        env->DeleteLocalRef(s);
    }
    env->DeleteLocalRef(stringClass);
    return out;
}

} // namespace

std::unique_ptr<Backend> MakeJniBackend() {
    return std::make_unique<JniBackend>();
}

std::string HashDisc(const ymir::media::Disc &disc) {
    if (disc.sessions.empty()) {
        return {};
    }
    const ymir::media::Session &session = disc.sessions.front();
    if (session.numTracks == 0) {
        return {};
    }
    const ymir::media::Track &track = session.tracks[session.firstTrackIndex];
    if (!track.binaryReader) {
        return {};
    }

    // The boot sector is the first one of track 1, after any pregap the image stores.
    std::array<uint8_t, 2048> sector{};
    bool found = false;
    for (const uint32_t frameAddress : {track.index01FrameAddress, track.startFrameAddress}) {
        if (track.ReadSectorUserData(frameAddress, sector) &&
            std::memcmp(sector.data(), "SEGA SEGASATURN ", 16) == 0) {
            found = true;
            break;
        }
    }
    if (!found) {
        LOGW("no Saturn header on track 1");
        return {};
    }

    md5_state_t md5;
    md5_byte_t digest[16];
    md5_init(&md5);
    md5_append(&md5, sector.data(), 512);
    md5_finish(&md5, digest);

    static constexpr char kHex[] = "0123456789abcdef";
    std::string hash;
    for (const md5_byte_t byte : digest) {
        hash += kHex[byte >> 4];
        hash += kHex[byte & 0xF];
    }
    return hash;
}

void BeginSession(const ymir::sys::SystemMemory &memory, const std::string &hash, const std::string &user,
                  const std::string &token) {
    EndSession();
    if (hash.empty() || user.empty() || token.empty()) {
        return;
    }

    auto session = std::make_unique<Session>();
    session->client = std::make_unique<Client>();
    session->client->start(MakeJniBackend(), [&memory](uint32_t address, uint8_t *buffer, uint32_t n) {
        return ReadWorkRAM(memory, address, buffer, n);
    });
    session->client->sign_in_with_token(user, token);
    session->client->load_game(hash);

    std::lock_guard lk{g_sessionLock};
    g_session = std::move(session);
}

void EndSession() {
    std::unique_ptr<Session> session;
    {
        std::lock_guard lk{g_sessionLock};
        session = std::move(g_session);
    }
    if (!session) {
        return;
    }
    // Shutting down waits for the request in flight (an unlock, say) to finish, which can take seconds: not on the
    // caller's thread.
    std::thread([s = std::move(session)]() mutable { s.reset(); }).detach();
}

void OnFrame() {
    std::lock_guard lk{g_sessionLock};
    if (g_session) {
        g_session->client->frame();
    }
}

void AfterSaveState(const std::string &statePath) {
    std::lock_guard lk{g_sessionLock};
    const std::string path = statePath + ".rap";
    std::vector<uint8_t> progress;
    if (g_session && g_session->client->serialize_progress(progress)) {
        WriteWholeFile(path, progress);
    } else {
        std::remove(path.c_str());
    }
}

void AfterLoadState(const std::string &statePath) {
    std::lock_guard lk{g_sessionLock};
    if (!g_session) {
        return;
    }
    std::vector<uint8_t> progress;
    if (!ReadWholeFile(statePath + ".rap", progress) || progress.empty() ||
        !g_session->client->deserialize_progress(progress.data(), progress.size())) {
        g_session->client->reset();
    }
}

std::vector<std::string> TakeMessages() {
    std::vector<std::string> out;
    std::lock_guard lk{g_sessionLock};
    if (!g_session) {
        return out;
    }
    for (const Message &m : g_session->client->take_messages()) {
        const char *kind = m.kind == Message::Kind::Unlock    ? "unlock"
                           : m.kind == Message::Kind::Problem ? "problem"
                                                              : "info";
        out.push_back(std::string(kind) + kSep + m.text + kSep + m.detail + kSep + std::to_string(m.points) + kSep +
                      m.badge_url);
    }
    return out;
}

std::vector<std::string> ListAchievements() {
    std::vector<std::string> out;
    std::lock_guard lk{g_sessionLock};
    if (!g_session) {
        out.push_back("off");
        return out;
    }
    Client &c = *g_session->client;
    const std::vector<Client::Achievement> list = c.achievements();
    uint32_t unlocked = 0, points = 0, earned = 0;
    for (const Client::Achievement &a : list) {
        points += a.points;
        if (a.unlocked) {
            unlocked++;
            earned += a.points;
        }
    }
    out.push_back(std::string(StateName(c.state())) + kSep + c.game_title() + kSep + c.game_badge_url() + kSep +
                  std::to_string(unlocked) + kSep + std::to_string(list.size()) + kSep + std::to_string(earned) +
                  kSep + std::to_string(points));
    for (const Client::Achievement &a : list) {
        out.push_back(std::string(a.unlocked ? "1" : "0") + kSep + (a.unsupported ? "1" : "0") + kSep +
                      std::to_string(a.points) + kSep + a.badge_url + kSep + a.badge_locked_url + kSep + a.title +
                      kSep + a.description + kSep + a.progress);
    }
    return out;
}

// ── JNI ───────────────────────────────────────────────────────────────────────

extern "C" JNIEXPORT jobjectArray JNICALL Java_com_izzy2lost_neonsaturn_CheevosNative_nativeMessages(JNIEnv *env,
                                                                                                    jobject) {
    return ToJavaStrings(env, TakeMessages());
}

extern "C" JNIEXPORT jobjectArray JNICALL Java_com_izzy2lost_neonsaturn_CheevosNative_nativeList(JNIEnv *env,
                                                                                                jobject) {
    return ToJavaStrings(env, ListAchievements());
}

// Blocking: { username, token, display name } on success, { "", reason } on failure.
extern "C" JNIEXPORT jobjectArray JNICALL Java_com_izzy2lost_neonsaturn_CheevosNative_nativeLogin(JNIEnv *env,
                                                                                                 jobject,
                                                                                                 jstring user,
                                                                                                 jstring password) {
    auto http = MakeJniBackend();
    std::string outUser, outToken, displayName, err;
    uint32_t score = 0;
    if (login_with_password(*http, FromJavaString(env, user), FromJavaString(env, password), outUser, outToken,
                            displayName, score, err)) {
        return ToJavaStrings(env, {outUser, outToken, displayName});
    }
    return ToJavaStrings(env, {"", err});
}

} // namespace neonsaturn::cheevos

// libmain is loaded by System.loadLibrary on a Java thread, so the app's classes are visible here; a native worker
// thread's FindClass would not see them.
extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *) {
    using namespace neonsaturn::cheevos;
    JNIEnv *env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK) {
        return JNI_VERSION_1_6;
    }
    g_vm = vm;
    jclass c = env->FindClass("com/izzy2lost/neonsaturn/CheevosHttp");
    if (!c) {
        env->ExceptionClear();
        LOGW("no CheevosHttp class");
        return JNI_VERSION_1_6;
    }
    g_httpClass = static_cast<jclass>(env->NewGlobalRef(c));
    env->DeleteLocalRef(c);
    g_perform = env->GetStaticMethodID(
        g_httpClass, "perform", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)[B");
    if (!g_perform) {
        env->ExceptionClear();
    }
    return JNI_VERSION_1_6;
}
