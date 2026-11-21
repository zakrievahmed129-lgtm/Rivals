#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN

#include "aim_assist.h"
#include "control_panel.h"
#include "ipc_receiver.h"
#include "math_utils.h"
#include "renderer.h"

#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

std::atomic<bool> g_quitRequested{ false };
std::atomic<HANDLE> g_wakeEvent{ nullptr };

BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType) {
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT || ctrlType == CTRL_CLOSE_EVENT) {
        g_quitRequested.store(true, std::memory_order_release);
        if (HANDLE ev = g_wakeEvent.load(std::memory_order_acquire)) {
            SetEvent(ev);
        }
        return TRUE;
    }
    return FALSE;
}

class KeyEdgeTracker {
public:
    bool PressedOnce(int vk) {
        const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
        bool triggered = false;
        for (auto& slot : states_) {
            if (slot.vk == vk) {
                triggered = down && !slot.was_down;
                slot.was_down = down;
                return triggered;
            }
        }
        states_.push_back({ vk, down });
        return false;
    }

private:
    struct Slot {
        int vk = 0;
        bool was_down = false;
    };
    std::vector<Slot> states_;
};

std::string ToLowerStr(std::string s) {
    for (char& ch : s) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return s;
}

bool IsSystemCursorHidden() {
    CURSORINFO ci{};
    ci.cbSize = sizeof(ci);
    if (!GetCursorInfo(&ci)) {
        return false;
    }
    return (ci.flags & CURSOR_SHOWING) == 0;
}

bool IsAimKeyPressed(const nah2::aim::AimConfig& cfg, const nah2::CameraState& cam, bool cursorHidden) {
    using nah2::aim::AimKeyMode;
    auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    switch (cfg.key_mode) {
        case AimKeyMode::AnyMouse: {
            const bool lmbAllowed = (cam.mouse_behavior != 0) || cfg.allow_unlocked_cursor || cursorHidden;
            return down(VK_RBUTTON) || down(VK_XBUTTON1) || down(VK_XBUTTON2) || (down(VK_LBUTTON) && lmbAllowed);
        }
        case AimKeyMode::RightMouse: return down(VK_RBUTTON);
        case AimKeyMode::LeftMouse:  return down(VK_LBUTTON);
        case AimKeyMode::Mouse4:     return down(VK_XBUTTON1);
        case AimKeyMode::Mouse5:     return down(VK_XBUTTON2);
        case AimKeyMode::LeftAlt:    return down(VK_MENU);
        case AimKeyMode::LeftShift:  return down(VK_SHIFT);
        case AimKeyMode::Always:     return true;
        default:                     return false;
    }
}

void SendRelativeMouseDelta(int dx, int dy) {
    if (dx == 0 && dy == 0) {
        return;
    }
    INPUT inp{};
    inp.type = INPUT_MOUSE;
    inp.mi.dx = static_cast<LONG>(dx);
    inp.mi.dy = static_cast<LONG>(dy);
    inp.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &inp, sizeof(INPUT));
}

