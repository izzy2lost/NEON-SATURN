#include "bootstrap.hpp"

#include "audio_system.hpp"
#include "rom_loader.hpp"

#include <ymir/core/hash.hpp>
#include <ymir/hw/scsp/scsp.hpp>
#include <ymir/hw/smpc/peripheral/peripheral_report.hpp>
#include <ymir/hw/vdp/renderer/vdp_renderer_base.hpp>
#include <ymir/hw/vdp/vdp.hpp>
#include <ymir/media/loader/loader.hpp>
#include <ymir/sys/saturn.hpp>
#include <ymir/util/callback.hpp>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <cereal/archives/portable_binary.hpp>
#include <jni.h>
#include <serdes/cereal_savestate.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace neonsaturn::android {

using ymir::peripheral::Button;

class EmulatorApp;

namespace {

struct ActionResult {
    bool success = false;
    std::string message;
};

constexpr std::uint32_t kTouchButtonA = 1u << 0u;
constexpr std::uint32_t kTouchButtonB = 1u << 1u;
constexpr std::uint32_t kTouchButtonC = 1u << 2u;
constexpr std::uint32_t kTouchButtonX = 1u << 3u;
constexpr std::uint32_t kTouchButtonY = 1u << 4u;
constexpr std::uint32_t kTouchButtonZ = 1u << 5u;
constexpr std::uint32_t kTouchButtonL = 1u << 6u;
constexpr std::uint32_t kTouchButtonR = 1u << 7u;
constexpr std::uint32_t kTouchButtonStart = 1u << 8u;

std::mutex g_activeAppMutex;
EmulatorApp *g_activeApp = nullptr;

std::string_view ArgValue(std::string_view argument, std::string_view prefix) {
    if (!argument.starts_with(prefix)) {
        return {};
    }
    return argument.substr(prefix.size());
}

BootstrapConfig BootstrapConfigFromArgs(int argc, char **argv) {
    BootstrapConfig config = GetBootstrapConfig();
    for (int index = 0; index < argc; ++index) {
        const std::string_view argument = argv[index] == nullptr ? std::string_view{} : std::string_view{argv[index]};
        if (const auto value = ArgValue(argument, "--ipl="); !value.empty()) {
            config.iplPath.assign(value);
        } else if (const auto value = ArgValue(argument, "--cdb="); !value.empty()) {
            config.cdbPath.assign(value);
        } else if (const auto value = ArgValue(argument, "--disc="); !value.empty()) {
            config.discPath.assign(value);
        } else if (const auto value = ArgValue(argument, "--data-root="); !value.empty()) {
            config.dataRoot.assign(value);
        } else if (const auto value = ArgValue(argument, "--gamecontrollerdb="); !value.empty()) {
            config.gameControllerDbPath.assign(value);
        } else if (const auto value = ArgValue(argument, "--aspect-ratio="); !value.empty()) {
            config.aspectRatio.assign(value);
        } else if (const auto value = ArgValue(argument, "--texture-filter="); !value.empty()) {
            config.textureFilter.assign(value);
        }
    }
    return config;
}

void SetActiveApp(EmulatorApp *app) {
    std::scoped_lock lock{g_activeAppMutex};
    g_activeApp = app;
}

EmulatorApp *GetActiveApp() {
    std::scoped_lock lock{g_activeAppMutex};
    return g_activeApp;
}

void RequestQuickActionsDialog() {
    auto *env = static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
    auto *activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (env == nullptr || activity == nullptr) {
        return;
    }

    jclass activityClass = env->GetObjectClass(activity);
    if (activityClass == nullptr) {
        return;
    }

    const jmethodID method = env->GetStaticMethodID(activityClass, "requestQuickActionsFromNative", "()V");
    if (method != nullptr) {
        env->CallStaticVoidMethod(activityClass, method);
        if (env->ExceptionCheck()) {
            env->ExceptionDescribe();
            env->ExceptionClear();
        }
    }

    env->DeleteLocalRef(activityClass);
}

[[nodiscard]] bool IsButtonPressed(Button buttons, Button button) {
    return static_cast<std::uint16_t>(buttons & button) == 0;
}

[[nodiscard]] float ClampAnalog(float value) {
    return std::clamp(value, -1.0f, 1.0f);
}

[[nodiscard]] float ApplyDeadZone(float value, float deadZone = 0.12f) {
    return std::abs(value) < deadZone ? 0.0f : ClampAnalog(value);
}

[[nodiscard]] std::uint8_t FloatToAnalogAxis(float value) {
    const float clamped = ClampAnalog(value);
    return static_cast<std::uint8_t>(std::lround((clamped + 1.0f) * 127.5f));
}

[[nodiscard]] float StrongerAnalogValue(float lhs, float rhs) {
    return std::abs(rhs) > std::abs(lhs) ? rhs : lhs;
}

} // namespace

