#include "on_screen_controls.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace app::on_screen_controls {

namespace {

struct StyleColors {
    ImU32 fill;
    ImU32 fillActive;
    ImU32 border;
    ImU32 text;
    ImU32 textActive;
    ImU32 shadow;
    ImU32 accent;
};

ImVec4 WithAlpha(ImVec4 color, float alpha) {
    color.w *= alpha;
    return color;
}

ImVec4 ScaleRGB(ImVec4 color, float factor) {
    return {
        std::clamp(color.x * factor, 0.0f, 1.0f),
        std::clamp(color.y * factor, 0.0f, 1.0f),
        std::clamp(color.z * factor, 0.0f, 1.0f),
        color.w,
    };
}

ImVec2 ClampToUnitCircle(float x, float y) {
    const float lenSq = x * x + y * y;
    if (lenSq <= 1.0f) {
        return {x, y};
    }
    const float len = std::sqrt(lenSq);
    return {x / len, y / len};
}

float GetGlobalScale(const Settings::Input::OnScreenControls &controls, float displayScale) {
    return displayScale * controls.scale;
}

Geometry MakeCircle(ControlID id, const Settings::Input::OnScreenControls::Control &control, const Viewport &viewport,
                    float radius) {
    return {
        .id = id,
        .shape = Geometry::Shape::Circle,
        .center = {
            viewport.pos.x + control.position[0] * viewport.size.x,
            viewport.pos.y + control.position[1] * viewport.size.y,
        },
        .halfSize = {radius, radius},
        .radius = radius,
    };
}

Geometry MakePill(ControlID id, const Settings::Input::OnScreenControls::Control &control, const Viewport &viewport,
                  ImVec2 halfSize) {
    return {
        .id = id,
        .shape = Geometry::Shape::Pill,
        .center = {
            viewport.pos.x + control.position[0] * viewport.size.x,
            viewport.pos.y + control.position[1] * viewport.size.y,
        },
        .halfSize = halfSize,
        .radius = std::min(halfSize.x, halfSize.y),
    };
}

StyleColors GetColors(ControlID id, float alpha, bool active, bool highlighted) {
    const ImVec4 border = WithAlpha(ImVec4(0.08f, 0.08f, 0.10f, 1.00f), alpha);
    const ImVec4 shadow = WithAlpha(ImVec4(0.0f, 0.0f, 0.0f, 0.35f), alpha);
    const ImVec4 activeBorder = WithAlpha(ImVec4(0.98f, 0.97f, 0.95f, 0.95f), alpha);

    ImVec4 fill{};
    ImVec4 text{};
    ImVec4 accent{};
    switch (id) {
    case ControlID::A:
        fill = ImVec4(0.20f, 0.62f, 0.34f, 0.92f);
        text = ImVec4(0.06f, 0.12f, 0.08f, 1.00f);
        accent = ImVec4(0.74f, 0.95f, 0.78f, 0.95f);
        break;
    case ControlID::B:
        fill = ImVec4(0.96f, 0.80f, 0.08f, 0.94f);
        text = ImVec4(0.16f, 0.12f, 0.02f, 1.00f);
        accent = ImVec4(1.00f, 0.96f, 0.72f, 0.95f);
        break;
    case ControlID::C:
        fill = ImVec4(0.16f, 0.27f, 0.82f, 0.94f);
        text = ImVec4(0.95f, 0.97f, 1.00f, 1.00f);
        accent = ImVec4(0.75f, 0.82f, 1.00f, 0.95f);
        break;
    case ControlID::L:
    case ControlID::R:
    case ControlID::Start:
        fill = ImVec4(0.82f, 0.05f, 0.40f, 0.90f);
        text = ImVec4(0.98f, 0.95f, 0.97f, 1.00f);
        accent = ImVec4(1.00f, 0.70f, 0.84f, 0.95f);
        break;
    case ControlID::Menu:
        fill = ImVec4(0.15f, 0.16f, 0.20f, 0.88f);
        text = ImVec4(0.94f, 0.95f, 0.98f, 1.00f);
        accent = ImVec4(0.72f, 0.78f, 0.90f, 0.95f);
        break;
    case ControlID::DPad:
    case ControlID::AnalogStick:
        fill = ImVec4(0.12f, 0.12f, 0.14f, 0.86f);
        text = ImVec4(0.94f, 0.95f, 0.98f, 1.00f);
        accent = ImVec4(0.85f, 0.16f, 0.48f, 0.90f);
        break;
    default:
        fill = ImVec4(0.48f, 0.49f, 0.52f, 0.90f);
        text = ImVec4(0.10f, 0.10f, 0.12f, 1.00f);
        accent = ImVec4(0.92f, 0.92f, 0.94f, 0.95f);
        break;
    }

    if (highlighted) {
        fill = ScaleRGB(fill, 1.10f);
    }

    const ImVec4 fillActive = ScaleRGB(fill, active ? 1.20f : 1.00f);
    const ImVec4 textActive = active ? ImVec4(0.98f, 0.99f, 1.00f, 1.00f) : text;
    return {
        .fill = ImGui::ColorConvertFloat4ToU32(WithAlpha(fill, alpha)),
        .fillActive = ImGui::ColorConvertFloat4ToU32(WithAlpha(fillActive, alpha)),
        .border = ImGui::ColorConvertFloat4ToU32(highlighted ? activeBorder : border),
        .text = ImGui::ColorConvertFloat4ToU32(WithAlpha(text, alpha)),
        .textActive = ImGui::ColorConvertFloat4ToU32(WithAlpha(textActive, alpha)),
        .shadow = ImGui::ColorConvertFloat4ToU32(shadow),
        .accent = ImGui::ColorConvertFloat4ToU32(WithAlpha(accent, alpha)),
    };
}

ImVec2 CalcTextSize(ImFont *font, float size, const char *text) {
    const ImVec2 measured = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    return {measured.x, measured.y};
}

void DrawLabel(ImDrawList *drawList, ImFont *font, float fontSize, ImVec2 center, ImU32 color, const char *text) {
    if (text == nullptr || text[0] == '\0') {
        return;
    }

    const ImVec2 size = CalcTextSize(font, fontSize, text);
    const ImVec2 pos{center.x - size.x * 0.5f, center.y - size.y * 0.5f};
    drawList->AddText(font, fontSize, ImVec2(pos.x + 1.0f, pos.y + 1.0f), IM_COL32(0, 0, 0, 140), text);
    drawList->AddText(font, fontSize, pos, color, text);
}

float GetCircleRadius(ControlID id, float scale) {
    switch (id) {
    case ControlID::DPad: return 46.0f * scale;
    case ControlID::AnalogStick: return 48.0f * scale;
    case ControlID::A:
    case ControlID::B:
    case ControlID::C: return 28.0f * scale;
    case ControlID::X:
    case ControlID::Y:
    case ControlID::Z: return 24.0f * scale;
    default: return 24.0f * scale;
    }
}

ImVec2 GetPillHalfSize(ControlID id, float scale) {
    switch (id) {
    case ControlID::L:
    case ControlID::R: return {48.0f * scale, 18.0f * scale};
    case ControlID::Menu: return {36.0f * scale, 18.0f * scale};
    case ControlID::Start: return {30.0f * scale, 16.0f * scale};
    default: return {28.0f * scale, 14.0f * scale};
    }
}

float GetTextScale(ControlID id, const SharedContext &context, const Settings::Input::OnScreenControls &controls) {
    const float scale = controls.scale * context.displayScale;
    switch (id) {
    case ControlID::Menu: return std::max(11.0f * scale, context.fontSizes.small * 0.70f * context.displayScale);
    case ControlID::Start:
    case ControlID::L:
    case ControlID::R: return std::max(12.0f * scale, context.fontSizes.small * 0.85f * context.displayScale);
    default: return std::max(18.0f * scale, context.fontSizes.medium * 0.95f * context.displayScale);
    }
}

} // namespace

