#include "data_pipeline.h"
#include <iostream>
#include <algorithm>

namespace turbine_monitor {

DataPipeline::DataPipeline(const Config& config)
    : config_(config), running_(false),
      lastFeatureExtraction_(0),
      lastCavitationDetection_(0),
      lastLifeAssessment_(0) {

    memset(&stats_, 0, sizeof(stats_));

    udpServer_ = std::make_shared<UDPServer>(
        config.udp_host, config.udp_port, PROCESS_THREAD_COUNT);

    clickhouseClient_ = std::make_shared<ClickHouseClient>(
        config.clickhouse_host, config.clickhouse_port,
        config.clickhouse_user, config.clickhouse_pass,
        config.clickhouse_db);

    signalProcessor_ = std::make_shared<SignalProcessor>();

    cavitationDetector_ = std::make_shared<CavitationDetector>(
        config.enable_autoencoder,
        config.enable_isolation_forest,
        config.autoencoder_model_path,
        config.isolation_forest_model_path);

    lifeAssessor_ = std::make_shared<LifeAssessor>();

    ThresholdConfig alarmThresholds{
        config.cavitation_threshold_warning,
        config.cavitation_threshold_critical,
        config.vibration_threshold_warning,
        config.vibration_threshold_critical,
        config.life_threshold_critical
    };

    alarmManager_ = std::make_shared<AlarmManager>(
        alarmThresholds,
        config.enable_iec61850_push,
        config.iec61850_server,
        config.iec61850_port);

    apiServer_ = std::make_shared<APIServer>(
        config.api_host, config.api_port, shared_from_this());

    for (uint8_t i = 1; i <= TURBINE_COUNT; ++i) {
        TurbineConfig tc;
        tc.turbine_id = i;
        tc.turbine_name = std::to_string(i) + "号水轮机";
        tc.turbine_type = "Francis";
        tc.rated_power = 700.0f;
        tc.rated_head = 120.0f;
        tc.rated_flow = 650.0f;
        tc.rated_speed = 107.1f;
        tc.blade_count = BLADE_COUNT;
        tc.material = "13Cr4Ni";
        tc.ultimate_tensile_strength = 750.0f;
        tc.fatigue_limit = 250.0f;
        tc.fracture_toughness = 60.0f;
        tc.cavitation_threshold = config.cavitation_threshold_warning;
        tc.vibration_threshold = config.vibration_threshold_warning;
        tc.expected_life_hours = 200000.0f;
        turbineConfigs_.push_back(tc);
    }
}

DataPipeline::~DataPipeline() {
    stop();
}

bool DataPipeline::isRunning() const {
    return running_;
}

DataPipelineStats DataPipeline::getStats() {
    std::lock_guard<std::mutex> lock(statsMutex_);
    DataPipelineStats s = stats_;

    std::lock_guard<std::mutex> qLock(rawDataMutex_);
    s.queueSize = rawDataQueue_.size();

    return s;
}

void DataPipeline::resetStats() {
    std::lock_guard<std::mutex> lock(statsMutex_);
    memset(&stats_, 0, sizeof(stats_));
}

bool DataPipeline::start() {
    if (running_) return true;

    if (!clickhouseClient_->connect()) {
        std::cerr << "Failed to connect to ClickHouse" << std::endl;
        return false;
    }

    cavitationDetector_->loadModels();
    cavitationDetector_->setThresholds(0.3f, 0.6f, 0.8f);

    clickhouseClient_->startBatchWriter(BATCH_WRITE_SIZE, 1000);

    udpServer_->setDataCallback([this](const RawSensorData& data) {
        onDataReceived(data);
    });

    if (!udpServer_->start()) {
        std::cerr << "Failed to start UDP server" << std::endl;
        return false;
    }

    alarmManager_->setClickHouseWriter([this](const AlarmLog& alarm) {
        clickhouseClient_->insertAlarmLog(alarm);
    });

    alarmManager_->setAlarmCallback([this](const AlarmLog& alarm) {
        onAlarm(alarm);
    });

    alarmManager_->start(config_.alarm_check_interval_ms);

    if (!apiServer_->start()) {
        std::cerr << "Failed to start API server" << std::endl;
        return false;
    }

    running_ = true;

    featureExtractionThread_ = std::thread(&DataPipeline::featureExtractionLoop, this);
    cavitationDetectionThread_ = std::thread(&DataPipeline::cavitationDetectionLoop, this);
    lifeAssessmentThread_ = std::thread(&DataPipeline::lifeAssessmentLoop, this);

    std::cout << "Data pipeline started successfully" << std::endl;
    return true;
}

void DataPipeline::stop() {
    if (!running_) return;

    running_ = false;

    rawDataCond_.notify_all();
    featureCond_.notify_all();
    detectionCond_.notify_all();

    udpServer_->stop();
    alarmManager_->stop();
    apiServer_->stop();

    if (featureExtractionThread_.joinable()) {
        featureExtractionThread_.join();
    }
    if (cavitationDetectionThread_.joinable()) {
        cavitationDetectionThread_.join();
    }
    if (lifeAssessmentThread_.joinable()) {
        lifeAssessmentThread_.join();
    }

    clickhouseClient_->stopBatchWriter();
    clickhouseClient_->disconnect();

    std::cout << "Data pipeline stopped" << std::endl;
}

void DataPipeline::onDataReceived(const RawSensorData& data) {
    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        stats_.totalPacketsReceived++;
        stats_.totalBytesReceived += data.raw_data.size() * sizeof(float) + sizeof(RawSensorData);
    }