class EmulatorApp {
public:
    explicit EmulatorApp(BootstrapConfig config)
        : m_config(std::move(config)) {}

    void SetPaused(bool paused) {
        if (m_paused.exchange(paused) == paused) {
            return;
        }

        ResetInputs();

        if (paused) {
            if (m_audioStarted && m_audioSystem.IsRunning()) {
                m_audioSystem.Stop();
            }
        } else if (m_audioStarted && !m_audioSystem.IsRunning()) {
            m_audioSystem.Start();
        }
    }

    [[nodiscard]] ActionResult SaveStateToSlot(std::size_t slotIndex) {
        if (slotIndex >= 10) {
            return {.success = false, .message = "Invalid save state slot"};
        }

        try {
            std::scoped_lock lock{m_coreMutex};

            auto state = std::make_unique<ymir::savestate::SaveState>();
            m_saturn.SaveState(*state);

            std::error_code error{};
            const auto gameStatesPath = SaveStatesDirectory(state->discHash);
            std::filesystem::create_directories(gameStatesPath, error);
            if (error) {
                return {.success = false, .message = "Could not prepare save state storage"};
            }

            const auto statePath = gameStatesPath / (std::to_string(slotIndex) + ".savestate");
            std::ofstream out{statePath, std::ios::binary};
            if (!out) {
                return {.success = false, .message = "Could not open the save state slot"};
            }

            cereal::PortableBinaryOutputArchive archive{out};
            archive(*state);
            return {.success = true, .message = "State " + std::to_string(slotIndex) + " saved"};
        } catch (const cereal::Exception &e) {
            return {.success = false, .message = std::string{"Save failed: "} + e.what()};
        } catch (const std::exception &e) {
            return {.success = false, .message = std::string{"Save failed: "} + e.what()};
        } catch (...) {
            return {.success = false, .message = "Save failed"};
        }
    }

    [[nodiscard]] ActionResult LoadStateFromSlot(std::size_t slotIndex) {
        if (slotIndex >= 10) {
            return {.success = false, .message = "Invalid save state slot"};
        }

        try {
            std::scoped_lock lock{m_coreMutex};

            const auto discHash = m_saturn.GetDiscHash();
            const auto statePath = SaveStatesDirectory(discHash) / (std::to_string(slotIndex) + ".savestate");
            std::ifstream in{statePath, std::ios::binary};
            if (!in) {
                return {.success = false, .message = "State 1 is empty"};
            }

            auto state = std::make_unique<ymir::savestate::SaveState>();
            cereal::PortableBinaryInputArchive archive{in};
            archive(*state);

            if (!state->ValidateDiscHash(discHash)) {
                return {.success = false, .message = "That state belongs to a different game"};
            }

            if (!m_saturn.LoadState(*state, true)) {
                return {.success = false, .message = "Could not load that state"};
            }

            ResetInputs();
            return {.success = true, .message = "State " + std::to_string(slotIndex) + " loaded"};
        } catch (const cereal::Exception &e) {
            return {.success = false, .message = std::string{"Load failed: "} + e.what()};
        } catch (const std::exception &e) {
            return {.success = false, .message = std::string{"Load failed: "} + e.what()};
        } catch (...) {
            return {.success = false, .message = "Load failed"};
        }
    }

