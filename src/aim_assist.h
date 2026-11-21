#pragma once

#include "math_utils.h"
#include "telemetry_types.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace nah2::aim {

enum class AimProfile : uint8_t {
    UltraLegit = 0,
    Competitive = 1,
    Tracking = 2,
};

enum class AimBoneMode : uint8_t {
    Smart = 0,
    Head = 1,
    Neck = 2,
    Chest = 3,
};

enum class AimKeyMode : uint8_t {
    AnyMouse = 0,
    RightMouse = 1,
    LeftMouse = 2,
    Mouse4 = 3,
    Mouse5 = 4,
    LeftAlt = 5,
    LeftShift = 6,
    Always = 7,
};

struct AimConfig {
    bool enabled = true;
    AimProfile profile = AimProfile::UltraLegit;
    AimBoneMode bone_mode = AimBoneMode::Smart;
    AimKeyMode key_mode = AimKeyMode::AnyMouse;

    float fov_px = 140.0f;
    float strength = 1.0f;      // Puissance globale 0.10..2.00 (scaling non-lineaire humain)
    float smooth_scale = 1.0f;
    float deadzone_ratio = 0.30f;
    float velocity_lead = 0.85f;
    float sens_override = 0.0f; // > 0 force le multiplicateur de sensibilite de depart

    bool team_check = true;
    bool visibility_check = true;
    bool allow_unlocked_cursor = false;
    bool draw_fov_circle = true;
    bool draw_target_marker = true;
};

struct AimDebugState {
    bool enabled = false;
    bool actively_engaging = false;
    bool has_target = false;
    bool in_reaction_delay = false;
    bool draw_fov_circle = true;
    bool draw_target_marker = true;
    AimProfile profile = AimProfile::UltraLegit;
    uint32_t locked_entity_id = 0;
    std::string locked_name;
    Vec2 aim_origin{};
    Vec2 target_screen_pt{};
    float fov_px = 140.0f;
    float strength = 1.0f;
    float px_per_count_x = 2.0f;
    float px_per_count_y = 2.0f;
};

struct AimStepResult {
    int dx = 0;
    int dy = 0;
    bool aim_key_down = false;
    bool has_target = false;
    bool actively_moving = false;
    uint32_t target_id = 0;
};

const char* ProfileName(AimProfile p);
const char* BoneModeName(AimBoneMode b);
const char* KeyModeName(AimKeyMode k);
void ApplyProfileDefaults(AimConfig& cfg, AimProfile profile, bool preserveFov = false);

class AimAssistEngine {
public:
    explicit AimAssistEngine(const AimConfig& cfg = {});

    void SetConfig(const AimConfig& cfg);
    AimConfig& Config() { return config_; }
    const AimConfig& Config() const { return config_; }

    void CycleProfile();
    void Reseed(uint32_t seed);
    void ResetSession();

    // Appele des reception d'une nouvelle trame IPC pour mettre a jour velocites & calibration auto.
    void ObserveTelemetry(const TelemetryFrame& frame, double nowSec);

    // Appele sur evenement WM_INPUT materiel (hDevice != nullptr) pour synergie main humaine.
    void NotifyHardwareMouseDelta(int dx, int dy, double nowSec);

    // Avance la simulation moteur humaine sur dt (typiquement 2-4ms quand actif) et produit les deltas souris.
    AimStepResult Step(
        float dt,
        double nowSec,
        bool targetForeground,
        bool aimKeyPressed,
        bool systemCursorHidden,
        float viewWidth,
        float viewHeight
    );

    // Indique si la boucle principale doit utiliser une cadence haute frequence (2ms).
    bool WantsHighRateLoop() const;

    const AimDebugState& DebugState() const { return debug_state_; }

    // Permet aux tests unitaires de simuler la reponse camera a un delta injecte.
    float EstimatedPxPerCountX() const { return px_per_count_x_; }
    float EstimatedPxPerCountY() const { return px_per_count_y_; }

private:
    struct ProfileTuning {
        float fov_px = 140.0f;
        float base_gain = 11.5f;
        float linear_gain = 4.2f;
        float max_speed_px_s = 1750.0f;
        float accel_rate = 26.0f;
        float ramp_up_sec = 0.048f;
        float switch_delay_min_sec = 0.105f;
        float switch_delay_max_sec = 0.165f;
        float deadzone_ratio = 0.30f;
        float arc_max = 0.065f;
        float drift_amp_ratio = 0.28f;
        float tremor_base_px_s = 3.5f;
        float tremor_vel_ratio = 0.016f;
        float tracking_ff = 0.68f;
        float lead_sec = 0.040f;
        float overshoot_prob = 0.22f;
        float overshoot_scale = 0.045f;
        float head_preference = 0.85f;
    };

