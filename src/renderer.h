#pragma once

#include "aim_assist.h"
#include "telemetry_types.h"

#include <d2d1_1.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <windows.h>
#include <wrl/client.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace nah2 {

struct OverlayConfig {
    std::string target_filter = "Roblox";
    bool overlay_enabled = true;
    bool esp_enabled = true;
    bool show_2d_box = true;
    bool show_3d_box = true;
    bool show_axes = true;
    bool show_skeleton = true;
    bool show_hud = true;
    bool always_visible = false;
    bool vsync = false;
    bool antialias = false;
    int vp_offset_x = 0;
    int vp_offset_y = 0;
};

struct RenderDiagnostics {
    double fps = 0.0;
    uint32_t visible_entities = 0;
    bool target_found = false;
    bool target_active = false;
    bool target_foreground = false;
    bool matched_child_viewport = false;
    RECT overlay_rect{};
};

class OverlayRenderer {
public:
    explicit OverlayRenderer(const OverlayConfig& config = {});
    ~OverlayRenderer();

    OverlayRenderer(const OverlayRenderer&) = delete;
    OverlayRenderer& operator=(const OverlayRenderer&) = delete;

    bool Initialize(HINSTANCE hInstance);
    void Shutdown();

    // Synchronise position/taille et Z-order avec la fenetre cible (Roblox Studio).
    // Retourne true si l'overlay doit etre rendu sur cette frame.
    bool SyncWithTargetWindow(const CameraState& cam);

    // Dessine une frame complete (boites 2D, boites 3D OBB, vecteurs, squelettes, aim assist, HUD).
    void RenderFrame(
        const CameraState& cam,
        const std::vector<EntityTelemetry>& entities,
        const IpcStats& ipcStats,
        double nowSec,
        const aim::AimDebugState* aimDebug = nullptr
    );

    void ClearAndHide();

    using RawMouseCallback = std::function<void(int dx, int dy)>;
    void SetRawMouseCallback(RawMouseCallback cb) { raw_mouse_cb_ = std::move(cb); }

    HWND GetHwnd() const { return hwnd_; }
    float OverlayWidth() const { return static_cast<float>(width_); }
    float OverlayHeight() const { return static_cast<float>(height_); }
    OverlayConfig& Config() { return config_; }
    const OverlayConfig& Config() const { return config_; }
    const RenderDiagnostics& Diagnostics() const { return diag_; }

private:
    bool CreateOverlayWindow(HINSTANCE hInstance);
    bool InitDeviceResources(UINT width, UINT height);
    bool CreateBackbufferTarget();
    void ReleaseDeviceResources();
    bool ResizeSwapChain(UINT width, UINT height);

    void DrawEntity2DBox(const EntityTelemetry& e, const CameraState& cam, float w, float h, float dist);
    void DrawEntity3DBox(const EntityTelemetry& e, const CameraState& cam, float w, float h);
    void DrawEntityAxes(const EntityTelemetry& e, const CameraState& cam, float w, float h);
    void DrawEntitySkeleton(const EntityTelemetry& e, const CameraState& cam, float w, float h);
    void DrawAimOverlay(const aim::AimDebugState& aimDebug);
    void DrawHud(const CameraState& cam, const IpcStats& ipcStats, float w, double nowSec, const aim::AimDebugState* aimDebug);

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    OverlayConfig config_;
    RenderDiagnostics diag_{};
    RawMouseCallback raw_mouse_cb_;
    HWND hwnd_ = nullptr;
    ATOM class_atom_ = 0;
    HINSTANCE hinstance_ = nullptr;

    UINT width_ = 1280;
    UINT height_ = 720;
    RECT last_sync_rect_{};
    bool is_visible_ = false;
    double last_target_scan_sec_ = -10.0;
    HWND cached_target_hwnd_ = nullptr;
    HWND cached_child_hwnd_ = nullptr;
    float cached_child_vp_w_ = 0.0f;
    float cached_child_vp_h_ = 0.0f;
    double last_child_scan_sec_ = -10.0;

    // Compteur FPS
    double fps_window_start_ = 0.0;
    uint32_t fps_frames_ = 0;

    // Pipeline materiel Direct3D 11 + DXGI + DirectComposition + Direct2D 1.1 + DirectWrite
    Microsoft::WRL::ComPtr<ID3D11Device> d3d_device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3d_context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swap_chain_;
    Microsoft::WRL::ComPtr<IDCompositionDevice> dcomp_device_;
    Microsoft::WRL::ComPtr<IDCompositionTarget> dcomp_target_;
    Microsoft::WRL::ComPtr<IDCompositionVisual> dcomp_visual_;

    Microsoft::WRL::ComPtr<ID2D1Factory1> d2d_factory_;
    Microsoft::WRL::ComPtr<ID2D1Device> d2d_device_;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> d2d_ctx_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> target_bitmap_;

    Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_factory_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> text_format_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> hud_format_;

    // Pinceaux caches (zero allocation par frame)
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_rig_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_ally_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_prop_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_box_fill_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_skeleton_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_joint_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_axis_x_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_axis_y_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_axis_z_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_aim_fov_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_aim_lock_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_hud_bg_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_hud_border_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_text_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_text_shadow_;
};

} // namespace nah2
