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

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

bool Near(float a, float b, float eps = 1e-2f) {
    return std::abs(a - b) <= eps;
}

void TestWorldToScreenAndBehindCamera() {
    nah2::CameraState cam;
    cam.pos = { 0.0f, 10.0f, 0.0f };
    cam.right = { 1.0f, 0.0f, 0.0f };
    cam.up = { 0.0f, 1.0f, 0.0f };
    cam.look = { 0.0f, 0.0f, -1.0f };
    cam.fov_deg = 90.0f; // tan(45 deg) = 1.0 -> calcul exact trivial
    cam.viewport_w = 1920.0f;
    cam.viewport_h = 1080.0f;

    nah2::Vec2 pt;
    float w = 0.0f;

    // 1. Point droit devant a 10 studs -> centre de l'ecran (960, 540)
    assert(nah2::math::WorldToScreen({ 0.0f, 10.0f, -10.0f }, cam, 1920.0f, 1080.0f, pt, &w));
    assert(Near(w, 10.0f));
    assert(Near(pt.x, 960.0f));
    assert(Near(pt.y, 540.0f));

    // 2. Point sur le bord superieur du frustum (dy = +10 a w = 10 avec FOV 90) -> y = 0
    assert(nah2::math::WorldToScreen({ 0.0f, 20.0f, -10.0f }, cam, 1920.0f, 1080.0f, pt));
    assert(Near(pt.x, 960.0f));
    assert(Near(pt.y, 0.0f));

    // 3. Point a droite (dx = +5.4 a w = 10 -> halfHeight=540 -> 960 + 291.6)
    assert(nah2::math::WorldToScreen({ 5.0f, 10.0f, -10.0f }, cam, 1920.0f, 1080.0f, pt));
    assert(Near(pt.x, 960.0f + 270.0f));
    assert(Near(pt.y, 540.0f));

    // 4. Point DERRIERE la camera (z = +5 -> W = -5 <= 0) -> rejete
    assert(!nah2::math::WorldToScreen({ 2.0f, 10.0f, 5.0f }, cam, 1920.0f, 1080.0f, pt, &w));
    assert(w < 0.0f);

    // 5. Point sur le plan focal (W = 0) -> rejete
    assert(!nah2::math::WorldToScreen({ 3.0f, 12.0f, 0.0f }, cam, 1920.0f, 1080.0f, pt));

    std::printf("[PASS] TestWorldToScreenAndBehindCamera\n");
}

void TestSegment3DNearPlaneClipping() {
    nah2::CameraState cam;
    cam.pos = { 0.0f, 0.0f, 0.0f };
    cam.right = { 1.0f, 0.0f, 0.0f };
    cam.up = { 0.0f, 1.0f, 0.0f };
    cam.look = { 0.0f, 0.0f, -1.0f };
    cam.fov_deg = 90.0f;
    cam.viewport_w = 1000.0f;
    cam.viewport_h = 1000.0f;

    // Segment traversant la camera de z=+10 (derriere) a z=-10 (devant) a x=1
    nah2::Vec2 a, b;
    assert(nah2::math::ProjectSegment3D(
        { 1.0f, 0.0f, 10.0f },
        { 1.0f, 0.0f, -10.0f },
        cam, 1000.0f, 1000.0f, a, b));
    assert(std::isfinite(a.x) && std::isfinite(a.y));
    assert(std::isfinite(b.x) && std::isfinite(b.y));
    // b est a w=10, x=1 -> 500 + (1/10)*500 = 550
    assert(Near(b.x, 550.0f));
    assert(a.x > 1000.0f); // Clippe a w=0.05 (cote droit, jamais inverse a gauche)

    // Segment entierement derriere -> rejete
    assert(!nah2::math::ProjectSegment3D(
        { 0.0f, 0.0f, 5.0f },
        { 1.0f, 1.0f, 10.0f },
        cam, 1000.0f, 1000.0f, a, b));

    std::printf("[PASS] TestSegment3DNearPlaneClipping\n");
}

