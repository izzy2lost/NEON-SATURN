#include "bootstrap.hpp"

#include "audio_system.hpp"
#include "rom_loader.hpp"

#include <ymir/hw/scsp/scsp.hpp>
#include <ymir/hw/smpc/peripheral/peripheral_report.hpp>
#include <ymir/hw/vdp/renderer/vdp_renderer_base.hpp>
#include <ymir/hw/vdp/vdp.hpp>
#include <ymir/media/loader/loader.hpp>
#include <ymir/sys/saturn.hpp>
#include <ymir/util/callback.hpp>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace neonsaturn::android {

using ymir::peripheral::Button;

namespace {

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
        }
    }
    return config;
}

} // namespace

class EmulatorApp {
public:
    explicit EmulatorApp(BootstrapConfig config)
        : m_config(std::move(config)) {}

    int Run() {
        SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");

        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD | SDL_INIT_EVENTS)) {
            SDL_Log("SDL_Init failed: %s", SDL_GetError());
            return 1;
        }

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

        while (m_running) {
            SDL_Event event{};
            while (SDL_PollEvent(&event)) {
                HandleEvent(event);
            }

            if (!m_running) {
                break;
            }

            m_saturn.RunFrame();
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

    std::vector<std::uint32_t> m_framebuffer;
    std::uint32_t m_frameWidth = 320;
    std::uint32_t m_frameHeight = 224;
    std::uint32_t m_textureWidth = 0;
    std::uint32_t m_textureHeight = 0;
    bool m_frameDirty = false;
    bool m_running = true;
    bool m_audioStarted = false;

    Button m_buttons = Button::Default;
    bool m_dpadUp = false;
    bool m_dpadDown = false;
    bool m_dpadLeft = false;
    bool m_dpadRight = false;
    bool m_stickUp = false;
    bool m_stickDown = false;
    bool m_stickLeft = false;
    bool m_stickRight = false;

    std::filesystem::path StateDirectory() const {
        return std::filesystem::path{m_config.dataRoot} / "state";
    }

    std::filesystem::path SavesDirectory() const {
        return std::filesystem::path{m_config.dataRoot} / "saves";
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
        m_saturn.SMPC.GetPeripheralPort1().ConnectControlPad();

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
            if (m_audioStarted && !m_audioSystem.IsRunning()) {
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
            }
            break;

        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            HandleGamepadAxis(event);
            break;

        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
            HandleGamepadButton(event.gbutton.button, event.gbutton.down);
            break;

        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            HandleKeyboard(event.key.scancode, event.key.down);
            break;

        default:
            break;
        }
    }

    void HandleKeyboard(SDL_Scancode scancode, bool pressed) {
        switch (scancode) {
        case SDL_SCANCODE_UP: m_dpadUp = pressed; RebuildDirections(); break;
        case SDL_SCANCODE_DOWN: m_dpadDown = pressed; RebuildDirections(); break;
        case SDL_SCANCODE_LEFT: m_dpadLeft = pressed; RebuildDirections(); break;
        case SDL_SCANCODE_RIGHT: m_dpadRight = pressed; RebuildDirections(); break;
        case SDL_SCANCODE_RETURN: SetButtonState(Button::Start, pressed); break;
        case SDL_SCANCODE_Z: SetButtonState(Button::A, pressed); break;
        case SDL_SCANCODE_X: SetButtonState(Button::B, pressed); break;
        case SDL_SCANCODE_C: SetButtonState(Button::C, pressed); break;
        case SDL_SCANCODE_A: SetButtonState(Button::X, pressed); break;
        case SDL_SCANCODE_S: SetButtonState(Button::Y, pressed); break;
        case SDL_SCANCODE_D: SetButtonState(Button::Z, pressed); break;
        case SDL_SCANCODE_Q: SetButtonState(Button::L, pressed); break;
        case SDL_SCANCODE_W: SetButtonState(Button::R, pressed); break;
        case SDL_SCANCODE_ESCAPE:
            if (pressed) {
                m_running = false;
            }
            break;
        default:
            break;
        }
    }

    void HandleGamepadAxis(const SDL_Event &event) {
        const float value =
            event.gaxis.value < 0 ? event.gaxis.value / 32768.0f : event.gaxis.value / 32767.0f;

        switch (static_cast<SDL_GamepadAxis>(event.gaxis.axis)) {
        case SDL_GAMEPAD_AXIS_LEFTX:
            m_stickLeft = value <= -0.45f;
            m_stickRight = value >= 0.45f;
            RebuildDirections();
            break;
        case SDL_GAMEPAD_AXIS_LEFTY:
            m_stickUp = value <= -0.45f;
            m_stickDown = value >= 0.45f;
            RebuildDirections();
            break;
        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:
            SetButtonState(Button::C, value >= 0.5f);
            break;
        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:
            SetButtonState(Button::Z, value >= 0.5f);
            break;
        default:
            break;
        }
    }

    void HandleGamepadButton(std::uint8_t button, bool pressed) {
        switch (static_cast<SDL_GamepadButton>(button)) {
        case SDL_GAMEPAD_BUTTON_SOUTH: SetButtonState(Button::A, pressed); break;
        case SDL_GAMEPAD_BUTTON_EAST: SetButtonState(Button::B, pressed); break;
        case SDL_GAMEPAD_BUTTON_WEST: SetButtonState(Button::X, pressed); break;
        case SDL_GAMEPAD_BUTTON_NORTH: SetButtonState(Button::Y, pressed); break;
        case SDL_GAMEPAD_BUTTON_START: SetButtonState(Button::Start, pressed); break;
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: SetButtonState(Button::L, pressed); break;
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: SetButtonState(Button::R, pressed); break;
        case SDL_GAMEPAD_BUTTON_DPAD_UP: m_dpadUp = pressed; RebuildDirections(); break;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: m_dpadDown = pressed; RebuildDirections(); break;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: m_dpadLeft = pressed; RebuildDirections(); break;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: m_dpadRight = pressed; RebuildDirections(); break;
        case SDL_GAMEPAD_BUTTON_BACK:
            if (pressed) {
                m_running = false;
            }
            break;
        default:
            break;
        }
    }

    void RebuildDirections() {
        SetButtonState(Button::Up, m_dpadUp || m_stickUp);
        SetButtonState(Button::Down, m_dpadDown || m_stickDown);
        SetButtonState(Button::Left, m_dpadLeft || m_stickLeft);
        SetButtonState(Button::Right, m_dpadRight || m_stickRight);
    }

    void SetButtonState(Button button, bool pressed) {
        if (pressed) {
            m_buttons &= ~button;
        } else {
            m_buttons |= button;
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
        if (report.type == ymir::peripheral::PeripheralType::ControlPad) {
            report.report.controlPad.buttons = m_buttons;
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

        SDL_SetTextureScaleMode(m_texture, SDL_SCALEMODE_NEAREST);
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
            SDL_RenderTexture(m_renderer, m_texture, nullptr, nullptr);
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

} // namespace neonsaturn::android

int SDL_main(int argc, char **argv) {
    auto config = neonsaturn::android::BootstrapConfigFromArgs(argc, argv);
    SDL_Log(
        "Bootstrap args parsed: ipl=%s disc=%s cdb=%s dataRoot=%s",
        config.iplPath.empty() ? "missing" : "set",
        config.discPath.empty() ? "missing" : "set",
        config.cdbPath.empty() ? "missing" : "unset",
        config.dataRoot.empty() ? "missing" : "set");
    auto app = std::make_unique<neonsaturn::android::EmulatorApp>(std::move(config));
    return app->Run();
}