    {
        std::lock_guard<std::mutex> lock(rawDataMutex_);
        if (rawDataQueue_.size() > 100000) {
            rawDataQueue_.pop();
            std::lock_guard<std::mutex> sLock(statsMutex_);
            stats_.droppedPackets++;
            return;
        }
        rawDataQueue_.push(data);
        rawDataCond_.notify_one();
    }

    clickhouseClient_->insertRawData(data);

    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        stats_.rawDataInserted++;
    }
}

void DataPipeline::featureExtractionLoop() {
    while (running_) {
        std::unique_lock<std::mutex> lock(rawDataMutex_);
        rawDataCond_.wait_for(lock, std::chrono::milliseconds(10),
            [this]() { return !rawDataQueue_.empty() || !running_; });

        if (!running_ && rawDataQueue_.empty()) break;

        if (rawDataQueue_.empty()) continue;

        RawSensorData data = std::move(rawDataQueue_.front());
        rawDataQueue_.pop();
        lock.unlock();

        uint64_t now = currentTimestampMs();

        if (now - lastFeatureExtraction_ >= config_.feature_extraction_interval_ms) {
            processRawData(data);
            lastFeatureExtraction_ = now;
        }

        {
            std::lock_guard<std::mutex> sLock(statsMutex_);
            stats_.totalPacketsProcessed++;
            stats_.processingLatencyMs = now - data.timestamp;
        }
    }
}

void DataPipeline::processRawData(const RawSensorData& data) {
    if (data.raw_data.size() < 8) return;

    auto spectrum = signalProcessor_->extractSpectrumFeatures(
        data.raw_data, data.sample_rate,
        data.timestamp, data.turbine_id,
        data.sensor_type, data.sensor_id, data.blade_id);

    auto wavelet = signalProcessor_->extractWaveletFeatures(
        data.raw_data, data.timestamp,
        data.turbine_id, data.sensor_id, data.blade_id,
        config_.wavelet_basis, config_.wavelet_decomposition_level);

    clickhouseClient_->insertSpectrumFeatures(spectrum);
    clickhouseClient_->insertWaveletFeatures(wavelet);

    {
        std::lock_guard<std::mutex> sLock(statsMutex_);
        stats_.featuresExtracted++;
    }

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        currentSpectrum_[data.turbine_id][data.sensor_id] = spectrum;
    }

    {
        std::lock_guard<std::mutex> lock(featureMutex_);
        featureQueue_.push({
            data.timestamp,
            data.turbine_id,
            data.sensor_id,
            data.blade_id,
            data.sensor_type,
            spectrum,
            wavelet
        });
        featureCond_.notify_one();
    }
}

void DataPipeline::cavitationDetectionLoop() {
    while (running_) {
        std::unique_lock<std::mutex> lock(featureMutex_);
        featureCond_.wait_for(lock, std::chrono::milliseconds(10),
            [this]() { return !featureQueue_.empty() || !running_; });

        if (!running_ && featureQueue_.empty()) break;

        if (featureQueue_.empty()) continue;

        FeatureData data = std::move(featureQueue_.front());
        featureQueue_.pop();
        lock.unlock();

        uint64_t now = currentTimestampMs();

        if (now - lastCavitationDetection_ >= config_.cavitation_detection_interval_ms) {
            processFeatures(data.turbineId, data.sensorId, data.bladeId,
                           data.spectrum, data.wavelet);
            lastCavitationDetection_ = now;
        }
    }
}

void DataPipeline::processFeatures(uint8_t turbineId, uint8_t sensorId, uint8_t bladeId,
                                   const SpectrumFeatures& spectrum,
                                   const WaveletFeatures& wavelet) {
    if (bladeId == 0) return;

    CavitationState state = cavitationDetector_->detect(
        spectrum, wavelet, currentTimestampMs(), turbineId, bladeId);

    clickhouseClient_->insertCavitationState(state);

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        currentCavitation_[turbineId][bladeId] = state;
    }

    {
        std::lock_guard<std::mutex> sLock(statsMutex_);
        stats_.cavitationDetections++;
    }

    alarmManager_->checkCavitation(state);

    {
        std::lock_guard<std::mutex> lock(detectionMutex_);
        detectionQueue_.push({state.timestamp, turbineId, bladeId, state});
        detectionCond_.notify_one();
    }
}

