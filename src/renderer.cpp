#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN

#include "renderer.h"
#include "ipc_receiver.h"
#include "math_utils.h"

#include <dwmapi.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string_view>

namespace nah2 {

namespace {

// Topologie squelette R15 (paires d'indices de slots 0..14)
inline constexpr std::array<std::pair<uint8_t, uint8_t>, 14> kR15Bones = {{
    { 0, 1 },  // Head -> UpperTorso
    { 1, 2 },  // UpperTorso -> LowerTorso
    { 1, 3 },  // UpperTorso -> LeftUpperArm
    { 3, 4 },  // LeftUpperArm -> LeftLowerArm
    { 4, 5 },  // LeftLowerArm -> LeftHand
    { 1, 6 },  // UpperTorso -> RightUpperArm
    { 6, 7 },  // RightUpperArm -> RightLowerArm
    { 7, 8 },  // RightLowerArm -> RightHand
    { 2, 9 },  // LowerTorso -> LeftUpperLeg
    { 9, 10 }, // LeftUpperLeg -> LeftLowerLeg
    { 10, 11 },// LeftLowerLeg -> LeftFoot
    { 2, 12 }, // LowerTorso -> RightUpperLeg
    { 12, 13 },// RightUpperLeg -> RightLowerLeg
    { 13, 14 } // RightLowerLeg -> RightFoot
}};

// Topologie squelette R6 (paires d'indices de slots 0..5)
inline constexpr std::array<std::pair<uint8_t, uint8_t>, 5> kR6Bones = {{
    { 0, 1 }, // Head -> Torso
    { 1, 2 }, // Torso -> Left Arm
    { 1, 3 }, // Torso -> Right Arm
    { 1, 4 }, // Torso -> Left Leg
    { 1, 5 }  // Torso -> Right Leg
}};

int Utf8ToWideBuf(std::string_view utf8, wchar_t* outBuf, int cap) {
    if (utf8.empty() || cap <= 0) return 0;
    const int n = MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), outBuf, cap - 1);
    if (n <= 0) return 0;
    outBuf[n] = L'\0';
    return n;
}

std::string WideToUtf8(std::wstring_view wide) {
    if (wide.empty()) return {};
    const int needed = WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string utf8(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), needed, nullptr, nullptr);
    return utf8;
}

std::string ToLowerAscii(std::string_view s) {
    std::string out(s);
    for (char& ch : out) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return out;
}

std::string GetWindowProcessExeLower(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) return {};
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return {};

    wchar_t path[MAX_PATH]{};
    DWORD size = MAX_PATH;
    std::string exeName;
    if (QueryFullProcessImageNameW(proc, 0, path, &size)) {
        std::wstring_view sv(path, size);
        const std::size_t slash = sv.find_last_of(L"\\/");
        if (slash != std::wstring_view::npos) {
            sv.remove_prefix(slash + 1);
        }
        exeName = ToLowerAscii(WideToUtf8(sv));
    }
    CloseHandle(proc);
    return exeName;
}

struct TargetSearchCtx {
    DWORD self_pid = 0;
    HWND fg_hwnd = nullptr;
    DWORD fg_pid = 0;
    std::string filter_lower;
    HWND best_hwnd = nullptr;
    int best_score = -1;
};

BOOL CALLBACK EnumTopWindowsProc(HWND hwnd, LPARAM lParam) {
    auto* ctx = reinterpret_cast<TargetSearchCtx*>(lParam);
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) {
        return TRUE;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == ctx->self_pid) {
        return TRUE;
    }

    const int titleLen = GetWindowTextLengthW(hwnd);
    if (titleLen <= 0) {
        return TRUE;
    }
    std::wstring titleW(static_cast<std::size_t>(titleLen), L'\0');
    GetWindowTextW(hwnd, titleW.data(), titleLen + 1);
    const std::string titleLower = ToLowerAscii(WideToUtf8(titleW));
    const std::string exeLower = GetWindowProcessExeLower(hwnd);

    int score = -1;
    if (!ctx->filter_lower.empty()) {
        const bool titleMatch = titleLower.find(ctx->filter_lower) != std::string::npos;
        const bool exeMatch = exeLower.find(ctx->filter_lower) != std::string::npos;
        if (!titleMatch && !exeMatch) {
            return TRUE;
        }
        score = 10;
    }

    if (exeLower == "robloxstudiobeta.exe") {
        score = 100;
    } else if (exeLower == "robloxplayerbeta.exe") {
        score = 90;
    } else if (titleLower == "roblox") {
        score = 85;
    } else if (titleLower.find("roblox studio") != std::string::npos) {
        score = 80;
    }

    if (score >= 0 && (hwnd == ctx->fg_hwnd || (ctx->fg_pid != 0 && pid == ctx->fg_pid))) {
        score += 50;
    }

    RECT rc{};
    GetClientRect(hwnd, &rc);
    if ((rc.right - rc.left) < 200 || (rc.bottom - rc.top) < 150) {
        return TRUE;
    }

    if (score > ctx->best_score) {
        ctx->best_score = score;
        ctx->best_hwnd = hwnd;
    }
    return TRUE;
}

