#include "input_settings_view.hpp"

#include <app/events/gui_event_factory.hpp>
#include <app/on_screen_controls.hpp>

#include <app/ui/widgets/common_widgets.hpp>
#include <app/ui/widgets/peripheral_widgets.hpp>

#include <app/input/input_utils.hpp>

#include <app/imgui_data.hpp>

#include <SDL3/SDL_misc.h>

#include <algorithm>

using namespace ymir;

namespace app::ui {

InputSettingsView::InputSettingsView(SharedContext &context)
    : SettingsViewBase(context) {}

void InputSettingsView::Display() {
    const YmirImGuiData *imguiData = GetYmirImGuiData();
    auto &settings = GetSettings().input;

    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);

    if (ImGui::BeginTable("periph_ports", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableNextRow();
        if (ImGui::TableNextColumn()) {
            ImGui::PushFont(imguiData->fonts.sansSerif.bold, imguiData->fontSizes.large);
            ImGui::SeparatorText("Port 1");
            ImGui::PopFont();

            if (MakeDirty(widgets::Port1PeripheralSelector(m_context))) {
                m_context.EnqueueEvent(events::gui::RebindInputs());
            }
        }
        if (ImGui::TableNextColumn()) {
            ImGui::PushFont(imguiData->fonts.sansSerif.bold, imguiData->fontSizes.large);
            ImGui::SeparatorText("Port 2");
            ImGui::PopFont();

            if (MakeDirty(widgets::Port2PeripheralSelector(m_context))) {
                m_context.EnqueueEvent(events::gui::RebindInputs());
            }
        }
        ImGui::EndTable();
    }

    // -------------------------------------------------------------------------

    ImGui::PushFont(imguiData->fonts.sansSerif.bold, imguiData->fontSizes.large);
    ImGui::SeparatorText("Mouse");
    ImGui::PopFont();

    auto mouseCaptureModeRadio = [&](const char *name, Settings::Input::Mouse::CaptureMode value,
                                     const char *explanation) {
        if (MakeDirty(ImGui::RadioButton(name, settings.mouse.captureMode == value))) {
            settings.mouse.captureMode = value;
        }
        widgets::ExplanationTooltip(explanation, imguiData->displayScale);
    };

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Capture mode:");
    ImGui::SameLine();
    mouseCaptureModeRadio(
        "System cursor", Settings::Input::Mouse::CaptureMode::SystemCursor,
        "Binds the system mouse cursor to a single Virtua Gun controller.\n"
        "\n"
        "This mode gives the smoothest experience - you simply point the cursor at the display and click to shoot. "
        "The cursor is still free to interact with the user interface.\n"
        "\n"
        "To bind a controller to the mouse, click the screen. The first available controller will be bound. You cannot "
        "bind the mouse to more than one controller in this mode.\n"
        "\n"
        "This mode only works with Virtua Gun controllers. If a Shuttle Mouse is connected to any port, the mode "
        "selection is ignored and mouse capture behaves as if using the Physical mouse mode.");
    ImGui::SameLine();
    mouseCaptureModeRadio(
        "Physical mouse", Settings::Input::Mouse::CaptureMode::PhysicalMouse,
        "Binds a physical mouse to a Virtua Gun controller.\n"
        "\n"
        "This mode allows you to simultaneously bind multiple mice to many controllers.\n"
        "\n"
        "To bind controllers, first you must capture the mouse cursor by clicking the display with any mouse. While in "
        "this mode, click with the mice you wish to bind to controllers. The first available controller will be bound "
        "to each mouse.\n"
        "\n"
        "While any mouse is captured, the system cursor will be completely disabled. You can press ESC to release all "
        "mice and regain control of the system cursor.\n"
        "The system cursor is reenabled if Ymir loses focus or a window is opened by other means, such as opening the "
        "Settings window with the shortcut or triggering a debugger breakpoint.");

    MakeDirty(ImGui::Checkbox("Lock mouse cursor to window", &settings.mouse.lockToDisplay));
    widgets::ExplanationTooltip("When this option is enabled, if using system cursor capture mode, the mouse cursor "
                                "will be constrained to the window area.",
                                imguiData->displayScale);

    // TODO: preferred device capture order

    // -------------------------------------------------------------------------

    ImGui::PushFont(imguiData->fonts.sansSerif.bold, imguiData->fontSizes.large);
    ImGui::SeparatorText("Gamepads");
    ImGui::PopFont();

    if (m_context.gameControllerDBCount == 0) {
        ImGui::TextUnformatted("Game controller database not found or empty");
    } else {
        ImGui::Text("Game controller database: %d controllers", m_context.gameControllerDBCount);
    }
    ImGui::TextUnformatted("Database paths:");
    ImGui::Indent();
    ImGui::Text("Included with emulator: %s",
                fmt::format("{}", Profile::GetPortableProfilePath() / kGameControllerDBFile).c_str());
    ImGui::Text("Your profile: %s",
                fmt::format("{}", m_context.profile.GetPath(ProfilePath::Root) / kGameControllerDBFile).c_str());
    ImGui::Unindent();
    if (ImGui::Button("Open emulator folder##gamecontrollerdb")) {
        SDL_OpenURL(fmt::format("file:///{}", Profile::GetPortableProfilePath()).c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button("Open profile folder##gamecontrollerdb")) {
        SDL_OpenURL(fmt::format("file:///{}", m_context.profile.GetPath(ProfilePath::Root)).c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload database")) {
        m_context.EnqueueEvent(events::gui::ReloadGameControllerDatabase());
    }

    if (ImGui::BeginTable("input_settings", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("left", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("right", ImGuiTableColumnFlags_WidthStretch);

        ImGui::TableNextRow();
        if (ImGui::TableNextColumn()) {
            ImGui::TextUnformatted("Left stick deadzone");
            widgets::ExplanationTooltip("Adjusts the deadzone for the left stick.\n"
                                        "The active range is mapped linearly from 0 to 1.",
                                        imguiData->displayScale);
        }
        if (ImGui::TableNextColumn()) {
            float dz = settings.gamepad.lsDeadzone * 100.0f;
            ImGui::SetNextItemWidth(-1.0f);
            if (MakeDirty(
                    ImGui::SliderFloat("##ls_deadzones", &dz, 5.f, 90.f, "%.2f%%", ImGuiSliderFlags_AlwaysClamp))) {
                settings.gamepad.lsDeadzone = dz / 100.0f;
            }
        }

        ImGui::TableNextRow();
        if (ImGui::TableNextColumn()) {
            ImGui::TextUnformatted("Right stick deadzone");
            widgets::ExplanationTooltip("Adjusts the deadzone for the right stick.\n"
                                        "The active range is mapped linearly from 0 to 1.",
                                        imguiData->displayScale);
        }
        if (ImGui::TableNextColumn()) {
            float dz = settings.gamepad.rsDeadzone * 100.0f;
            ImGui::SetNextItemWidth(-1.0f);
            if (MakeDirty(
                    ImGui::SliderFloat("##rs_deadzones", &dz, 5.f, 90.f, "%.2f%%", ImGuiSliderFlags_AlwaysClamp))) {
                settings.gamepad.rsDeadzone = dz / 100.0f;
            }
        }

        ImGui::TableNextRow();
        if (ImGui::TableNextColumn()) {
            ImGui::TextUnformatted("Analog to digital sensitivity");
            widgets::ExplanationTooltip("Affects how far analog inputs must be pushed to trigger buttons.",
                                        imguiData->displayScale);
        }
        if (ImGui::TableNextColumn()) {
            float sens = settings.gamepad.analogToDigitalSensitivity * 100.0f;
            ImGui::SetNextItemWidth(-1.0f);
            if (MakeDirty(ImGui::SliderFloat("##analog_to_digital_sens", &sens, 5.f, 90.f, "%.2f%%",
                                             ImGuiSliderFlags_AlwaysClamp))) {
                settings.gamepad.analogToDigitalSensitivity = sens / 100.0f;
            }
        }

        ImGui::EndTable();
    }

    // -------------------------------------------------------------------------

    ImGui::PushFont(m_context.fonts.sansSerif.bold, m_context.fontSizes.large);
    ImGui::SeparatorText("On-screen controls");
    ImGui::PopFont();

    auto &onScreenControls = settings.onScreenControls;

    MakeDirty(ImGui::Checkbox("Enable on-screen controls", &onScreenControls.enabled));

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Target port");
    ImGui::SameLine();
    if (ImGui::RadioButton("Port 1##on_screen_port", onScreenControls.port == 0)) {
        onScreenControls.port = 0;
        MakeDirty();
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Port 2##on_screen_port", onScreenControls.port == 1)) {
        onScreenControls.port = 1;
        MakeDirty();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(selected port: %s)", peripheral::GetPeripheralName(settings.ports[onScreenControls.port].type).data());

    float opacity = onScreenControls.opacity * 100.0f;
    ImGui::SetNextItemWidth(280.0f * m_context.displayScale);
    if (MakeDirty(ImGui::SliderFloat("Opacity##on_screen_controls", &opacity, 10.0f, 100.0f, "%.0f%%",
                                     ImGuiSliderFlags_AlwaysClamp))) {
        onScreenControls.opacity = opacity / 100.0f;
    }

    float controlScale = onScreenControls.scale * 100.0f;
    ImGui::SetNextItemWidth(280.0f * m_context.displayScale);
    if (MakeDirty(ImGui::SliderFloat("Scale##on_screen_controls", &controlScale, 60.0f, 180.0f, "%.0f%%",
                                     ImGuiSliderFlags_AlwaysClamp))) {
        onScreenControls.scale = controlScale / 100.0f;
    }

    ImGui::TextWrapped(
        "Drag controls around the preview to place them. The analog stick always works as digital directions too, and "
        "its full analog range is used when the selected port is configured as a Saturn 3D Control Pad.");

    if (ImGui::Button("Reset layout##on_screen_controls")) {
        on_screen_controls::ResetLayout(onScreenControls);
        m_selectedOnScreenControl = -1;
        MakeDirty();
    }
    ImGui::SameLine();
    if (m_selectedOnScreenControl >= 0) {
        ImGui::Text("Selected: %s",
                    on_screen_controls::GetName(
                        static_cast<on_screen_controls::ControlID>(m_selectedOnScreenControl)));
    } else {
        ImGui::TextUnformatted("Selected: none");
    }

    const ImVec2 canvasSize{
        std::max(420.0f * m_context.displayScale, ImGui::GetContentRegionAvail().x),
        340.0f * m_context.displayScale,
    };
    const ImVec2 canvasPos = ImGui::GetCursorScreenPos();

    ImGui::InvisibleButton("##on_screen_controls_canvas", canvasSize);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        m_selectedOnScreenControl = -1;
    }

    auto *previewDrawList = ImGui::GetWindowDrawList();
    const ImVec2 canvasMin = canvasPos;
    const ImVec2 canvasMax{canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y};
    previewDrawList->AddRectFilled(canvasMin, canvasMax, IM_COL32(24, 26, 31, 220), 16.0f * m_context.displayScale);
    previewDrawList->AddRectFilled(
        ImVec2(canvasMin.x + 14.0f * m_context.displayScale, canvasMin.y + 14.0f * m_context.displayScale),
        ImVec2(canvasMax.x - 14.0f * m_context.displayScale, canvasMax.y - 14.0f * m_context.displayScale),
        IM_COL32(214, 214, 216, 42), 14.0f * m_context.displayScale);
    for (int i = 0; i < 10; ++i) {
        const float x = canvasMin.x + (canvasSize.x / 10.0f) * i;
        previewDrawList->AddLine(ImVec2(x, canvasMin.y), ImVec2(x, canvasMax.y), IM_COL32(255, 255, 255, 8),
                                 1.0f * m_context.displayScale);
    }
    for (int i = 0; i < 6; ++i) {
        const float y = canvasMin.y + (canvasSize.y / 6.0f) * i;
        previewDrawList->AddLine(ImVec2(canvasMin.x, y), ImVec2(canvasMax.x, y), IM_COL32(255, 255, 255, 8),
                                 1.0f * m_context.displayScale);
    }

    const float previewPadding = 18.0f * m_context.displayScale;
    const on_screen_controls::Viewport previewViewport{
        .pos = {canvasMin.x + previewPadding, canvasMin.y + previewPadding},
        .size = {canvasSize.x - previewPadding * 2.0f, canvasSize.y - previewPadding * 2.0f},
    };
    const auto geometry =
        on_screen_controls::BuildGeometry(onScreenControls, previewViewport, m_context.displayScale);

    previewDrawList->PushClipRect(canvasMin, canvasMax, true);
    for (auto id : on_screen_controls::kControlIDs) {
        const auto index = on_screen_controls::ToIndex(id);
        const auto &g = geometry[index];

        ImGui::PushID(static_cast<int>(index));
        if (g.shape == on_screen_controls::Geometry::Shape::Circle) {
            ImGui::SetCursorScreenPos(ImVec2(g.center.x - g.radius, g.center.y - g.radius));
            ImGui::InvisibleButton("##on_screen_control", ImVec2(g.radius * 2.0f, g.radius * 2.0f));
        } else {
            ImGui::SetCursorScreenPos(ImVec2(g.center.x - g.halfSize.x, g.center.y - g.halfSize.y));
            ImGui::InvisibleButton("##on_screen_control", ImVec2(g.halfSize.x * 2.0f, g.halfSize.y * 2.0f));
        }

        if (ImGui::IsItemActivated()) {
            m_selectedOnScreenControl = static_cast<int>(id);
        }
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            auto &control = on_screen_controls::GetControl(onScreenControls, id);
            std::array<float, 2> nextPosition{
                (ImGui::GetIO().MousePos.x - previewViewport.pos.x) / previewViewport.size.x,
                (ImGui::GetIO().MousePos.y - previewViewport.pos.y) / previewViewport.size.y,
            };
            nextPosition = on_screen_controls::ClampNormalizedPosition(onScreenControls, id, previewViewport,
                                                                       m_context.displayScale, nextPosition);
            if (control.position != nextPosition) {
                control.position = nextPosition;
                MakeDirty();
            }
        }
        ImGui::PopID();
    }

    on_screen_controls::VisualState previewVisuals{};
    if (m_selectedOnScreenControl >= 0) {
        previewVisuals.highlighted =
            static_cast<on_screen_controls::ControlID>(m_selectedOnScreenControl);
    }
    on_screen_controls::Draw(previewDrawList, m_context, onScreenControls, previewViewport, previewVisuals, 1.0f);
    previewDrawList->PopClipRect();

    auto *drawList = ImGui::GetWindowDrawList();

    auto &inputContext = m_context.inputContext;

    for (uint32 id : inputContext.GetConnectedGamepads()) {
        auto [lsx, lsy] = inputContext.GetAxis2D(id, input::GamepadAxis2D::LeftStick);
        auto [rsx, rsy] = inputContext.GetAxis2D(id, input::GamepadAxis2D::RightStick);
        auto lt = inputContext.GetAxis1D(id, input::GamepadAxis1D::LeftTrigger);
        auto rt = inputContext.GetAxis1D(id, input::GamepadAxis1D::RightTrigger);

        static constexpr float kWidgetSize = 100.0f;
        static constexpr ImU32 kBorderColor = 0xE0F5D4C6;
        static constexpr ImU32 kBackgroundColor = 0xAA401A0A;
        static constexpr ImU32 kDeadzoneBackgroundColor = 0xC02F2A69;
        static constexpr ImU32 kSensBackgroundColor = 0xC02A5669;
        static constexpr ImU32 kAxisActiveColor = 0xF05FF58F;
        static constexpr ImU32 kAxisAtRestColor = 0xF08F5FF5;

        const ImU32 textColor = ImGui::ColorConvertFloat4ToU32(ImGui::GetStyle().Colors[ImGuiCol_Text]);

        const float widgetSize = kWidgetSize * imguiData->displayScale;

        auto drawStick = [&](const char *name, float x, float y, float dz, float sens) {
            static constexpr float kArrowSize = 8.0f;
            static constexpr float kPadding = 4.0f;
            static constexpr float kCircleRadius = (kWidgetSize - kArrowSize - kPadding) * 0.5f;
            static constexpr ImU32 kOctantLineColor = 0x9AA89992;
            static constexpr ImU32 kOrthoLineColor = 0x755C504A;
            static constexpr ImU32 kStickLineColor = 0xE0BAD1DB;
            static constexpr ImU32 kAdjustedStickPointColor = 0xF0F58F5F;
            static constexpr ImU32 kArrowColor = kAxisActiveColor;

            const float arrowSize = kArrowSize * imguiData->displayScale;
            const float circleRadius = kCircleRadius * imguiData->displayScale;

            const float lineSpacing = ImGui::GetStyle().ItemSpacing.y;
            const float lineHeight = ImGui::GetTextLineHeightWithSpacing();

            const float circleBorderThickness = 1.5f * imguiData->displayScale;
            const float octantLineThickness = 1.0f * imguiData->displayScale;
            const float orthoLineThickness = 0.7f * imguiData->displayScale;
            const float stickLineThickness = 1.2f * imguiData->displayScale;
            const float stickPointRadius = 2.0f * imguiData->displayScale;

            const auto pos = ImGui::GetCursorScreenPos();
            const float left = pos.x;
            const float top = pos.y;
            const float right = pos.x + widgetSize;
            const float bottom = pos.y + widgetSize;
            const ImVec2 center{pos.x + widgetSize * 0.5f, pos.y + widgetSize * 0.5f};

            const auto [xAdj, yAdj] = input::ApplyDeadzone(x, y, dz);
            const bool zero = xAdj == 0.0f && yAdj == 0.0f;

            ImGui::Dummy(ImVec2(widgetSize, widgetSize + lineSpacing + lineHeight * 3));

            static constexpr ImVec2 kOctantDir{0.38268343f, 0.9238795f};
            static constexpr ImVec2 kDiagonalDir{0.70710678f, 0.70710678f};

            // Circle background, sensitivity zone, deadzone
            drawList->AddCircleFilled(center, circleRadius, kBackgroundColor);
            drawList->AddCircleFilled(center, (dz + (1.0f - dz) * sens) * circleRadius, kSensBackgroundColor);
            drawList->AddCircleFilled(center, dz * circleRadius, kDeadzoneBackgroundColor);

            // Octant dividers
            drawList->AddLine(ImVec2(center.x + kOctantDir.x * circleRadius, center.y + kOctantDir.y * circleRadius),
                              ImVec2(center.x - kOctantDir.x * circleRadius, center.y - kOctantDir.y * circleRadius),
                              kOctantLineColor, octantLineThickness);
            drawList->AddLine(ImVec2(center.x - kOctantDir.x * circleRadius, center.y + kOctantDir.y * circleRadius),
                              ImVec2(center.x + kOctantDir.x * circleRadius, center.y - kOctantDir.y * circleRadius),
                              kOctantLineColor, octantLineThickness);
            drawList->AddLine(ImVec2(center.x + kOctantDir.y * circleRadius, center.y + kOctantDir.x * circleRadius),
                              ImVec2(center.x - kOctantDir.y * circleRadius, center.y - kOctantDir.x * circleRadius),
                              kOctantLineColor, octantLineThickness);
            drawList->AddLine(ImVec2(center.x - kOctantDir.y * circleRadius, center.y + kOctantDir.x * circleRadius),
                              ImVec2(center.x + kOctantDir.y * circleRadius, center.y - kOctantDir.x * circleRadius),
                              kOctantLineColor, octantLineThickness);

            // Orthogonals and diagonals
            drawList->AddLine(ImVec2(center.x, top + arrowSize), ImVec2(center.x, bottom - arrowSize), kOrthoLineColor,
                              orthoLineThickness);
            drawList->AddLine(ImVec2(left + arrowSize, center.y), ImVec2(right - arrowSize, center.y), kOrthoLineColor,
                              orthoLineThickness);
            drawList->AddLine(
                ImVec2(center.x + kDiagonalDir.x * circleRadius, center.y + kDiagonalDir.y * circleRadius),
                ImVec2(center.x - kDiagonalDir.x * circleRadius, center.y - kDiagonalDir.y * circleRadius),
                kOrthoLineColor, orthoLineThickness);
            drawList->AddLine(
                ImVec2(center.x - kDiagonalDir.x * circleRadius, center.y + kDiagonalDir.y * circleRadius),
                ImVec2(center.x + kDiagonalDir.x * circleRadius, center.y - kDiagonalDir.y * circleRadius),
                kOrthoLineColor, orthoLineThickness);

            // Border
            drawList->AddCircle(center, circleRadius, kBorderColor, 0, circleBorderThickness);

            // Stick line
            const ImVec2 stickPos{center.x + x * circleRadius, center.y + y * circleRadius};
            drawList->AddLine(center, stickPos, kStickLineColor, stickLineThickness);
            drawList->AddCircleFilled(stickPos, stickPointRadius, zero ? kAxisAtRestColor : kAxisActiveColor);

            if (!zero) {
                const ImVec2 adjustedStickPos{center.x + xAdj * circleRadius, center.y + yAdj * circleRadius};
                drawList->AddCircle(adjustedStickPos, stickPointRadius, kAdjustedStickPointColor);
            }

            auto drawText = [&](const char *text, float lineNum, ImU32 color) {
                const float textWidth = ImGui::CalcTextSize(text).x;
                drawList->AddText(ImVec2(center.x - textWidth * 0.5f, bottom + lineSpacing + lineHeight * lineNum),
                                  color, text);
            };

            // Label and values
            drawText(name, 0, textColor);
            drawText(fmt::format("{:.2f}x{:.2f} ({:.2f})", x, y, sqrt(x * x + y * y)).c_str(), 1,
                     zero ? kAxisAtRestColor : kAxisActiveColor);
            drawText(fmt::format("{:.2f}x{:.2f} ({:.2f})", xAdj, yAdj, sqrt(xAdj * xAdj + yAdj * yAdj)).c_str(), 2,
                     kAdjustedStickPointColor);

            // D-Pad arrows
            const float distSq = xAdj * xAdj + yAdj * yAdj;
            if (distSq > 0.0f && distSq >= sens * sens) {
                static constexpr uint32 kPosX = 1u << 0u;
                static constexpr uint32 kNegX = 1u << 1u;
                static constexpr uint32 kPosY = 1u << 2u;
                static constexpr uint32 kNegY = 1u << 3u;
                const uint32 out = input::AnalogToDigital2DAxis(x, y, sens, kPosX, kNegX, kPosY, kNegY);

                float digX = (out & kPosX) ? +1.0f : (out & kNegX) ? -1.0f : 0.0f;
                float digY = (out & kPosY) ? +1.0f : (out & kNegY) ? -1.0f : 0.0f;
                const float dist = sqrt(digX * digX + digY * digY);
                digX /= dist;
                digY /= dist;

                const ImVec2 arrowBase{center.x + digX * circleRadius, center.y + digY * circleRadius};
                const ImVec2 arrowHead{center.x + digX * (circleRadius + arrowSize),
                                       center.y + digY * (circleRadius + arrowSize)};

                const ImVec2 triangle[3] = {
                    arrowHead,
                    {arrowBase.x - digY * arrowSize, arrowBase.y + digX * arrowSize},
                    {arrowBase.x + digY * arrowSize, arrowBase.y - digX * arrowSize},
                };

                drawList->AddConvexPolyFilled(triangle, std::size(triangle), kArrowColor);
            }
        };

        auto drawTrigger = [&](const char *name, float value, float sens) {
            // TODO: draw vertical bar
            static constexpr float kWidth = 50.0f;

            const float width = kWidth * imguiData->displayScale;
            const float height = widgetSize;

            const float lineSpacing = ImGui::GetStyle().ItemSpacing.y;
            const float lineHeight = ImGui::GetTextLineHeightWithSpacing();

            const auto pos = ImGui::GetCursorScreenPos();
            const float left = pos.x;
            const float top = pos.y;
            const float right = pos.x + width;
            const float bottom = pos.y + height;
            const ImVec2 center{pos.x + width * 0.5f, pos.y + height * 0.5f};

            const float borderThickness = 1.5f * imguiData->displayScale;

            const bool active = value >= sens;

            auto drawText = [&](const char *text, float lineNum, ImU32 color) {
                const float textWidth = ImGui::CalcTextSize(text).x;
                drawList->AddText(ImVec2(center.x - textWidth * 0.5f, bottom + lineSpacing + lineHeight * lineNum),
                                  color, text);
            };

            ImGui::Dummy(ImVec2(width, height + lineSpacing + lineHeight * 2));

            // Background, sensitivity area, current value, border
            drawList->AddRectFilled(ImVec2(left, top), ImVec2(right, bottom), kBackgroundColor);
            drawList->AddRectFilled(ImVec2(left, bottom - height * sens), ImVec2(right, bottom),
                                    kDeadzoneBackgroundColor);
            drawList->AddRectFilled(ImVec2(left, bottom - height * value), ImVec2(right, bottom),
                                    active ? kAxisActiveColor : kAxisAtRestColor);
            drawList->AddRect(ImVec2(left, top), ImVec2(right, bottom), kBorderColor, 0.0f, borderThickness);

            // Label and values
            drawText(name, 0, textColor);
            drawText(fmt::format("{:.2f}%", value * 100.0f).c_str(), 1, active ? kAxisActiveColor : kAxisAtRestColor);
        };

        ImGui::PushFont(imguiData->fonts.sansSerif.bold, imguiData->fontSizes.medium);
        ImGui::Text("Gamepad %u", id + 1);
        ImGui::PopFont();
        ImGui::PushID(id);
        drawStick("Left Stick", lsx, lsy, settings.gamepad.lsDeadzone, settings.gamepad.analogToDigitalSensitivity);
        ImGui::SameLine();
        drawStick("Right Stick", rsx, rsy, settings.gamepad.rsDeadzone, settings.gamepad.analogToDigitalSensitivity);
        ImGui::SameLine();
        drawTrigger("LT", lt, settings.gamepad.analogToDigitalSensitivity);
        ImGui::SameLine();
        drawTrigger("RT", rt, settings.gamepad.analogToDigitalSensitivity);
        ImGui::PopID();
    }

    ImGui::PopTextWrapPos();
}

} // namespace app::ui