const char *GetName(ControlID id) {
    switch (id) {
    case ControlID::DPad: return "D-Pad";
    case ControlID::AnalogStick: return "Analog Stick";
    case ControlID::A: return "A";
    case ControlID::B: return "B";
    case ControlID::C: return "C";
    case ControlID::X: return "X";
    case ControlID::Y: return "Y";
    case ControlID::Z: return "Z";
    case ControlID::L: return "L";
    case ControlID::R: return "R";
    case ControlID::Start: return "Start";
    case ControlID::Menu: return "Menu";
    default: return "";
    }
}

const char *GetLabel(ControlID id) {
    switch (id) {
    case ControlID::DPad:
    case ControlID::AnalogStick: return "";
    case ControlID::Start: return "START";
    case ControlID::Menu: return "MENU";
    default: return GetName(id);
    }
}

Settings::Input::OnScreenControls::Control &GetControl(Settings::Input::OnScreenControls &controls, ControlID id) {
    switch (id) {
    case ControlID::DPad: return controls.dpad;
    case ControlID::AnalogStick: return controls.analogStick;
    case ControlID::A: return controls.a;
    case ControlID::B: return controls.b;
    case ControlID::C: return controls.c;
    case ControlID::X: return controls.x;
    case ControlID::Y: return controls.y;
    case ControlID::Z: return controls.z;
    case ControlID::L: return controls.l;
    case ControlID::R: return controls.r;
    case ControlID::Start: return controls.start;
    case ControlID::Menu: return controls.menu;
    default: return controls.menu;
    }
}