HWND FindTargetWindow(const std::string& filter) {
    TargetSearchCtx ctx;
    ctx.self_pid = GetCurrentProcessId();
    ctx.fg_hwnd = GetForegroundWindow();
    if (ctx.fg_hwnd) {
        GetWindowThreadProcessId(ctx.fg_hwnd, &ctx.fg_pid);
    }
    ctx.filter_lower = ToLowerAscii(filter);
    EnumWindows(EnumTopWindowsProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.best_hwnd;
}

bool GetClientScreenRect(HWND hwnd, RECT& outRect) {
    RECT rc{};
    if (!GetClientRect(hwnd, &rc)) return false;
    POINT pts[2] = { { rc.left, rc.top }, { rc.right, rc.bottom } };
    MapWindowPoints(hwnd, nullptr, pts, 2);
    outRect = { pts[0].x, pts[0].y, pts[1].x, pts[1].y };
    return (outRect.right - outRect.left) > 0 && (outRect.bottom - outRect.top) > 0;
}

struct ChildViewportCtx {
    float vp_w = 0.0f;
    float vp_h = 0.0f;
    float dpi_scale = 1.0f;
    HWND matched_hwnd = nullptr;
    RECT matched_rect{};
    float best_err = 1e9f;
};

BOOL CALLBACK EnumChildViewportProc(HWND child, LPARAM lParam) {
    auto* ctx = reinterpret_cast<ChildViewportCtx*>(lParam);
    if (!IsWindowVisible(child)) {
        return TRUE;
    }
    RECT rc{};
    if (!GetClientScreenRect(child, rc)) {
        return TRUE;
    }
    const float w = static_cast<float>(rc.right - rc.left);
    const float h = static_cast<float>(rc.bottom - rc.top);

    const float err1x = std::abs(w - ctx->vp_w) + std::abs(h - ctx->vp_h);
    const float errDpi = std::abs(w - ctx->vp_w * ctx->dpi_scale) +
                         std::abs(h - ctx->vp_h * ctx->dpi_scale);
    const float err = std::min(err1x, errDpi);

    // Tolerance 8px pour bordures/docks Qt dans Roblox Studio
    if (err <= 8.0f && err < ctx->best_err) {
        ctx->best_err = err;
        ctx->matched_hwnd = child;
        ctx->matched_rect = rc;
    }
    return TRUE;
}

bool ResolveTargetViewportRect(
    HWND topHwnd,
    const CameraState& cam,
    int offX,
    int offY,
    double nowSec,
    HWND& cachedChild,
    float& cachedVpW,
    float& cachedVpH,
    double& lastChildScanSec,
    RECT& outRect,
    bool& outMatchedChild
) {
    outMatchedChild = false;
    if (!GetClientScreenRect(topHwnd, outRect)) {
        return false;
    }

    if (cam.viewport_w > 32.0f && cam.viewport_h > 32.0f) {
        const bool vpChanged = (std::abs(cam.viewport_w - cachedVpW) > 1.0f) ||
                               (std::abs(cam.viewport_h - cachedVpH) > 1.0f);
        const bool needRescan = vpChanged ||
                                (cachedChild != nullptr && (!IsWindow(cachedChild) || !IsChild(topHwnd, cachedChild))) ||
                                ((nowSec - lastChildScanSec) > 0.75 && cachedChild == nullptr);

        if (needRescan) {
            ChildViewportCtx cctx;
            cctx.vp_w = cam.viewport_w;
            cctx.vp_h = cam.viewport_h;
            const UINT dpi = GetDpiForWindow(topHwnd);
            cctx.dpi_scale = (dpi > 0) ? (static_cast<float>(dpi) / 96.0f) : 1.0f;
            EnumChildWindows(topHwnd, EnumChildViewportProc, reinterpret_cast<LPARAM>(&cctx));
            cachedChild = cctx.matched_hwnd;
            cachedVpW = cam.viewport_w;
            cachedVpH = cam.viewport_h;
            lastChildScanSec = nowSec;
            if (cachedChild != nullptr) {
                outRect = cctx.matched_rect;
                outMatchedChild = true;
            }
        } else if (cachedChild != nullptr && IsWindowVisible(cachedChild)) {
            RECT childRc{};
            if (GetClientScreenRect(cachedChild, childRc)) {
                outRect = childRc;
                outMatchedChild = true;
            } else {
                cachedChild = nullptr;
            }
        }
    }

    outRect.left += offX;
    outRect.top += offY;
    outRect.right += offX;
    outRect.bottom += offY;
    return (outRect.right - outRect.left) >= 64 && (outRect.bottom - outRect.top) >= 64;
}

inline D2D1_POINT_2F ToD2D(const Vec2& p) {
    return D2D1::Point2F(p.x, p.y);
}

} // namespace

OverlayRenderer::OverlayRenderer(const OverlayConfig& config) : config_(config) {}

