#include "aim_assist.h"

#include <algorithm>
#include <cmath>

namespace nah2::aim {

namespace {

inline float SmoothStep(float t) {
    const float c = math::Clamp(t, 0.0f, 1.0f);
    return c * c * (3.0f - 2.0f * c);
}

// Minimum-jerk (Flash & Hogan) : derivees 1re et 2e nulles aux bornes
inline float SmootherStep(float t) {
    const float c = math::Clamp(t, 0.0f, 1.0f);
    return c * c * c * (c * (c * 6.0f - 15.0f) + 10.0f);
}

inline Vec2 Sub2D(const Vec2& a, const Vec2& b) {
    return { a.x - b.x, a.y - b.y };
}

inline Vec2 Normalize2D(const Vec2& v) {
    const float lenSq = v.x * v.x + v.y * v.y;
    if (lenSq < 1e-8f) {
        return { 0.0f, 0.0f };
    }
    const float inv = 1.0f / std::sqrt(lenSq);
    return { v.x * inv, v.y * inv };
}

inline float ComputeFocalPx(const CameraState& cam, float viewHeight) {
    const float vpH = (cam.viewport_h > 1.0f) ? cam.viewport_h : viewHeight;
    const float fovClamped = math::Clamp(cam.fov_deg, 1.0f, 179.0f);
    const float halfTanV = std::tan((fovClamped * (math::kPi / 180.0f)) * 0.5f);
    return (vpH * 0.5f) / std::max(halfTanV, 1e-4f);
}

} // namespace

const char* ProfileName(AimProfile p) {
    switch (p) {
        case AimProfile::UltraLegit:  return "LEGIT";
        case AimProfile::Competitive: return "PRO";
        case AimProfile::Tracking:    return "TRACK";
        default:                      return "LEGIT";
    }
}

const char* BoneModeName(AimBoneMode b) {
    switch (b) {
        case AimBoneMode::Smart: return "smart";
        case AimBoneMode::Head:  return "head";
        case AimBoneMode::Neck:  return "neck";
        case AimBoneMode::Chest: return "chest";
        default:                 return "smart";
    }
}

const char* KeyModeName(AimKeyMode k) {
    switch (k) {
        case AimKeyMode::AnyMouse:   return "any";
        case AimKeyMode::RightMouse: return "rmb";
        case AimKeyMode::LeftMouse:  return "lmb";
        case AimKeyMode::Mouse4:     return "x1";
        case AimKeyMode::Mouse5:     return "x2";
        case AimKeyMode::LeftAlt:    return "alt";
        case AimKeyMode::LeftShift:  return "shift";
        case AimKeyMode::Always:     return "always";
        default:                     return "any";
    }
}

void ApplyProfileDefaults(AimConfig& cfg, AimProfile profile, bool preserveFov) {
    cfg.profile = profile;
    switch (profile) {
        case AimProfile::UltraLegit:
            if (!preserveFov) cfg.fov_px = 140.0f;
            cfg.deadzone_ratio = 0.30f;
            cfg.velocity_lead = 0.80f;
            break;
        case AimProfile::Competitive:
            if (!preserveFov) cfg.fov_px = 185.0f;
            cfg.deadzone_ratio = 0.16f;
            cfg.velocity_lead = 0.95f;
            break;
        case AimProfile::Tracking:
            if (!preserveFov) cfg.fov_px = 210.0f;
            cfg.deadzone_ratio = 0.22f;
            cfg.velocity_lead = 1.00f;
            break;
    }
}

AimAssistEngine::AimAssistEngine(const AimConfig& cfg) : config_(cfg) {
    recent_inputs_.reserve(64);
    recent_cam_obs_.reserve(32);
    debug_state_.enabled = config_.enabled;
    debug_state_.profile = config_.profile;
    debug_state_.fov_px = config_.fov_px;
    debug_state_.strength = config_.strength;
    debug_state_.draw_fov_circle = config_.draw_fov_circle;
    debug_state_.draw_target_marker = config_.draw_target_marker;
}

void AimAssistEngine::SetConfig(const AimConfig& cfg) {
    config_ = cfg;
    config_.strength = math::Clamp(config_.strength, 0.10f, 2.00f);
    debug_state_.enabled = config_.enabled;
    debug_state_.profile = config_.profile;
    debug_state_.fov_px = config_.fov_px;
    debug_state_.strength = config_.strength;
    debug_state_.draw_fov_circle = config_.draw_fov_circle;
    debug_state_.draw_target_marker = config_.draw_target_marker;
}

void AimAssistEngine::CycleProfile() {
    const uint8_t nextIdx = static_cast<uint8_t>((static_cast<uint8_t>(config_.profile) + 1) % 3);
    ApplyProfileDefaults(config_, static_cast<AimProfile>(nextIdx), false);
    debug_state_.profile = config_.profile;
    debug_state_.fov_px = config_.fov_px;
}

void AimAssistEngine::Reseed(uint32_t seed) {
    rng_state_ = (seed != 0u) ? seed : 0x9E3779B9u;
}

void AimAssistEngine::ResetSession() {
    has_frame_ = false;
    has_prev_cam_ = false;
    cam_vel_ = {};
    trackers_.clear();
    recent_inputs_.clear();
    recent_cam_obs_.clear();
    locked_id_ = 0;
    cur_vel_x_ = 0.0f;
    cur_vel_y_ = 0.0f;
    rem_x_ = 0.0f;
    rem_y_ = 0.0f;
    human_vel_x_ = 0.0f;
    human_vel_y_ = 0.0f;
    assist_authority_ = 1.0f;
    was_engaging_ = false;
    wants_high_rate_ = false;
    sens_initialized_ = false;
}

AimAssistEngine::ProfileTuning AimAssistEngine::GetTuning() const {
    ProfileTuning t;
    switch (config_.profile) {
        case AimProfile::UltraLegit:
            t.fov_px = 140.0f;
            t.base_gain = 11.5f;
            t.linear_gain = 4.2f;
            t.max_speed_px_s = 1750.0f;
            t.accel_rate = 26.0f;
            t.ramp_up_sec = 0.048f;
            t.switch_delay_min_sec = 0.105f;
            t.switch_delay_max_sec = 0.165f;
            t.deadzone_ratio = 0.30f;
            t.arc_max = 0.065f;
            t.drift_amp_ratio = 0.28f;
            t.tremor_base_px_s = 3.5f;
            t.tremor_vel_ratio = 0.016f;
            t.tracking_ff = 0.68f;
            t.lead_sec = 0.040f;
            t.overshoot_prob = 0.22f;
            t.overshoot_scale = 0.045f;
            t.head_preference = 0.85f;
            break;
        case AimProfile::Competitive:
            t.fov_px = 185.0f;
            t.base_gain = 17.5f;
            t.linear_gain = 7.2f;
            t.max_speed_px_s = 2650.0f;
            t.accel_rate = 38.0f;
            t.ramp_up_sec = 0.026f;
            t.switch_delay_min_sec = 0.048f;
            t.switch_delay_max_sec = 0.088f;
            t.deadzone_ratio = 0.16f;
            t.arc_max = 0.042f;
            t.drift_amp_ratio = 0.16f;
            t.tremor_base_px_s = 2.0f;
            t.tremor_vel_ratio = 0.010f;
            t.tracking_ff = 0.88f;
            t.lead_sec = 0.055f;
            t.overshoot_prob = 0.12f;
            t.overshoot_scale = 0.025f;
            t.head_preference = 0.72f;
            break;
        case AimProfile::Tracking:
            t.fov_px = 210.0f;
            t.base_gain = 13.5f;
            t.linear_gain = 6.8f;
            t.max_speed_px_s = 2100.0f;
            t.accel_rate = 34.0f;
            t.ramp_up_sec = 0.032f;
            t.switch_delay_min_sec = 0.065f;
            t.switch_delay_max_sec = 0.115f;
            t.deadzone_ratio = 0.22f;
            t.arc_max = 0.038f;
            t.drift_amp_ratio = 0.22f;
            t.tremor_base_px_s = 2.5f;
            t.tremor_vel_ratio = 0.012f;
            t.tracking_ff = 0.94f;
            t.lead_sec = 0.060f;
            t.overshoot_prob = 0.08f;
            t.overshoot_scale = 0.020f;
            t.head_preference = 0.82f;
            break;
    }
    t.deadzone_ratio = math::Clamp(config_.deadzone_ratio, 0.0f, 0.90f);
    return t;
}

float AimAssistEngine::NextRand01() {
    uint32_t x = rng_state_;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state_ = x;
    return static_cast<float>(x & 0x00FFFFFFu) / static_cast<float>(0x01000000u);
}

float AimAssistEngine::RandRange(float lo, float hi) {
    return lo + (hi - lo) * NextRand01();
}

void AimAssistEngine::ObserveTelemetry(const TelemetryFrame& frame, double nowSec) {
    latest_frame_ = frame;
    has_frame_ = true;
    last_telemetry_sec_ = nowSec;

    if (frame.seq == 0 || frame.seq != last_observed_seq_) {
        last_observed_seq_ = frame.seq;
        UpdateCalibration(frame, nowSec);
        UpdateTrackers(frame, nowSec);
    }
}

void AimAssistEngine::NotifyHardwareMouseDelta(int dx, int dy, double nowSec) {
    if (dx == 0 && dy == 0) {
        return;
    }

    recent_inputs_.push_back({ nowSec, dx, dy });

    const float dtHum = static_cast<float>(nowSec - last_human_input_sec_);
    if (dtHum > 0.0f && dtHum <= 0.12f) {
        const float instVx = static_cast<float>(dx) / std::max(dtHum, 0.002f);
        const float instVy = static_cast<float>(dy) / std::max(dtHum, 0.002f);
        const float alpha = math::ComputeExpLerpAlpha(28.0f, dtHum);
        human_vel_x_ = math::Lerp(human_vel_x_, instVx, alpha);
        human_vel_y_ = math::Lerp(human_vel_y_, instVy, alpha);
    } else {
        human_vel_x_ = static_cast<float>(dx) * 60.0f;
        human_vel_y_ = static_cast<float>(dy) * 60.0f;
    }
    last_human_input_sec_ = nowSec;
}

void AimAssistEngine::UpdateCalibration(const TelemetryFrame& frame, double nowSec) {
    const CameraState& cam = frame.camera;
    const float vpW = (cam.viewport_w > 1.0f) ? cam.viewport_w : ((last_view_w_ > 1.0f) ? last_view_w_ : 1920.0f);
    const float vpH = (cam.viewport_h > 1.0f) ? cam.viewport_h : ((last_view_h_ > 1.0f) ? last_view_h_ : 1080.0f);
    const float viewW = (last_view_w_ > 1.0f) ? last_view_w_ : vpW;
    const float viewH = (last_view_h_ > 1.0f) ? last_view_h_ : vpH;
    const float scaleX = viewW / vpW;
    const float scaleY = viewH / vpH;
    const float focalPx = ComputeFocalPx(cam, vpH);

    if (!sens_initialized_) {
        const float sensMult = (config_.sens_override > 0.0f)
            ? config_.sens_override
            : ((cam.game_mouse_sens > 0.0f) ? math::Clamp(cam.game_mouse_sens, 0.05f, 5.0f) : 0.65f);
        const float prior = math::Clamp(focalPx * scaleY * 0.0044f * sensMult, 0.25f, 8.0f);
        px_per_count_x_ = prior;
        px_per_count_y_ = prior * 0.90f;
        sens_initialized_ = true;
    }

    if (has_prev_cam_) {
        const float dtCam = static_cast<float>(nowSec - prev_cam_sec_);
        if (dtCam >= 0.001f && dtCam <= 0.12f) {
            // 1. Vitesse de translation de la camera (pour compenser le strafe local)
            const Vec3 dPos = math::Sub(cam.pos, prev_cam_.pos);
            const Vec3 rawCamVel = math::Scale(dPos, 1.0f / dtCam);
            if (math::LengthSq(rawCamVel) < (200.0f * 200.0f)) {
                const float alphaCam = math::ComputeExpLerpAlpha(14.0f, dtCam);
                cam_vel_ = math::Lerp(cam_vel_, rawCamVel, alphaCam);
            } else {
                cam_vel_ = {};
            }

            // 2. Rotation observee pour calibrer pixels/count en boucle fermee (echelle overlay)
            if (math::Dot(prev_cam_.look, cam.look) > 0.70f) {
                float obsPxX = 0.0f;
                float obsPxY = 0.0f;
                if (cam.mouse_behavior == 0 && cam.has_mouse_loc && prev_cam_.has_mouse_loc) {
                    obsPxX = (cam.mouse_loc.x - prev_cam_.mouse_loc.x) * scaleX;
                    obsPxY = (cam.mouse_loc.y - prev_cam_.mouse_loc.y) * scaleY;
                } else {
                    const float dYaw = std::atan2(
                        math::Dot(cam.look, prev_cam_.right),
                        math::Dot(cam.look, prev_cam_.look));
                    const float dPitch = -std::atan2(
                        math::Dot(cam.look, prev_cam_.up),
                        math::Dot(cam.look, prev_cam_.look));
                    obsPxX = focalPx * scaleX * dYaw;
                    obsPxY = focalPx * scaleY * dPitch;
                }

                if (std::isfinite(obsPxX) && std::isfinite(obsPxY)) {
                    recent_cam_obs_.push_back({ nowSec, obsPxX, obsPxY });
                }
            }
        }
    }

    prev_cam_ = cam;
    prev_cam_sec_ = nowSec;
    has_prev_cam_ = true;

    // Purger les echantillons vieux (> 110ms)
    const double cutoff = nowSec - 0.11;
    std::erase_if(recent_inputs_, [cutoff](const PendingInputSample& s) { return s.time_sec < cutoff; });
    std::erase_if(recent_cam_obs_, [cutoff](const ObservedMotionSample& s) { return s.time_sec < cutoff; });

    if ((nowSec - last_calib_update_sec_) >= 0.045 && recent_inputs_.size() >= 2 && recent_cam_obs_.size() >= 2) {
        double sumInX = 0.0, sumInY = 0.0;
        for (const auto& in : recent_inputs_) {
            sumInX += static_cast<double>(in.dx);
            sumInY += static_cast<double>(in.dy);
        }
        double sumObsX = 0.0, sumObsY = 0.0;
        for (const auto& ob : recent_cam_obs_) {
            sumObsX += static_cast<double>(ob.obs_px_x);
            sumObsY += static_cast<double>(ob.obs_px_y);
        }

        if (std::abs(sumInX) >= 8.0 && (sumObsX * sumInX) > 0.0 && std::abs(sumObsX) >= 4.0) {
            const float measX = static_cast<float>(std::abs(sumObsX) / std::abs(sumInX));
            if (measX >= px_per_count_x_ * 0.25f && measX <= px_per_count_x_ * 4.0f) {
                px_per_count_x_ = math::Clamp(math::Lerp(px_per_count_x_, measX, 0.15f), 0.15f, 10.0f);
            }
        }
        if (std::abs(sumInY) >= 8.0 && (sumObsY * sumInY) > 0.0 && std::abs(sumObsY) >= 4.0) {
            const float measY = static_cast<float>(std::abs(sumObsY) / std::abs(sumInY));
            if (measY >= px_per_count_y_ * 0.25f && measY <= px_per_count_y_ * 4.0f) {
                px_per_count_y_ = math::Clamp(math::Lerp(px_per_count_y_, measY, 0.15f), 0.15f, 10.0f);
            }
        }
        last_calib_update_sec_ = nowSec;
    }
}

void AimAssistEngine::UpdateTrackers(const TelemetryFrame& frame, double nowSec) {
    for (const auto& e : frame.entities) {
        if (e.kind != 1 && e.joint_mask == 0) {
            continue;
        }
        if (e.health >= 0.0f && e.health <= 0.01f) {
            trackers_.erase(e.id);
            continue;
        }

        TargetMotionState& st = trackers_[e.id];
        const float dt = static_cast<float>(nowSec - st.last_frame_sec);

        if (st.has_pos && dt >= 0.001f && dt <= 0.15f) {
            const Vec3 fdVel = math::Scale(math::Sub(e.pos, st.last_pos), 1.0f / dt);
            if (math::LengthSq(fdVel) > (250.0f * 250.0f)) {
                st.vel = e.has_vel ? e.vel : Vec3{};
                st.accel = {};
            } else {
                const Vec3 measVel = e.has_vel ? math::Lerp(fdVel, e.vel, 0.72f) : fdVel;
                const float alphaV = math::ComputeExpLerpAlpha(e.has_vel ? 24.0f : 16.0f, dt);
                st.vel = math::Lerp(st.vel, measVel, alphaV);

                const Vec3 rawAcc = math::Scale(math::Sub(st.vel, st.prev_vel), 1.0f / dt);
                st.accel = math::Lerp(st.accel, rawAcc, math::ComputeExpLerpAlpha(10.0f, dt));
            }
            st.prev_vel = st.vel;
        } else if (!st.has_pos) {
            st.vel = e.has_vel ? e.vel : Vec3{};
            st.prev_vel = st.vel;
            st.accel = {};
        }

        st.last_pos = e.pos;
        st.last_frame_sec = nowSec;
        st.last_seen_sec = nowSec;
        st.has_pos = true;
        if (e.visible != 0) {
            st.last_visible_sec = nowSec;
        }
    }

    for (auto it = trackers_.begin(); it != trackers_.end();) {
        if ((nowSec - it->second.last_seen_sec) > 0.65) {
            it = trackers_.erase(it);
        } else {
            ++it;
        }
    }
}

Vec2 AimAssistEngine::ResolveAimOrigin(float viewWidth, float viewHeight) const {
    if (has_frame_ && latest_frame_.camera.mouse_behavior == 0 && latest_frame_.camera.has_mouse_loc) {
        const CameraState& cam = latest_frame_.camera;
        const float vpW = (cam.viewport_w > 1.0f) ? cam.viewport_w : viewWidth;
        const float vpH = (cam.viewport_h > 1.0f) ? cam.viewport_h : viewHeight;
        return {
            cam.mouse_loc.x * (viewWidth / vpW),
            cam.mouse_loc.y * (viewHeight / vpH)
        };
    }
    return { viewWidth * 0.5f, viewHeight * 0.5f };
}

Vec3 AimAssistEngine::SelectAimBoneWorld(
    const EntityTelemetry& e,
    const Vec2& aimOrigin,
    float viewWidth,
    float viewHeight,
    float headPref
) const {
    const bool hasHead = (e.joint_mask & 1u) != 0u;
    const bool hasChest = (e.joint_mask & 2u) != 0u;

    const Vec3 headPos = hasHead ? e.joints[0] : math::Add(e.pos, math::Scale(e.up, e.size.y * 0.36f));
    const Vec3 chestPos = hasChest ? e.joints[1] : math::Add(e.pos, math::Scale(e.up, e.size.y * 0.14f));
    const Vec3 neckPos = math::Lerp(chestPos, headPos, 0.64f);

    switch (config_.bone_mode) {
        case AimBoneMode::Head:  return headPos;
        case AimBoneMode::Neck:  return neckPos;
        case AimBoneMode::Chest: return chestPos;
        case AimBoneMode::Smart:
        default: {
            const CameraState& cam = latest_frame_.camera;
            Vec2 sHead, sNeck, sChest;
            float bestScore = 1e9f;
            Vec3 bestWorld = neckPos;

            if (math::WorldToScreen(headPos, cam, viewWidth, viewHeight, sHead)) {
                const float d = std::hypot(sHead.x - aimOrigin.x, sHead.y - aimOrigin.y) * headPref;
                if (d < bestScore) {
                    bestScore = d;
                    bestWorld = headPos;
                }
            }
            if (math::WorldToScreen(neckPos, cam, viewWidth, viewHeight, sNeck)) {
                const float d = std::hypot(sNeck.x - aimOrigin.x, sNeck.y - aimOrigin.y) * 0.94f;
                if (d < bestScore) {
                    bestScore = d;
                    bestWorld = neckPos;
                }
            }
            if (math::WorldToScreen(chestPos, cam, viewWidth, viewHeight, sChest)) {
                const float d = std::hypot(sChest.x - aimOrigin.x, sChest.y - aimOrigin.y) * 1.12f;
                if (d < bestScore) {
                    bestScore = d;
                    bestWorld = chestPos;
                }
            }
            return bestWorld;
        }
    }
}

bool AimAssistEngine::SelectTarget(
    float viewWidth,
    float viewHeight,
    double nowSec,
    const Vec2& aimOrigin,
    CandidateTarget& outTarget
) const {
    if (!has_frame_ || latest_frame_.entities.empty()) {
        return false;
    }

    const ProfileTuning tuning = GetTuning();
    const CameraState& cam = latest_frame_.camera;
    const float focalPx = ComputeFocalPx(cam, viewHeight) * (viewHeight / ((cam.viewport_h > 1.0f) ? cam.viewport_h : viewHeight));
    const float subFrameAge = static_cast<float>(std::clamp(nowSec - last_telemetry_sec_, 0.0, 0.045));
    const float resScale = math::Clamp(viewHeight / 1080.0f, 0.55f, 2.5f);
    const float baseFovPx = config_.fov_px * resScale;

    bool found = false;
    float bestScore = 1e9f;

    for (const auto& e : latest_frame_.entities) {
        if (e.kind != 1 && e.joint_mask == 0) {
            continue;
        }
        if (e.health >= 0.0f && e.health <= 0.01f) {
            continue;
        }
        if (config_.team_check && e.is_ally) {
            continue;
        }

        const bool isLocked = (e.id == locked_id_ && locked_id_ != 0);
        auto trIt = trackers_.find(e.id);
        const TargetMotionState* st = (trIt != trackers_.end()) ? &trIt->second : nullptr;

        bool inVisGrace = false;
        if (config_.visibility_check && e.visible == 0) {
            if (isLocked && st && st->last_visible_sec >= 0.0 && (nowSec - st->last_visible_sec) < 0.085) {
                inVisGrace = true;
            } else {
                continue;
            }
        }

        Vec3 targetVel = st ? st->vel : (e.has_vel ? e.vel : Vec3{});
        Vec3 targetAcc = st ? st->accel : Vec3{};

        // Reduction de l'anticipation si la cible change brusquement de direction (anti-ADAD overshoot)
        float leadSec = tuning.lead_sec * math::Clamp(config_.velocity_lead, 0.0f, 1.5f);
        if (math::Dot(targetVel, targetAcc) < -25.0f) {
            leadSec *= 0.35f;
        }

        const Vec3 baseBone = SelectAimBoneWorld(e, aimOrigin, viewWidth, viewHeight, tuning.head_preference);
        const Vec3 predictedWorld = math::Add(baseBone, math::Scale(targetVel, subFrameAge + leadSec));

        Vec2 screenPt{};
        float depthW = 0.0f;
        if (!math::WorldToScreen(predictedWorld, cam, viewWidth, viewHeight, screenPt, &depthW)) {
            continue;
        }

        const float distPx = std::hypot(screenPt.x - aimOrigin.x, screenPt.y - aimOrigin.y);
        const float allowedFov = isLocked ? (baseFovPx * 1.25f) : baseFovPx;
        if (distPx > allowedFov) {
            continue;
        }

        const float score = distPx * (isLocked ? 0.68f : 1.0f);
        if (score < bestScore) {
            bestScore = score;
            outTarget.id = e.id;
            outTarget.name = e.name;
            outTarget.aim_world = predictedWorld;
            outTarget.rel_vel = math::Sub(targetVel, cam_vel_);
            outTarget.screen_pt = screenPt;
            outTarget.depth_w = depthW;
            outTarget.dist_px = distPx;
            outTarget.hitbox_radius_px = math::Clamp((0.62f * focalPx) / std::max(depthW, 1.0f), 3.0f, 34.0f);
            outTarget.score = score;
            outTarget.in_vis_grace = inVisGrace;
            found = true;
        }
    }

    return found;
}

AimStepResult AimAssistEngine::Step(
    float dt,
    double nowSec,
    bool targetForeground,
    bool aimKeyPressed,
    bool systemCursorHidden,
    float viewWidth,
    float viewHeight
) {
    AimStepResult out;
    out.aim_key_down = aimKeyPressed;

    last_view_w_ = viewWidth;
    last_view_h_ = viewHeight;

    const float resScale = math::Clamp(viewHeight / 1080.0f, 0.55f, 2.5f);
    const float power = math::Clamp(config_.strength, 0.10f, 2.00f);
    const Vec2 aimOrigin = ResolveAimOrigin(viewWidth, viewHeight);
    const ProfileTuning tuning = GetTuning();

    debug_state_.enabled = config_.enabled;
    debug_state_.profile = config_.profile;
    debug_state_.draw_fov_circle = config_.draw_fov_circle;
    debug_state_.draw_target_marker = config_.draw_target_marker;
    debug_state_.aim_origin = aimOrigin;
    debug_state_.fov_px = config_.fov_px * resScale;
    debug_state_.strength = power;
    debug_state_.px_per_count_x = px_per_count_x_;
    debug_state_.px_per_count_y = px_per_count_y_;
    debug_state_.actively_engaging = false;
    debug_state_.has_target = false;
    debug_state_.in_reaction_delay = false;

    const bool frameFresh = has_frame_ && (nowSec - last_telemetry_sec_) < 0.25;
    const bool uiBlocked = has_frame_ && latest_frame_.camera.typing_in_ui;

    // Securite anti-menu : en mode LMB/Always, exige une camera verrouillee (ou curseur masque), sauf si allow_unlocked_cursor
    bool cursorModeOk = true;
    if (has_frame_ && (config_.key_mode == AimKeyMode::LeftMouse || config_.key_mode == AimKeyMode::Always ||
                       config_.key_mode == AimKeyMode::AnyMouse)) {
        const bool isLockedCam = (latest_frame_.camera.mouse_behavior != 0) || systemCursorHidden;
        if (!isLockedCam && !config_.allow_unlocked_cursor) {
            if (config_.key_mode == AimKeyMode::LeftMouse || config_.key_mode == AimKeyMode::Always) {
                cursorModeOk = false;
            }
            // Note : pour AnyMouse, le caller filtre deja LMB vs RMB/X1/X2 ; si mouse_behavior == 0 et LMB seul sans curseur cache, on bloque ici si necessaire.
        }
    }

    const bool canEngage = config_.enabled && targetForeground && aimKeyPressed && frameFresh && !uiBlocked && cursorModeOk;

    if (!canEngage || dt <= 0.0f) {
        const float decay = math::ComputeExpLerpAlpha(26.0f, std::max(dt, 0.001f));
        cur_vel_x_ = math::Lerp(cur_vel_x_, 0.0f, decay);
        cur_vel_y_ = math::Lerp(cur_vel_y_, 0.0f, decay);
        if (std::abs(cur_vel_x_) < 1.0f) cur_vel_x_ = 0.0f;
        if (std::abs(cur_vel_y_) < 1.0f) cur_vel_y_ = 0.0f;
        rem_x_ = 0.0f;
        rem_y_ = 0.0f;
        if (!aimKeyPressed) {
            locked_id_ = 0;
            assist_authority_ = 1.0f;
        }
        was_engaging_ = false;
        wants_high_rate_ = false;
        debug_state_.locked_entity_id = 0;
        debug_state_.locked_name.clear();
        return out;
    }

    CandidateTarget tgt{};
    if (!SelectTarget(viewWidth, viewHeight, nowSec, aimOrigin, tgt)) {
        const float decay = math::ComputeExpLerpAlpha(22.0f, dt);
        cur_vel_x_ = math::Lerp(cur_vel_x_, 0.0f, decay);
        cur_vel_y_ = math::Lerp(cur_vel_y_, 0.0f, decay);
        rem_x_ *= 0.6f;
        rem_y_ *= 0.6f;
        locked_id_ = 0;
        was_engaging_ = true;
        wants_high_rate_ = true; // Reste reactif si l'utilisateur balaye avec le bouton appuye
        debug_state_.locked_entity_id = 0;
        debug_state_.locked_name.clear();
        return out;
    }

    // Changement ou nouvelle acquisition de cible
    if (tgt.id != locked_id_) {
        const bool directSwitch = (locked_id_ != 0) && ((nowSec - last_lock_active_sec_) < 0.45);
        const float switchScale = math::Clamp(1.0f / std::pow(power, 0.42f), 0.58f, 1.45f);
        const float delaySec = directSwitch
            ? std::max(0.038f, RandRange(tuning.switch_delay_min_sec, tuning.switch_delay_max_sec) * switchScale)
            : RandRange(0.006f, 0.018f);
        engage_after_sec_ = nowSec + static_cast<double>(delaySec);
        locked_id_ = tgt.id;
        flick_elapsed_sec_ = 0.0;
        initial_flick_dist_ = std::max(tgt.dist_px, 1.0f);
        const float arcMag = RandRange(tuning.arc_max * 0.28f, tuning.arc_max);
        arc_bias_ = (NextRand01() < 0.5f) ? -arcMag : arcMag;
        arc_asym_ = RandRange(-0.32f, 0.32f);

        if (initial_flick_dist_ > (85.0f * resScale) && NextRand01() < tuning.overshoot_prob) {
            overshoot_px_ = std::min(initial_flick_dist_ * tuning.overshoot_scale, tgt.hitbox_radius_px * 0.85f);
            overshoot_dir_ = Normalize2D(Sub2D(tgt.screen_pt, aimOrigin));
        } else {
            overshoot_px_ = 0.0f;
            overshoot_dir_ = {};
        }
    }
    last_lock_active_sec_ = nowSec;
    was_engaging_ = true;
    wants_high_rate_ = true;

    debug_state_.has_target = true;
    debug_state_.locked_entity_id = tgt.id;
    debug_state_.locked_name = tgt.name;
    debug_state_.target_screen_pt = tgt.screen_pt;

    if (nowSec < engage_after_sec_) {
        const float decay = math::ComputeExpLerpAlpha(18.0f, dt);
        cur_vel_x_ = math::Lerp(cur_vel_x_, 0.0f, decay);
        cur_vel_y_ = math::Lerp(cur_vel_y_, 0.0f, decay);
        debug_state_.in_reaction_delay = true;
        out.has_target = true;
        out.target_id = tgt.id;
        return out;
    }

    debug_state_.actively_engaging = true;
    flick_elapsed_sec_ += static_cast<double>(dt);

    // 1. Derive organique Ornstein-Uhlenbeck dans la zone de confort du hitbox
    if (nowSec >= next_drift_impulse_sec_) {
        const float angle = RandRange(0.0f, 2.0f * math::kPi);
        const float rad = tgt.hitbox_radius_px * tuning.drift_amp_ratio * std::sqrt(NextRand01());
        drift_goal_x_ = std::cos(angle) * rad;
        drift_goal_y_ = std::sin(angle) * rad * 0.85f;
        next_drift_impulse_sec_ = nowSec + static_cast<double>(RandRange(0.35f, 0.85f));
    }
    const float driftAlpha = math::ComputeExpLerpAlpha(3.6f, dt);
    drift_x_ = math::Lerp(drift_x_, drift_goal_x_, driftAlpha);
    drift_y_ = math::Lerp(drift_y_, drift_goal_y_, driftAlpha);

    // 2. Reduction de l'overshoot initial en fin d'approche
    if (tgt.dist_px < (32.0f * resScale) || flick_elapsed_sec_ > 0.14) {
        overshoot_px_ *= std::exp(-22.0f * dt);
        if (overshoot_px_ < 0.05f) overshoot_px_ = 0.0f;
    }

    const Vec2 aimTargetPt = {
        tgt.screen_pt.x + drift_x_ + overshoot_dir_.x * overshoot_px_,
        tgt.screen_pt.y + drift_y_ + overshoot_dir_.y * overshoot_px_
    };
    debug_state_.target_screen_pt = aimTargetPt;

    const float errX = aimTargetPt.x - aimOrigin.x;
    const float errY = aimTargetPt.y - aimOrigin.y;
    const float dist = std::hypot(errX, errY);
    const float dirX = (dist > 1e-4f) ? (errX / dist) : 0.0f;
    const float dirY = (dist > 1e-4f) ? (errY / dist) : 0.0f;

    // 3. Synergie main humaine / Override manuel (breakaway si le joueur tire dans le sens oppose)
    if ((nowSec - last_human_input_sec_) > 0.085) {
        const float hDecay = math::ComputeExpLerpAlpha(20.0f, dt);
        human_vel_x_ = math::Lerp(human_vel_x_, 0.0f, hDecay);
        human_vel_y_ = math::Lerp(human_vel_y_, 0.0f, hDecay);
    }
    const float humPxX = human_vel_x_ * px_per_count_x_;
    const float humPxY = human_vel_y_ * px_per_count_y_;
    const float humSpeedPx = std::hypot(humPxX, humPxY);
    float targetAuth = 1.0f;
    if (dist > 4.0f && humSpeedPx > 40.0f) {
        const float along = humPxX * dirX + humPxY * dirY;
        if (humSpeedPx > (280.0f * resScale) && along < (-180.0f * resScale)) {
            targetAuth = 0.0f; // Le joueur tourne volontairement la camera ailleurs
        } else if (along > (65.0f * resScale)) {
            targetAuth = 1.22f; // Le joueur vise deja vers la cible : synergie fluide
        }
    }
    const float authRate = (targetAuth < assist_authority_) ? 32.0f : 11.0f;
    assist_authority_ = math::Lerp(assist_authority_, targetAuth, math::ComputeExpLerpAlpha(authRate, dt));

    // 4. Zone morte douce interieure + Profil de Fitts + Courbure naturelle du poignet
    const float deadzonePx = tgt.hitbox_radius_px * tuning.deadzone_ratio;
    float posGain = 1.0f;
    if (deadzonePx > 0.5f) {
        const float innerCore = deadzonePx * 0.40f;
        if (dist <= innerCore) {
            posGain = 0.0f;
        } else if (dist < deadzonePx) {
            posGain = SmoothStep((dist - innerCore) / std::max(deadzonePx - innerCore, 0.1f));
        }
    }

    const float powerDrive = std::pow(power, 0.78f);
    const float powerAccel = std::pow(power, 0.55f);
    const float smoothDiv = math::Clamp(config_.smooth_scale, 0.35f, 2.5f);
    const float effectiveAccel = (tuning.accel_rate * powerAccel) / smoothDiv;
    const float fovEdgeRatio = (debug_state_.fov_px > 1.0f) ? (tgt.dist_px / debug_state_.fov_px) : 0.0f;
    const float fovTaper = 1.0f - 0.65f * SmoothStep((fovEdgeRatio - 0.70f) / 0.30f);

    const float rawSpeed = ((tuning.base_gain * std::pow(dist, 0.74f) + tuning.linear_gain * dist) * powerDrive * fovTaper) / smoothDiv;
    // Borne de gain critique (zeta >= 1.02) : elimine toute oscillation quel que soit dist ou strength
    const float maxDampedSpeed = (effectiveAccel * 0.24f) * dist;
    const float maxCapSpeed = (tuning.max_speed_px_s * resScale * std::pow(power, 0.45f)) / smoothDiv;
    const float desiredSpeed = std::min({ rawSpeed, maxDampedSpeed, maxCapSpeed }) * posGain;

    const float effectiveRamp = std::max(tuning.ramp_up_sec / std::pow(power, 0.32f), 0.020f);
    const float onsetAlpha = SmootherStep(static_cast<float>(flick_elapsed_sec_) / effectiveRamp);

    const float ratio = math::Clamp(dist / std::max(initial_flick_dist_, 25.0f * resScale), 0.0f, 1.0f);
    const float arcPowerScale = math::Clamp(1.0f / std::pow(power, 0.18f), 0.74f, 1.15f);
    const float arcShape = std::sin(math::kPi * ratio) + arc_asym_ * std::sin(2.0f * math::kPi * ratio);
    const float arcFactor = (dist > (9.0f * resScale)) ? (arc_bias_ * arcPowerScale * arcShape) : 0.0f;
    const float velPosX = (dirX - dirY * arcFactor) * desiredSpeed * onsetAlpha;
    const float velPosY = (dirY + dirX * arcFactor) * desiredSpeed * onsetAlpha;

    // 5. Feedforward analytique de vitesse ecran (regle du quotient sur la projection perspective)
    const CameraState& cam = latest_frame_.camera;
    const math::CameraSpacePoint cs = math::ToCameraSpace(tgt.aim_world, cam);
    float screenVelX = 0.0f;
    float screenVelY = 0.0f;
    if (cs.w > math::kNearPlaneW) {
        const float vpW = (cam.viewport_w > 1.0f) ? cam.viewport_w : viewWidth;
        const float vpH = (cam.viewport_h > 1.0f) ? cam.viewport_h : viewHeight;
        const float focalVp = ComputeFocalPx(cam, viewHeight);
        const float vXc = math::Dot(tgt.rel_vel, cam.right);
        const float vYc = math::Dot(tgt.rel_vel, cam.up);
        const float vW = math::Dot(tgt.rel_vel, cam.look);
        const float invW2 = 1.0f / (cs.w * cs.w);
        screenVelX = ((vXc * cs.w - cs.xc * vW) * invW2) * focalVp * (viewWidth / vpW);
        screenVelY = -((vYc * cs.w - cs.yc * vW) * invW2) * focalVp * (viewHeight / vpH);

        const float velMag = std::hypot(screenVelX, screenVelY);
        const float maxScreenVel = 1400.0f * resScale;
        if (velMag > maxScreenVel) {
            const float s = maxScreenVel / velMag;
            screenVelX *= s;
            screenVelY *= s;
        }
    }

    const float ffFalloff = 1.0f - SmoothStep((dist - debug_state_.fov_px * 0.45f) / std::max(debug_state_.fov_px * 0.35f, 1.0f));
    const float ffPower = math::Clamp(tuning.tracking_ff * (0.40f + 0.60f * std::pow(power, 0.65f)), 0.12f, 0.96f);
    const float ffGain = ffPower * onsetAlpha * ffFalloff * math::Clamp(config_.velocity_lead, 0.0f, 1.5f);
    const float visScale = tgt.in_vis_grace ? 0.35f : 1.0f;

    const float velDesX = (velPosX + screenVelX * ffGain) * assist_authority_ * visScale;
    const float velDesY = (velPosY + screenVelY * ffGain) * assist_authority_ * visScale;

    // 6. Inertie musculaire du 2e ordre (courbe C1 fluide)
    const float velAlpha = math::ComputeExpLerpAlpha(effectiveAccel, dt);
    cur_vel_x_ = math::Lerp(cur_vel_x_, velDesX, velAlpha);
    cur_vel_y_ = math::Lerp(cur_vel_y_, velDesY, velAlpha);

    // 7. Micro-tremblement physiologique multi-octave (depend du signal moteur)
    noise_phase_1_ += dt * 14.4f;  // ~2.3 Hz
    noise_phase_2_ += dt * 60.9f;  // ~9.7 Hz
    noise_phase_3_ += dt * 23.2f;  // ~3.7 Hz
    if (noise_phase_1_ > 628.3f) noise_phase_1_ -= 628.3f;
    if (noise_phase_2_ > 628.3f) noise_phase_2_ -= 628.3f;
    if (noise_phase_3_ > 628.3f) noise_phase_3_ -= 628.3f;

    const float curSpeedMag = std::hypot(cur_vel_x_, cur_vel_y_);
    const float tremorGain = onsetAlpha * posGain * assist_authority_ * visScale;
    const float tremorAmp = (tuning.tremor_base_px_s * resScale + tuning.tremor_vel_ratio * curSpeedMag) * tremorGain;
    const float tremorX = (0.65f * std::sin(noise_phase_1_) + 0.35f * std::cos(noise_phase_2_)) * tremorAmp;
    const float tremorY = (0.65f * std::cos(noise_phase_3_) + 0.35f * std::sin(noise_phase_2_ * 0.93f)) * tremorAmp;

    // 8. Conversion pixels -> counts souris avec accumulation sous-pixel
    const float movePxX = (cur_vel_x_ + tremorX) * dt;
    const float movePxY = (cur_vel_y_ + tremorY) * dt;

    rem_x_ += movePxX / std::max(px_per_count_x_, 0.10f);
    rem_y_ += movePxY / std::max(px_per_count_y_, 0.10f);

    const int dx = std::clamp(static_cast<int>(std::lround(rem_x_)), -64, 64);
    const int dy = std::clamp(static_cast<int>(std::lround(rem_y_)), -64, 64);
    rem_x_ -= static_cast<float>(dx);
    rem_y_ -= static_cast<float>(dy);

    if (dx != 0 || dy != 0) {
        recent_inputs_.push_back({ nowSec, dx, dy });
    }

    out.dx = dx;
    out.dy = dy;
    out.has_target = true;
    out.actively_moving = (dx != 0 || dy != 0);
    out.target_id = tgt.id;
    return out;
}

bool AimAssistEngine::WantsHighRateLoop() const {
    return wants_high_rate_;
}

} // namespace nah2::aim