    struct TargetMotionState {
        Vec3 last_pos{};
        Vec3 vel{};
        Vec3 prev_vel{};
        Vec3 accel{};
        double last_frame_sec = -1.0;
        double last_seen_sec = -1.0;
        double last_visible_sec = -1.0;
        bool has_pos = false;
    };

    struct PendingInputSample {
        double time_sec = 0.0;
        int dx = 0;
        int dy = 0;
    };

    struct ObservedMotionSample {
        double time_sec = 0.0;
        float obs_px_x = 0.0f;
        float obs_px_y = 0.0f;
    };

    struct CandidateTarget {
        uint32_t id = 0;
        std::string name;
        Vec3 aim_world{};
        Vec3 rel_vel{};
        Vec2 screen_pt{};
        float depth_w = 0.0f;
        float dist_px = 0.0f;
        float hitbox_radius_px = 10.0f;
        float score = 1e9f;
        bool in_vis_grace = false;
    };

    ProfileTuning GetTuning() const;
    float NextRand01();
    float RandRange(float lo, float hi);

    void UpdateCalibration(const TelemetryFrame& frame, double nowSec);
    void UpdateTrackers(const TelemetryFrame& frame, double nowSec);
    bool SelectTarget(
        float viewWidth,
        float viewHeight,
        double nowSec,
        const Vec2& aimOrigin,
        CandidateTarget& outTarget
    ) const;

    Vec2 ResolveAimOrigin(float viewWidth, float viewHeight) const;
    Vec3 SelectAimBoneWorld(const EntityTelemetry& e, const Vec2& aimOrigin, float viewWidth, float viewHeight, float headPref) const;

    AimConfig config_{};
    AimDebugState debug_state_{};

    bool has_frame_ = false;
    TelemetryFrame latest_frame_{};
    uint64_t last_observed_seq_ = 0;
    double last_telemetry_sec_ = -1.0;

    bool has_prev_cam_ = false;
    CameraState prev_cam_{};
    Vec3 cam_vel_{};
    double prev_cam_sec_ = -1.0;

    std::unordered_map<uint32_t, TargetMotionState> trackers_;
    std::vector<PendingInputSample> recent_inputs_;
    std::vector<ObservedMotionSample> recent_cam_obs_;

    // Calibration en ligne pixels/count (adaptee automatiquement au jeu & FOV)
    float px_per_count_x_ = 2.0f;
    float px_per_count_y_ = 2.0f;
    float last_view_w_ = 0.0f;
    float last_view_h_ = 0.0f;
    bool sens_initialized_ = false;
    double last_calib_update_sec_ = -10.0;

    // Etat de poursuite / trajectoire humaine
    uint32_t locked_id_ = 0;
    double last_lock_active_sec_ = -10.0;
    double engage_after_sec_ = 0.0;
    double flick_elapsed_sec_ = 0.0;
    float initial_flick_dist_ = 0.0f;
    float arc_bias_ = 0.0f;
    float arc_asym_ = 0.0f;
    float overshoot_px_ = 0.0f;
    Vec2 overshoot_dir_{};

    // Derive organique (Ornstein-Uhlenbeck) dans la hitbox + bruit moteur double octave
    float drift_x_ = 0.0f;
    float drift_y_ = 0.0f;
    double next_drift_impulse_sec_ = 0.0;
    float drift_goal_x_ = 0.0f;
    float drift_goal_y_ = 0.0f;
    float noise_phase_1_ = 0.7f;
    float noise_phase_2_ = 2.1f;
    float noise_phase_3_ = 4.3f;

    // Etat dynamique du curseur / vitesse synthetisee + accumulateur sous-pixel
    float cur_vel_x_ = 0.0f;
    float cur_vel_y_ = 0.0f;
    float rem_x_ = 0.0f;
    float rem_y_ = 0.0f;

    // Synergie main humaine (WM_INPUT materiel)
    float human_vel_x_ = 0.0f;
    float human_vel_y_ = 0.0f;
    double last_human_input_sec_ = -10.0;
    float assist_authority_ = 1.0f;
    bool was_engaging_ = false;
    bool wants_high_rate_ = false;

    uint32_t rng_state_ = 0x9E3779B9u;
};

} // namespace nah2::aim