OverlayRenderer::~OverlayRenderer() {
    Shutdown();
}

LRESULT CALLBACK OverlayRenderer::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return TRUE;
    }

    auto* self = reinterpret_cast<OverlayRenderer*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_INPUT: {
            if (self && self->raw_mouse_cb_) {
                UINT size = 0;
                GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
                if (size > 0 && size <= 128) {
                    alignas(8) BYTE buf[128];
                    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER)) == size) {
                        const RAWINPUT* raw = reinterpret_cast<const RAWINPUT*>(buf);
                        if (raw->header.dwType == RIM_TYPEMOUSE && raw->header.hDevice != nullptr) {
                            if ((raw->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0 &&
                                (raw->data.mouse.lLastX != 0 || raw->data.mouse.lLastY != 0)) {
                                self->raw_mouse_cb_(raw->data.mouse.lLastX, raw->data.mouse.lLastY);
                            }
                        }
                    }
                }
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

bool OverlayRenderer::Initialize(HINSTANCE hInstance) {
    hinstance_ = hInstance;
    if (!CreateOverlayWindow(hInstance)) {
        return false;
    }
    if (!InitDeviceResources(width_, height_)) {
        return false;
    }
    return true;
}

void OverlayRenderer::Shutdown() {
    ReleaseDeviceResources();
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (class_atom_ && hinstance_) {
        UnregisterClassW(L"Nah2OverlayWindowClass", hinstance_);
        class_atom_ = 0;
    }
}

bool OverlayRenderer::CreateOverlayWindow(HINSTANCE hInstance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = OverlayRenderer::WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"Nah2OverlayWindowClass";
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));

    class_atom_ = RegisterClassExW(&wc);
    if (!class_atom_) {
        return false;
    }

    // Fenetre sans bordure, toujours au premier plan, non-activable et transparente aux entrees
    const DWORD exStyle = WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED |
                          WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    hwnd_ = CreateWindowExW(
        exStyle,
        L"Nah2OverlayWindowClass",
        L"nah2 Spatial Diagnostics",
        WS_POPUP,
        100, 100, static_cast<int>(width_), static_cast<int>(height_),
        nullptr, nullptr, hInstance, this);

    if (!hwnd_) {
        return false;
    }

    // Active l'attribut layered + etend la transparence DWM sur toute la zone cliente
    SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);
    MARGINS margins = { -1, -1, -1, -1 };
    DwmExtendFrameIntoClientArea(hwnd_, &margins);

    return true;
}