    void RequestStop() {
        ResetInputs();
        m_running = false;
        SDL_Event quit_event{};
        quit_event.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&quit_event);
    }

    void SetTouchControls(std::uint32_t buttonMask, int dpadX, int dpadY, float analogX, float analogY) {
        std::scoped_lock lock{m_inputMutex};

        m_touchButtons = Button::Default;
        SetButtonState(m_touchButtons, Button::A, (buttonMask & kTouchButtonA) != 0);
        SetButtonState(m_touchButtons, Button::B, (buttonMask & kTouchButtonB) != 0);
        SetButtonState(m_touchButtons, Button::C, (buttonMask & kTouchButtonC) != 0);
        SetButtonState(m_touchButtons, Button::X, (buttonMask & kTouchButtonX) != 0);
        SetButtonState(m_touchButtons, Button::Y, (buttonMask & kTouchButtonY) != 0);
        SetButtonState(m_touchButtons, Button::Z, (buttonMask & kTouchButtonZ) != 0);
        SetButtonState(m_touchButtons, Button::L, (buttonMask & kTouchButtonL) != 0);
        SetButtonState(m_touchButtons, Button::R, (buttonMask & kTouchButtonR) != 0);
        SetButtonState(m_touchButtons, Button::Start, (buttonMask & kTouchButtonStart) != 0);

        m_touchDpadX = std::clamp(dpadX, -1, 1);
        m_touchDpadY = std::clamp(dpadY, -1, 1);
        m_touchAnalogX = ClampAnalog(analogX);
        m_touchAnalogY = ClampAnalog(analogY);
        RebuildTouchDirections();
    }

    int Run() {
        SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");

        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD | SDL_INIT_EVENTS)) {
            SDL_Log("SDL_Init failed: %s", SDL_GetError());
            return 1;
        }

        LoadGameControllerDatabase();

        if (!CreateWindowAndRenderer()) {
            Shutdown();
            return 1;
        }

        if (!InitializeAudio()) {
            Shutdown();
            return 1;
        }

        if (!InitializeCore()) {
            Shutdown();
            return 1;
        }

        SetActiveApp(this);

        while (m_running.load()) {
            SDL_Event event{};
            if (m_paused.load()) {
                if (SDL_WaitEventTimeout(&event, 16)) {
                    HandleEvent(event);
                    while (SDL_PollEvent(&event)) {
                        HandleEvent(event);
                    }
                }
                continue;
            }

            while (SDL_PollEvent(&event)) {
                HandleEvent(event);
            }

            if (!m_running.load()) {
                break;
            }

            {
                std::scoped_lock lock{m_coreMutex};
                if (!m_running.load() || m_paused.load()) {
                    continue;
                }
                m_saturn.RunFrame();
            }
            Present();
        }

        Shutdown();
        return 0;
    }