const Settings::Input::OnScreenControls::Control &GetControl(const Settings::Input::OnScreenControls &controls,
                                                             ControlID id) {
    switch (id) {
    case ControlID::DPad: return controls.dpad;
    case ControlID::AnalogStick: return controls.analogStick;
    case ControlID::A: return controls.a;
    case ControlID::B: return controls.b;
    case ControlID::C: return controls.c;
    case ControlID::X: return controls.x;
    case ControlID::Y: return controls.y;
    case ControlID::Z: return controls.z;
    case ControlID::L: return controls.l;
    case ControlID::R: return controls.r;
    case ControlID::Start: return controls.start;
    case ControlID::Menu: return controls.menu;
    default: return controls.menu;
    }
}

void ResetLayout(Settings::Input::OnScreenControls &controls) {
    controls.dpad.position = {0.16f, 0.74f};
    controls.analogStick.position = {0.33f, 0.82f};
    controls.a.position = {0.71f, 0.80f};
    controls.b.position = {0.81f, 0.74f};
    controls.c.position = {0.91f, 0.68f};
    controls.x.position = {0.69f, 0.60f};
    controls.y.position = {0.79f, 0.54f};
    controls.z.position = {0.89f, 0.49f};
    controls.l.position = {0.16f, 0.18f};
    controls.r.position = {0.84f, 0.18f};
    controls.start.position = {0.47f, 0.74f};
    controls.menu.position = {0.59f, 0.74f};
}

std::array<Geometry, kControlCount> BuildGeometry(const Settings::Input::OnScreenControls &controls,
                                                  const Viewport &viewport, float displayScale) {
    std::array<Geometry, kControlCount> geometry{};
    const float scale = GetGlobalScale(controls, displayScale);

    geometry[ToIndex(ControlID::DPad)] = MakeCircle(ControlID::DPad, controls.dpad, viewport,
                                                    GetCircleRadius(ControlID::DPad, scale));
    geometry[ToIndex(ControlID::AnalogStick)] =
        MakeCircle(ControlID::AnalogStick, controls.analogStick, viewport, GetCircleRadius(ControlID::AnalogStick, scale));
    geometry[ToIndex(ControlID::A)] = MakeCircle(ControlID::A, controls.a, viewport, GetCircleRadius(ControlID::A, scale));
    geometry[ToIndex(ControlID::B)] = MakeCircle(ControlID::B, controls.b, viewport, GetCircleRadius(ControlID::B, scale));
    geometry[ToIndex(ControlID::C)] = MakeCircle(ControlID::C, controls.c, viewport, GetCircleRadius(ControlID::C, scale));
    geometry[ToIndex(ControlID::X)] = MakeCircle(ControlID::X, controls.x, viewport, GetCircleRadius(ControlID::X, scale));
    geometry[ToIndex(ControlID::Y)] = MakeCircle(ControlID::Y, controls.y, viewport, GetCircleRadius(ControlID::Y, scale));
    geometry[ToIndex(ControlID::Z)] = MakeCircle(ControlID::Z, controls.z, viewport, GetCircleRadius(ControlID::Z, scale));
    geometry[ToIndex(ControlID::L)] = MakePill(ControlID::L, controls.l, viewport, GetPillHalfSize(ControlID::L, scale));
    geometry[ToIndex(ControlID::R)] = MakePill(ControlID::R, controls.r, viewport, GetPillHalfSize(ControlID::R, scale));
    geometry[ToIndex(ControlID::Start)] =
        MakePill(ControlID::Start, controls.start, viewport, GetPillHalfSize(ControlID::Start, scale));
    geometry[ToIndex(ControlID::Menu)] =
        MakePill(ControlID::Menu, controls.menu, viewport, GetPillHalfSize(ControlID::Menu, scale));
    return geometry;
}