bool OverlayRenderer::InitDeviceResources(UINT width, UINT height) {
    width_ = std::max<UINT>(width, 64);
    height_ = std::max<UINT>(height, 64);

    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };
    D3D_FEATURE_LEVEL achieved{};
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        featureLevels, ARRAYSIZE(featureLevels), D3D11_SDK_VERSION,
        &d3d_device_, &achieved, &d3d_context_);

    if (FAILED(hr)) {
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
            featureLevels, ARRAYSIZE(featureLevels), D3D11_SDK_VERSION,
            &d3d_device_, &achieved, &d3d_context_);
    }
    if (FAILED(hr)) {
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(d3d_device_.As(&dxgiDevice))) return false;

    Microsoft::WRL::ComPtr<IDXGIDevice1> dxgiDevice1;
    if (SUCCEEDED(dxgiDevice.As(&dxgiDevice1))) {
        dxgiDevice1->SetMaximumFrameLatency(1);
    }

    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDevice->GetAdapter(&adapter))) return false;

    Microsoft::WRL::ComPtr<IDXGIFactory2> dxgiFactory;
    if (FAILED(adapter->GetParent(IID_PPV_ARGS(&dxgiFactory)))) return false;

    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width = width_;
    scd.Height = height_;
    scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    scd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;

    if (FAILED(dxgiFactory->CreateSwapChainForComposition(
            d3d_device_.Get(), &scd, nullptr, &swap_chain_))) {
        return false;
    }

    if (FAILED(DCompositionCreateDevice(dxgiDevice.Get(), IID_PPV_ARGS(&dcomp_device_)))) {
        return false;
    }
    if (FAILED(dcomp_device_->CreateTargetForHwnd(hwnd_, TRUE, &dcomp_target_))) {
        return false;
    }
    if (FAILED(dcomp_device_->CreateVisual(&dcomp_visual_))) {
        return false;
    }
    if (FAILED(dcomp_visual_->SetContent(swap_chain_.Get())) ||
        FAILED(dcomp_target_->SetRoot(dcomp_visual_.Get())) ||
        FAILED(dcomp_device_->Commit())) {
        return false;
    }

    D2D1_FACTORY_OPTIONS d2dOpts{};
    if (FAILED(D2D1CreateFactory(
            D2D1_FACTORY_TYPE_SINGLE_THREADED,
            __uuidof(ID2D1Factory1),
            &d2dOpts,
            reinterpret_cast<void**>(d2d_factory_.GetAddressOf())))) {
        return false;
    }
    if (FAILED(d2d_factory_->CreateDevice(dxgiDevice.Get(), &d2d_device_))) {
        return false;
    }
    if (FAILED(d2d_device_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2d_ctx_))) {
        return false;
    }
    d2d_ctx_->SetDpi(96.0f, 96.0f);
    d2d_ctx_->SetAntialiasMode(config_.antialias ? D2D1_ANTIALIAS_MODE_PER_PRIMITIVE : D2D1_ANTIALIAS_MODE_ALIASED);
    d2d_ctx_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

    if (FAILED(DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(dwrite_factory_.GetAddressOf())))) {
        return false;
    }

    if (FAILED(dwrite_factory_->CreateTextFormat(
            L"Consolas", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            12.0f, L"en-US", &text_format_))) {
        return false;
    }

    if (FAILED(dwrite_factory_->CreateTextFormat(
            L"Consolas", nullptr, DWRITE_FONT_WEIGHT_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            12.5f, L"en-US", &hud_format_))) {
        return false;
    }

    // Palette diagnostique haut contraste
    d2d_ctx_->CreateSolidColorBrush(D2D1::ColorF(0.10f, 0.88f, 0.96f, 0.92f), &brush_rig_);
    d2d_ctx_->CreateSolidColorBrush(D2D1::ColorF(0.24f, 0.90f, 0.56f, 0.72f), &brush_ally_);
    d2d_ctx_->CreateSolidColorBrush(D2D1::ColorF(1.00f, 0.72f, 0.18f, 0.90f), &brush_prop_);
    d2d_ctx_->CreateSolidColorBrush(D2D1::ColorF(0.10f, 0.88f, 0.96f, 0.06f), &brush_box_fill_);
    d2d_ctx_->CreateSolidColorBrush(D2D1::ColorF(0.95f, 0.97f, 1.00f, 0.88f), &brush_skeleton_);
    d2d_ctx_->CreateSolidColorBrush(D2D1::ColorF(1.00f, 0.86f, 0.25f, 0.95f), &brush_joint_);
    d2d_ctx_->CreateSolidColorBrush(D2D1::ColorF(0.98f, 0.28f, 0.30f, 0.95f), &brush_axis_x_);
    d2d_ctx_->CreateSolidColorBrush(D2D1::ColorF(0.24f, 0.92f, 0.45f, 0.95f), &brush_axis_y_);
    d2d_ctx_->CreateSolidColorBrush(D2D1::ColorF(0.22f, 0.68f, 1.00f, 0.98f), &brush_axis_z_);
    d2d_ctx_->CreateSolidColorBrush(D2D1::ColorF(0.80f, 0.94f, 1.00f, 0.28f), &brush_aim_fov_);
    d2d_ctx_->CreateSolidColorBrush(D2D1::ColorF(0.32f, 0.98f, 0.58f, 0.92f), &brush_aim_lock_);
    d2d_ctx_->CreateSolidColorBrush(D2D1::ColorF(0.05f, 0.07f, 0.10f, 0.72f), &brush_hud_bg_);
    d2d_ctx_->CreateSolidColorBrush(D2D1::ColorF(0.10f, 0.88f, 0.96f, 0.55f), &brush_hud_border_);
    d2d_ctx_->CreateSolidColorBrush(D2D1::ColorF(0.96f, 0.98f, 1.00f, 0.98f), &brush_text_);
    d2d_ctx_->CreateSolidColorBrush(D2D1::ColorF(0.00f, 0.00f, 0.00f, 0.65f), &brush_text_shadow_);

    return CreateBackbufferTarget();
}

bool OverlayRenderer::CreateBackbufferTarget() {
    if (!swap_chain_ || !d2d_ctx_) return false;
    Microsoft::WRL::ComPtr<IDXGISurface> surface;
    if (FAILED(swap_chain_->GetBuffer(0, IID_PPV_ARGS(&surface)))) {
        return false;
    }
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);

    return SUCCEEDED(d2d_ctx_->CreateBitmapFromDxgiSurface(surface.Get(), &bp, &target_bitmap_));
}

void OverlayRenderer::ReleaseDeviceResources() {
    if (d2d_ctx_) {
        d2d_ctx_->SetTarget(nullptr);
    }
    target_bitmap_.Reset();
    brush_rig_.Reset();
    brush_ally_.Reset();
    brush_prop_.Reset();
    brush_box_fill_.Reset();
    brush_skeleton_.Reset();
    brush_joint_.Reset();
    brush_axis_x_.Reset();
    brush_axis_y_.Reset();
    brush_axis_z_.Reset();
    brush_aim_fov_.Reset();
    brush_aim_lock_.Reset();
    brush_hud_bg_.Reset();
    brush_hud_border_.Reset();
    brush_text_.Reset();
    brush_text_shadow_.Reset();
    text_format_.Reset();
    hud_format_.Reset();
    dwrite_factory_.Reset();
    d2d_ctx_.Reset();
    d2d_device_.Reset();
    d2d_factory_.Reset();
    dcomp_visual_.Reset();
    dcomp_target_.Reset();
    dcomp_device_.Reset();
    swap_chain_.Reset();
    d3d_context_.Reset();
    d3d_device_.Reset();
}

