#include "config.h"
#include <fstream>
#include <sstream>
#include <json/json.h>

namespace turbine_monitor {

static Config g_config;

Config& getConfig() {
    return g_config;
}

bool loadConfig(const std::string& configFile) {
    std::ifstream file(configFile);
    if (!file.is_open()) {
        return false;
    }

    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errs;
    if (!Json::parseFromStream(builder, file, &root, &errs)) {
        return false;
    }

    if (root.isMember("clickhouse")) {
        const auto& ch = root["clickhouse"];
        g_config.clickhouse_host = ch.get("host", "127.0.0.1").asString();
        g_config.clickhouse_port = ch.get("port", 8123).asUInt();
        g_config.clickhouse_user = ch.get("user", "default").asString();
        g_config.clickhouse_pass = ch.get("password", "").asString();
        g_config.clickhouse_db   = ch.get("database", "turbine_monitor").asString();
    }

    if (root.isMember("udp")) {
        const auto& udp = root["udp"];
        g_config.udp_host = udp.get("host", "0.0.0.0").asString();
        g_config.udp_port = udp.get("port", 9000).asUInt();
    }

    if (root.isMember("api")) {
        const auto& api = root["api"];
        g_config.api_host = api.get("host", "0.0.0.0").asString();
        g_config.api_port = api.get("port", 8080).asUInt();
    }

    if (root.isMember("iec61850")) {
        const auto& iec = root["iec61850"];
        g_config.iec61850_server = iec.get("server", "127.0.0.1").asString();
        g_config.iec61850_port   = iec.get("port", 102).asUInt();
        g_config.enable_iec61850_push = iec.get("enable_push", false).asBool();
    }

    if (root.isMember("thresholds")) {
        const auto& th = root["thresholds"];
        g_config.cavitation_threshold_warning  = th.get("cavitation_warning", 0.3f).asFloat();
        g_config.cavitation_threshold_critical = th.get("cavitation_critical", 0.6f).asFloat();
        g_config.vibration_threshold_warning   = th.get("vibration_warning", 3.0f).asFloat();
        g_config.vibration_threshold_critical  = th.get("vibration_critical", 5.0f).asFloat();
        g_config.life_threshold_critical       = th.get("life_critical", 0.8f).asFloat();
    }

    if (root.isMember("intervals")) {
        const auto& iv = root["intervals"];
        g_config.feature_extraction_interval_ms = iv.get("feature_extraction", 100).asUInt();
        g_config.cavitation_detection_interval_ms = iv.get("cavitation_detection", 100).asUInt();
        g_config.life_assessment_interval_ms    = iv.get("life_assessment", 1000).asUInt();
        g_config.alarm_check_interval_ms        = iv.get("alarm_check", 500).asUInt();
    }

    if (root.isMember("wavelet")) {
        const auto& wv = root["wavelet"];
        g_config.wavelet_decomposition_level = wv.get("decomposition_level", 5).asInt();
        g_config.wavelet_basis               = wv.get("basis", "db4").asString();
    }

    if (root.isMember("models")) {
        const auto& md = root["models"];
        g_config.autoencoder_model_path      = md.get("autoencoder_path", "models/autoencoder.onnx").asString();
        g_config.isolation_forest_model_path = md.get("isolation_forest_path", "models/isolation_forest.json").asString();
        g_config.enable_autoencoder          = md.get("enable_autoencoder", true).asBool();
        g_config.enable_isolation_forest     = md.get("enable_isolation_forest", true).asBool();
    }

    return true;
}

}
