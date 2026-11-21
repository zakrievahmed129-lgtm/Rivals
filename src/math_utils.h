#pragma once

#include "telemetry_types.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace nah2::math {

inline constexpr float kPi = 3.14159265358979323846f;
inline constexpr float kNearPlaneW = 0.05f;

inline float Clamp(float v, float lo, float hi) {
    return std::max(lo, std::min(v, hi));
}

inline Vec3 Add(const Vec3& a, const Vec3& b) {
    return { a.x + b.x, a.y + b.y, a.z + b.z };
}

inline Vec3 Sub(const Vec3& a, const Vec3& b) {
    return { a.x - b.x, a.y - b.y, a.z - b.z };
}

inline Vec3 Scale(const Vec3& v, float s) {
    return { v.x * s, v.y * s, v.z * s };
}

inline float Dot(const Vec3& a, const Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline Vec3 Cross(const Vec3& a, const Vec3& b) {
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    };
}

inline float LengthSq(const Vec3& v) {
    return Dot(v, v);
}

inline float Length(const Vec3& v) {
    return std::sqrt(LengthSq(v));
}

inline Vec3 Normalize(const Vec3& v, const Vec3& fallback = { 0.0f, 0.0f, -1.0f }) {
    const float lenSq = LengthSq(v);
    if (lenSq < 1e-10f) {
        return fallback;
    }
    const float inv = 1.0f / std::sqrt(lenSq);
    return { v.x * inv, v.y * inv, v.z * inv };
}

// Re-orthonormalise une base Roblox main droite (Right x Up = -Look, Look x Up = Right)
inline void OrthonormalizeBasis(Vec3& right, Vec3& up, Vec3& look) {
    look = Normalize(look, { 0.0f, 0.0f, -1.0f });
    right = Normalize(Cross(look, up), { 1.0f, 0.0f, 0.0f });
    up = Normalize(Cross(right, look), { 0.0f, 1.0f, 0.0f });
}

inline float Lerp(float a, float b, float t) {
    const float c = Clamp(t, 0.0f, 1.0f);
    return a + (b - a) * c;
}

inline Vec3 Lerp(const Vec3& a, const Vec3& b, float t) {
    const float c = Clamp(t, 0.0f, 1.0f);
    return {
        a.x + (b.x - a.x) * c,
        a.y + (b.y - a.y) * c,
        a.z + (b.z - a.z) * c
    };
}

// Facteur d'interpolation exponentielle independant du framerate : alpha = 1 - exp(-speed * dt)
inline float ComputeExpLerpAlpha(float speed, float dt) {
    if (speed <= 0.0f) {
        return 1.0f;
    }
    if (dt <= 0.0f) {
        return 0.0f;
    }
    return Clamp(1.0f - std::exp(-speed * dt), 0.0f, 1.0f);
}

struct CameraSpacePoint {
    float xc = 0.0f;
    float yc = 0.0f;
    float w = 0.0f; // Profondeur avant le long de LookVector (composante W perspective)
};

inline CameraSpacePoint ToCameraSpace(const Vec3& worldPos, const CameraState& cam) {
    const Vec3 delta = Sub(worldPos, cam.pos);
    return {
        Dot(delta, cam.right),
        Dot(delta, cam.up),
        Dot(delta, cam.look)
    };
}

inline Vec2 ProjectCameraSpaceToScreen(
    const CameraSpacePoint& cs,
    const CameraState& cam,
    float targetWidth,
    float targetHeight
) {
    const float vpW = (cam.viewport_w > 1.0f) ? cam.viewport_w : targetWidth;
    const float vpH = (cam.viewport_h > 1.0f) ? cam.viewport_h : targetHeight;

    const float fovClamped = Clamp(cam.fov_deg, 1.0f, 179.0f);
    const float halfTanV = std::tan((fovClamped * (kPi / 180.0f)) * 0.5f);
    const float focalPx = (vpH * 0.5f) / halfTanV;

    const float invW = 1.0f / cs.w;
    const float vpX = (vpW * 0.5f) + (cs.xc * invW) * focalPx;
    const float vpY = (vpH * 0.5f) - (cs.yc * invW) * focalPx;

    return {
        vpX * (targetWidth / vpW),
        vpY * (targetHeight / vpH)
    };
}

// Projette un point 3D monde vers l'ecran 2D. Retourne false si le point est derriere la camera (W <= kNearPlaneW).
inline bool WorldToScreen(
    const Vec3& worldPos,
    const CameraState& cam,
    float targetWidth,
    float targetHeight,
    Vec2& outScreen,
    float* outDepthW = nullptr
) {
    const CameraSpacePoint cs = ToCameraSpace(worldPos, cam);
    if (outDepthW) {
        *outDepthW = cs.w;
    }
    if (cs.w <= kNearPlaneW) {
        return false;
    }
    outScreen = ProjectCameraSpaceToScreen(cs, cam, targetWidth, targetHeight);
    return true;
}

