#include "clickhouse_client.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <curl/curl.h>
#include <json/json.h>

namespace turbine_monitor {

static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* s) {
    size_t newLength = size * nmemb;
    try {
        s->append((char*)contents, newLength);
    } catch (std::bad_alloc& e) {
        return 0;
    }
    return newLength;
}

ClickHouseClient::ClickHouseClient(const std::string& host, uint16_t port,
                                   const std::string& user, const std::string& password,
                                   const std::string& database)
    : host_(host), port_(port), user_(user), password_(password),
      database_(database), connected_(false), writerRunning_(false),
      batchSize_(1000), flushIntervalMs_(1000) {
    curl_global_init(CURL_GLOBAL_ALL);
}

ClickHouseClient::~ClickHouseClient() {
    stopBatchWriter();
    disconnect();
    curl_global_cleanup();
}

bool ClickHouseClient::connect() {
    std::string query = "SELECT 1";
    connected_ = executeQuery(query);
    return connected_;
}

void ClickHouseClient::disconnect() {
    connected_ = false;
}

bool ClickHouseClient::executeQuery(const std::string& query) {
    std::lock_guard<std::mutex> lock(queryMutex_);

    CURL* curl = curl_easy_init();
    if (!curl) {
        return false;
    }

    std::string url = "http://" + host_ + ":" + std::to_string(port_) + "/?user=" + user_ +
                      "&password=" + password_ + "&database=" + database_;

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, query.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, query.size());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    return res == CURLE_OK;
}

std::string ClickHouseClient::formatArray(const std::vector<float>& arr) {
    std::stringstream ss;
    ss << "[";
    for (size_t i = 0; i < arr.size(); ++i) {
        if (i > 0) ss << ",";
        ss << std::fixed << std::setprecision(6) << arr[i];
    }
    ss << "]";
    return ss.str();
}

void ClickHouseClient::insertRawData(const RawSensorData& data) {
    std::stringstream ss;
    ss << "INSERT INTO raw_sensor_data VALUES ("
       << data.timestamp << ","
       << static_cast<uint32_t>(data.turbine_id) << ","
       << static_cast<uint32_t>(data.sensor_type) << ","
       << static_cast<uint32_t>(data.sensor_id) << ","
       << static_cast<uint32_t>(data.sensor_position) << ","
       << static_cast<uint32_t>(data.blade_id) << ","
       << std::fixed << std::setprecision(6) << data.amplitude << ","
       << formatArray(data.raw_data) << ","
       << data.sample_rate << ","
       << data.batch_id << ")";

    std::lock_guard<std::mutex> lock(queueMutex_);
    insertQueue_.push(ss.str());
    queueCond_.notify_one();
}

void ClickHouseClient::insertSpectrumFeatures(const SpectrumFeatures& f) {
    std::stringstream ss;
    ss << "INSERT INTO spectrum_features VALUES ("
       << f.timestamp << ","
       << static_cast<uint32_t>(f.turbine_id) << ","
       << static_cast<uint32_t>(f.sensor_type) << ","
       << static_cast<uint32_t>(f.sensor_id) << ","
       << static_cast<uint32_t>(f.blade_id) << ","
       << std::fixed << std::setprecision(6)
       << f.peak_frequency << "," << f.rms_value << "," << f.crest_factor << ","
       << f.kurtosis << "," << f.skewness << "," << f.band_energy_low << ","
       << f.band_energy_mid << "," << f.band_energy_high << "," << f.harmonic_ratio << ","
       << f.spectral_centroid << "," << f.spectral_bandwidth << ")";

    std::lock_guard<std::mutex> lock(queueMutex_);
    insertQueue_.push(ss.str());
    queueCond_.notify_one();
}

void ClickHouseClient::insertWaveletFeatures(const WaveletFeatures& f) {
    std::stringstream ss;
    ss << "INSERT INTO wavelet_features VALUES ("
       << f.timestamp << ","
       << static_cast<uint32_t>(f.turbine_id) << ","
       << static_cast<uint32_t>(f.sensor_id) << ","
       << static_cast<uint32_t>(f.blade_id) << ",'"
       << f.wavelet_basis << "',"
       << static_cast<uint32_t>(f.decomposition_level) << ","
       << formatArray(f.band_energy) << ","
       << std::fixed << std::setprecision(6) << f.energy_entropy << ","
       << formatArray(f.energy_ratio) << ","
       << f.total_energy << ")";

    std::lock_guard<std::mutex> lock(queueMutex_);
    insertQueue_.push(ss.str());
    queueCond_.notify_one();
}

void ClickHouseClient::insertCavitationState(const CavitationState& s) {
    std::stringstream ss;
    ss << "INSERT INTO cavitation_state VALUES ("
       << s.timestamp << ","
       << static_cast<uint32_t>(s.turbine_id) << ","
       << static_cast<uint32_t>(s.blade_id) << ","
       << static_cast<uint32_t>(s.cavitation_stage) << ","
       << std::fixed << std::setprecision(6) << s.cavitation_intensity << ","
       << s.confidence << ","
       << static_cast<uint32_t>(s.model_type) << ","
       << s.anomaly_score << ","
       << s.reconstruction_error << ","
       << formatArray(s.feature_vector) << ")";

    std::lock_guard<std::mutex> lock(queueMutex_);
    insertQueue_.push(ss.str());
    queueCond_.notify_one();
}