bool OverlayRenderer::ResizeSwapChain(UINT width, UINT height) {
    width = std::max<UINT>(width, 64);
    height = std::max<UINT>(height, 64);
    if (width == width_ && height == height_ && target_bitmap_) {
        return true;
    }
    if (!swap_chain_ || !d2d_ctx_) {
        return false;
    }

    d2d_ctx_->SetTarget(nullptr);
    target_bitmap_.Reset();

    const HRESULT hr = swap_chain_->ResizeBuffers(2, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
    if (FAILED(hr)) {
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            ReleaseDeviceResources();
            return InitDeviceResources(width, height);
        }
        return false;
    }

    width_ = width;
    height_ = height;
    return CreateBackbufferTarget();
}

void OverlayRenderer::ClearAndHide() {
    if (is_visible_ && hwnd_) {
        ShowWindow(hwnd_, SW_HIDE);
        is_visible_ = false;
    }
}

bool OverlayRenderer::SyncWithTargetWindow(const CameraState& cam) {
    const double nowSec = static_cast<double>(IpcReceiver::NowMicroseconds()) / 1000000.0;
    const HWND fgHwnd = GetForegroundWindow();
    DWORD fgPid = 0;
    if (fgHwnd) {
        GetWindowThreadProcessId(fgHwnd, &fgPid);
    }

    const bool cachedValid = cached_target_hwnd_ && IsWindow(cached_target_hwnd_) &&
                             IsWindowVisible(cached_target_hwnd_) && !IsIconic(cached_target_hwnd_);
    DWORD targetPid = 0;
    if (cachedValid) {
        GetWindowThreadProcessId(cached_target_hwnd_, &targetPid);
    }

    const bool fgIsTarget = cachedValid && (fgPid == targetPid) && (targetPid != 0);
    const bool fgIsSelf = (fgPid == GetCurrentProcessId());
    if (!cachedValid || (!fgIsTarget && !fgIsSelf && (nowSec - last_target_scan_sec_) > 0.75)) {
        const HWND prevTarget = cached_target_hwnd_;
        cached_target_hwnd_ = FindTargetWindow(config_.target_filter);
        last_target_scan_sec_ = nowSec;
        if (cached_target_hwnd_ != prevTarget) {
            cached_child_hwnd_ = nullptr;
            cached_child_vp_w_ = 0.0f;
            cached_child_vp_h_ = 0.0f;
            last_child_scan_sec_ = -10.0;
        }
        if (cached_target_hwnd_) {
            GetWindowThreadProcessId(cached_target_hwnd_, &targetPid);
        }
    }

    RECT desiredRect{};
    bool matchedChild = false;
    bool shouldShow = false;

    if (cached_target_hwnd_ && IsWindowVisible(cached_target_hwnd_) && !IsIconic(cached_target_hwnd_)) {
        diag_.target_found = true;
        diag_.target_foreground = (fgPid != 0 && fgPid == targetPid);
        const bool isActive = config_.always_visible || diag_.target_foreground || fgIsSelf;
        diag_.target_active = isActive;

        if (isActive && ResolveTargetViewportRect(
                cached_target_hwnd_, cam, config_.vp_offset_x, config_.vp_offset_y,
                nowSec, cached_child_hwnd_, cached_child_vp_w_, cached_child_vp_h_, last_child_scan_sec_,
                desiredRect, matchedChild)) {
            shouldShow = true;
        }
    } else {
        diag_.target_found = false;
        diag_.target_active = false;
        diag_.target_foreground = false;
        if (config_.always_visible) {
            desiredRect = { 100, 100, 1124, 676 };
            shouldShow = true;
        }
    }

    diag_.matched_child_viewport = matchedChild;

    if (shouldShow) {
        const UINT targetW = static_cast<UINT>(std::max<LONG>(64, desiredRect.right - desiredRect.left));
        const UINT targetH = static_cast<UINT>(std::max<LONG>(64, desiredRect.bottom - desiredRect.top));
        diag_.overlay_rect = desiredRect;

        if (!config_.overlay_enabled) {
            width_ = targetW;
            height_ = targetH;
            ClearAndHide();
            return false;
        }

        const bool rectChanged = !is_visible_ ||
            desiredRect.left != last_sync_rect_.left ||
            desiredRect.top != last_sync_rect_.top ||
            desiredRect.right != last_sync_rect_.right ||
            desiredRect.bottom != last_sync_rect_.bottom;

        if (rectChanged) {
            SetWindowPos(
                hwnd_, HWND_TOPMOST,
                desiredRect.left, desiredRect.top,
                static_cast<int>(targetW), static_cast<int>(targetH),
                SWP_NOACTIVATE | SWP_SHOWWINDOW);
            ResizeSwapChain(targetW, targetH);
            last_sync_rect_ = desiredRect;
            is_visible_ = true;
        }
        return true;
    }

    ClearAndHide();
    return false;
}

