#pragma once

#include "settings.hpp"
#include "shared_context.hpp"

#include <array>
#include <optional>

namespace app::on_screen_controls {

enum class ControlID : uint8 {
    DPad,
    AnalogStick,
    A,
    B,
    C,
    X,
    Y,
    Z,
    L,
    R,
    Start,
    Menu,
    Count,
};

inline constexpr size_t kControlCount = static_cast<size_t>(ControlID::Count);
inline constexpr std::array<ControlID, kControlCount> kControlIDs = {
    ControlID::DPad,        ControlID::AnalogStick, ControlID::A,     ControlID::B,
    ControlID::C,           ControlID::X,           ControlID::Y,     ControlID::Z,
    ControlID::L,           ControlID::R,           ControlID::Start, ControlID::Menu,
};

struct Viewport {
    ImVec2 pos{};
    ImVec2 size{};

    [[nodiscard]] bool IsValid() const {
        return size.x > 1.0f && size.y > 1.0f;
    }
};

struct Geometry {
    enum class Shape { Circle, Pill };

    ControlID id{};
    Shape shape = Shape::Circle;
    ImVec2 center{};
    ImVec2 halfSize{};
    float radius = 0.0f;
};

struct VisualState {
    std::array<bool, kControlCount> active{};
    ImVec2 dpad{};
    ImVec2 analogStick{};
    std::optional<ControlID> highlighted;
};

constexpr size_t ToIndex(ControlID id) {
    return static_cast<size_t>(id);
}

const char *GetName(ControlID id);
const char *GetLabel(ControlID id);

Settings::Input::OnScreenControls::Control &GetControl(Settings::Input::OnScreenControls &controls, ControlID id);
const Settings::Input::OnScreenControls::Control &GetControl(const Settings::Input::OnScreenControls &controls,
                                                             ControlID id);

void ResetLayout(Settings::Input::OnScreenControls &controls);

std::array<Geometry, kControlCount> BuildGeometry(const Settings::Input::OnScreenControls &controls,
                                                  const Viewport &viewport, float displayScale);

std::optional<ControlID> HitTest(const std::array<Geometry, kControlCount> &geometry, ImVec2 point);

ImVec2 EvaluateDPadVector(const Geometry &geometry, ImVec2 point);
ImVec2 EvaluateAnalogVector(const Geometry &geometry, ImVec2 point);

std::array<float, 2> ClampNormalizedPosition(const Settings::Input::OnScreenControls &controls, ControlID id,
                                             const Viewport &viewport, float displayScale,
                                             std::array<float, 2> position);

void Draw(ImDrawList *drawList, const SharedContext &context, const Settings::Input::OnScreenControls &controls,
          const Viewport &viewport, const VisualState &visuals, float alphaScale = 1.0f);

} // namespace app::on_screen_controls