void ClickHouseClient::insertBladeStress(const BladeStress& s) {
    std::stringstream ss;
    ss << "INSERT INTO blade_stress VALUES ("
       << s.timestamp << ","
       << static_cast<uint32_t>(s.turbine_id) << ","
       << static_cast<uint32_t>(s.blade_id) << ","
       << std::fixed << std::setprecision(6)
       << s.vibration_stress << "," << s.cavitation_stress << "," << s.combined_stress << ","
       << s.stress_cycles << "," << s.max_stress << "," << s.min_stress << ","
       << s.mean_stress << "," << s.stress_amplitude << ","
       << formatArray(s.rainflow_cycles) << ")";

    std::lock_guard<std::mutex> lock(queueMutex_);
    insertQueue_.push(ss.str());
    queueCond_.notify_one();
}

void ClickHouseClient::insertLifeAssessment(const LifeAssessment& a) {
    std::stringstream ss;
    ss << "INSERT INTO life_assessment VALUES ("
       << a.timestamp << ","
       << static_cast<uint32_t>(a.turbine_id) << ","
       << static_cast<uint32_t>(a.blade_id) << ","
       << std::fixed << std::setprecision(6)
       << a.cumulative_damage << "," << a.remaining_life_hours << "," << a.remaining_life_days << ","
       << a.miner_sum << "," << a.fatigue_damage << "," << a.cavitation_damage << ","
       << a.material_constant_k << "," << a.material_constant_m << ","
       << a.stress_range << "," << a.cycle_count << ",'"
       << a.assessment_method << "')";

    std::lock_guard<std::mutex> lock(queueMutex_);
    insertQueue_.push(ss.str());
    queueCond_.notify_one();
}

void ClickHouseClient::insertAlarmLog(const AlarmLog& a) {
    std::stringstream ss;
    ss << "INSERT INTO alarm_logs VALUES ("
       << a.timestamp << ",'" << a.alarm_id << "',"
       << static_cast<uint32_t>(a.turbine_id) << ","
       << static_cast<uint32_t>(a.blade_id) << ","
       << static_cast<uint32_t>(a.alarm_type) << ","
       << static_cast<uint32_t>(a.alarm_level) << ",'"
       << a.alarm_message << "',"
       << std::fixed << std::setprecision(6) << a.threshold_value << ","
       << a.actual_value << ","
       << (a.iec61850_pushed ? 1 : 0) << ","
       << (a.acknowledged ? 1 : 0) << ",'"
       << a.maintenance_suggestion << "',"
       << a.acknowledged_at << ",'"
       << a.acknowledged_by << "')";

    std::lock_guard<std::mutex> lock(queueMutex_);
    insertQueue_.push(ss.str());
    queueCond_.notify_one();
}

void ClickHouseClient::batchWriterLoop() {
    while (writerRunning_) {
        std::vector<std::string> batch;
        batch.reserve(batchSize_);

        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            queueCond_.wait_for(lock, std::chrono::milliseconds(flushIntervalMs_),
                [this]() { return !insertQueue_.empty() || !writerRunning_; });

            while (!insertQueue_.empty() && batch.size() < batchSize_) {
                batch.push_back(std::move(insertQueue_.front()));
                insertQueue_.pop();
            }
        }

        if (!batch.empty()) {
            std::string combinedQuery;
            for (const auto& q : batch) {
                combinedQuery += q + ";\n";
            }
            executeQuery(combinedQuery);
        }
    }
}

void ClickHouseClient::startBatchWriter(uint32_t batchSize, uint32_t flushIntervalMs) {
    batchSize_ = batchSize;
    flushIntervalMs_ = flushIntervalMs;
    writerRunning_ = true;
    writerThread_ = std::thread(&ClickHouseClient::batchWriterLoop, this);
}

void ClickHouseClient::stopBatchWriter() {
    writerRunning_ = false;
    queueCond_.notify_all();
    if (writerThread_.joinable()) {
        writerThread_.join();
    }
    batchInsert();
}

void ClickHouseClient::batchInsert() {
    std::vector<std::string> batch;
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        while (!insertQueue_.empty()) {
            batch.push_back(std::move(insertQueue_.front()));
            insertQueue_.pop();
        }
    }

    if (!batch.empty()) {
        std::string combinedQuery;
        for (const auto& q : batch) {
            combinedQuery += q + ";\n";
        }
        executeQuery(combinedQuery);
    }
}

std::vector<CavitationState> ClickHouseClient::queryCavitationHistory(
    uint8_t turbineId, uint8_t bladeId, uint64_t startTime, uint64_t endTime) {
    std::vector<CavitationState> results;
    return results;
}

std::vector<LifeAssessment> ClickHouseClient::queryLifeHistory(
    uint8_t turbineId, uint8_t bladeId, uint64_t startTime, uint64_t endTime) {
    std::vector<LifeAssessment> results;
    return results;
}

std::vector<SpectrumFeatures> ClickHouseClient::querySpectrumHistory(
    uint8_t turbineId, uint8_t sensorId, uint64_t startTime, uint64_t endTime) {
    std::vector<SpectrumFeatures> results;
    return results;
}

std::vector<AlarmLog> ClickHouseClient::queryActiveAlarms(uint8_t turbineId) {
    std::vector<AlarmLog> results;
    return results;
}

bool ClickHouseClient::acknowledgeAlarm(const std::string& alarmId, const std::string& user) {
    std::stringstream ss;
    ss << "ALTER TABLE alarm_logs UPDATE acknowledged = 1, acknowledged_at = "
       << currentTimestampMs() << ", acknowledged_by = '" << user
       << "' WHERE alarm_id = '" << alarmId << "'";
    return executeQuery(ss.str());
}

}