void OverlayRenderer::DrawEntity2DBox(
    const EntityTelemetry& e,
    const CameraState& cam,
    float w,
    float h,
    float dist
) {
    float minX = 0, minY = 0, maxX = 0, maxY = 0;
    if (!math::ComputeProjectedBounds2D(e, cam, w, h, minX, minY, maxX, maxY)) {
        return;
    }
    if (maxX < 0.0f || minX > w || maxY < 0.0f || minY > h) {
        return;
    }

    const float boxW = maxX - minX;
    const float boxH = maxY - minY;
    if (boxW < 2.0f || boxH < 2.0f || boxW > w * 3.0f || boxH > h * 3.0f) {
        return;
    }

    ID2D1SolidColorBrush* strokeBrush = (e.kind == 1)
        ? (e.is_ally ? brush_ally_.Get() : brush_rig_.Get())
        : brush_prop_.Get();

    if (e.kind == 1 && !e.is_ally) {
        d2d_ctx_->FillRectangle(D2D1::RectF(minX, minY, maxX, maxY), brush_box_fill_.Get());
    }
    d2d_ctx_->DrawRectangle(D2D1::RectF(minX, minY, maxX, maxY), strokeBrush, 1.25f);

    // Coins tactiques renforces
    const float tick = std::min(12.0f, std::min(boxW, boxH) * 0.25f);
    if (tick >= 3.0f) {
        const float thick = 2.2f;
        d2d_ctx_->DrawLine({ minX, minY }, { minX + tick, minY }, strokeBrush, thick);
        d2d_ctx_->DrawLine({ minX, minY }, { minX, minY + tick }, strokeBrush, thick);
        d2d_ctx_->DrawLine({ maxX, minY }, { maxX - tick, minY }, strokeBrush, thick);
        d2d_ctx_->DrawLine({ maxX, minY }, { maxX, minY + tick }, strokeBrush, thick);
        d2d_ctx_->DrawLine({ minX, maxY }, { minX + tick, maxY }, strokeBrush, thick);
        d2d_ctx_->DrawLine({ minX, maxY }, { minX, maxY - tick }, strokeBrush, thick);
        d2d_ctx_->DrawLine({ maxX, maxY }, { maxX - tick, maxY }, strokeBrush, thick);
        d2d_ctx_->DrawLine({ maxX, maxY }, { maxX, maxY - tick }, strokeBrush, thick);
    }

    // Etiquette nom + distance (studs)
    char labelUtf8[128];
    const char* nameStr = e.name.empty() ? "Entity" : e.name.c_str();
    const int labelLen = std::snprintf(labelUtf8, sizeof(labelUtf8), "%s [%.1f st]", nameStr, dist);
    if (labelLen > 0) {
        wchar_t labelW[128];
        const int wLen = Utf8ToWideBuf(
            std::string_view(labelUtf8, static_cast<std::size_t>(std::min(labelLen, static_cast<int>(sizeof(labelUtf8) - 1)))),
            labelW,
            static_cast<int>(std::size(labelW)));
        if (wLen > 0) {
            const float textY = std::max(4.0f, minY - 17.0f);
            const float badgeW = std::clamp(static_cast<float>(wLen) * 7.2f + 6.0f, 24.0f, 260.0f);
            D2D1_RECT_F bgRc = D2D1::RectF(minX - 2.0f, textY - 1.0f, minX + badgeW, textY + 15.0f);
            D2D1_RECT_F textRc = D2D1::RectF(minX, textY, minX + 260.0f, textY + 18.0f);
            d2d_ctx_->FillRectangle(bgRc, brush_text_shadow_.Get());
            d2d_ctx_->DrawText(labelW, static_cast<UINT32>(wLen), text_format_.Get(), textRc, brush_text_.Get());
        }
    }
}

void OverlayRenderer::DrawEntity3DBox(const EntityTelemetry& e, const CameraState& cam, float w, float h) {
    const auto corners = math::ComputeObbCorners(e);
    ID2D1SolidColorBrush* strokeBrush = (e.kind == 1)
        ? (e.is_ally ? brush_ally_.Get() : brush_rig_.Get())
        : brush_prop_.Get();

    for (const auto& [i0, i1] : math::kBoxEdges) {
        Vec2 p0, p1;
        if (math::ProjectSegment3D(corners[i0], corners[i1], cam, w, h, p0, p1)) {
            d2d_ctx_->DrawLine(ToD2D(p0), ToD2D(p1), strokeBrush, 1.35f);
        }
    }
}