void PrintUsage() {
    std::printf(
        "nah2 - Diagnostic spatial & Aim Assist humain temps reel pour Roblox\n"
        "Usage: nah2.exe [options]\n"
        "  --port <N>                 Port IPC local sur 127.0.0.1 (defaut: 27015)\n"
        "  --target <texte>           Filtre titre/processus fenetre cible (defaut: Roblox)\n"
        "  --smooth <vitesse>         Vitesse du lerp entites (defaut: 0 = zero latence)\n"
        "  --cam-smooth <v>           Vitesse du lerp camera (defaut: 0.0)\n"
        "  --always-show              Garde l'overlay visible meme si Roblox n'est pas actif\n"
        "  --vsync                    Active la VSync DXGI (desactivee par defaut)\n"
        "  --no-vsync                 Desactive la VSync DXGI (defaut)\n"
        "  --aa                       Active l'anticrenelage Direct2D\n"
        "  --no-overlay               Demarre avec l'overlay visuel masque (F8 pour afficher)\n"
        "  --esp / --no-esp           Active / desactive l'ESP visuel (defaut: actif)\n"
        "  --no-panel                 Masque la fenetre de controle graphique au demarrage (INS pour ouvrir)\n"
        "  --no-hud                   Masque la barre HUD\n"
        "  --no-2d                    Masque les boites 2D\n"
        "  --no-3d                    Masque les boites 3D filaires (OBB)\n"
        "  --no-axes                  Masque les vecteurs d'orientation\n"
        "  --no-skeleton              Masque les squelettes R6/R15\n"
        "  --vp-offset <x,y>          Decalage manuel du viewport en pixels\n"
        "\n"
        "Options Aim Assist Humain :\n"
        "  --aim / --no-aim           Active / desactive l'aim assist (defaut: actif)\n"
        "  --aim-profile <nom>        Profil: legit | pro | tracking (defaut: legit)\n"
        "  --aim-bone <cible>         Zone: smart | head | neck | chest (defaut: smart)\n"
        "  --aim-key <touche>         Touche: any | rmb | lmb | x1 | x2 | alt | shift | always\n"
        "  --aim-fov <px>             Rayon FOV en pixels (defaut selon profil)\n"
        "  --aim-strength <mult>      Puissance non-lineaire 0.10..2.00 (defaut 1.0)\n"
        "  --aim-smooth <mult>        Facteur lissage (<1 plus rapide, >1 plus doux, defaut 1.0)\n"
        "  --aim-deadzone <ratio>     Zone morte interieure hitbox 0.0..0.9\n"
        "  --aim-lead <mult>          Prediction velocite cible (defaut selon profil)\n"
        "  --aim-sens <mult>          Sensibilite souris initiale (0 = auto-calibree)\n"
        "  --no-team-check            Cible aussi les equipiers\n"
        "  --no-vis-check             Ignore le test de visibilite (raycast)\n"
        "  --aim-unlocked             Autorise LMB/Always meme avec curseur deverrouille\n"
        "  --no-aim-fov               Masque le cercle FOV sur l'overlay\n"
        "\n"
        "Raccourcis clavier (non-bloquants) :\n"
        "  F5   Activer / desactiver l'ESP visuel\n"
        "  F6   Activer / desactiver l'Aim Assist\n"
        "  F7   Changer de profil Aim (LEGIT / PRO / TRACK)\n"
        "  F8   Afficher / masquer tout l'overlay\n"
        "  F9   Afficher / masquer les squelettes\n"
        "  F10  Afficher / masquer le HUD\n"
        "  INS  Afficher / masquer le panneau de controle\n"
        "  END  Quitter nah2\n");
}

} // namespace

