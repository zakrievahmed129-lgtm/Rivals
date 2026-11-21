#pragma once

#include "aim_assist.h"
#include "renderer.h"
#include "telemetry_types.h"

#include <windows.h>

#include <cstdint>
#include <functional>
#include <vector>

namespace nah2 {

struct PanelStatus {
    IpcStats ipc{};
    bool target_found = false;
    bool target_foreground = false;
    float px_per_count_x = 0.0f;
    float px_per_count_y = 0.0f;
    bool aim_engaging = false;
};

class ControlPanel {
public:
    ControlPanel() = default;
    ~ControlPanel();

    ControlPanel(const ControlPanel&) = delete;
    ControlPanel& operator=(const ControlPanel&) = delete;

    bool Initialize(
        HINSTANCE hInstance,
        OverlayConfig* overlayCfg,
        aim::AimConfig* aimCfg,
        std::function<void()> onConfigChanged,
        std::function<void(aim::AimProfile)> onProfileSelected,
        std::function<void()> onQuitRequested
    );

    void Shutdown();

    void SetVisible(bool visible);
    bool IsVisible() const { return visible_; }
    void ToggleVisible() { SetVisible(!visible_); }

    void UpdateStatus(const PanelStatus& status);
    void NotifyExternalChange();

    HWND GetHwnd() const { return hwnd_; }

private:
    enum class WidgetKind : uint8_t {
        ToggleEsp,
        Toggle2D,
        Toggle3D,
        ToggleAxes,
        ToggleSkeleton,
        ToggleHud,
        ToggleOverlay,
        ToggleAim,
        SelectProfile,
        SelectBone,
        SelectKey,
        ToggleTeam,
        ToggleVis,
        ToggleFovCircle,
        ToggleUnlocked,
        SliderStrength,
        SliderFov,
        SliderSmooth,
        SliderLead,
        SliderDeadzone,
    };

    struct HitZone {
        RECT rect{};
        WidgetKind kind{};
        int param = 0;
    };

    void RebuildLayout();
    void Paint(HDC targetDc);
    void EnsureBackbuffer(HDC refDc, int w, int h);
    void ReleaseBackbuffer();
    void RecreateFonts();

    void HandleMouseDown(int x, int y);
    void HandleMouseMove(int x, int y);
    void HandleMouseUp();
    void UpdateSliderFromX(int sliderIdx, int x);
    int Sx(float logicalPx) const;

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    HWND hwnd_ = nullptr;
    ATOM class_atom_ = 0;
    HINSTANCE hinstance_ = nullptr;

    OverlayConfig* overlay_cfg_ = nullptr;
    aim::AimConfig* aim_cfg_ = nullptr;
    std::function<void()> on_config_changed_;
    std::function<void(aim::AimProfile)> on_profile_selected_;
    std::function<void()> on_quit_requested_;

    PanelStatus status_{};
    bool visible_ = true;
    float dpi_scale_ = 1.0f;
    int client_w_ = 410;
    int client_h_ = 600;

    int active_slider_ = -1; // 0=Puissance, 1=FOV, 2=Smooth, 3=Lead, 4=Deadzone
    int hover_zone_ = -1;
    std::vector<HitZone> hit_zones_;

    HDC mem_dc_ = nullptr;
    HBITMAP mem_bmp_ = nullptr;
    HBITMAP old_bmp_ = nullptr;
    int mem_w_ = 0;
    int mem_h_ = 0;

    HFONT font_title_ = nullptr;
    HFONT font_section_ = nullptr;
    HFONT font_body_ = nullptr;
    HFONT font_small_ = nullptr;
};

} // namespace nah2