std::optional<ControlID> HitTest(const std::array<Geometry, kControlCount> &geometry, ImVec2 point) {
    for (ControlID id : kControlIDs) {
        const Geometry &g = geometry[ToIndex(id)];
        if (g.shape == Geometry::Shape::Circle) {
            const float dx = point.x - g.center.x;
            const float dy = point.y - g.center.y;
            if (dx * dx + dy * dy <= g.radius * g.radius) {
                return id;
            }
        } else {
            const float left = g.center.x - g.halfSize.x;
            const float right = g.center.x + g.halfSize.x;
            const float top = g.center.y - g.halfSize.y;
            const float bottom = g.center.y + g.halfSize.y;
            if (point.x >= left && point.x <= right && point.y >= top && point.y <= bottom) {
                return id;
            }
        }
    }
    return {};
}

ImVec2 EvaluateDPadVector(const Geometry &geometry, ImVec2 point) {
    const float nx = (point.x - geometry.center.x) / geometry.radius;
    const float ny = (point.y - geometry.center.y) / geometry.radius;
    const ImVec2 clamped = ClampToUnitCircle(nx, ny);

    static constexpr float kDeadzone = 0.25f;
    static constexpr float kThreshold = 0.45f;
    if (clamped.x * clamped.x + clamped.y * clamped.y < kDeadzone * kDeadzone) {
        return {};
    }

    ImVec2 out{};
    if (std::abs(clamped.x) >= kThreshold) {
        out.x = std::signbit(clamped.x) ? -1.0f : 1.0f;
    }
    if (std::abs(clamped.y) >= kThreshold) {
        out.y = std::signbit(clamped.y) ? -1.0f : 1.0f;
    }
    return out;
}

ImVec2 EvaluateAnalogVector(const Geometry &geometry, ImVec2 point) {
    const float nx = (point.x - geometry.center.x) / geometry.radius;
    const float ny = (point.y - geometry.center.y) / geometry.radius;
    const ImVec2 clamped = ClampToUnitCircle(nx, ny);

    static constexpr float kDeadzone = 0.12f;
    if (clamped.x * clamped.x + clamped.y * clamped.y < kDeadzone * kDeadzone) {
        return {};
    }
    return clamped;
}

std::array<float, 2> ClampNormalizedPosition(const Settings::Input::OnScreenControls &controls, ControlID id,
                                             const Viewport &viewport, float displayScale,
                                             std::array<float, 2> position) {
    if (!viewport.IsValid()) {
        return position;
    }

    const float scale = GetGlobalScale(controls, displayScale);
    float extentX = 0.0f;
    float extentY = 0.0f;
    switch (id) {
    case ControlID::L:
    case ControlID::R:
    case ControlID::Start:
    case ControlID::Menu: {
        const ImVec2 halfSize = GetPillHalfSize(id, scale);
        extentX = halfSize.x;
        extentY = halfSize.y;
        break;
    }
    default: {
        const float radius = GetCircleRadius(id, scale);
        extentX = radius;
        extentY = radius;
        break;
    }
    }

    const float minX = extentX / viewport.size.x;
    const float minY = extentY / viewport.size.y;
    position[0] = std::clamp(position[0], minX, 1.0f - minX);
    position[1] = std::clamp(position[1], minY, 1.0f - minY);
    return position;
}