void OverlayRenderer::DrawEntityAxes(const EntityTelemetry& e, const CameraState& cam, float w, float h) {
    const float axisLen = std::clamp(std::max({ e.size.x, e.size.y, e.size.z }) * 0.65f, 1.8f, 6.0f);
    const float lookLen = axisLen * 1.45f;

    const Vec3 origin = e.pos;
    const Vec3 tipX = math::Add(origin, math::Scale(e.right, axisLen));
    const Vec3 tipY = math::Add(origin, math::Scale(e.up, axisLen));
    const Vec3 tipZ = math::Add(origin, math::Scale(e.look, lookLen));

    Vec2 sOrigin, sTip;
    if (math::ProjectSegment3D(origin, tipX, cam, w, h, sOrigin, sTip)) {
        d2d_ctx_->DrawLine(ToD2D(sOrigin), ToD2D(sTip), brush_axis_x_.Get(), 1.8f);
    }
    if (math::ProjectSegment3D(origin, tipY, cam, w, h, sOrigin, sTip)) {
        d2d_ctx_->DrawLine(ToD2D(sOrigin), ToD2D(sTip), brush_axis_y_.Get(), 1.8f);
    }
    if (math::ProjectSegment3D(origin, tipZ, cam, w, h, sOrigin, sTip)) {
        d2d_ctx_->DrawLine(ToD2D(sOrigin), ToD2D(sTip), brush_axis_z_.Get(), 2.3f);
        d2d_ctx_->FillRectangle(D2D1::RectF(sTip.x - 2.5f, sTip.y - 2.5f, sTip.x + 2.5f, sTip.y + 2.5f), brush_axis_z_.Get());
    }
}

void OverlayRenderer::DrawEntitySkeleton(const EntityTelemetry& e, const CameraState& cam, float w, float h) {
    if (e.joint_mask == 0) {
        return;
    }

    auto drawBones = [&](const auto& bonePairs) {
        for (const auto& [a, b] : bonePairs) {
            const uint16_t maskA = static_cast<uint16_t>(1u << a);
            const uint16_t maskB = static_cast<uint16_t>(1u << b);
            if ((e.joint_mask & maskA) && (e.joint_mask & maskB)) {
                Vec2 pA, pB;
                if (math::ProjectSegment3D(e.joints[a], e.joints[b], cam, w, h, pA, pB)) {
                    d2d_ctx_->DrawLine(ToD2D(pA), ToD2D(pB), brush_skeleton_.Get(), 1.8f);
                }
            }
        }
    };

    if (e.rig_type == 15) {
        drawBones(kR15Bones);
    } else if (e.rig_type == 6) {
        drawBones(kR6Bones);
    }

    for (std::size_t j = 0; j < kMaxRigJoints; ++j) {
        if (e.joint_mask & static_cast<uint16_t>(1u << j)) {
            Vec2 pt;
            if (math::WorldToScreen(e.joints[j], cam, w, h, pt)) {
                if (pt.x >= -16.0f && pt.x <= w + 16.0f && pt.y >= -16.0f && pt.y <= h + 16.0f) {
                    const float radius = (j == 0) ? 3.2f : 2.0f;
                    d2d_ctx_->FillRectangle(D2D1::RectF(pt.x - radius, pt.y - radius, pt.x + radius, pt.y + radius), brush_joint_.Get());
                }
            }
        }
    }
}

void OverlayRenderer::DrawAimOverlay(const aim::AimDebugState& aimDebug) {
    if (!aimDebug.enabled) {
        return;
    }
    if (aimDebug.draw_fov_circle && aimDebug.fov_px >= 4.0f) {
        d2d_ctx_->DrawEllipse(
            D2D1::Ellipse(ToD2D(aimDebug.aim_origin), aimDebug.fov_px, aimDebug.fov_px),
            brush_aim_fov_.Get(),
            1.0f);
    }
    if (aimDebug.draw_target_marker && aimDebug.has_target) {
        ID2D1SolidColorBrush* b = aimDebug.actively_engaging ? brush_aim_lock_.Get() : brush_prop_.Get();
        const float r = aimDebug.actively_engaging ? 3.2f : 2.6f;
        const Vec2 pt = aimDebug.target_screen_pt;
        d2d_ctx_->FillEllipse(D2D1::Ellipse(ToD2D(pt), r, r), b);
    }
}