void DataPipeline::lifeAssessmentLoop() {
    while (running_) {
        std::unique_lock<std::mutex> lock(detectionMutex_);
        detectionCond_.wait_for(lock, std::chrono::milliseconds(100),
            [this]() { return !detectionQueue_.empty() || !running_; });

        if (!running_ && detectionQueue_.empty()) break;

        if (detectionQueue_.empty()) continue;

        DetectionData data = std::move(detectionQueue_.front());
        detectionQueue_.pop();
        lock.unlock();

        uint64_t now = currentTimestampMs();

        if (now - lastLifeAssessment_ >= config_.life_assessment_interval_ms) {
            std::vector<float> vibrationSignal(256);
            for (auto& v : vibrationSignal) {
                v = static_cast<float>(rand()) / RAND_MAX * 2.0f - 1.0f;
            }

            BladeStress stress = lifeAssessor_->computeStress(
                vibrationSignal, data.cavitationState.cavitation_intensity,
                data.timestamp, data.turbineId, data.bladeId);

            clickhouseClient_->insertBladeStress(stress);

            {
                std::lock_guard<std::mutex> sLock(stateMutex_);
                currentStress_[data.turbineId][data.bladeId] = stress;
            }

            LifeAssessment assessment = lifeAssessor_->assessLife(
                stress, data.cavitationState, data.turbineId, data.bladeId);

            clickhouseClient_->insertLifeAssessment(assessment);

            {
                std::lock_guard<std::mutex> sLock(stateMutex_);
                currentLife_[data.turbineId][data.bladeId] = assessment;
            }

            {
                std::lock_guard<std::mutex> sLock(statsMutex_);
                stats_.lifeAssessments++;
            }

            alarmManager_->checkLife(assessment);

            lastLifeAssessment_ = now;
        }
    }
}

void DataPipeline::onAlarm(const AlarmLog& alarm) {
    std::lock_guard<std::mutex> lock(statsMutex_);
    stats_.alarmsGenerated++;

    std::cout << "[ALARM] " << alarm.alarm_message << std::endl;
}

std::vector<CavitationState> DataPipeline::getCurrentCavitationState(uint8_t turbineId) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    std::vector<CavitationState> result;

    auto it = currentCavitation_.find(turbineId);
    if (it != currentCavitation_.end()) {
        for (auto& [bladeId, state] : it->second) {
            result.push_back(state);
        }
    }

    return result;
}

std::vector<LifeAssessment> DataPipeline::getCurrentLifeAssessment(uint8_t turbineId) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    std::vector<LifeAssessment> result;

    auto it = currentLife_.find(turbineId);
    if (it != currentLife_.end()) {
        for (auto& [bladeId, assessment] : it->second) {
            result.push_back(assessment);
        }
    }

    return result;
}

std::vector<SpectrumFeatures> DataPipeline::getCurrentSpectrum(uint8_t turbineId) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    std::vector<SpectrumFeatures> result;

    auto it = currentSpectrum_.find(turbineId);
    if (it != currentSpectrum_.end()) {
        for (auto& [sensorId, spectrum] : it->second) {
            result.push_back(spectrum);
        }
    }

    return result;
}

std::vector<CavitationState> DataPipeline::getCavitationState(
    uint8_t turbineId, uint64_t startTime, uint64_t endTime) {
    return getCurrentCavitationState(turbineId);
}

std::vector<LifeAssessment> DataPipeline::getLifeAssessment(
    uint8_t turbineId, uint8_t bladeId, uint64_t startTime, uint64_t endTime) {

    std::lock_guard<std::mutex> lock(stateMutex_);
    std::vector<LifeAssessment> result;

    auto it = currentLife_.find(turbineId);
    if (it != currentLife_.end()) {
        if (bladeId == 0) {
            for (auto& [bId, assessment] : it->second) {
                result.push_back(assessment);
            }
        } else {
            auto bladeIt = it->second.find(bladeId);
            if (bladeIt != it->second.end()) {
                result.push_back(bladeIt->second);
            }
        }
    }

    return result;
}

std::vector<SpectrumFeatures> DataPipeline::getSpectrumData(
    uint8_t turbineId, uint8_t sensorId, uint64_t startTime, uint64_t endTime) {

    std::lock_guard<std::mutex> lock(stateMutex_);
    std::vector<SpectrumFeatures> result;

    auto it = currentSpectrum_.find(turbineId);
    if (it != currentLife_.end()) {
        if (sensorId == 0) {
            for (auto& [sId, spectrum] : it->second) {
                result.push_back(spectrum);
            }
        } else {
            auto sensorIt = it->second.find(sensorId);
            if (sensorIt != it->second.end()) {
                result.push_back(sensorIt->second);
            }
        }
    }

    return result;
}

std::vector<AlarmLog> DataPipeline::getActiveAlarms(uint8_t turbineId) {
    return alarmManager_->getActiveAlarms(turbineId);
}

std::vector<TurbineConfig> DataPipeline::getTurbineConfigs() {
    return turbineConfigs_;
}

bool DataPipeline::acknowledgeAlarm(const std::string& alarmId, const std::string& user) {
    alarmManager_->acknowledgeAlarm(alarmId, user);
    return clickhouseClient_->acknowledgeAlarm(alarmId, user);
}

bool DataPipeline::suppressAlarm(uint8_t turbineId, uint8_t bladeId,
                                 AlarmType type, uint32_t durationMs) {
    alarmManager_->suppressAlarm(turbineId, bladeId, type, durationMs);
    return true;
}

}
