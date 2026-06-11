#pragma once

#include <string>
#include <cstdint>

namespace turbine_monitor {

constexpr uint8_t  TURBINE_COUNT          = 6;
constexpr uint8_t  HYDROPHONE_COUNT       = 12;
constexpr uint8_t  ACCELEROMETER_COUNT    = 8;
constexpr uint8_t  BLADE_COUNT            = 15;
constexpr uint32_t SAMPLE_RATE_HYDROPHONE = 51200;
constexpr uint32_t SAMPLE_RATE_ACCEL      = 25600;
constexpr uint32_t SAMPLES_PER_MS_HYDRO   = 52;
constexpr uint32_t SAMPLES_PER_MS_ACCEL   = 26;
constexpr uint16_t UDP_PORT               = 9000;
constexpr uint32_t UDP_BUFFER_SIZE        = 65536;
constexpr uint32_t PROCESS_THREAD_COUNT   = 8;
constexpr uint32_t BATCH_WRITE_SIZE       = 1000;

struct Config {
    std::string clickhouse_host = "127.0.0.1";
    uint16_t    clickhouse_port = 8123;
    std::string clickhouse_user = "default";
    std::string clickhouse_pass = "";
    std::string clickhouse_db   = "turbine_monitor";

    uint16_t    udp_port        = UDP_PORT;
    std::string udp_host        = "0.0.0.0";

    uint16_t    api_port        = 8080;
    std::string api_host        = "0.0.0.0";

    std::string iec61850_server = "127.0.0.1";
    uint16_t    iec61850_port   = 102;

    float       cavitation_threshold_warning  = 0.3f;
    float       cavitation_threshold_critical = 0.6f;
    float       vibration_threshold_warning   = 3.0f;
    float       vibration_threshold_critical  = 5.0f;
    float       life_threshold_critical       = 0.8f;

    uint32_t    feature_extraction_interval_ms = 100;
    uint32_t    cavitation_detection_interval_ms = 100;
    uint32_t    life_assessment_interval_ms    = 1000;
    uint32_t    alarm_check_interval_ms        = 500;

    int         wavelet_decomposition_level    = 5;
    std::string wavelet_basis                  = "db4";

    std::string autoencoder_model_path         = "models/autoencoder.onnx";
    std::string isolation_forest_model_path    = "models/isolation_forest.json";

    bool        enable_iec61850_push           = false;
    bool        enable_autoencoder             = true;
    bool        enable_isolation_forest        = true;
};

Config& getConfig();
bool loadConfig(const std::string& configFile);

}