void TestDeltaTimeLerpAndSmoother() {
    // Verifie l'invariance au framerate de alpha = 1 - exp(-speed * dt)
    const float speed = 20.0f;
    float vOneStep = 0.0f;
    vOneStep = nah2::math::Lerp(vOneStep, 100.0f, nah2::math::ComputeExpLerpAlpha(speed, 0.016f));

    float vTwoSteps = 0.0f;
    const float halfAlpha = nah2::math::ComputeExpLerpAlpha(speed, 0.008f);
    vTwoSteps = nah2::math::Lerp(vTwoSteps, 100.0f, halfAlpha);
    vTwoSteps = nah2::math::Lerp(vTwoSteps, 100.0f, halfAlpha);
    assert(Near(vOneStep, vTwoSteps, 1e-3f));

    // Verifie SceneSmoother sur l'entite et l'eviction apres disparition
    nah2::math::SceneSmoother smoother;
    smoother.SetEntitySpeed(10.0f);

    nah2::EntityTelemetry e1;
    e1.id = 42;
    e1.name = "Dummy";
    e1.pos = { 0.0f, 0.0f, -10.0f };
    std::vector<nah2::EntityTelemetry> frame1 = { e1 };
    std::vector<nah2::EntityTelemetry> out;

    smoother.UpdateAndGetEntities(frame1, 0.016f, 1.0, true, out);
    assert(out.size() == 1);
    assert(Near(out[0].pos.x, 0.0f));

    e1.pos.x = 10.0f;
    frame1[0] = e1;
    smoother.UpdateAndGetEntities(frame1, 0.05f, 1.05, true, out);
    assert(out.size() == 1);
    assert(out[0].pos.x > 3.0f && out[0].pos.x < 5.0f);

    // Deux frames sans l'entite -> eviction propre
    std::vector<nah2::EntityTelemetry> empty;
    smoother.UpdateAndGetEntities(empty, 0.016f, 1.066, true, out);
    smoother.UpdateAndGetEntities(empty, 0.016f, 1.082, true, out);
    assert(out.empty());

    // Mode zero latence (entity_speed = 0.0f) : suivi instantane + expiration stale
    smoother.Reset();
    smoother.SetEntitySpeed(0.0f);
    e1.pos.x = 77.5f;
    frame1[0] = e1;
    smoother.UpdateAndGetEntities(frame1, 0.016f, 2.0, true, out);
    assert(out.size() == 1);
    assert(Near(out[0].pos.x, 77.5f));
    smoother.UpdateAndGetEntities(empty, 0.016f, 2.5, false, out);
    assert(out.empty());

    std::printf("[PASS] TestDeltaTimeLerpAndSmoother\n");
}

void TestJsonParser() {
    const std::string sample = R"({
        "seq": 77,
        "ts": 12.5,
        "extra_ignored": {"nested": [1, true, null, "ok"]},
        "cam": {
            "p": [1.5, 10.0, 25.0],
            "r": [1, 0, 0],
            "u": [0, 1, 0],
            "l": [0, 0, -1],
            "fov": 70,
            "vp": [1280, 720]
        },
        "ents": [
            {
                "id": 7,
                "n": "R15_Bot",
                "k": 1,
                "rig": 15,
                "p": [0, 5, -15],
                "s": [4, 5.5, 2],
                "r": [1, 0, 0],
                "u": [0, 1, 0],
                "l": [0, 0, -1],
                "j": [0, 0, 7.2, 0, 1, 0, 6.0, 0]
            }
        ]
    })";

    nah2::TelemetryFrame f;
    assert(nah2::ParseTelemetryJson(sample, f));
    assert(f.seq == 77);
    assert(Near(f.camera.pos.x, 1.5f));
    assert(Near(f.camera.fov_deg, 70.0f));
    assert(Near(f.camera.viewport_w, 1280.0f));
    assert(f.entities.size() == 1);
    assert(f.entities[0].id == 7);
    assert(f.entities[0].name == "R15_Bot");
    assert(f.entities[0].rig_type == 15);
    assert((f.entities[0].joint_mask & 0x3u) == 0x3u);
    assert(Near(f.entities[0].joints[0].y, 7.2f));
    assert(Near(f.entities[0].joints[1].y, 6.0f));

    // JSON invalide ou sans camera -> false
    assert(!nah2::ParseTelemetryJson("{\"seq\":1}", f));
    assert(!nah2::ParseTelemetryJson("{bad", f));

    std::printf("[PASS] TestJsonParser\n");
}