// Projette un segment 3D [worldA, worldB] avec decoupage (clipping) sur le plan proche W = kNearPlaneW.
inline bool ProjectSegment3D(
    const Vec3& worldA,
    const Vec3& worldB,
    const CameraState& cam,
    float targetWidth,
    float targetHeight,
    Vec2& outScreenA,
    Vec2& outScreenB
) {
    CameraSpacePoint a = ToCameraSpace(worldA, cam);
    CameraSpacePoint b = ToCameraSpace(worldB, cam);

    if (a.w <= kNearPlaneW && b.w <= kNearPlaneW) {
        return false;
    }

    if (a.w < kNearPlaneW) {
        const float denom = b.w - a.w;
        if (std::abs(denom) < 1e-6f) {
            return false;
        }
        const float t = (kNearPlaneW - a.w) / denom;
        a.xc += (b.xc - a.xc) * t;
        a.yc += (b.yc - a.yc) * t;
        a.w = kNearPlaneW;
    } else if (b.w < kNearPlaneW) {
        const float denom = a.w - b.w;
        if (std::abs(denom) < 1e-6f) {
            return false;
        }
        const float t = (kNearPlaneW - b.w) / denom;
        b.xc += (a.xc - b.xc) * t;
        b.yc += (a.yc - b.yc) * t;
        b.w = kNearPlaneW;
    }

    outScreenA = ProjectCameraSpaceToScreen(a, cam, targetWidth, targetHeight);
    outScreenB = ProjectCameraSpaceToScreen(b, cam, targetWidth, targetHeight);
    return true;
}

// Arettes d'une boite 3D (indices dans les 8 sommets ci-dessous)
inline constexpr std::array<std::pair<int, int>, 12> kBoxEdges = {{
    { 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 0 }, // Face arriere
    { 4, 5 }, { 5, 6 }, { 6, 7 }, { 7, 4 }, // Face avant
    { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 }  // Montants
}};

inline std::array<Vec3, 8> ComputeObbCorners(const EntityTelemetry& e) {
    const Vec3 rx = Scale(e.right, e.size.x * 0.5f);
    const Vec3 uy = Scale(e.up, e.size.y * 0.5f);
    const Vec3 lz = Scale(e.look, e.size.z * 0.5f);

    return {{
        Add(Sub(Sub(e.pos, rx), uy), Scale(lz, -1.0f)), // 0: -x, -y, -z
        Add(Sub(Add(e.pos, rx), uy), Scale(lz, -1.0f)), // 1: +x, -y, -z
        Add(Add(Add(e.pos, rx), uy), Scale(lz, -1.0f)), // 2: +x, +y, -z
        Add(Add(Sub(e.pos, rx), uy), Scale(lz, -1.0f)), // 3: -x, +y, -z
        Add(Sub(Sub(e.pos, rx), uy), lz),               // 4: -x, -y, +z
        Add(Sub(Add(e.pos, rx), uy), lz),               // 5: +x, -y, +z
        Add(Add(Add(e.pos, rx), uy), lz),               // 6: +x, +y, +z
        Add(Add(Sub(e.pos, rx), uy), lz)                // 7: -x, +y, +z
    }};
}

inline bool ComputeProjectedBounds2D(
    const EntityTelemetry& e,
    const CameraState& cam,
    float targetWidth,
    float targetHeight,
    float& outMinX,
    float& outMinY,
    float& outMaxX,
    float& outMaxY
) {
    const auto corners = ComputeObbCorners(e);
    bool anyVisible = false;
    float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;

    for (const auto& [i0, i1] : kBoxEdges) {
        Vec2 p0, p1;
        if (ProjectSegment3D(corners[i0], corners[i1], cam, targetWidth, targetHeight, p0, p1)) {
            anyVisible = true;
            minX = std::min({ minX, p0.x, p1.x });
            minY = std::min({ minY, p0.y, p1.y });
            maxX = std::max({ maxX, p0.x, p1.x });
            maxY = std::max({ maxY, p0.y, p1.y });
        }
    }

    if (!anyVisible) {
        return false;
    }

    outMinX = minX;
    outMinY = minY;
    outMaxX = maxX;
    outMaxY = maxY;
    return true;
}

// Lissage temporel (lerp base sur delta-time) des entites et de la camera
class SceneSmoother {
public:
    void SetEntitySpeed(float speed) { entity_speed_ = speed; }
    void SetCameraSpeed(float speed) { camera_speed_ = speed; }
    void SetStaleTimeout(double sec) { stale_timeout_sec_ = sec; }

    void Reset() {
        entities_.clear();
        has_camera_ = false;
        latest_seq_ = 0;
        last_frame_sec_ = -1.0;
    }

