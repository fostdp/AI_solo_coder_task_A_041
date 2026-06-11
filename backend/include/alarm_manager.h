#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <memory>
#include <thread>
#include <chrono>
#include <functional>
#include "data_structures.h"

namespace turbine_monitor {

struct ThresholdConfig {
    float cavitationWarning;
    float cavitationCritical;
    float vibrationWarning;
    float vibrationCritical;
    float lifeCritical;
};

struct AlarmSuppression {
    uint8_t turbineId;
    uint8_t bladeId;
    AlarmType alarmType;
    std::chrono::steady_clock::time_point suppressUntil;
};

class IEC61850Client {
public:
    IEC61850Client(const std::string& server, uint16_t port);
    ~IEC61850Client();

    bool connect();
    void disconnect();
    bool sendAlarm(const AlarmLog& alarm);
    bool isConnected() const;

private:
    std::string server_;
    uint16_t port_;
    bool connected_;
    mutable std::mutex mutex_;
};

class AlarmManager {
public:
    using AlarmCallback = std::function<void(const AlarmLog&)>;

    AlarmManager(const ThresholdConfig& thresholds,
                 bool enableIEC61850 = false,
                 const std::string& iecServer = "",
                 uint16_t iecPort = 102);
    ~AlarmManager();

    void start(uint32_t checkIntervalMs = 500);
    void stop();

    void checkCavitation(const CavitationState& state);
    void checkVibration(const SpectrumFeatures& features);
    void checkLife(const LifeAssessment& assessment);

    AlarmLog createAlarm(
        uint8_t turbineId,
        uint8_t bladeId,
        AlarmType type,
        AlarmLevel level,
        float threshold,
        float actual);

    std::string generateMaintenanceSuggestion(
        AlarmType type,
        AlarmLevel level,
        uint8_t turbineId,
        uint8_t bladeId);

    void acknowledgeAlarm(const std::string& alarmId, const std::string& user);
    void suppressAlarm(uint8_t turbineId, uint8_t bladeId, AlarmType type, uint32_t durationMs);
    bool isAlarmSuppressed(uint8_t turbineId, uint8_t bladeId, AlarmType type);

    std::vector<AlarmLog> getActiveAlarms(uint8_t turbineId = 0);
    std::vector<AlarmLog> getAlarmHistory(uint64_t startTime, uint64_t endTime, uint8_t turbineId = 0);

    void setAlarmCallback(AlarmCallback callback);
    void setClickHouseWriter(std::function<void(const AlarmLog&)> writer);

private:
    ThresholdConfig thresholds_;
    std::unique_ptr<IEC61850Client> iecClient_;
    bool enableIEC61850_;
    std::atomic<bool> running_;
    std::thread checkThread_;

    std::vector<AlarmLog> activeAlarms_;
    std::vector<AlarmSuppression> suppressedAlarms_;
    std::mutex alarmsMutex_;

    AlarmCallback alarmCallback_;
    std::function<void(const AlarmLog&)> clickhouseWriter_;

    std::string generateUUID();
    std::string alarmTypeToString(AlarmType type);
    std::string alarmLevelToString(AlarmLevel level);
    AlarmLevel getAlarmLevel(AlarmType type, float actualValue);

    void checkLoop(uint32_t intervalMs);
    void cleanupExpiredSuppressions();
    void pushToIEC61850(const AlarmLog& alarm);
};

}
