#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN

#include "control_panel.h"

#include <dwmapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace nah2 {

namespace {

constexpr int kLogicalW = 410;
constexpr int kLogicalH = 600;
constexpr wchar_t kPanelClassName[] = L"Nah2ControlPanelWindowClass";

COLORREF Rgb(uint8_t r, uint8_t g, uint8_t b) {
    return RGB(r, g, b);
}

void FillRoundRect(HDC hdc, const RECT& rc, int radius, COLORREF fill, COLORREF border) {
    HBRUSH br = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldBr = SelectObject(hdc, br);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
    SelectObject(hdc, oldBr);
    SelectObject(hdc, oldPen);
    DeleteObject(br);
    DeleteObject(pen);
}

void DrawTextUtf8(HDC hdc, const char* text, RECT rc, HFONT font, COLORREF color, UINT fmt) {
    if (!text || !text[0]) return;
    wchar_t wbuf[256];
    const int n = MultiByteToWideChar(CP_UTF8, 0, text, -1, wbuf, static_cast<int>(std::size(wbuf)));
    if (n <= 1) return;
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, color);
    HGDIOBJ oldF = SelectObject(hdc, font);
    DrawTextW(hdc, wbuf, n - 1, &rc, fmt);
    SelectObject(hdc, oldF);
}

bool PtInRectPx(const RECT& r, int x, int y) {
    return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

} // namespace

ControlPanel::~ControlPanel() {
    Shutdown();
}

int ControlPanel::Sx(float logicalPx) const {
    return static_cast<int>(std::lround(logicalPx * dpi_scale_));
}

bool ControlPanel::Initialize(
    HINSTANCE hInstance,
    OverlayConfig* overlayCfg,
    aim::AimConfig* aimCfg,
    std::function<void()> onConfigChanged,
    std::function<void(aim::AimProfile)> onProfileSelected,
    std::function<void()> onQuitRequested
) {
    hinstance_ = hInstance;
    overlay_cfg_ = overlayCfg;
    aim_cfg_ = aimCfg;
    on_config_changed_ = std::move(onConfigChanged);
    on_profile_selected_ = std::move(onProfileSelected);
    on_quit_requested_ = std::move(onQuitRequested);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = ControlPanel::WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.lpszClassName = kPanelClassName;
    class_atom_ = RegisterClassExW(&wc);
    if (!class_atom_) {
        return false;
    }

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rc = { 0, 0, kLogicalW, kLogicalH };
    AdjustWindowRectEx(&rc, style, FALSE, 0);

    hwnd_ = CreateWindowExW(
        0,
        kPanelClassName,
        L"nah2 — Controle ESP & Aim Assist",
        style,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, hInstance, this);

    if (!hwnd_) {
        return false;
    }

    const UINT dpi = GetDpiForWindow(hwnd_);
    dpi_scale_ = (dpi > 0) ? (static_cast<float>(dpi) / 96.0f) : 1.0f;

    // Ajuste exactement la zone cliente au DPI de l'ecran
    RECT scaledRc = { 0, 0, Sx(kLogicalW), Sx(kLogicalH) };
    AdjustWindowRectExForDpi(&scaledRc, style, FALSE, 0, dpi > 0 ? dpi : 96);
    SetWindowPos(
        hwnd_, nullptr, 0, 0,
        scaledRc.right - scaledRc.left, scaledRc.bottom - scaledRc.top,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    const DWORD darkMode = 1;
    DwmSetWindowAttribute(hwnd_, 20, &darkMode, sizeof(darkMode));

    RECT cli{};
    GetClientRect(hwnd_, &cli);
    client_w_ = std::max<int>(cli.right - cli.left, 1);
    client_h_ = std::max<int>(cli.bottom - cli.top, 1);

    RecreateFonts();
    RebuildLayout();

    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd_);
    visible_ = true;
    return true;
}

void ControlPanel::Shutdown() {
    ReleaseBackbuffer();
    if (font_title_) { DeleteObject(font_title_); font_title_ = nullptr; }
    if (font_section_) { DeleteObject(font_section_); font_section_ = nullptr; }
    if (font_body_) { DeleteObject(font_body_); font_body_ = nullptr; }
    if (font_small_) { DeleteObject(font_small_); font_small_ = nullptr; }

    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (class_atom_ && hinstance_) {
        UnregisterClassW(kPanelClassName, hinstance_);
        class_atom_ = 0;
    }
}