int main(int argc, char* argv[]) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    uint16_t port = 27015;
    float entitySmooth = 0.0f;
    float cameraSmooth = 0.0f;
    nah2::OverlayConfig cfg;
    nah2::aim::AimConfig aimCfg;
    nah2::aim::ApplyProfileDefaults(aimCfg, nah2::aim::AimProfile::UltraLegit);
    bool startPanelVisible = true;
    bool hasCustomFov = false;
    float customFov = 0.0f;
    bool hasCustomDeadzone = false;
    float customDeadzone = 0.0f;
    bool hasCustomLead = false;
    float customLead = 0.0f;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            PrintUsage();
            return 0;
        } else if (arg == "--port" && i + 1 < argc) {
            port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "--target" && i + 1 < argc) {
            cfg.target_filter = argv[++i];
        } else if (arg == "--smooth" && i + 1 < argc) {
            entitySmooth = static_cast<float>(std::atof(argv[++i]));
        } else if (arg == "--cam-smooth" && i + 1 < argc) {
            cameraSmooth = static_cast<float>(std::atof(argv[++i]));
        } else if (arg == "--always-show") {
            cfg.always_visible = true;
        } else if (arg == "--vsync") {
            cfg.vsync = true;
        } else if (arg == "--no-vsync") {
            cfg.vsync = false;
        } else if (arg == "--aa") {
            cfg.antialias = true;
        } else if (arg == "--no-overlay") {
            cfg.overlay_enabled = false;
        } else if (arg == "--esp") {
            cfg.esp_enabled = true;
        } else if (arg == "--no-esp") {
            cfg.esp_enabled = false;
        } else if (arg == "--no-panel") {
            startPanelVisible = false;
        } else if (arg == "--no-hud") {
            cfg.show_hud = false;
        } else if (arg == "--no-2d") {
            cfg.show_2d_box = false;
        } else if (arg == "--no-3d") {
            cfg.show_3d_box = false;
        } else if (arg == "--no-axes") {
            cfg.show_axes = false;
        } else if (arg == "--no-skeleton") {
            cfg.show_skeleton = false;
        } else if (arg == "--vp-offset" && i + 1 < argc) {
            int ox = 0, oy = 0;
            if (std::sscanf(argv[++i], "%d,%d", &ox, &oy) == 2) {
                cfg.vp_offset_x = ox;
                cfg.vp_offset_y = oy;
            }
        } else if (arg == "--aim") {
            aimCfg.enabled = true;
        } else if (arg == "--no-aim") {
            aimCfg.enabled = false;
        } else if (arg == "--aim-profile" && i + 1 < argc) {
            const std::string p = ToLowerStr(argv[++i]);
            if (p == "legit" || p == "ultralegit") {
                nah2::aim::ApplyProfileDefaults(aimCfg, nah2::aim::AimProfile::UltraLegit);
            } else if (p == "pro" || p == "competitive") {
                nah2::aim::ApplyProfileDefaults(aimCfg, nah2::aim::AimProfile::Competitive);
            } else if (p == "track" || p == "tracking") {
                nah2::aim::ApplyProfileDefaults(aimCfg, nah2::aim::AimProfile::Tracking);
            }
        } else if (arg == "--aim-bone" && i + 1 < argc) {
            const std::string b = ToLowerStr(argv[++i]);
            if (b == "smart") aimCfg.bone_mode = nah2::aim::AimBoneMode::Smart;
            else if (b == "head") aimCfg.bone_mode = nah2::aim::AimBoneMode::Head;
            else if (b == "neck") aimCfg.bone_mode = nah2::aim::AimBoneMode::Neck;
            else if (b == "chest") aimCfg.bone_mode = nah2::aim::AimBoneMode::Chest;
        } else if (arg == "--aim-key" && i + 1 < argc) {
            const std::string k = ToLowerStr(argv[++i]);
            if (k == "any") aimCfg.key_mode = nah2::aim::AimKeyMode::AnyMouse;
            else if (k == "rmb") aimCfg.key_mode = nah2::aim::AimKeyMode::RightMouse;
            else if (k == "lmb") aimCfg.key_mode = nah2::aim::AimKeyMode::LeftMouse;
            else if (k == "x1" || k == "mouse4") aimCfg.key_mode = nah2::aim::AimKeyMode::Mouse4;
            else if (k == "x2" || k == "mouse5") aimCfg.key_mode = nah2::aim::AimKeyMode::Mouse5;
            else if (k == "alt") aimCfg.key_mode = nah2::aim::AimKeyMode::LeftAlt;
            else if (k == "shift") aimCfg.key_mode = nah2::aim::AimKeyMode::LeftShift;
            else if (k == "always") aimCfg.key_mode = nah2::aim::AimKeyMode::Always;
        } else if (arg == "--aim-fov" && i + 1 < argc) {
            hasCustomFov = true;
            customFov = std::max(10.0f, static_cast<float>(std::atof(argv[++i])));
        } else if ((arg == "--aim-strength" || arg == "--aim-power") && i + 1 < argc) {
            aimCfg.strength = std::clamp(static_cast<float>(std::atof(argv[++i])), 0.10f, 2.00f);
        } else if (arg == "--aim-smooth" && i + 1 < argc) {
            aimCfg.smooth_scale = std::clamp(static_cast<float>(std::atof(argv[++i])), 0.35f, 2.5f);
        } else if (arg == "--aim-deadzone" && i + 1 < argc) {
            hasCustomDeadzone = true;
            customDeadzone = std::clamp(static_cast<float>(std::atof(argv[++i])), 0.0f, 0.9f);
        } else if (arg == "--aim-lead" && i + 1 < argc) {
            hasCustomLead = true;
            customLead = std::clamp(static_cast<float>(std::atof(argv[++i])), 0.0f, 1.5f);
        } else if (arg == "--aim-sens" && i + 1 < argc) {
            aimCfg.sens_override = std::max(0.0f, static_cast<float>(std::atof(argv[++i])));
        } else if (arg == "--no-team-check") {
            aimCfg.team_check = false;
        } else if (arg == "--no-vis-check") {
            aimCfg.visibility_check = false;
        } else if (arg == "--aim-unlocked") {
            aimCfg.allow_unlocked_cursor = true;
        } else if (arg == "--no-aim-fov") {
            aimCfg.draw_fov_circle = false;
        } else {
            std::fprintf(stderr, "[nah2] Option inconnue: %s\n", arg.c_str());
            PrintUsage();
            return 1;
        }
    }

    if (hasCustomFov) aimCfg.fov_px = customFov;
    if (hasCustomDeadzone) aimCfg.deadzone_ratio = customDeadzone;
    if (hasCustomLead) aimCfg.velocity_lead = customLead;

    timeBeginPeriod(1);

    nah2::IpcReceiver ipc(port);
    if (!ipc.Start()) {
        std::fprintf(stderr, "[nah2] Impossible de demarrer le serveur IPC sur 127.0.0.1:%u\n", port);
        timeEndPeriod(1);
        return 1;
    }
    HANDLE frameEvent = static_cast<HANDLE>(ipc.GetFrameEvent());
    g_wakeEvent.store(frameEvent, std::memory_order_release);
    std::printf("[nah2] Serveur IPC actif sur 127.0.0.1:%u (WebSocket /ws, HTTP POST /telemetry, UDP)\n",
                ipc.GetBoundPort());

    nah2::OverlayRenderer renderer(cfg);
    if (!renderer.Initialize(GetModuleHandleW(nullptr))) {
        std::fprintf(stderr, "[nah2] Echec initialisation fenetre / pipeline Direct3D 11 + Direct2D.\n");
        g_wakeEvent.store(nullptr, std::memory_order_release);
        ipc.Stop();
        timeEndPeriod(1);
        return 1;
    }

    nah2::aim::AimAssistEngine aimEngine(aimCfg);
    aimEngine.Reseed(static_cast<uint32_t>(nah2::IpcReceiver::NowMicroseconds() ^ GetCurrentProcessId()));

    bool uiConfigDirty = false;
    nah2::ControlPanel panel;
    if (panel.Initialize(
            GetModuleHandleW(nullptr),
            &renderer.Config(),
            &aimEngine.Config(),
            [&uiConfigDirty]() { uiConfigDirty = true; },
            [&aimEngine, &uiConfigDirty](nah2::aim::AimProfile p) {
                nah2::aim::ApplyProfileDefaults(aimEngine.Config(), p, false);
                uiConfigDirty = true;
            },
            []() {
                g_quitRequested.store(true, std::memory_order_release);
                if (HANDLE ev = g_wakeEvent.load(std::memory_order_acquire)) {
                    SetEvent(ev);
                }
            })) {
        if (!startPanelVisible) {
            panel.SetVisible(false);
        }
    }

    // Ecoute passive des deltas souris materiels (WM_INPUT hDevice != nullptr) pour synergie humaine
    renderer.SetRawMouseCallback([&aimEngine](int dx, int dy) {
        const double tSec = static_cast<double>(nah2::IpcReceiver::NowMicroseconds()) / 1000000.0;
        aimEngine.NotifyHardwareMouseDelta(dx, dy, tSec);
    });
    RAWINPUTDEVICE rawMouse{};
    rawMouse.usUsagePage = 0x01;
    rawMouse.usUsage = 0x02;
    rawMouse.dwFlags = RIDEV_INPUTSINK;
    rawMouse.hwndTarget = renderer.GetHwnd();
    const bool rawInputOk = (RegisterRawInputDevices(&rawMouse, 1, sizeof(rawMouse)) == TRUE);

    std::printf("[nah2] Overlay Direct3D 11 / DirectComposition pret (cible: \"%s\").\n",
                cfg.target_filter.c_str());
    std::printf("[nah2] ESP: %s | Aim Assist: %s (profil=%s, puissance=%.0f%%, zone=%s, touche=%s, FOV=%.0fpx, rawInput=%s)\n",
                cfg.esp_enabled ? "ON" : "OFF",
                aimCfg.enabled ? "ON" : "OFF",
                nah2::aim::ProfileName(aimCfg.profile),
                aimCfg.strength * 100.0f,
                nah2::aim::BoneModeName(aimCfg.bone_mode),
                nah2::aim::KeyModeName(aimCfg.key_mode),
                aimCfg.fov_px,
                rawInputOk ? "OK" : "OFF");
    std::printf("[nah2] Raccourcis: F5=ESP, F6=Aim, F7=Profil, F8=Overlay, F9=Squelette, F10=HUD, INS=Panneau, END=Quitter\n");

    nah2::TelemetryFrame latestFrame;
    latestFrame.entities.reserve(64);
    std::vector<nah2::EntityTelemetry> smoothedEntities;
    smoothedEntities.reserve(64);

    nah2::math::SceneSmoother smoother;
    smoother.SetEntitySpeed(entitySmooth);
    smoother.SetCameraSpeed(cameraSmooth);
    const bool zeroLatencyMode = (entitySmooth <= 0.0f && cameraSmooth <= 0.0f);

    KeyEdgeTracker keys;
    uint64_t prevUs = nah2::IpcReceiver::NowMicroseconds();
    double lastPacketSec = -1.0;
    double lastSyncSec = -1.0;
    double lastRenderSec = -1.0;
    double lastPanelStatusSec = -1.0;
    bool visibleLastTick = false;
    RECT lastSyncedRect{};

    MSG msg{};
    while (!g_quitRequested.load(std::memory_order_acquire)) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                g_quitRequested.store(true, std::memory_order_release);
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (g_quitRequested.load(std::memory_order_acquire)) {
            break;
        }

        if (keys.PressedOnce(VK_END)) {
            break;
        }
        bool hotkeyChanged = false;
        if (uiConfigDirty) {
            uiConfigDirty = false;
            aimEngine.SetConfig(aimEngine.Config());
            hotkeyChanged = true;
        }
        if (keys.PressedOnce(VK_F5)) {
            renderer.Config().esp_enabled = !renderer.Config().esp_enabled;
            hotkeyChanged = true;
        }
        if (keys.PressedOnce(VK_F6)) {
            aimEngine.Config().enabled = !aimEngine.Config().enabled;
            aimEngine.SetConfig(aimEngine.Config());
            hotkeyChanged = true;
        }
        if (keys.PressedOnce(VK_F7)) {
            aimEngine.CycleProfile();
            hotkeyChanged = true;
        }
        if (keys.PressedOnce(VK_F8)) {
            renderer.Config().overlay_enabled = !renderer.Config().overlay_enabled;
            hotkeyChanged = true;
        }
        if (keys.PressedOnce(VK_F9)) {
            renderer.Config().show_skeleton = !renderer.Config().show_skeleton;
            hotkeyChanged = true;
        }
        if (keys.PressedOnce(VK_F10)) {
            renderer.Config().show_hud = !renderer.Config().show_hud;
            hotkeyChanged = true;
        }
        if (keys.PressedOnce(VK_INSERT)) {
            panel.ToggleVisible();
        }
        if (hotkeyChanged) {
            panel.NotifyExternalChange();
        }

        const uint64_t nowUs = nah2::IpcReceiver::NowMicroseconds();
        const double nowSec = static_cast<double>(nowUs) / 1000000.0;
        const float dt = std::clamp(
            static_cast<float>(static_cast<double>(nowUs - prevUs) / 1000000.0),
            0.0f, 0.1f);
        prevUs = nowUs;

        const bool hasNewFrame = ipc.ConsumeLatestFrame(latestFrame);
        if (hasNewFrame) {
            lastPacketSec = nowSec;
            aimEngine.ObserveTelemetry(latestFrame, nowSec);
        } else if (zeroLatencyMode && lastPacketSec >= 0.0 && (nowSec - lastPacketSec) > 0.35 && !latestFrame.entities.empty()) {
            latestFrame.entities.clear();
            hotkeyChanged = true;
        }

        const bool syncDue = (lastSyncSec < 0.0) || ((nowSec - lastSyncSec) >= (visibleLastTick ? 0.033 : 0.060));
        bool isVisible = visibleLastTick;
        bool syncChanged = false;
        if (hasNewFrame || hotkeyChanged || !zeroLatencyMode || syncDue) {
            if (zeroLatencyMode) {
                isVisible = renderer.SyncWithTargetWindow(latestFrame.camera);
                lastSyncSec = nowSec;

                const RECT curRect = renderer.Diagnostics().overlay_rect;
                syncChanged = (isVisible != visibleLastTick) || !EqualRect(&curRect, &lastSyncedRect);
                lastSyncedRect = curRect;
                visibleLastTick = isVisible;
            }
        }

        // Sous-echantillonnage haute frequence (jusqu'a ~500 Hz) pour fluidite souris humaine
        const bool cursorHidden = IsSystemCursorHidden();
        const bool aimKeyDown = IsAimKeyPressed(aimEngine.Config(), latestFrame.camera, cursorHidden);
        const nah2::aim::AimStepResult aimStep = aimEngine.Step(
            dt,
            nowSec,
            renderer.Diagnostics().target_foreground,
            aimKeyDown,
            cursorHidden,
            renderer.OverlayWidth(),
            renderer.OverlayHeight());
        SendRelativeMouseDelta(aimStep.dx, aimStep.dy);

        const nah2::aim::AimDebugState& aimDebug = aimEngine.DebugState();

        if (panel.IsVisible() && (lastPanelStatusSec < 0.0 || (nowSec - lastPanelStatusSec) >= 0.12)) {
            nah2::PanelStatus pStatus{};
            pStatus.ipc = ipc.GetStats();
            pStatus.target_found = renderer.Diagnostics().target_found;
            pStatus.target_foreground = renderer.Diagnostics().target_foreground;
            pStatus.px_per_count_x = aimDebug.px_per_count_x;
            pStatus.px_per_count_y = aimDebug.px_per_count_y;
            pStatus.aim_engaging = aimDebug.actively_engaging;
            panel.UpdateStatus(pStatus);
            lastPanelStatusSec = nowSec;
        }

        if (zeroLatencyMode) {
            const bool needRender = isVisible && (
                hasNewFrame ||
                hotkeyChanged ||
                syncChanged ||
                (lastRenderSec < 0.0) ||
                ((nowSec - lastRenderSec) >= (aimStep.has_target ? 0.016 : 0.200)));
            if (needRender) {
                const nah2::IpcStats stats = ipc.GetStats();
                renderer.RenderFrame(latestFrame.camera, latestFrame.entities, stats, nowSec, &aimDebug);
                lastRenderSec = nowSec;
            }
        } else if (hasNewFrame || hotkeyChanged || syncDue || (nowSec - lastRenderSec) >= 0.008) {
            const nah2::CameraState cam = smoother.SmoothCamera(latestFrame.camera, dt);
            smoother.UpdateAndGetEntities(latestFrame.entities, dt, nowSec, hasNewFrame, smoothedEntities);
            if (renderer.SyncWithTargetWindow(cam)) {
                const nah2::IpcStats stats = ipc.GetStats();
                renderer.RenderFrame(cam, smoothedEntities, stats, nowSec, &aimDebug);
                visibleLastTick = true;
            } else {
                visibleLastTick = false;
            }
            lastSyncSec = nowSec;
            lastRenderSec = nowSec;
        }

        const DWORD waitTimeoutMs = aimEngine.WantsHighRateLoop()
            ? 2
            : (!zeroLatencyMode ? 4 : (visibleLastTick ? 33 : 60));
        if (frameEvent) {
            MsgWaitForMultipleObjectsEx(1, &frameEvent, waitTimeoutMs, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        } else {
            Sleep(waitTimeoutMs);
        }
    }

    if (rawInputOk) {
        rawMouse.dwFlags = RIDEV_REMOVE;
        rawMouse.hwndTarget = nullptr;
        RegisterRawInputDevices(&rawMouse, 1, sizeof(rawMouse));
    }
    g_wakeEvent.store(nullptr, std::memory_order_release);
    panel.Shutdown();
    renderer.Shutdown();
    ipc.Stop();
    timeEndPeriod(1);
    std::printf("[nah2] Arret propre termine.\n");
    return 0;
}