void OverlayRenderer::DrawHud(
    const CameraState& cam,
    const IpcStats& ipcStats,
    float w,
    double nowSec,
    const aim::AimDebugState* aimDebug
) {
    const uint64_t nowUs = static_cast<uint64_t>(nowSec * 1000000.0);
    const double ageMs = (ipcStats.last_packet_us > 0 && nowUs >= ipcStats.last_packet_us)
        ? static_cast<double>(nowUs - ipcStats.last_packet_us) / 1000.0
        : -1.0;

    char ageBuf[24] = "N/A";
    if (ageMs >= 0.0) {
        std::snprintf(ageBuf, sizeof(ageBuf), "%.0fms", ageMs);
    }

    char aimBuf[64] = "";
    if (aimDebug) {
        if (aimDebug->enabled) {
            std::snprintf(
                aimBuf, sizeof(aimBuf), " | AIM:%s %.0f%%%s",
                aim::ProfileName(aimDebug->profile),
                aimDebug->strength * 100.0f,
                aimDebug->actively_engaging ? "*" : "");
        } else {
            std::snprintf(aimBuf, sizeof(aimBuf), " | AIM:OFF");
        }
    }

    char espBuf[48] = "ESP:OFF";
    if (config_.esp_enabled) {
        std::snprintf(
            espBuf, sizeof(espBuf), "ESP:%s%s%s%s",
            config_.show_2d_box ? "2D " : "",
            config_.show_3d_box ? "OBB " : "",
            config_.show_axes ? "AX " : "",
            config_.show_skeleton ? "SKEL" : "");
    }

    char buf[384];
    const int bufLen = std::snprintf(
        buf, sizeof(buf),
        "nah2 | %s %.1f Hz | FPS %.0f | Ents %u | FOV %.0f | %s%s | Age %s",
        ipcStats.transport,
        ipcStats.packets_per_sec,
        diag_.fps,
        diag_.visible_entities,
        cam.fov_deg,
        espBuf,
        aimBuf,
        ageBuf);

    const float barW = std::min(w - 24.0f, 1020.0f);
    if (barW < 120.0f || bufLen <= 0) return;

    wchar_t textW[384];
    const int wLen = Utf8ToWideBuf(
        std::string_view(buf, static_cast<std::size_t>(std::min(bufLen, static_cast<int>(sizeof(buf) - 1)))),
        textW,
        static_cast<int>(std::size(textW)));
    if (wLen <= 0) return;

    const D2D1_ROUNDED_RECT panel = D2D1::RoundedRect(
        D2D1::RectF(12.0f, 10.0f, 12.0f + barW, 36.0f), 4.0f, 4.0f);
    d2d_ctx_->FillRoundedRectangle(panel, brush_hud_bg_.Get());
    d2d_ctx_->DrawRoundedRectangle(panel, brush_hud_border_.Get(), 1.0f);

    const D2D1_RECT_F textRc = D2D1::RectF(20.0f, 15.0f, 12.0f + barW - 8.0f, 34.0f);
    d2d_ctx_->DrawText(textW, static_cast<UINT32>(wLen), hud_format_.Get(), textRc, brush_text_.Get());
}

void OverlayRenderer::RenderFrame(
    const CameraState& cam,
    const std::vector<EntityTelemetry>& entities,
    const IpcStats& ipcStats,
    double nowSec,
    const aim::AimDebugState* aimDebug
) {
    if (!d2d_ctx_ || !target_bitmap_ || !swap_chain_) {
        return;
    }

    // Calcul FPS glissant sur 0.5s
    if (fps_window_start_ <= 0.0) {
        fps_window_start_ = nowSec;
    }
    ++fps_frames_;
    if (nowSec - fps_window_start_ >= 0.5) {
        diag_.fps = static_cast<double>(fps_frames_) / (nowSec - fps_window_start_);
        fps_window_start_ = nowSec;
        fps_frames_ = 0;
    }

    const float w = static_cast<float>(width_);
    const float h = static_cast<float>(height_);

    d2d_ctx_->SetTarget(target_bitmap_.Get());
    d2d_ctx_->BeginDraw();
    // Fond 100% transparent (alpha premultiplie a 0) pour DirectComposition
    d2d_ctx_->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    uint32_t drawnCount = 0;
    if (config_.esp_enabled) {
        for (const auto& e : entities) {
            if (e.kind == 1 && e.health >= 0.0f && e.health <= 0.01f) {
                continue;
            }
            const float dist = math::Length(math::Sub(e.pos, cam.pos));

            // Culling rapide : si le centre + rayon englobant est entierement derriere la camera
            const float radius = 0.5f * math::Length(e.size);
            const float forwardW = math::Dot(math::Sub(e.pos, cam.pos), cam.look);
            if (forwardW + radius < math::kNearPlaneW) {
                continue;
            }

            if (config_.show_3d_box) {
                DrawEntity3DBox(e, cam, w, h);
            }
            if (config_.show_2d_box) {
                DrawEntity2DBox(e, cam, w, h, dist);
            }
            if (config_.show_axes) {
                DrawEntityAxes(e, cam, w, h);
            }
            if (config_.show_skeleton) {
                DrawEntitySkeleton(e, cam, w, h);
            }
            ++drawnCount;
        }
    }
    diag_.visible_entities = drawnCount;

    if (aimDebug) {
        DrawAimOverlay(*aimDebug);
    }

    if (config_.show_hud) {
        DrawHud(cam, ipcStats, w, nowSec, aimDebug);
    }

    const HRESULT endHr = d2d_ctx_->EndDraw();
    if (endHr == D2DERR_RECREATE_TARGET) {
        ReleaseDeviceResources();
        InitDeviceResources(width_, height_);
        return;
    }

    const HRESULT presentHr = swap_chain_->Present(config_.vsync ? 1 : 0, 0);
    if (presentHr == DXGI_ERROR_DEVICE_REMOVED || presentHr == DXGI_ERROR_DEVICE_RESET) {
        ReleaseDeviceResources();
        InitDeviceResources(width_, height_);
    }
}

} // namespace nah2
