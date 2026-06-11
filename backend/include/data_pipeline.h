#pragma once

#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <vector>
#include <functional>
#include <unordered_map>
#include "data_structures.h"
#include "udp_server.h"
#include "clickhouse_client.h"
#include "signal_processor.h"
#include "cavitation_detector.h"
#include "life_assessor.h"
#include "alarm_manager.h"
#include "api_server.h"

namespace turbine_monitor {

struct DataPipelineStats {
    uint64_t totalPacketsReceived;
    uint64_t totalPacketsProcessed;
    uint64_t totalBytesReceived;
    uint64_t rawDataInserted;
    uint64_t featuresExtracted;
    uint64_t cavitationDetections;
    uint64_t lifeAssessments;
    uint64_t alarmsGenerated;
    double   processingLatencyMs;
    uint64_t queueSize;
    uint64_t droppedPackets;
};

class DataPipeline : public DataProvider,
                     public std::enable_shared_from_this<DataPipeline> {
public:
    DataPipeline(const Config& config);
    ~DataPipeline();

    bool start();
    void stop();
    bool isRunning() const;

    DataPipelineStats getStats();
    void resetStats();

    std::vector<CavitationState> getCurrentCavitationState(uint8_t turbineId);
    std::vector<LifeAssessment> getCurrentLifeAssessment(uint8_t turbineId);
    std::vector<SpectrumFeatures> getCurrentSpectrum(uint8_t turbineId);

    std::vector<CavitationState> getCavitationState(
        uint8_t turbineId, uint64_t startTime = 0, uint64_t endTime = 0) override;

    std::vector<LifeAssessment> getLifeAssessment(
        uint8_t turbineId, uint8_t bladeId, uint64_t startTime = 0, uint64_t endTime = 0) override;

    std::vector<SpectrumFeatures> getSpectrumData(
        uint8_t turbineId, uint8_t sensorId, uint64_t startTime = 0, uint64_t endTime = 0) override;

    std::vector<AlarmLog> getActiveAlarms(uint8_t turbineId = 0) override;

    std::vector<TurbineConfig> getTurbineConfigs() override;

    bool acknowledgeAlarm(const std::string& alarmId, const std::string& user) override;

    bool suppressAlarm(uint8_t turbineId, uint8_t bladeId,
                        AlarmType type, uint32_t durationMs) override;

    void onAlarm(const AlarmLog& alarm);

private:
    void onDataReceived(const RawSensorData& data);
    void featureExtractionLoop();
    void cavitationDetectionLoop();
    void lifeAssessmentLoop();

    void processRawData(const RawSensorData& data);
    void processFeatures(uint8_t turbineId, uint8_t sensorId, uint8_t bladeId,
                         const SpectrumFeatures& spectrum, const WaveletFeatures& wavelet);

    std::shared_ptr<UDPServer> udpServer_;
    std::shared_ptr<ClickHouseClient> clickhouseClient_;
    std::shared_ptr<SignalProcessor> signalProcessor_;
    std::shared_ptr<CavitationDetector> cavitationDetector_;
    std::shared_ptr<LifeAssessor> lifeAssessor_;
    std::shared_ptr<AlarmManager> alarmManager_;
    std::shared_ptr<APIServer> apiServer_;

    std::atomic<bool> running_;

    std::thread featureExtractionThread_;
    std::thread cavitationDetectionThread_;
    std::thread lifeAssessmentThread_;

    std::queue<RawSensorData> rawDataQueue_;
    std::mutex rawDataMutex_;
    std::condition_variable rawDataCond_;

    struct FeatureData {
        uint64_t timestamp;
        uint8_t turbineId;
        uint8_t sensorId;
        uint8_t bladeId;
        SensorType sensorType;
        SpectrumFeatures spectrum;
        WaveletFeatures wavelet;
    };

    std::queue<FeatureData> featureQueue_;
    std::mutex featureMutex_;
    std::condition_variable featureCond_;

    struct DetectionData {
        uint64_t timestamp;
        uint8_t turbineId;
        uint8_t bladeId;
        CavitationState cavitationState;
    };

    std::queue<DetectionData> detectionQueue_;
    std::mutex detectionMutex_;
    std::condition_variable detectionCond_;

    std::unordered_map<uint8_t, std::unordered_map<uint8_t, CavitationState>> currentCavitation_;
    std::unordered_map<uint8_t, std::unordered_map<uint8_t, LifeAssessment>> currentLife_;
    std::unordered_map<uint8_t, std::unordered_map<uint8_t, SpectrumFeatures>> currentSpectrum_;
    std::unordered_map<uint8_t, std::unordered_map<uint8_t, BladeStress>> currentStress_;

    mutable std::mutex stateMutex_;

    DataPipelineStats stats_;
    mutable std::mutex statsMutex_;

    Config config_;

    uint64_t lastFeatureExtraction_;
    uint64_t lastCavitationDetection_;
    uint64_t lastLifeAssessment_;

    std::unordered_map<uint8_t, std::vector<RawSensorData>> rawDataBuffer_;
    std::mutex bufferMutex_;

    std::vector<TurbineConfig> turbineConfigs_;
};

}