void Draw(ImDrawList *drawList, const SharedContext &context, const Settings::Input::OnScreenControls &controls,
          const Viewport &viewport, const VisualState &visuals, float alphaScale) {
    if (!viewport.IsValid()) {
        return;
    }

    const auto geometry = BuildGeometry(controls, viewport, context.displayScale);
    const float alpha = std::clamp(controls.opacity * alphaScale, 0.0f, 1.0f);
    const float globalScale = GetGlobalScale(controls, context.displayScale);

    const float borderThickness = std::max(2.0f * context.displayScale, 1.25f * globalScale);
    const float shadowOffset = 6.0f * context.displayScale;

    ImFont *font = context.fonts.sansSerif.bold != nullptr ? context.fonts.sansSerif.bold : context.fonts.sansSerif.regular;
    if (font == nullptr) {
        return;
    }

    for (ControlID id : kControlIDs) {
        const Geometry &g = geometry[ToIndex(id)];
        const bool active = visuals.active[ToIndex(id)];
        const bool highlighted = visuals.highlighted.has_value() && *visuals.highlighted == id;
        const StyleColors colors = GetColors(id, alpha, active, highlighted);

        if (g.shape == Geometry::Shape::Circle) {
            drawList->AddCircleFilled(ImVec2(g.center.x + shadowOffset, g.center.y + shadowOffset), g.radius, colors.shadow,
                                      32);
            drawList->AddCircleFilled(g.center, g.radius, active ? colors.fillActive : colors.fill, 32);
            drawList->AddCircle(g.center, g.radius, colors.border, 32, borderThickness);

            if (id == ControlID::DPad) {
                const float arm = g.radius * 0.70f;
                const float width = g.radius * 0.34f;
                drawList->AddRectFilled(ImVec2(g.center.x - width, g.center.y - arm), ImVec2(g.center.x + width, g.center.y + arm),
                                        IM_COL32(18, 18, 20, static_cast<int>(alpha * 255.0f)), g.radius * 0.18f);
                drawList->AddRectFilled(ImVec2(g.center.x - arm, g.center.y - width), ImVec2(g.center.x + arm, g.center.y + width),
                                        IM_COL32(18, 18, 20, static_cast<int>(alpha * 255.0f)), g.radius * 0.18f);
                drawList->AddCircleFilled(g.center, g.radius * 0.22f, IM_COL32(36, 36, 40, static_cast<int>(alpha * 255.0f)), 20);

                const ImVec2 dir = visuals.dpad;
                if (dir.x != 0.0f || dir.y != 0.0f) {
                    const ImVec2 pos{g.center.x + dir.x * g.radius * 0.38f, g.center.y + dir.y * g.radius * 0.38f};
                    drawList->AddCircleFilled(pos, g.radius * 0.20f, colors.accent, 20);
                }
            } else if (id == ControlID::AnalogStick) {
                drawList->AddCircle(g.center, g.radius * 0.62f, colors.accent, 24, std::max(2.0f, borderThickness * 0.65f));
                const ImVec2 stick = visuals.analogStick;
                const ImVec2 knobCenter{g.center.x + stick.x * g.radius * 0.36f, g.center.y + stick.y * g.radius * 0.36f};
                drawList->AddCircleFilled(knobCenter, g.radius * 0.38f, IM_COL32(40, 40, 46, static_cast<int>(alpha * 255.0f)), 28);
                drawList->AddCircle(knobCenter, g.radius * 0.38f, colors.border, 28, borderThickness * 0.85f);
                drawList->AddCircleFilled(knobCenter, g.radius * 0.12f, colors.accent, 20);
            } else {
                DrawLabel(drawList, font, GetTextScale(id, context, controls), g.center, active ? colors.textActive : colors.text,
                          GetLabel(id));
            }
        } else {
            const ImVec2 topLeft{g.center.x - g.halfSize.x, g.center.y - g.halfSize.y};
            const ImVec2 bottomRight{g.center.x + g.halfSize.x, g.center.y + g.halfSize.y};
            drawList->AddRectFilled(ImVec2(topLeft.x + shadowOffset, topLeft.y + shadowOffset),
                                    ImVec2(bottomRight.x + shadowOffset, bottomRight.y + shadowOffset), colors.shadow,
                                    g.radius);
            drawList->AddRectFilled(topLeft, bottomRight, active ? colors.fillActive : colors.fill, g.radius);
            drawList->AddRect(topLeft, bottomRight, colors.border, g.radius, ImDrawFlags_RoundCornersAll,
                              borderThickness);
            DrawLabel(drawList, font, GetTextScale(id, context, controls), g.center, active ? colors.textActive : colors.text,
                      GetLabel(id));
        }
    }
}

} // namespace app::on_screen_controls