private:
    static constexpr int kAudioSampleRate = 44100;
    static constexpr SDL_AudioFormat kAudioFormat = SDL_AUDIO_S16;
    static constexpr int kAudioChannels = 2;
    static constexpr std::uint32_t kAudioBufferFrames = 512;

    BootstrapConfig m_config;
    ymir::Saturn m_saturn{};
    app::AudioSystem m_audioSystem{};

    SDL_Window *m_window = nullptr;
    SDL_Renderer *m_renderer = nullptr;
    SDL_Texture *m_texture = nullptr;
    SDL_Gamepad *m_gamepad = nullptr;
    SDL_JoystickID m_gamepadId = 0;
    std::mutex m_coreMutex{};
    std::mutex m_inputMutex{};

    std::vector<std::uint32_t> m_framebuffer;
    std::uint32_t m_frameWidth = 320;
    std::uint32_t m_frameHeight = 224;
    std::uint32_t m_textureWidth = 0;
    std::uint32_t m_textureHeight = 0;
    bool m_frameDirty = false;
    std::atomic_bool m_running = true;
    std::atomic_bool m_paused = false;
    bool m_audioStarted = false;

    Button m_physicalButtons = Button::Default;
    Button m_touchButtons = Button::Default;
    bool m_physicalDpadUp = false;
    bool m_physicalDpadDown = false;
    bool m_physicalDpadLeft = false;
    bool m_physicalDpadRight = false;
    float m_physicalStickX = 0.0f;
    float m_physicalStickY = 0.0f;
    int m_touchDpadX = 0;
    int m_touchDpadY = 0;
    float m_touchAnalogX = 0.0f;
    float m_touchAnalogY = 0.0f;

    std::filesystem::path StateDirectory() const {
        return std::filesystem::path{m_config.dataRoot} / "state";
    }

    std::filesystem::path SavesDirectory() const {
        return std::filesystem::path{m_config.dataRoot} / "saves";
    }

    std::filesystem::path SaveStatesRootDirectory() const {
        return StateDirectory() / "savestates";
    }

    std::filesystem::path SaveStatesDirectory(const ymir::XXH128Hash &discHash) const {
        return SaveStatesRootDirectory() / ymir::ToString(discHash);
    }

    void LoadGameControllerDatabase() const {
        if (m_config.gameControllerDbPath.empty()) {
            return;
        }

        SDL_Log("Loading controller database from %s", m_config.gameControllerDbPath.c_str());
        const int result = SDL_AddGamepadMappingsFromFile(m_config.gameControllerDbPath.c_str());
        if (result < 0) {
            SDL_LogWarn(
                SDL_LOG_CATEGORY_APPLICATION, "Failed to load controller database: %s", SDL_GetError());
            return;
        }

        SDL_Log("Loaded %d controller mappings", result);
    }

    bool CreateWindowAndRenderer() {
        m_window = SDL_CreateWindow(
            "NEON SATURN", 1280, 720, SDL_WINDOW_FULLSCREEN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
        if (m_window == nullptr) {
            SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
            return false;
        }

        if (!CreateRenderer("opengles2") && !CreateRenderer(nullptr)) {
            SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
            return false;
        }

        return EnsureTexture(m_frameWidth, m_frameHeight);
    }

    bool CreateRenderer(const char *rendererName) {
        if (m_renderer != nullptr) {
            SDL_DestroyRenderer(m_renderer);
            m_renderer = nullptr;
        }

        SDL_PropertiesID rendererProps = SDL_CreateProperties();
        if (rendererProps == 0) {
            SDL_Log("SDL_CreateProperties failed: %s", SDL_GetError());
            return false;
        }

        SDL_SetPointerProperty(rendererProps, SDL_PROP_RENDERER_CREATE_WINDOW_POINTER, m_window);
        SDL_SetNumberProperty(rendererProps, SDL_PROP_RENDERER_CREATE_PRESENT_VSYNC_NUMBER, 1);
        if (rendererName != nullptr) {
            SDL_SetStringProperty(rendererProps, SDL_PROP_RENDERER_CREATE_NAME_STRING, rendererName);
        }

        m_renderer = SDL_CreateRendererWithProperties(rendererProps);
        SDL_DestroyProperties(rendererProps);
        return m_renderer != nullptr;
    }

    bool InitializeAudio() {
        if (!m_audioSystem.Init(kAudioSampleRate, kAudioFormat, kAudioChannels, kAudioBufferFrames)) {
            SDL_Log("Audio init failed: %s", SDL_GetError());
            return false;
        }

        if (!m_audioSystem.Start()) {
            SDL_Log("Audio start failed: %s", SDL_GetError());
            return false;
        }

        m_audioStarted = true;
        return true;
    }

    bool InitializeCore() {
        if (m_config.iplPath.empty() || m_config.discPath.empty()) {
            SDL_Log("Bootstrap config is incomplete");
            return false;
        }

        std::error_code error{};
        std::filesystem::create_directories(StateDirectory(), error);
        if (error) {
            SDL_Log("Failed to create state directory: %s", error.message().c_str());
            return false;
        }

        std::filesystem::create_directories(SavesDirectory(), error);
        if (error) {
            SDL_Log("Failed to create saves directory: %s", error.message().c_str());
            return false;
        }

        m_saturn.configuration.cdblock.useLLE = !m_config.cdbPath.empty();
        m_saturn.configuration.NotifyObservers();

        if (auto *renderer = m_saturn.VDP.UseSoftwareRenderer(); renderer == nullptr) {
            SDL_Log("Failed to activate software renderer");
            return false;
        }

        m_saturn.VDP.GetRenderer().Callbacks.VDP2ResolutionChanged =
            util::MakeClassMemberOptionalCallback<&EmulatorApp::OnResolutionChanged>(this);
        m_saturn.VDP.SetSoftwareRenderCallback(
            util::MakeClassMemberOptionalCallback<&EmulatorApp::OnFrameComplete>(this));

        m_saturn.SCSP.SetSampleCallback(
            util::MakeClassMemberOptionalCallback<&app::AudioSystem::ReceiveSample>(&m_audioSystem));

        m_saturn.SMPC.GetPeripheralPort1().SetPeripheralReportCallback(
            util::MakeClassMemberOptionalCallback<&EmulatorApp::OnPeripheralReport>(this));
        m_saturn.SMPC.GetPeripheralPort1().ConnectAnalogPad();

        const auto iplLoadResult = util::LoadIPLROM(m_config.iplPath, m_saturn);
        if (!iplLoadResult.succeeded) {
            SDL_Log("IPL load failed: %s", iplLoadResult.errorMessage.c_str());
            return false;
        }

        if (!m_config.cdbPath.empty()) {
            const auto cdbLoadResult = util::LoadCDBlockROM(m_config.cdbPath, m_saturn);
            if (!cdbLoadResult.succeeded) {
                SDL_Log("CD Block ROM load failed: %s", cdbLoadResult.errorMessage.c_str());
                return false;
            }
        }

        m_saturn.LoadInternalBackupMemoryImage(SavesDirectory() / "internal.bkr", false, error);
        if (error) {
            SDL_Log("Internal backup RAM setup failed: %s", error.message().c_str());
            return false;
        }

        error.clear();
        m_saturn.SMPC.LoadPersistentDataFrom(StateDirectory() / "smpc.bin", error);
        if (error && error.value() != ENOENT) {
            SDL_Log("SMPC persistent load warning: %s", error.message().c_str());
        }

        ymir::media::Disc disc{};
        const bool discLoaded = ymir::media::LoadDisc(
            m_config.discPath, disc, false,
            [](ymir::media::MessageType type, std::string message) {
                switch (type) {
                case ymir::media::MessageType::Error:
                case ymir::media::MessageType::NotValid: SDL_Log("Disc load: %s", message.c_str()); break;
                default: SDL_LogVerbose(SDL_LOG_CATEGORY_APPLICATION, "Disc load: %s", message.c_str()); break;
                }
            });
        if (!discLoaded) {
            SDL_Log("Disc load failed for %s", m_config.discPath.c_str());
            return false;
        }

        m_saturn.LoadDisc(std::move(disc));
        return true;
    }

    void HandleEvent(const SDL_Event &event) {
        switch (event.type) {
        case SDL_EVENT_QUIT:
            m_running = false;
            break;

        case SDL_EVENT_RENDER_DEVICE_RESET:
            DestroyTexture();
            EnsureTexture(m_frameWidth, m_frameHeight);
            break;

        case SDL_EVENT_WILL_ENTER_BACKGROUND:
            if (m_audioStarted && m_audioSystem.IsRunning()) {
                m_audioSystem.Stop();
            }
            break;

        case SDL_EVENT_DID_ENTER_FOREGROUND:
            if (!m_paused.load() && m_audioStarted && !m_audioSystem.IsRunning()) {
                m_audioSystem.Start();
            }
            break;

        case SDL_EVENT_GAMEPAD_ADDED:
            if (m_gamepad == nullptr) {
                m_gamepad = SDL_OpenGamepad(event.gdevice.which);
                m_gamepadId = event.gdevice.which;
            }
            break;

        case SDL_EVENT_GAMEPAD_REMOVED:
            if (m_gamepad != nullptr && event.gdevice.which == m_gamepadId) {
                SDL_CloseGamepad(m_gamepad);
                m_gamepad = nullptr;
                m_gamepadId = 0;
                ResetInputs();
            }
            break;

        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            if (!m_paused.load()) {
                HandleGamepadAxis(event);
            }
            break;

        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
            if (event.gbutton.button == SDL_GAMEPAD_BUTTON_BACK && event.gbutton.down) {
                RequestQuickActionsDialog();
                break;
            }
            if (!m_paused.load()) {
                HandleGamepadButton(event.gbutton.button, event.gbutton.down);
            }
            break;

        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            if (!m_paused.load()) {
                HandleKeyboard(event.key.scancode, event.key.down);
            }
            break;

        default:
            break;
        }
    }

    void HandleKeyboard(SDL_Scancode scancode, bool pressed) {
        std::scoped_lock lock{m_inputMutex};
        switch (scancode) {
        case SDL_SCANCODE_UP: m_physicalDpadUp = pressed; RebuildPhysicalDirections(); break;
        case SDL_SCANCODE_DOWN: m_physicalDpadDown = pressed; RebuildPhysicalDirections(); break;
        case SDL_SCANCODE_LEFT: m_physicalDpadLeft = pressed; RebuildPhysicalDirections(); break;
        case SDL_SCANCODE_RIGHT: m_physicalDpadRight = pressed; RebuildPhysicalDirections(); break;
        case SDL_SCANCODE_RETURN: SetButtonState(m_physicalButtons, Button::Start, pressed); break;
        case SDL_SCANCODE_Z: SetButtonState(m_physicalButtons, Button::A, pressed); break;
        case SDL_SCANCODE_X: SetButtonState(m_physicalButtons, Button::B, pressed); break;
        case SDL_SCANCODE_C: SetButtonState(m_physicalButtons, Button::C, pressed); break;
        case SDL_SCANCODE_A: SetButtonState(m_physicalButtons, Button::X, pressed); break;
        case SDL_SCANCODE_S: SetButtonState(m_physicalButtons, Button::Y, pressed); break;
        case SDL_SCANCODE_D: SetButtonState(m_physicalButtons, Button::Z, pressed); break;
        case SDL_SCANCODE_Q: SetButtonState(m_physicalButtons, Button::L, pressed); break;
        case SDL_SCANCODE_W: SetButtonState(m_physicalButtons, Button::R, pressed); break;
        default:
            break;
        }
    }

    void HandleGamepadAxis(const SDL_Event &event) {
        const float value =
            event.gaxis.value < 0 ? event.gaxis.value / 32768.0f : event.gaxis.value / 32767.0f;

        std::scoped_lock lock{m_inputMutex};
        switch (static_cast<SDL_GamepadAxis>(event.gaxis.axis)) {
        case SDL_GAMEPAD_AXIS_LEFTX:
            m_physicalStickX = ApplyDeadZone(value);
            RebuildPhysicalDirections();
            break;
        case SDL_GAMEPAD_AXIS_LEFTY:
            m_physicalStickY = ApplyDeadZone(value);
            RebuildPhysicalDirections();
            break;
        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:
            SetButtonState(m_physicalButtons, Button::C, value >= 0.5f);
            break;
        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:
            SetButtonState(m_physicalButtons, Button::Z, value >= 0.5f);
            break;
        default:
            break;
        }
    }

    void HandleGamepadButton(std::uint8_t button, bool pressed) {
        std::scoped_lock lock{m_inputMutex};
        switch (static_cast<SDL_GamepadButton>(button)) {
        case SDL_GAMEPAD_BUTTON_SOUTH: SetButtonState(m_physicalButtons, Button::A, pressed); break;
        case SDL_GAMEPAD_BUTTON_EAST: SetButtonState(m_physicalButtons, Button::B, pressed); break;
        case SDL_GAMEPAD_BUTTON_WEST: SetButtonState(m_physicalButtons, Button::X, pressed); break;
        case SDL_GAMEPAD_BUTTON_NORTH: SetButtonState(m_physicalButtons, Button::Y, pressed); break;
        case SDL_GAMEPAD_BUTTON_START: SetButtonState(m_physicalButtons, Button::Start, pressed); break;
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: SetButtonState(m_physicalButtons, Button::L, pressed); break;
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: SetButtonState(m_physicalButtons, Button::R, pressed); break;
        case SDL_GAMEPAD_BUTTON_DPAD_UP: m_physicalDpadUp = pressed; RebuildPhysicalDirections(); break;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: m_physicalDpadDown = pressed; RebuildPhysicalDirections(); break;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: m_physicalDpadLeft = pressed; RebuildPhysicalDirections(); break;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: m_physicalDpadRight = pressed; RebuildPhysicalDirections(); break;
        default:
            break;
        }
    }

    void ResetInputs() {
        std::scoped_lock lock{m_inputMutex};
        m_physicalButtons = Button::Default;
        m_touchButtons = Button::Default;
        m_physicalDpadUp = false;
        m_physicalDpadDown = false;
        m_physicalDpadLeft = false;
        m_physicalDpadRight = false;
        m_physicalStickX = 0.0f;
        m_physicalStickY = 0.0f;
        m_touchDpadX = 0;
        m_touchDpadY = 0;
        m_touchAnalogX = 0.0f;
        m_touchAnalogY = 0.0f;
    }

    void RebuildPhysicalDirections() {
        static constexpr float kDirectionThreshold = 0.45f;
        SetButtonState(m_physicalButtons, Button::Up, m_physicalDpadUp || m_physicalStickY <= -kDirectionThreshold);
        SetButtonState(m_physicalButtons, Button::Down, m_physicalDpadDown || m_physicalStickY >= kDirectionThreshold);
        SetButtonState(m_physicalButtons, Button::Left, m_physicalDpadLeft || m_physicalStickX <= -kDirectionThreshold);
        SetButtonState(m_physicalButtons, Button::Right, m_physicalDpadRight || m_physicalStickX >= kDirectionThreshold);
    }

    void RebuildTouchDirections() {
        static constexpr float kDirectionThreshold = 0.45f;
        SetButtonState(m_touchButtons, Button::Up, m_touchDpadY < 0 || m_touchAnalogY <= -kDirectionThreshold);
        SetButtonState(m_touchButtons, Button::Down, m_touchDpadY > 0 || m_touchAnalogY >= kDirectionThreshold);
        SetButtonState(m_touchButtons, Button::Left, m_touchDpadX < 0 || m_touchAnalogX <= -kDirectionThreshold);
        SetButtonState(m_touchButtons, Button::Right, m_touchDpadX > 0 || m_touchAnalogX >= kDirectionThreshold);
    }

    void SetButtonState(Button &buttonMask, Button button, bool pressed) {
        if (pressed) {
            buttonMask &= ~button;
        } else {
            buttonMask |= button;
        }
    }

    void OnResolutionChanged(std::uint32_t width, std::uint32_t height) {
        if (width == 0 || height == 0) {
            return;
        }

        m_frameWidth = width;
        m_frameHeight = height;
        m_framebuffer.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    }

    void OnFrameComplete(std::uint32_t *framebuffer, std::uint32_t width, std::uint32_t height) {
        OnResolutionChanged(width, height);
        if (m_framebuffer.empty()) {
            return;
        }

        std::copy_n(
            framebuffer,
            static_cast<std::size_t>(width) * static_cast<std::size_t>(height),
            m_framebuffer.begin());
        m_frameDirty = true;
    }

    void OnPeripheralReport(ymir::peripheral::PeripheralReport &report) {
        std::scoped_lock lock{m_inputMutex};
        const Button combinedButtons = m_physicalButtons & m_touchButtons;

        if (report.type == ymir::peripheral::PeripheralType::AnalogPad) {
            const float analogX = StrongerAnalogValue(m_physicalStickX, m_touchAnalogX);
            const float analogY = StrongerAnalogValue(m_physicalStickY, m_touchAnalogY);

            report.report.analogPad.buttons = combinedButtons;
            report.report.analogPad.analog = (std::abs(analogX) > 0.01f || std::abs(analogY) > 0.01f);
            report.report.analogPad.x = FloatToAnalogAxis(analogX);
            report.report.analogPad.y = FloatToAnalogAxis(analogY);
            report.report.analogPad.l = IsButtonPressed(combinedButtons, Button::L) ? 0xFF : 0x00;
            report.report.analogPad.r = IsButtonPressed(combinedButtons, Button::R) ? 0xFF : 0x00;
        } else if (report.type == ymir::peripheral::PeripheralType::ControlPad) {
            report.report.controlPad.buttons = combinedButtons;
        }
    }

    bool EnsureTexture(std::uint32_t width, std::uint32_t height) {
        if (m_renderer == nullptr || width == 0 || height == 0) {
            return false;
        }

        if (m_texture != nullptr && m_textureWidth == width && m_textureHeight == height) {
            return true;
        }

        DestroyTexture();
        m_texture = SDL_CreateTexture(
            m_renderer, SDL_PIXELFORMAT_XBGR8888, SDL_TEXTUREACCESS_STREAMING, static_cast<int>(width),
            static_cast<int>(height));
        if (m_texture == nullptr) {
            SDL_Log("SDL_CreateTexture failed: %s", SDL_GetError());
            return false;
        }

        SDL_SetTextureScaleMode(m_texture,
            m_config.textureFilter == "bilinear" ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
        m_textureWidth = width;
        m_textureHeight = height;
        return true;
    }

    void Present() {
        if (!EnsureTexture(m_frameWidth, m_frameHeight)) {
            m_running = false;
            return;
        }

        if (m_frameDirty && !m_framebuffer.empty()) {
            SDL_UpdateTexture(
                m_texture, nullptr, m_framebuffer.data(), static_cast<int>(m_frameWidth * sizeof(std::uint32_t)));
            m_frameDirty = false;
        }

        SDL_SetRenderDrawColor(m_renderer, 0, 0, 0, 255);
        SDL_RenderClear(m_renderer);
        if (m_texture != nullptr) {
            if (m_config.aspectRatio == "stretch") {
                SDL_RenderTexture(m_renderer, m_texture, nullptr, nullptr);
            } else {
                int outW = 0;
                int outH = 0;
                SDL_GetRenderOutputSize(m_renderer, &outW, &outH);

                const float targetAspect = m_config.aspectRatio == "16:9"
                    ? 16.0f / 9.0f
                    : 4.0f / 3.0f;
                const float windowAspect =
                    static_cast<float>(outW) / static_cast<float>(outH);

                SDL_FRect dest{};
                if (windowAspect > targetAspect) {
                    dest.h = static_cast<float>(outH);
                    dest.w = dest.h * targetAspect;
                    dest.x = (static_cast<float>(outW) - dest.w) / 2.0f;
                    dest.y = 0.0f;
                } else {
                    dest.w = static_cast<float>(outW);
                    dest.h = dest.w / targetAspect;
                    dest.x = 0.0f;
                    dest.y = (static_cast<float>(outH) - dest.h) / 2.0f;
                }
                SDL_RenderTexture(m_renderer, m_texture, nullptr, &dest);
            }
        }
        SDL_RenderPresent(m_renderer);
    }

    void DestroyTexture() {
        if (m_texture != nullptr) {
            SDL_DestroyTexture(m_texture);
            m_texture = nullptr;
        }
        m_textureWidth = 0;
        m_textureHeight = 0;
    }

    void Shutdown() {
        SetActiveApp(nullptr);

        if (!m_config.dataRoot.empty()) {
            std::error_code error{};
            m_saturn.SMPC.SavePersistentDataTo(StateDirectory() / "smpc.bin", error);
            if (error) {
                SDL_Log("SMPC persistent save warning: %s", error.message().c_str());
            }
        }

        if (m_gamepad != nullptr) {
            SDL_CloseGamepad(m_gamepad);
            m_gamepad = nullptr;
        }

        if (m_audioStarted && m_audioSystem.IsRunning()) {
            m_audioSystem.Stop();
        }
        m_audioSystem.Deinit();

        DestroyTexture();
        if (m_renderer != nullptr) {
            SDL_DestroyRenderer(m_renderer);
            m_renderer = nullptr;
        }
        if (m_window != nullptr) {
            SDL_DestroyWindow(m_window);
            m_window = nullptr;
        }

        SDL_Quit();
    }
};

extern "C" JNIEXPORT void JNICALL
Java_com_izzy2lost_neonsaturn_EmulatorActivity_nativeSetPaused(JNIEnv *, jobject, jboolean paused) {
    if (auto *app = GetActiveApp(); app != nullptr) {
        app->SetPaused(paused == JNI_TRUE);
    }
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_izzy2lost_neonsaturn_EmulatorActivity_nativeSaveState(JNIEnv *env, jobject, jint slotIndex) {
    if (auto *app = GetActiveApp(); app != nullptr) {
        return env->NewStringUTF(app->SaveStateToSlot(static_cast<std::size_t>(slotIndex)).message.c_str());
    }

    return env->NewStringUTF("Emulator is not ready yet");
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_izzy2lost_neonsaturn_EmulatorActivity_nativeLoadState(JNIEnv *env, jobject, jint slotIndex) {
    if (auto *app = GetActiveApp(); app != nullptr) {
        return env->NewStringUTF(app->LoadStateFromSlot(static_cast<std::size_t>(slotIndex)).message.c_str());
    }

    return env->NewStringUTF("Emulator is not ready yet");
}

extern "C" JNIEXPORT void JNICALL
Java_com_izzy2lost_neonsaturn_EmulatorActivity_nativeExitEmulator(JNIEnv *, jobject) {
    if (auto *app = GetActiveApp(); app != nullptr) {
        app->RequestStop();
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_izzy2lost_neonsaturn_EmulatorActivity_nativeUpdateTouchControls(
    JNIEnv *,
    jobject,
    jint buttonMask,
    jint dpadX,
    jint dpadY,
    jfloat analogX,
    jfloat analogY) {
    if (auto *app = GetActiveApp(); app != nullptr) {
        app->SetTouchControls(
            static_cast<std::uint32_t>(buttonMask),
            static_cast<int>(dpadX),
            static_cast<int>(dpadY),
            static_cast<float>(analogX),
            static_cast<float>(analogY));
    }
}

} // namespace neonsaturn::android

int SDL_main(int argc, char **argv) {
    auto config = neonsaturn::android::BootstrapConfigFromArgs(argc, argv);
    SDL_Log(
        "Bootstrap args parsed: ipl=%s disc=%s cdb=%s controllerdb=%s dataRoot=%s",
        config.iplPath.empty() ? "missing" : "set",
        config.discPath.empty() ? "missing" : "set",
        config.cdbPath.empty() ? "missing" : "unset",
        config.gameControllerDbPath.empty() ? "unset" : "set",
        config.dataRoot.empty() ? "missing" : "set");
    auto app = std::make_unique<neonsaturn::android::EmulatorApp>(std::move(config));
    return app->Run();
}
