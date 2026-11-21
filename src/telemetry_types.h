#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace nah2 {

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct CameraState {
    Vec3 pos{};
    Vec3 right{ 1.0f, 0.0f, 0.0f };
    Vec3 up{ 0.0f, 1.0f, 0.0f };
    Vec3 look{ 0.0f, 0.0f, -1.0f };
    float fov_deg = 70.0f;
    float viewport_w = 1280.0f;
    float viewport_h = 720.0f;
    uint8_t mouse_behavior = 0; // 0 = Default, 1 = LockCenter, 2 = LockCurrentPosition
    bool typing_in_ui = false;
    bool has_mouse_loc = false;
    Vec2 mouse_loc{};
    float game_mouse_sens = 0.0f;
};

inline constexpr std::size_t kMaxRigJoints = 15;

struct EntityTelemetry {
    uint32_t id = 0;
    uint8_t kind = 0;     // 0 = BasePart/Model OBB, 1 = Character/Humanoid Rig
    uint8_t rig_type = 0; // 0 = None, 6 = R6, 15 = R15
    std::string name;
    Vec3 pos{};
    Vec3 size{ 1.0f, 1.0f, 1.0f };
    Vec3 right{ 1.0f, 0.0f, 0.0f };
    Vec3 up{ 0.0f, 1.0f, 0.0f };
    Vec3 look{ 0.0f, 0.0f, -1.0f };
    uint16_t joint_mask = 0;
    std::array<Vec3, kMaxRigJoints> joints{};
    float health = -1.0f;
    float max_health = -1.0f;
    bool is_ally = false;
    int8_t visible = -1;  // -1 = inconnu, 0 = occulte, 1 = visible
    bool has_vel = false;
    Vec3 vel{};
};

struct TelemetryFrame {
    uint64_t seq = 0;
    double client_ts = 0.0;
    uint64_t received_us = 0;
    CameraState camera{};
    std::vector<EntityTelemetry> entities;
};

struct IpcStats {
    uint64_t packets_received = 0;
    uint64_t parse_errors = 0;
    uint32_t active_ws_clients = 0;
    double packets_per_sec = 0.0;
    uint64_t last_packet_us = 0;
    char transport[8] = "WAIT";
};

} // namespace nah2
