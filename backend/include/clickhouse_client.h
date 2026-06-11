#pragma once

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <functional>
#include "data_structures.h"

namespace turbine_monitor {

class ClickHouseClient {
public:
    ClickHouseClient(const std::string& host, uint16_t port,
                     const std::string& user, const std::string& password,
                     const std::string& database);
    ~ClickHouseClient();

    bool connect();
    void disconnect();

    void insertRawData(const RawSensorData& data);
    void insertSpectrumFeatures(const SpectrumFeatures& features);
    void insertWaveletFeatures(const WaveletFeatures& features);
    void insertCavitationState(const CavitationState& state);
    void insertBladeStress(const BladeStress& stress);
    void insertLifeAssessment(const LifeAssessment& assessment);
    void insertAlarmLog(const AlarmLog& alarm);

    void batchInsert();
    void startBatchWriter(uint32_t batchSize = 1000, uint32_t flushIntervalMs = 1000);
    void stopBatchWriter();

    std::vector<CavitationState> queryCavitationHistory(
        uint8_t turbineId, uint8_t bladeId,
        uint64_t startTime, uint64_t endTime);

    std::vector<LifeAssessment> queryLifeHistory(
        uint8_t turbineId, uint8_t bladeId,
        uint64_t startTime, uint64_t endTime);

    std::vector<SpectrumFeatures> querySpectrumHistory(
        uint8_t turbineId, uint8_t sensorId,
        uint64_t startTime, uint64_t endTime);

    std::vector<AlarmLog> queryActiveAlarms(uint8_t turbineId = 0);

    bool acknowledgeAlarm(const std::string& alarmId, const std::string& user);

private:
    std::string buildInsertQuery(const std::string& table, const std::vector<std::string>& columns);
    std::string formatArray(const std::vector<float>& arr);
    bool executeQuery(const std::string& query);

    void batchWriterLoop();

    std::string host_;
    uint16_t    port_;
    std::string user_;
    std::string password_;
    std::string database_;
    bool        connected_;

    std::queue<std::string> insertQueue_;
    std::mutex              queueMutex_;
    std::condition_variable queueCond_;
    std::atomic<bool>       writerRunning_;
    std::thread             writerThread_;
    uint32_t                batchSize_;
    uint32_t                flushIntervalMs_;

    mutable std::mutex      queryMutex_;
};

}