    CameraState SmoothCamera(const CameraState& target, float dt) {
        if (!has_camera_ || camera_speed_ <= 0.0f) {
            smoothed_camera_ = target;
            has_camera_ = true;
            return smoothed_camera_;
        }
        const float alpha = ComputeExpLerpAlpha(camera_speed_, dt);
        smoothed_camera_.pos = Lerp(smoothed_camera_.pos, target.pos, alpha);
        smoothed_camera_.right = Lerp(smoothed_camera_.right, target.right, alpha);
        smoothed_camera_.up = Lerp(smoothed_camera_.up, target.up, alpha);
        smoothed_camera_.look = Lerp(smoothed_camera_.look, target.look, alpha);
        OrthonormalizeBasis(smoothed_camera_.right, smoothed_camera_.up, smoothed_camera_.look);
        smoothed_camera_.fov_deg = Lerp(smoothed_camera_.fov_deg, target.fov_deg, alpha);
        smoothed_camera_.viewport_w = target.viewport_w;
        smoothed_camera_.viewport_h = target.viewport_h;
        smoothed_camera_.mouse_behavior = target.mouse_behavior;
        smoothed_camera_.typing_in_ui = target.typing_in_ui;
        smoothed_camera_.has_mouse_loc = target.has_mouse_loc;
        smoothed_camera_.mouse_loc = target.mouse_loc;
        smoothed_camera_.game_mouse_sens = target.game_mouse_sens;
        return smoothed_camera_;
    }

    void UpdateAndGetEntities(
        const std::vector<EntityTelemetry>& latest,
        float dt,
        double nowSec,
        bool hasNewFrame,
        std::vector<EntityTelemetry>& outEntities
    ) {
        if (entity_speed_ <= 0.0f) {
            if (hasNewFrame) {
                outEntities = latest;
                last_frame_sec_ = nowSec;
            } else if (last_frame_sec_ >= 0.0 && (nowSec - last_frame_sec_) > stale_timeout_sec_) {
                outEntities.clear();
            }
            return;
        }

        if (hasNewFrame) {
            last_frame_sec_ = nowSec;
            ++latest_seq_;
            for (const auto& src : latest) {
                auto it = entities_.find(src.id);
                if (it == entities_.end()) {
                    TrackedState st;
                    st.current = src;
                    st.target = src;
                    st.last_seen_sec = nowSec;
                    st.last_seen_seq = latest_seq_;
                    entities_.emplace(src.id, std::move(st));
                } else {
                    it->second.target = src;
                    it->second.last_seen_sec = nowSec;
                    it->second.last_seen_seq = latest_seq_;
                }
            }
        }

        const float alpha = ComputeExpLerpAlpha(entity_speed_, dt);
        outEntities.clear();
        outEntities.reserve(entities_.size());

        for (auto it = entities_.begin(); it != entities_.end();) {
            const bool missingInPackets = (latest_seq_ > it->second.last_seen_seq) &&
                                          ((latest_seq_ - it->second.last_seen_seq) >= 2);
            if (missingInPackets || (nowSec - it->second.last_seen_sec) > stale_timeout_sec_) {
                it = entities_.erase(it);
                continue;
            }

            TrackedState& st = it->second;
            const EntityTelemetry& tgt = st.target;
            EntityTelemetry& cur = st.current;

            cur.kind = tgt.kind;
            cur.rig_type = tgt.rig_type;
            cur.name = tgt.name;
            cur.health = tgt.health;
            cur.max_health = tgt.max_health;
            cur.is_ally = tgt.is_ally;
            cur.visible = tgt.visible;
            cur.has_vel = tgt.has_vel;
            if (tgt.has_vel) {
                cur.vel = Lerp(cur.vel, tgt.vel, alpha);
            } else {
                cur.vel = {};
            }

            cur.pos = Lerp(cur.pos, tgt.pos, alpha);
            cur.size = Lerp(cur.size, tgt.size, alpha);
            cur.right = Lerp(cur.right, tgt.right, alpha);
            cur.up = Lerp(cur.up, tgt.up, alpha);
            cur.look = Lerp(cur.look, tgt.look, alpha);
            OrthonormalizeBasis(cur.right, cur.up, cur.look);

            const uint16_t prevMask = cur.joint_mask;
            cur.joint_mask = tgt.joint_mask;
            for (std::size_t j = 0; j < kMaxRigJoints; ++j) {
                const uint16_t bit = static_cast<uint16_t>(1u << j);
                if (tgt.joint_mask & bit) {
                    if (prevMask & bit) {
                        cur.joints[j] = Lerp(cur.joints[j], tgt.joints[j], alpha);
                    } else {
                        cur.joints[j] = tgt.joints[j];
                    }
                }
            }

            outEntities.push_back(cur);
            ++it;
        }
    }

private:
    struct TrackedState {
        EntityTelemetry current;
        EntityTelemetry target;
        double last_seen_sec = 0.0;
        uint64_t last_seen_seq = 0;
    };

    float entity_speed_ = 0.0f;  // 0 = suivi instantane zero latence
    float camera_speed_ = 0.0f;  // 0 = suivi instantane de CurrentCamera
    double stale_timeout_sec_ = 0.35;

    bool has_camera_ = false;
    uint64_t latest_seq_ = 0;
    double last_frame_sec_ = -1.0;
    CameraState smoothed_camera_{};
    std::unordered_map<uint32_t, TrackedState> entities_;
};

} // namespace nah2::math
