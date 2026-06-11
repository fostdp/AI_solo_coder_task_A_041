#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <array>
#include <chrono>

namespace turbine_monitor {

enum class SensorType : uint8_t {
    HYDROPHONE    = 1,
    ACCELEROMETER = 2
};

enum class SensorPosition : uint8_t {
    SPIRAL_INLET  = 1,
    GUIDE_VANE    = 2,
    DRAFT_TUBE    = 3,
    RUNNER_BLADE  = 4
};

enum class CavitationStage : uint8_t {
    NORMAL      = 0,
    INCIPIENT   = 1,
    CRITICAL    = 2,
    DEVELOPED   = 3
};

enum class ModelType : uint8_t {
    AUTOENCODER      = 1,
    ISOLATION_FOREST = 2,
    ENSEMBLE         = 3
};

enum class AlarmType : uint8_t {
    CAVITATION_EXCEED = 1,
    VIBRATION_EXCEED  = 2,
    LIFE_CRITICAL     = 3,
    SENSOR_FAULT      = 4
};

enum class AlarmLevel : uint8_t {
    WARNING   = 1,
    CRITICAL  = 2,
    EMERGENCY = 3
};

struct RawSensorData {
    uint64_t                 timestamp;
    uint8_t                  turbine_id;
    SensorType               sensor_type;
    uint8_t                  sensor_id;
    SensorPosition           sensor_position;
    uint8_t                  blade_id;
    float                    amplitude;
    std::vector<float>       raw_data;
    uint32_t                 sample_rate;
    uint64_t                 batch_id;
};

struct SpectrumFeatures {
    uint64_t    timestamp;
    uint8_t     turbine_id;
    SensorType  sensor_type;
    uint8_t     sensor_id;
    uint8_t     blade_id;
    float       peak_frequency;
    float       rms_value;
    float       crest_factor;
    float       kurtosis;
    float       skewness;
    float       band_energy_low;
    float       band_energy_mid;
    float       band_energy_high;
    float       harmonic_ratio;
    float       spectral_centroid;
    float       spectral_bandwidth;
};

struct WaveletFeatures {
    uint64_t                 timestamp;
    uint8_t                  turbine_id;
    uint8_t                  sensor_id;
    uint8_t                  blade_id;
    std::string              wavelet_basis;
    uint8_t                  decomposition_level;
    std::vector<float>       band_energy;
    float                    energy_entropy;
    std::vector<float>       energy_ratio;
    float                    total_energy;
};

struct CavitationState {
    uint64_t                 timestamp;
    uint8_t                  turbine_id;
    uint8_t                  blade_id;
    CavitationStage          cavitation_stage;
    float                    cavitation_intensity;
    float                    confidence;
    ModelType                model_type;
    float                    anomaly_score;
    float                    reconstruction_error;
    std::vector<float>       feature_vector;
};

struct BladeStress {
    uint64_t                 timestamp;
    uint8_t                  turbine_id;
    uint8_t                  blade_id;
    float                    vibration_stress;
    float                    cavitation_stress;
    float                    combined_stress;
    uint32_t                 stress_cycles;
    float                    max_stress;
    float                    min_stress;
    float                    mean_stress;
    float                    stress_amplitude;
    std::vector<float>       rainflow_cycles;
};

struct LifeAssessment {
    uint64_t    timestamp;
    uint8_t     turbine_id;
    uint8_t     blade_id;
    float       cumulative_damage;
    float       remaining_life_hours;
    float       remaining_life_days;
    float       miner_sum;
    float       fatigue_damage;
    float       cavitation_damage;
    float       material_constant_k;
    float       material_constant_m;
    float       stress_range;
    uint32_t    cycle_count;
    std::string assessment_method;
};

struct AlarmLog {
    uint64_t    timestamp;
    std::string alarm_id;
    uint8_t     turbine_id;
    uint8_t     blade_id;
    AlarmType   alarm_type;
    AlarmLevel  alarm_level;
    std::string alarm_message;
    float       threshold_value;
    float       actual_value;
    bool        iec61850_pushed;
    bool        acknowledged;
    std::string maintenance_suggestion;
    uint64_t    acknowledged_at;
    std::string acknowledged_by;
};

struct TurbineConfig {
    uint8_t     turbine_id;
    std::string turbine_name;
    std::string turbine_type;
    float       rated_power;
    float       rated_head;
    float       rated_flow;
    float       rated_speed;
    uint8_t     blade_count;
    std::string material;
    float       ultimate_tensile_strength;
    float       fatigue_limit;
    float       fracture_toughness;
    float       cavitation_threshold;
    float       vibration_threshold;
    float       expected_life_hours;
};

struct SensorConfig {
    uint8_t         turbine_id;
    SensorType      sensor_type;
    uint8_t         sensor_id;
    SensorPosition  sensor_position;
    uint8_t         blade_id;
    float           calibration_factor;
    float           sensitivity;
    uint32_t        sampling_rate;
    float           range_min;
    float           range_max;
    std::chrono::system_clock::time_point install_date;
    std::chrono::system_clock::time_point last_calibration;
};

struct UDPDataPacket {
    uint64_t    timestamp;
    uint64_t    batch_id;
    uint8_t     turbine_id;
    uint8_t     sensor_type;
    uint8_t     sensor_id;
    uint8_t     sensor_position;
    uint8_t     blade_id;
    uint16_t    sample_count;
    uint32_t    sample_rate;
    float       amplitude;
    float       data[128];
} __attribute__((packed));

inline uint64_t currentTimestampMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

}