void ControlPanel::SetVisible(bool visible) {
    visible_ = visible;
    if (!hwnd_) return;
    if (visible) {
        ShowWindow(hwnd_, SW_SHOW);
        SetForegroundWindow(hwnd_);
        InvalidateRect(hwnd_, nullptr, FALSE);
    } else {
        ShowWindow(hwnd_, SW_HIDE);
    }
}

void ControlPanel::UpdateStatus(const PanelStatus& status) {
    const bool changed =
        status_.target_found != status.target_found ||
        status_.target_foreground != status.target_foreground ||
        status_.aim_engaging != status.aim_engaging ||
        std::abs(status_.ipc.packets_per_sec - status.ipc.packets_per_sec) >= 0.5 ||
        std::strcmp(status_.ipc.transport, status.ipc.transport) != 0 ||
        std::abs(status_.px_per_count_x - status.px_per_count_x) >= 0.05f;

    status_ = status;
    if (changed && hwnd_ && visible_) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void ControlPanel::NotifyExternalChange() {
    if (hwnd_ && visible_) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void ControlPanel::RecreateFonts() {
    if (font_title_) DeleteObject(font_title_);
    if (font_section_) DeleteObject(font_section_);
    if (font_body_) DeleteObject(font_body_);
    if (font_small_) DeleteObject(font_small_);

    auto makeFont = [this](float pt, int weight, const wchar_t* face) {
        return CreateFontW(
            -Sx(pt), 0, 0, 0, weight, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
    };

    font_title_ = makeFont(14.5f, FW_BOLD, L"Segoe UI");
    font_section_ = makeFont(13.0f, FW_SEMIBOLD, L"Segoe UI");
    font_body_ = makeFont(11.5f, FW_MEDIUM, L"Segoe UI");
    font_small_ = makeFont(10.5f, FW_NORMAL, L"Consolas");
}

void ControlPanel::EnsureBackbuffer(HDC refDc, int w, int h) {
    if (mem_dc_ && mem_bmp_ && mem_w_ == w && mem_h_ == h) {
        return;
    }
    ReleaseBackbuffer();
    mem_dc_ = CreateCompatibleDC(refDc);
    mem_bmp_ = CreateCompatibleBitmap(refDc, w, h);
    old_bmp_ = static_cast<HBITMAP>(SelectObject(mem_dc_, mem_bmp_));
    mem_w_ = w;
    mem_h_ = h;
}

void ControlPanel::ReleaseBackbuffer() {
    if (mem_dc_) {
        if (old_bmp_) {
            SelectObject(mem_dc_, old_bmp_);
            old_bmp_ = nullptr;
        }
        if (mem_bmp_) {
            DeleteObject(mem_bmp_);
            mem_bmp_ = nullptr;
        }
        DeleteDC(mem_dc_);
        mem_dc_ = nullptr;
    }
    mem_w_ = 0;
    mem_h_ = 0;
}

void ControlPanel::RebuildLayout() {
    hit_zones_.clear();

    // 1. Carte ESP (y = 52..156)
    hit_zones_.push_back({ { Sx(284), Sx(62), Sx(384), Sx(92) }, WidgetKind::ToggleEsp, 0 });

    const WidgetKind espPills[6] = {
        WidgetKind::Toggle2D,
        WidgetKind::Toggle3D,
        WidgetKind::ToggleAxes,
        WidgetKind::ToggleSkeleton,
        WidgetKind::ToggleHud,
        WidgetKind::ToggleOverlay,
    };
    const int espX0 = Sx(24);
    const int espGap = Sx(6);
    const int espPillW = Sx(55);
    for (int i = 0; i < 6; ++i) {
        const int x = espX0 + i * (espPillW + espGap);
        hit_zones_.push_back({ { x, Sx(106), x + espPillW, Sx(136) }, espPills[i], 0 });
    }

    // 2. Carte Aim Assist (y = 166..524)
    hit_zones_.push_back({ { Sx(284), Sx(176), Sx(384), Sx(206) }, WidgetKind::ToggleAim, 0 });

    // Profils (3 pills)
    for (int i = 0; i < 3; ++i) {
        const int x = Sx(96) + i * Sx(98);
        hit_zones_.push_back({ { x, Sx(218), x + Sx(90), Sx(246) }, WidgetKind::SelectProfile, i });
    }

    // Zones (4 pills)
    for (int i = 0; i < 4; ++i) {
        const int x = Sx(96) + i * Sx(73);
        hit_zones_.push_back({ { x, Sx(254), x + Sx(67), Sx(282) }, WidgetKind::SelectBone, i });
    }

    // Touches (8 pills compacts)
    for (int i = 0; i < 8; ++i) {
        const int x = Sx(96) + i * Sx(36);
        hit_zones_.push_back({ { x, Sx(290), x + Sx(32), Sx(318) }, WidgetKind::SelectKey, i });
    }

    // Sliders (Puissance, FOV, Smooth, Lead, Deadzone)
    const WidgetKind sliderKinds[5] = {
        WidgetKind::SliderStrength,
        WidgetKind::SliderFov,
        WidgetKind::SliderSmooth,
        WidgetKind::SliderLead,
        WidgetKind::SliderDeadzone,
    };
    for (int i = 0; i < 5; ++i) {
        const int y = Sx(334 + i * 34);
        hit_zones_.push_back({ { Sx(118), y, Sx(322), y + Sx(26) }, sliderKinds[i], i });
    }

    // Options bas Aim Assist (4 pills)
    const WidgetKind aimOpts[4] = {
        WidgetKind::ToggleTeam,
        WidgetKind::ToggleVis,
        WidgetKind::ToggleFovCircle,
        WidgetKind::ToggleUnlocked,
    };
    for (int i = 0; i < 4; ++i) {
        const int x = Sx(24) + i * Sx(91);
        hit_zones_.push_back({ { x, Sx(512), x + Sx(85), Sx(540) }, aimOpts[i], 0 });
    }
}

void ControlPanel::UpdateSliderFromX(int sliderIdx, int x) {
    if (!aim_cfg_) return;
    const int left = Sx(122);
    const int right = Sx(318);
    const float t = math::Clamp(static_cast<float>(x - left) / static_cast<float>(std::max(right - left, 1)), 0.0f, 1.0f);

    switch (sliderIdx) {
        case 0:
            aim_cfg_->strength = std::round((0.10f + t * 1.90f) * 20.0f) * 0.05f;
            break;
        case 1:
            aim_cfg_->fov_px = std::round(40.0f + t * 320.0f);
            break;
        case 2:
            aim_cfg_->smooth_scale = 0.35f + t * 2.15f;
            break;
        case 3:
            aim_cfg_->velocity_lead = t * 1.50f;
            break;
        case 4:
            aim_cfg_->deadzone_ratio = t * 0.90f;
            break;
        default:
            return;
    }

    if (on_config_changed_) {
        on_config_changed_();
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void ControlPanel::HandleMouseDown(int x, int y) {
    if (!overlay_cfg_ || !aim_cfg_) return;

    for (std::size_t i = 0; i < hit_zones_.size(); ++i) {
        const auto& hz = hit_zones_[i];
        if (!PtInRectPx(hz.rect, x, y)) {
            continue;
        }

        switch (hz.kind) {
            case WidgetKind::ToggleEsp:
                overlay_cfg_->esp_enabled = !overlay_cfg_->esp_enabled;
                break;
            case WidgetKind::Toggle2D:
                overlay_cfg_->show_2d_box = !overlay_cfg_->show_2d_box;
                break;
            case WidgetKind::Toggle3D:
                overlay_cfg_->show_3d_box = !overlay_cfg_->show_3d_box;
                break;
            case WidgetKind::ToggleAxes:
                overlay_cfg_->show_axes = !overlay_cfg_->show_axes;
                break;
            case WidgetKind::ToggleSkeleton:
                overlay_cfg_->show_skeleton = !overlay_cfg_->show_skeleton;
                break;
            case WidgetKind::ToggleHud:
                overlay_cfg_->show_hud = !overlay_cfg_->show_hud;
                break;
            case WidgetKind::ToggleOverlay:
                overlay_cfg_->overlay_enabled = !overlay_cfg_->overlay_enabled;
                break;
            case WidgetKind::ToggleAim:
                aim_cfg_->enabled = !aim_cfg_->enabled;
                break;
            case WidgetKind::SelectProfile:
                if (on_profile_selected_) {
                    on_profile_selected_(static_cast<aim::AimProfile>(hz.param));
                }
                break;
            case WidgetKind::SelectBone:
                aim_cfg_->bone_mode = static_cast<aim::AimBoneMode>(hz.param);
                break;
            case WidgetKind::SelectKey:
                aim_cfg_->key_mode = static_cast<aim::AimKeyMode>(hz.param);
                break;
            case WidgetKind::ToggleTeam:
                aim_cfg_->team_check = !aim_cfg_->team_check;
                break;
            case WidgetKind::ToggleVis:
                aim_cfg_->visibility_check = !aim_cfg_->visibility_check;
                break;
            case WidgetKind::ToggleFovCircle:
                aim_cfg_->draw_fov_circle = !aim_cfg_->draw_fov_circle;
                break;
            case WidgetKind::ToggleUnlocked:
                aim_cfg_->allow_unlocked_cursor = !aim_cfg_->allow_unlocked_cursor;
                break;
            case WidgetKind::SliderStrength:
            case WidgetKind::SliderFov:
            case WidgetKind::SliderSmooth:
            case WidgetKind::SliderLead:
            case WidgetKind::SliderDeadzone:
                active_slider_ = hz.param;
                SetCapture(hwnd_);
                UpdateSliderFromX(active_slider_, x);
                return;
        }

        if (on_config_changed_) {
            on_config_changed_();
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
}

void ControlPanel::HandleMouseMove(int x, int y) {
    if (active_slider_ >= 0) {
        UpdateSliderFromX(active_slider_, x);
        return;
    }
    int newHover = -1;
    for (std::size_t i = 0; i < hit_zones_.size(); ++i) {
        if (PtInRectPx(hit_zones_[i].rect, x, y)) {
            newHover = static_cast<int>(i);
            break;
        }
    }
    if (newHover != hover_zone_) {
        hover_zone_ = newHover;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void ControlPanel::HandleMouseUp() {
    if (active_slider_ >= 0) {
        active_slider_ = -1;
        ReleaseCapture();
    }
}

void ControlPanel::Paint(HDC targetDc) {
    if (!overlay_cfg_ || !aim_cfg_) return;

    EnsureBackbuffer(targetDc, client_w_, client_h_);
    HDC hdc = mem_dc_;

    const COLORREF cBg = Rgb(15, 20, 28);
    const COLORREF cCard = Rgb(23, 31, 43);
    const COLORREF cBorder = Rgb(42, 56, 76);
    const COLORREF cText = Rgb(238, 244, 252);
    const COLORREF cMuted = Rgb(140, 158, 182);
    const COLORREF cEspOn = Rgb(22, 212, 236);
    const COLORREF cAimOn = Rgb(54, 226, 140);
    const COLORREF cOff = Rgb(45, 58, 76);
    const COLORREF cOffHover = Rgb(58, 74, 98);
    const COLORREF cDarkTxt = Rgb(10, 18, 26);

    RECT bgRc = { 0, 0, client_w_, client_h_ };
    HBRUSH bgBr = CreateSolidBrush(cBg);
    FillRect(hdc, &bgRc, bgBr);
    DeleteObject(bgBr);

    // 1. Barre de statut en haut
    FillRoundRect(hdc, { Sx(14), Sx(10), Sx(396), Sx(42) }, Sx(8), cCard, cBorder);
    char ipcText[96];
    std::snprintf(ipcText, sizeof(ipcText), "IPC: %s %.0f Hz", status_.ipc.transport, status_.ipc.packets_per_sec);
    DrawTextUtf8(hdc, ipcText, { Sx(24), Sx(15), Sx(210), Sx(38) }, font_small_,
                 status_.ipc.packets_per_sec > 0.5 ? cEspOn : cMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    const char* rbxText = status_.target_foreground ? "Roblox: Actif" : (status_.target_found ? "Roblox: Detecte" : "Roblox: Attente");
    COLORREF rbxCol = status_.target_foreground ? cAimOn : (status_.target_found ? Rgb(245, 185, 65) : cMuted);
    DrawTextUtf8(hdc, rbxText, { Sx(210), Sx(15), Sx(386), Sx(38) }, font_small_, rbxCol, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    // 2. Carte ESP
    FillRoundRect(hdc, { Sx(14), Sx(52), Sx(396), Sx(150) }, Sx(10), cCard, overlay_cfg_->esp_enabled ? Rgb(24, 96, 114) : cBorder);
    DrawTextUtf8(hdc, "ESP Spatial [F5]", { Sx(24), Sx(60), Sx(270), Sx(80) }, font_title_, cText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextUtf8(hdc, "Boites 2D/3D, axes locaux et squelettes", { Sx(24), Sx(80), Sx(275), Sx(96) }, font_small_, cMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    auto drawPill = [&](std::size_t idx, const char* label, bool active, COLORREF activeColor, HFONT f) {
        const RECT& rc = hit_zones_[idx].rect;
        const bool hovered = (hover_zone_ == static_cast<int>(idx));
        const COLORREF fill = active ? activeColor : (hovered ? cOffHover : cOff);
        const COLORREF border = active ? activeColor : cBorder;
        const COLORREF txt = active ? cDarkTxt : cText;
        FillRoundRect(hdc, rc, Sx(8), fill, border);
        DrawTextUtf8(hdc, label, rc, f, txt, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    };

    drawPill(0, overlay_cfg_->esp_enabled ? "ESP: ON" : "ESP: OFF", overlay_cfg_->esp_enabled, cEspOn, font_section_);

    const char* espLabels[6] = { "2D", "OBB", "Axes", "Skel", "HUD", "Ovl F8" };
    const bool espStates[6] = {
        overlay_cfg_->show_2d_box,
        overlay_cfg_->show_3d_box,
        overlay_cfg_->show_axes,
        overlay_cfg_->show_skeleton,
        overlay_cfg_->show_hud,
        overlay_cfg_->overlay_enabled,
    };
    for (int i = 0; i < 6; ++i) {
        drawPill(static_cast<std::size_t>(1 + i), espLabels[i], espStates[i], cEspOn, font_body_);
    }

    // 3. Carte Aim Assist
    FillRoundRect(hdc, { Sx(14), Sx(162), Sx(396), Sx(556) }, Sx(10), cCard, aim_cfg_->enabled ? Rgb(30, 102, 74) : cBorder);
    DrawTextUtf8(hdc, "Aim Assist Humain [F6]", { Sx(24), Sx(172), Sx(275), Sx(192) }, font_title_, cText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    char sensBuf[64];
    if (status_.px_per_count_x > 0.05f) {
        std::snprintf(sensBuf, sizeof(sensBuf), "Auto-Sens: %.2f / %.2f px/cnt%s",
                      status_.px_per_count_x, status_.px_per_count_y,
                      status_.aim_engaging ? " [LOCK]" : "");
    } else {
        std::snprintf(sensBuf, sizeof(sensBuf), "Simulation motrice humaine + WM_INPUT");
    }
    DrawTextUtf8(hdc, sensBuf, { Sx(24), Sx(192), Sx(278), Sx(208) }, font_small_,
                 status_.aim_engaging ? cAimOn : cMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    drawPill(7, aim_cfg_->enabled ? "AIM: ON" : "AIM: OFF", aim_cfg_->enabled, cAimOn, font_section_);

    // Profils
    DrawTextUtf8(hdc, "Profil [F7]", { Sx(24), Sx(218), Sx(92), Sx(246) }, font_body_, cMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    const char* profLabels[3] = { "Ultra-Legit", "Competitif", "Tracking" };
    for (int i = 0; i < 3; ++i) {
        drawPill(static_cast<std::size_t>(8 + i), profLabels[i],
                 static_cast<int>(aim_cfg_->profile) == i, cAimOn, font_body_);
    }

    // Zones
    DrawTextUtf8(hdc, "Zone", { Sx(24), Sx(254), Sx(92), Sx(282) }, font_body_, cMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    const char* boneLabels[4] = { "Smart", "Tete", "Cou", "Torse" };
    for (int i = 0; i < 4; ++i) {
        drawPill(static_cast<std::size_t>(11 + i), boneLabels[i],
                 static_cast<int>(aim_cfg_->bone_mode) == i, cAimOn, font_body_);
    }

    // Touche
    DrawTextUtf8(hdc, "Touche", { Sx(24), Sx(290), Sx(92), Sx(318) }, font_body_, cMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    const char* keyLabels[8] = { "Any", "RMB", "LMB", "X1", "X2", "Alt", "Shf", "All" };
    for (int i = 0; i < 8; ++i) {
        drawPill(static_cast<std::size_t>(15 + i), keyLabels[i],
                 static_cast<int>(aim_cfg_->key_mode) == i, cAimOn, font_small_);
    }

    // Sliders
    auto drawSlider = [&](std::size_t zoneIdx, const char* name, float normVal, const char* valStr) {
        const RECT& hit = hit_zones_[zoneIdx].rect;
        const int rowTop = hit.top;
        const int rowBot = hit.bottom;
        DrawTextUtf8(hdc, name, { Sx(24), rowTop, Sx(114), rowBot }, font_body_, cText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        DrawTextUtf8(hdc, valStr, { Sx(328), rowTop, Sx(386), rowBot }, font_small_, cAimOn, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

        const int trkLeft = Sx(122);
        const int trkRight = Sx(318);
        const int trkMidY = (rowTop + rowBot) / 2;
        const int trkTop = trkMidY - Sx(3);
        const int trkBot = trkMidY + Sx(3);
        FillRoundRect(hdc, { trkLeft, trkTop, trkRight, trkBot }, Sx(4), Rgb(40, 52, 70), Rgb(54, 70, 92));

        const int fillRight = trkLeft + static_cast<int>(std::lround(math::Clamp(normVal, 0.0f, 1.0f) * (trkRight - trkLeft)));
        if (fillRight > trkLeft + Sx(2)) {
            FillRoundRect(hdc, { trkLeft, trkTop, fillRight, trkBot }, Sx(4), cAimOn, cAimOn);
        }
        const int knobR = Sx(6);
        FillRoundRect(hdc, { fillRight - knobR, trkMidY - knobR, fillRight + knobR, trkMidY + knobR }, Sx(12), cText, cAimOn);
    };

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.0f%%", aim_cfg_->strength * 100.0f);
    drawSlider(23, "Puissance", (aim_cfg_->strength - 0.10f) / 1.90f, buf);

    std::snprintf(buf, sizeof(buf), "%.0f px", aim_cfg_->fov_px);
    drawSlider(24, "FOV", (aim_cfg_->fov_px - 40.0f) / 320.0f, buf);

    std::snprintf(buf, sizeof(buf), "%.2fx", aim_cfg_->smooth_scale);
    drawSlider(25, "Lissage", (aim_cfg_->smooth_scale - 0.35f) / 2.15f, buf);

    std::snprintf(buf, sizeof(buf), "%.2fx", aim_cfg_->velocity_lead);
    drawSlider(26, "Prediction", aim_cfg_->velocity_lead / 1.50f, buf);

    std::snprintf(buf, sizeof(buf), "%.0f%%", aim_cfg_->deadzone_ratio * 100.0f);
    drawSlider(27, "Zone morte", aim_cfg_->deadzone_ratio / 0.90f, buf);

    // Options bas Aim Assist
    const char* aimOptLabels[4] = { "Equipe", "Visibilite", "Cercle FOV", "Libre" };
    const bool aimOptStates[4] = {
        aim_cfg_->team_check,
        aim_cfg_->visibility_check,
        aim_cfg_->draw_fov_circle,
        aim_cfg_->allow_unlocked_cursor,
    };
    for (int i = 0; i < 4; ++i) {
        drawPill(static_cast<std::size_t>(28 + i), aimOptLabels[i], aimOptStates[i], cAimOn, font_body_);
    }

    // 4. Pied de page raccourcis
    DrawTextUtf8(
        hdc,
        "F5 ESP | F6 Aim | F7 Profil | F8 Ovl | INS Panneau | END Quitter",
        { Sx(14), Sx(564), Sx(396), Sx(590) },
        font_small_, cMuted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    BitBlt(targetDc, 0, 0, client_w_, client_h_, hdc, 0, 0, SRCCOPY);
}

LRESULT CALLBACK ControlPanel::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return TRUE;
    }

    auto* self = reinterpret_cast<ControlPanel*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) {
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            self->Paint(dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_SIZE: {
            self->client_w_ = std::max<int>(LOWORD(lParam), 1);
            self->client_h_ = std::max<int>(HIWORD(lParam), 1);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_DPICHANGED: {
            const UINT newDpi = HIWORD(wParam);
            self->dpi_scale_ = (newDpi > 0) ? (static_cast<float>(newDpi) / 96.0f) : 1.0f;
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(
                hwnd, nullptr,
                suggested->left, suggested->top,
                suggested->right - suggested->left,
                suggested->bottom - suggested->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            self->RecreateFonts();
            self->RebuildLayout();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            const int x = static_cast<short>(LOWORD(lParam));
            const int y = static_cast<short>(HIWORD(lParam));
            self->HandleMouseDown(x, y);
            return 0;
        }
        case WM_MOUSEMOVE: {
            const int x = static_cast<short>(LOWORD(lParam));
            const int y = static_cast<short>(HIWORD(lParam));
            self->HandleMouseMove(x, y);
            return 0;
        }
        case WM_LBUTTONUP:
        case WM_CAPTURECHANGED:
            self->HandleMouseUp();
            return 0;
        case WM_CLOSE:
            if (self->on_quit_requested_) {
                self->on_quit_requested_();
            }
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

} // namespace nah2