std::string RecvUntil(SOCKET s, std::string_view marker) {
    std::string acc;
    char buf[1024];
    for (int iter = 0; iter < 50; ++iter) {
        if (acc.find(marker) != std::string::npos) break;
        const int n = ::recv(s, buf, sizeof(buf), 0);
        if (n > 0) {
            acc.append(buf, static_cast<std::size_t>(n));
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    return acc;
}

void TestIpcLoopbackHttpWsUdp() {
    nah2::IpcReceiver ipc(0); // Port ephemere libre
    assert(ipc.Start());
    const uint16_t port = ipc.GetBoundPort();
    assert(port != 0);

    WSADATA wsa{};
    assert(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);

    // 1. Test HTTP/1.1 Keep-Alive POST /telemetry
    {
        SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        assert(s != INVALID_SOCKET);
        assert(::connect(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);

        const std::string body = R"({"seq":101,"cam":{"p":[0,5,0],"r":[1,0,0],"u":[0,1,0],"l":[0,0,-1],"fov":70,"vp":[800,600]},"ents":[]})";
        const std::string req =
            "POST /telemetry HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\nConnection: keep-alive\r\n\r\n" + body;
        assert(::send(s, req.data(), static_cast<int>(req.size()), 0) == static_cast<int>(req.size()));

        const std::string resp = RecvUntil(s, "\r\n\r\n");
        if (resp.find("204 No Content") == std::string::npos) {
            std::fprintf(stderr, "HTTP resp unexpected (%zu bytes): [%s]\n", resp.size(), resp.c_str());
        }
        assert(resp.find("204 No Content") != std::string::npos);

        nah2::TelemetryFrame frame;
        bool consumed = false;
        for (int i = 0; i < 40; ++i) {
            if (ipc.ConsumeLatestFrame(frame)) {
                consumed = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        assert(consumed);
        assert(frame.seq == 101);
        assert(std::string(ipc.GetStats().transport) == "HTTP");

        // 1b. Test HTTP POST avec Expect: 100-continue (ex: clients libcurl > 1KB)
        const std::string body2 = R"({"seq":102,"cam":{"p":[1,2,3],"r":[1,0,0],"u":[0,1,0],"l":[0,0,-1],"fov":75,"vp":[1280,720]},"ents":[]})";
        const std::string headers2 =
            "POST /telemetry HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nExpect: 100-continue\r\nContent-Length: " +
            std::to_string(body2.size()) + "\r\nConnection: close\r\n\r\n";
        assert(::send(s, headers2.data(), static_cast<int>(headers2.size()), 0) == static_cast<int>(headers2.size()));
        const std::string contResp = RecvUntil(s, "100 Continue\r\n\r\n");
        assert(contResp.find("100 Continue") != std::string::npos);

        assert(::send(s, body2.data(), static_cast<int>(body2.size()), 0) == static_cast<int>(body2.size()));
        const std::string resp2 = RecvUntil(s, "204 No Content\r\n");
        assert(resp2.find("204 No Content") != std::string::npos);

        consumed = false;
        for (int i = 0; i < 40; ++i) {
            if (ipc.ConsumeLatestFrame(frame)) {
                consumed = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        assert(consumed);
        assert(frame.seq == 102);
        closesocket(s);
    }

    // 2. Test WebSocket RFC 6455 (handshake + trame masquee)
    {
        SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        assert(s != INVALID_SOCKET);
        assert(::connect(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);

        const char* handshake =
            "GET /ws HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n";
        assert(::send(s, handshake, static_cast<int>(std::strlen(handshake)), 0) > 0);

        const std::string resp = RecvUntil(s, "\r\n\r\n");
        assert(resp.find("101 Switching Protocols") != std::string::npos);
        assert(resp.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos);

        // Envoi trame WebSocket texte masquee (RFC 6455)
        const std::string payload = R"({"seq":202,"cam":{"p":[3,4,5],"r":[1,0,0],"u":[0,1,0],"l":[0,0,-1],"fov":65,"vp":[1280,720]},"ents":[]})";
        std::vector<uint8_t> wsFrame;
        wsFrame.push_back(0x81); // FIN + Text
        assert(payload.size() <= 125);
        wsFrame.push_back(static_cast<uint8_t>(0x80u | payload.size())); // Masked
        const uint8_t mask[4] = { 0x37, 0xFA, 0x21, 0x3D };
        wsFrame.insert(wsFrame.end(), mask, mask + 4);
        for (std::size_t i = 0; i < payload.size(); ++i) {
            wsFrame.push_back(static_cast<uint8_t>(payload[i]) ^ mask[i & 3]);
        }
        assert(::send(s, reinterpret_cast<const char*>(wsFrame.data()), static_cast<int>(wsFrame.size()), 0) ==
               static_cast<int>(wsFrame.size()));

        nah2::TelemetryFrame frame;
        bool consumed = false;
        for (int i = 0; i < 40; ++i) {
            if (ipc.ConsumeLatestFrame(frame)) {
                consumed = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        assert(consumed);
        assert(frame.seq == 202);
        assert(Near(frame.camera.fov_deg, 65.0f));
        assert(std::string(ipc.GetStats().transport) == "WS");

        // Envoi trame WebSocket binaire (opcode 0x82) contenant du JSON
        const std::string binPayload = R"({"seq":203,"cam":{"p":[4,5,6],"r":[1,0,0],"u":[0,1,0],"l":[0,0,-1],"fov":68,"vp":[1280,720]},"ents":[]})";
        std::vector<uint8_t> wsBinFrame;
        wsBinFrame.push_back(0x82); // FIN + Binary
        assert(binPayload.size() <= 125);
        wsBinFrame.push_back(static_cast<uint8_t>(0x80u | binPayload.size()));
        wsBinFrame.insert(wsBinFrame.end(), mask, mask + 4);
        for (std::size_t i = 0; i < binPayload.size(); ++i) {
            wsBinFrame.push_back(static_cast<uint8_t>(binPayload[i]) ^ mask[i & 3]);
        }
        assert(::send(s, reinterpret_cast<const char*>(wsBinFrame.data()), static_cast<int>(wsBinFrame.size()), 0) ==
               static_cast<int>(wsBinFrame.size()));

        HANDLE frameEvent = static_cast<HANDLE>(ipc.GetFrameEvent());
        assert(frameEvent != nullptr);
        assert(WaitForSingleObject(frameEvent, 500) == WAIT_OBJECT_0);
        assert(ipc.ConsumeLatestFrame(frame));
        assert(frame.seq == 203);
        closesocket(s);
    }

    // 3. Test UDP JSON
    {
        SOCKET u = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        assert(u != INVALID_SOCKET);
        const std::string payload = R"({"seq":303,"cam":{"p":[9,8,7],"r":[1,0,0],"u":[0,1,0],"l":[0,0,-1],"fov":80,"vp":[1920,1080]},"ents":[]})";
        assert(::sendto(u, payload.data(), static_cast<int>(payload.size()), 0,
                        reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == static_cast<int>(payload.size()));

        nah2::TelemetryFrame frame;
        bool consumed = false;
        for (int i = 0; i < 40; ++i) {
            if (ipc.ConsumeLatestFrame(frame)) {
                consumed = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        assert(consumed);
        assert(frame.seq == 303);
        assert(std::string(ipc.GetStats().transport) == "UDP");
        closesocket(u);
    }

    WSACleanup();
    ipc.Stop();
    std::printf("[PASS] TestIpcLoopbackHttpWsUdp\n");
}

void TestRendererPipelineSmoke() {
    nah2::OverlayConfig cfg;
    cfg.always_visible = true;
    nah2::OverlayRenderer renderer(cfg);
    assert(renderer.Initialize(GetModuleHandleW(nullptr)));

    nah2::CameraState cam;
    cam.pos = { 0.0f, 5.0f, 15.0f };
    cam.viewport_w = 1024.0f;
    cam.viewport_h = 576.0f;

    nah2::EntityTelemetry ent;
    ent.id = 1;
    ent.kind = 1;
    ent.rig_type = 15;
    ent.name = "SmokeRig";
    ent.pos = { 0.0f, 5.0f, 0.0f };
    ent.size = { 4.0f, 5.5f, 2.0f };
    ent.joint_mask = 0x07u;
    ent.joints[0] = { 0.0f, 7.2f, 0.0f };
    ent.joints[1] = { 0.0f, 5.8f, 0.0f };
    ent.joints[2] = { 0.0f, 4.4f, 0.0f };

    nah2::IpcStats stats{};
    std::snprintf(stats.transport, sizeof(stats.transport), "WS");
    stats.packets_per_sec = 60.0;

    nah2::aim::AimDebugState aimDbg;
    aimDbg.enabled = true;
    aimDbg.actively_engaging = true;
    aimDbg.has_target = true;
    aimDbg.aim_origin = { 512.0f, 288.0f };
    aimDbg.target_screen_pt = { 520.0f, 260.0f };
    aimDbg.fov_px = 140.0f;

    renderer.RenderFrame(cam, { ent }, stats, 1.0, &aimDbg);
    assert(renderer.Diagnostics().visible_entities == 1);

    renderer.Config().esp_enabled = false;
    renderer.RenderFrame(cam, { ent }, stats, 1.1, &aimDbg);
    assert(renderer.Diagnostics().visible_entities == 0);

    nah2::aim::AimConfig aimCfg;
    nah2::ControlPanel panel;
    assert(panel.Initialize(
        GetModuleHandleW(nullptr),
        &renderer.Config(),
        &aimCfg,
        []() {},
        [](nah2::aim::AimProfile) {},
        []() {}));
    panel.SetVisible(false);
    assert(!panel.IsVisible());
    panel.Shutdown();

    renderer.Shutdown();
    std::printf("[PASS] TestRendererPipelineSmoke\n");
}

void TestExtendedTelemetryJson() {
    const std::string payload = R"({
        "seq": 99,
        "ts": 42.1,
        "cam": {
            "p": [0, 5, 0],
            "r": [1, 0, 0],
            "u": [0, 1, 0],
            "l": [0, 0, -1],
            "fov": 70,
            "vp": [1920, 1080],
            "mb": 1,
            "ui": 1,
            "ml": [960, 540],
            "sens": 0.45
        },
        "ents": [
            {
                "id": 10,
                "n": "Enemy",
                "k": 1,
                "rig": 15,
                "p": [2, 5, -20],
                "s": [4, 5.5, 2],
                "r": [1, 0, 0],
                "u": [0, 1, 0],
                "l": [0, 0, -1],
                "hp": 85.5,
                "mhp": 100,
                "ally": 0,
                "vis": 1,
                "v": [14.5, 0, -2.0],
                "j": [0, 2, 7.2, -20]
            }
        ]
    })";

    nah2::TelemetryFrame f;
    assert(nah2::ParseTelemetryJson(payload, f));
    assert(f.camera.mouse_behavior == 1);
    assert(f.camera.typing_in_ui == true);
    assert(f.camera.has_mouse_loc == true);
    assert(Near(f.camera.mouse_loc.x, 960.0f));
    assert(Near(f.camera.game_mouse_sens, 0.45f));
    assert(f.entities.size() == 1);
    assert(Near(f.entities[0].health, 85.5f));
    assert(Near(f.entities[0].max_health, 100.0f));
    assert(f.entities[0].is_ally == false);
    assert(f.entities[0].visible == 1);
    assert(f.entities[0].has_vel == true);
    assert(Near(f.entities[0].vel.x, 14.5f));
    std::printf("[PASS] TestExtendedTelemetryJson\n");
}

nah2::CameraState RotateCameraByCounts(const nah2::CameraState& cam, int dx, int dy, float radPerCount) {
    nah2::CameraState out = cam;
    const float yaw = static_cast<float>(dx) * radPerCount;
    const float pitch = -static_cast<float>(dy) * radPerCount;

    // Rotation yaw autour de world Up (0,1,0) et pitch autour de cam.right
    const float cy = std::cos(yaw);
    const float sy = std::sin(yaw);
    auto rotY = [cy, sy](const nah2::Vec3& v) -> nah2::Vec3 {
        return { v.x * cy - v.z * sy, v.y, v.x * sy + v.z * cy };
    };
    out.look = rotY(out.look);
    out.right = rotY(out.right);
    out.up = rotY(out.up);

    const float cp = std::cos(pitch);
    const float sp = std::sin(pitch);
    out.look = nah2::math::Add(nah2::math::Scale(out.look, cp), nah2::math::Scale(out.up, sp));
    nah2::math::OrthonormalizeBasis(out.right, out.up, out.look);
    return out;
}

void TestAimTargetFilteringAndSwitchDelay() {
    nah2::aim::AimConfig cfg;
    nah2::aim::ApplyProfileDefaults(cfg, nah2::aim::AimProfile::UltraLegit);
    cfg.fov_px = 300.0f;
    cfg.team_check = true;
    cfg.visibility_check = true;

    nah2::aim::AimAssistEngine engine(cfg);
    engine.Reseed(1337u);

    nah2::TelemetryFrame frame;
    frame.seq = 1;
    frame.camera.pos = { 0.0f, 5.0f, 0.0f };
    frame.camera.fov_deg = 70.0f;
    frame.camera.viewport_w = 1920.0f;
    frame.camera.viewport_h = 1080.0f;
    frame.camera.mouse_behavior = 1;

    // 1. Allie proche du viseur, mort proche, occulte proche, ennemi valide un peu decale
    nah2::EntityTelemetry ally;
    ally.id = 1; ally.kind = 1; ally.is_ally = true; ally.health = 100.0f; ally.visible = 1;
    ally.pos = { 0.2f, 5.0f, -30.0f }; ally.size = { 4, 5.5f, 2 };

    nah2::EntityTelemetry dead;
    dead.id = 2; dead.kind = 1; dead.is_ally = false; dead.health = 0.0f; dead.visible = 1;
    dead.pos = { -0.2f, 5.0f, -30.0f }; dead.size = { 4, 5.5f, 2 };

    nah2::EntityTelemetry occluded;
    occluded.id = 3; occluded.kind = 1; occluded.is_ally = false; occluded.health = 100.0f; occluded.visible = 0;
    occluded.pos = { 0.5f, 5.0f, -30.0f }; occluded.size = { 4, 5.5f, 2 };

    nah2::EntityTelemetry valid;
    valid.id = 4; valid.kind = 1; valid.is_ally = false; valid.health = 100.0f; valid.visible = 1;
    valid.pos = { 3.5f, 5.0f, -30.0f }; valid.size = { 4, 5.5f, 2 };
    valid.joint_mask = 0x3u;
    valid.joints[0] = { 3.5f, 7.0f, -30.0f };
    valid.joints[1] = { 3.5f, 5.5f, -30.0f };

    frame.entities = { ally, dead, occluded, valid };

    double t = 1.0;
    engine.ObserveTelemetry(frame, t);

    // UI ouverte -> aucun mouvement
    frame.camera.typing_in_ui = true;
    engine.ObserveTelemetry(frame, t);
    auto stBlocked = engine.Step(0.004f, t, true, true, true, 1920.0f, 1080.0f);
    assert(stBlocked.dx == 0 && stBlocked.dy == 0 && !stBlocked.has_target);

    // En jeu -> cible uniquement l'ennemi #4 (ignore allie #1, mort #2, mur #3)
    frame.camera.typing_in_ui = false;
    frame.seq = 2;
    t += 0.016;
    engine.ObserveTelemetry(frame, t);
    bool locked4 = false;
    for (int i = 0; i < 15; ++i) {
        t += 0.004;
        auto s = engine.Step(0.004f, t, true, true, true, 1920.0f, 1080.0f);
        if (s.has_target) {
            assert(s.target_id == 4);
            locked4 = true;
        }
    }
    assert(locked4);

    // #4 meurt et #3 devient visible -> delai reaction humain avant de tirer vers #3
    frame.entities[2].visible = 1;
    frame.entities[3].health = 0.0f;
    frame.seq = 3;
    t += 0.004;
    engine.ObserveTelemetry(frame, t);
    auto sw = engine.Step(0.004f, t, true, true, true, 1920.0f, 1080.0f);
    assert(sw.has_target && sw.target_id == 3);
    assert(sw.dx == 0 && sw.dy == 0); // Pause reaction naturelle au switch

    std::printf("[PASS] TestAimTargetFilteringAndSwitchDelay\n");
}

void TestAimHumanConvergenceAndOverride() {
    nah2::aim::AimConfig cfg;
    nah2::aim::ApplyProfileDefaults(cfg, nah2::aim::AimProfile::Competitive);
    cfg.fov_px = 450.0f;
    cfg.bone_mode = nah2::aim::AimBoneMode::Head;

    nah2::aim::AimAssistEngine engine(cfg);
    engine.Reseed(424242u);

    nah2::CameraState cam;
    cam.pos = { 0.0f, 5.0f, 0.0f };
    cam.fov_deg = 70.0f;
    cam.viewport_w = 1920.0f;
    cam.viewport_h = 1080.0f;
    cam.mouse_behavior = 1;

    const float trueRadPerCount = 0.0028f; // ~2.16 px/count a 1080p FOV 70
    nah2::Vec3 enemyPos = { 8.0f, 7.2f, -40.0f }; // Decale droite + haut (trajectoire diagonale)
    const nah2::Vec3 enemyVel = { 8.0f, 0.0f, 0.0f }; // Ennemi en strafe lateral

    double t = 1.0;
    const float simDt = 0.002f; // 500 Hz sous-echantillonnage souris
    int maxStepDelta = 0;
    int firstMoveDelta = 0;
    bool observedCurvature = false;
    float initialRatio = 0.0f;

    for (int step = 0; step < 175; ++step) { // 350ms simulation boucle fermee
        t += simDt;
        enemyPos = nah2::math::Add(enemyPos, nah2::math::Scale(enemyVel, simDt));

        if (step % 4 == 0) { // Telemetrie Roblox ~125 Hz
            nah2::TelemetryFrame f;
            f.seq = static_cast<uint64_t>(step / 4 + 1);
            f.camera = cam;
            nah2::EntityTelemetry e;
            e.id = 100;
            e.kind = 1;
            e.health = 100.0f;
            e.visible = 1;
            e.pos = { enemyPos.x, enemyPos.y - 2.0f, enemyPos.z };
            e.size = { 4.0f, 5.5f, 2.0f };
            e.has_vel = true;
            e.vel = enemyVel;
            e.joint_mask = 0x3u;
            e.joints[0] = enemyPos;
            e.joints[1] = { enemyPos.x, enemyPos.y - 1.4f, enemyPos.z };
            f.entities.push_back(e);
            engine.ObserveTelemetry(f, t);
        }

        const auto res = engine.Step(simDt, t, true, true, true, 1920.0f, 1080.0f);
        if (res.dx != 0 || res.dy != 0) {
            if (firstMoveDelta == 0) {
                firstMoveDelta = std::abs(res.dx) + std::abs(res.dy);
                initialRatio = (res.dx != 0) ? static_cast<float>(res.dy) / static_cast<float>(res.dx) : 0.0f;
            } else if (res.dx != 0) {
                const float curRatio = static_cast<float>(res.dy) / static_cast<float>(res.dx);
                if (std::abs(curRatio - initialRatio) > 0.05f) {
                    observedCurvature = true;
                }
            }
            maxStepDelta = std::max(maxStepDelta, std::max(std::abs(res.dx), std::abs(res.dy)));
            cam = RotateCameraByCounts(cam, res.dx, res.dy, trueRadPerCount);
        }
    }

    // Verifie que la camera a converge en douceur sur la tete de la cible mobile
    nah2::Vec2 finalScreen{};
    assert(nah2::math::WorldToScreen(enemyPos, cam, 1920.0f, 1080.0f, finalScreen));
    const float finalErrPx = std::hypot(finalScreen.x - 960.0f, finalScreen.y - 540.0f);
    assert(finalErrPx < 16.0f);
    assert(firstMoveDelta > 0 && firstMoveDelta <= 8); // Demarrage progressif sans snap brutal
    assert(maxStepDelta <= 25); // Aucun saut robotique
    assert(observedCurvature); // Trajectoire humaine courbee non-colineaire

    // Verifie la calibration en ligne (convergence vers ~2.16 px/count)
    assert(engine.EstimatedPxPerCountX() > 1.2f && engine.EstimatedPxPerCountX() < 3.5f);

    // Verifie le breakaway manuel : si le joueur tire fort a gauche, l'assist cede la main
    for (int k = 0; k < 15; ++k) {
        t += 0.004;
        engine.NotifyHardwareMouseDelta(-18, 0, t);
        engine.Step(0.004f, t, true, true, true, 1920.0f, 1080.0f);
    }
    const auto afterOverride = engine.Step(0.004f, t + 0.004, true, true, true, 1920.0f, 1080.0f);
    assert(std::abs(afterOverride.dx) <= 2);

    std::printf("[PASS] TestAimHumanConvergenceAndOverride (finalErr=%.2fpx, estPxPerCount=%.2f)\n",
                finalErrPx, engine.EstimatedPxPerCountX());
}

void TestAimAllProfilesStability() {
    const nah2::aim::AimProfile profiles[] = {
        nah2::aim::AimProfile::UltraLegit,
        nah2::aim::AimProfile::Competitive,
        nah2::aim::AimProfile::Tracking,
    };

    for (auto p : profiles) {
        nah2::aim::AimConfig cfg;
        nah2::aim::ApplyProfileDefaults(cfg, p);
        cfg.fov_px = 400.0f;

        nah2::aim::AimAssistEngine engine(cfg);
        engine.Reseed(9001u);

        nah2::CameraState cam;
        cam.pos = { 0.0f, 5.0f, 0.0f };
        cam.fov_deg = 70.0f;
        cam.viewport_w = 1920.0f;
        cam.viewport_h = 1080.0f;
        cam.mouse_behavior = 1;

        nah2::Vec3 enemyHead = { -5.0f, 6.8f, -35.0f };
        const nah2::Vec3 vel = { 6.0f, 0.0f, 0.0f };
        double t = 2.0;

        for (int step = 0; step < 220; ++step) { // 440ms
            t += 0.002;
            enemyHead = nah2::math::Add(enemyHead, nah2::math::Scale(vel, 0.002f));
            if (step % 4 == 0) {
                nah2::TelemetryFrame f;
                f.seq = static_cast<uint64_t>(step / 4 + 1);
                f.camera = cam;
                nah2::EntityTelemetry e;
                e.id = 77;
                e.kind = 1;
                e.health = 100.0f;
                e.visible = 1;
                e.pos = { enemyHead.x, enemyHead.y - 1.8f, enemyHead.z };
                e.size = { 4.0f, 5.5f, 2.0f };
                e.has_vel = true;
                e.vel = vel;
                e.joint_mask = 0x3u;
                e.joints[0] = enemyHead;
                e.joints[1] = { enemyHead.x, enemyHead.y - 1.3f, enemyHead.z };
                f.entities.push_back(e);
                engine.ObserveTelemetry(f, t);
            }
            const auto res = engine.Step(0.002f, t, true, true, true, 1920.0f, 1080.0f);
            cam = RotateCameraByCounts(cam, res.dx, res.dy, 0.0026f);
        }

        nah2::Vec2 finalScreen{};
        assert(nah2::math::WorldToScreen(enemyHead, cam, 1920.0f, 1080.0f, finalScreen));
        const float err = std::hypot(finalScreen.x - 960.0f, finalScreen.y - 540.0f);
        assert(err < 25.0f);
    }
    std::printf("[PASS] TestAimAllProfilesStability\n");
}

void TestAimStrengthNonLinearity() {
    auto runAtStrength = [](float strength, int& outFirstMove, bool& outCurved, float& outErrAt140ms) {
        nah2::aim::AimConfig cfg;
        nah2::aim::ApplyProfileDefaults(cfg, nah2::aim::AimProfile::UltraLegit);
        cfg.fov_px = 450.0f;
        cfg.strength = strength;
        cfg.bone_mode = nah2::aim::AimBoneMode::Head;

        nah2::aim::AimAssistEngine engine(cfg);
        engine.Reseed(777u);

        nah2::CameraState cam;
        cam.pos = { 0.0f, 5.0f, 0.0f };
        cam.fov_deg = 70.0f;
        cam.viewport_w = 1920.0f;
        cam.viewport_h = 1080.0f;
        cam.mouse_behavior = 1;

        const nah2::Vec3 enemyHead = { 7.5f, 7.2f, -38.0f };
        double t = 1.0;
        outFirstMove = 0;
        outCurved = false;
        float firstRatio = 0.0f;

        for (int step = 0; step < 120; ++step) { // 240ms
            t += 0.002;
            if (step % 4 == 0) {
                nah2::TelemetryFrame f;
                f.seq = static_cast<uint64_t>(step / 4 + 1);
                f.camera = cam;
                nah2::EntityTelemetry e;
                e.id = 88;
                e.kind = 1;
                e.health = 100.0f;
                e.visible = 1;
                e.pos = { enemyHead.x, enemyHead.y - 1.8f, enemyHead.z };
                e.size = { 4.0f, 5.5f, 2.0f };
                e.joint_mask = 0x3u;
                e.joints[0] = enemyHead;
                e.joints[1] = { enemyHead.x, enemyHead.y - 1.3f, enemyHead.z };
                f.entities.push_back(e);
                engine.ObserveTelemetry(f, t);
            }
            const auto res = engine.Step(0.002f, t, true, true, true, 1920.0f, 1080.0f);
            if (res.dx != 0 || res.dy != 0) {
                if (outFirstMove == 0) {
                    outFirstMove = std::abs(res.dx) + std::abs(res.dy);
                    firstRatio = (res.dx != 0) ? static_cast<float>(res.dy) / static_cast<float>(res.dx) : 0.0f;
                } else if (res.dx != 0) {
                    const float curRatio = static_cast<float>(res.dy) / static_cast<float>(res.dx);
                    if (std::abs(curRatio - firstRatio) > 0.05f) {
                        outCurved = true;
                    }
                }
                cam = RotateCameraByCounts(cam, res.dx, res.dy, 0.0026f);
            }
            if (step == 69) { // ~140ms
                nah2::Vec2 pt{};
                assert(nah2::math::WorldToScreen(enemyHead, cam, 1920.0f, 1080.0f, pt));
                outErrAt140ms = std::hypot(pt.x - 960.0f, pt.y - 540.0f);
            }
        }
    };

    int firstLow = 0, firstHigh = 0;
    bool curvedLow = false, curvedHigh = false;
    float errLow = 0.0f, errHigh = 0.0f;
    runAtStrength(0.45f, firstLow, curvedLow, errLow);
    runAtStrength(1.85f, firstHigh, curvedHigh, errHigh);

    assert(errHigh < errLow);                 // Plus puissant -> converge plus vite
    assert(firstHigh > 0 && firstHigh <= 10); // Meme a haute puissance, demarrage doux sans snap brutal
    assert(curvedLow && curvedHigh);          // Mouvement toujours courbe et non-lineaire

    std::printf("[PASS] TestAimStrengthNonLinearity (err140ms: low=%.1fpx, high=%.1fpx)\n", errLow, errHigh);
}

} // namespace

int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    TestWorldToScreenAndBehindCamera();
    TestSegment3DNearPlaneClipping();
    TestDeltaTimeLerpAndSmoother();
    TestJsonParser();
    TestExtendedTelemetryJson();
    TestIpcLoopbackHttpWsUdp();
    TestRendererPipelineSmoke();
    TestAimTargetFilteringAndSwitchDelay();
    TestAimHumanConvergenceAndOverride();
    TestAimAllProfilesStability();
    TestAimStrengthNonLinearity();
    std::printf("ALL TESTS PASSED\n");
    return 0;
}
