#include "alarm_manager.h"
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <random>
#include <sstream>

namespace turbine_monitor {

IEC61850Client::IEC61850Client(const std::string& server, uint16_t port)
    : server_(server), port_(port), connected_(false) {
}

IEC61850Client::~IEC61850Client() {
    disconnect();
}

bool IEC61850Client::connect() {
    std::lock_guard<std::mutex> lock(mutex_);
    connected_ = true;
    return connected_;
}

void IEC61850Client::disconnect() {
    std::lock_guard<std::mutex> lock(mutex_);
    connected_ = false;
}

bool IEC61850Client::isConnected() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return connected_;
}

bool IEC61850Client::sendAlarm(const AlarmLog& alarm) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!connected_) return false;

    std::cout << "[IEC61850] Sending alarm: " << alarm.alarm_message
              << " to " << server_ << ":" << port_ << std::endl;

    return true;
}

AlarmManager::AlarmManager(const ThresholdConfig& thresholds,
                           bool enableIEC61850,
                           const std::string& iecServer,
                           uint16_t iecPort)
    : thresholds_(thresholds),
      enableIEC61850_(enableIEC61850),
      running_(false) {

    if (enableIEC61850_) {
        iecClient_ = std::make_unique<IEC61850Client>(iecServer, iecPort);
        iecClient_->connect();
    }
}

AlarmManager::~AlarmManager() {
    stop();
    if (iecClient_) {
        iecClient_->disconnect();
    }
}

void AlarmManager::start(uint32_t checkIntervalMs) {
    running_ = true;
    checkThread_ = std::thread(&AlarmManager::checkLoop, this, checkIntervalMs);
}

void AlarmManager::stop() {
    running_ = false;
    if (checkThread_.joinable()) {
        checkThread_.join();
    }
}

void AlarmManager::setAlarmCallback(AlarmCallback callback) {
    alarmCallback_ = std::move(callback);
}

void AlarmManager::setClickHouseWriter(std::function<void(const AlarmLog&)> writer) {
    clickhouseWriter_ = std::move(writer);
}

std::string AlarmManager::generateUUID() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, 15);

    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for (int i = 0; i < 8; ++i) ss << dis(gen);
    ss << "-";
    for (int i = 0; i < 4; ++i) ss << dis(gen);
    ss << "-4";
    for (int i = 0; i < 3; ++i) ss << dis(gen);
    ss << "-";
    ss << (dis(gen) & 0x3 | 0x8);
    for (int i = 0; i < 3; ++i) ss << dis(gen);
    ss << "-";
    for (int i = 0; i < 12; ++i) ss << dis(gen);

    return ss.str();
}

std::string AlarmManager::alarmTypeToString(AlarmType type) {
    switch (type) {
        case AlarmType::CAVITATION_EXCEED: return "cavitation_exceed";
        case AlarmType::VIBRATION_EXCEED:  return "vibration_exceed";
        case AlarmType::LIFE_CRITICAL:     return "life_critical";
        case AlarmType::SENSOR_FAULT:      return "sensor_fault";
    }
    return "unknown";
}

std::string AlarmManager::alarmLevelToString(AlarmLevel level) {
    switch (level) {
        case AlarmLevel::WARNING:   return "warning";
        case AlarmLevel::CRITICAL:  return "critical";
        case AlarmLevel::EMERGENCY: return "emergency";
    }
    return "unknown";
}

AlarmLevel AlarmManager::getAlarmLevel(AlarmType type, float actualValue) {
    switch (type) {
        case AlarmType::CAVITATION_EXCEED:
            if (actualValue >= thresholds_.cavitationCritical) return AlarmLevel::EMERGENCY;
            if (actualValue >= thresholds_.cavitationWarning)  return AlarmLevel::CRITICAL;
            return AlarmLevel::WARNING;

        case AlarmType::VIBRATION_EXCEED:
            if (actualValue >= thresholds_.vibrationCritical) return AlarmLevel::EMERGENCY;
            if (actualValue >= thresholds_.vibrationWarning)  return AlarmLevel::CRITICAL;
            return AlarmLevel::WARNING;

        case AlarmType::LIFE_CRITICAL:
            return AlarmLevel::CRITICAL;

        case AlarmType::SENSOR_FAULT:
            return AlarmLevel::WARNING;
    }
    return AlarmLevel::WARNING;
}

std::string AlarmManager::generateMaintenanceSuggestion(
    AlarmType type, AlarmLevel level, uint8_t turbineId, uint8_t bladeId) {

    std::stringstream ss;
    ss << "【" << (turbineId > 0 ? std::to_string(turbineId) + "号水轮机" : "系统")
       << (bladeId > 0 ? " " + std::to_string(bladeId) + "号叶片" : "") << "】";

    switch (type) {
        case AlarmType::CAVITATION_EXCEED:
            if (level == AlarmLevel::EMERGENCY) {
                ss << "空化强度严重超标，建议：1.立即降低负荷运行；2.检查导叶开度和运行水头；"
                   << "3.安排紧急停机检修，进行叶片表面补焊修复；4.检查尾水管压力脉动情况。";
            } else if (level == AlarmLevel::CRITICAL) {
                ss << "空化强度超标，建议：1.适当调整运行工况，避开空化高发区；"
                   << "2.优化导叶协联关系；3.近期安排停机检查叶片表面损伤情况；"
                   << "4.考虑进行补气装置运行优化。";
            } else {
                ss << "检测到空化初生迹象，建议：1.密切监测空化强度变化趋势；"
                   << "2.优化运行工况，避免在空化敏感区长期运行；3.做好检修计划准备。";
            }
            break;

        case AlarmType::VIBRATION_EXCEED:
            if (level == AlarmLevel::EMERGENCY) {
                ss << "振动严重超标，建议：1.立即停机检查；2.检查轴承间隙和轴系对中；"
                   << "3.检查转轮裂纹情况；4.检测基础松动情况。";
            } else if (level == AlarmLevel::CRITICAL) {
                ss << "振动超标，建议：1.降低负荷运行观察；2.检查动平衡情况；"
                   << "3.安排详细振动测试和分析；4.检查导叶间隙是否均匀。";
            } else {
                ss << "振动值偏高，建议：1.加强振动监测；2.检查轴承温度趋势；"
                   << "3.分析振动频谱特征判断振源。";
            }
            break;

        case AlarmType::LIFE_CRITICAL:
            ss << "叶片剩余寿命已达临界值，建议：1.立即评估叶片损伤程度；"
               << "2.制定详细检修方案，考虑叶片修复或更换；3.降低运行负荷，减少应力幅；"
               << "4.增加检测频次，密切监视裂纹扩展情况。";
            break;

        case AlarmType::SENSOR_FAULT:
            ss << "传感器工作异常，建议：1.检查传感器接线和供电；"
               << "2.进行传感器校准检查；3.必要时更换传感器。";
            break;
    }

    return ss.str();
}

AlarmLog AlarmManager::createAlarm(
    uint8_t turbineId, uint8_t bladeId,
    AlarmType type, AlarmLevel level,
    float threshold, float actual) {

    AlarmLog alarm{};
    alarm.timestamp = currentTimestampMs();
    alarm.alarm_id = generateUUID();
    alarm.turbine_id = turbineId;
    alarm.blade_id = bladeId;
    alarm.alarm_type = type;
    alarm.alarm_level = level;
    alarm.threshold_value = threshold;
    alarm.actual_value = actual;
    alarm.acknowledged = false;
    alarm.iec61850_pushed = false;
    alarm.acknowledged_at = 0;

    std::stringstream msg;
    msg << (turbineId > 0 ? std::to_string(turbineId) + "号水轮机" : "系统");
    if (bladeId > 0) msg << " " << std::to_string(bladeId) << "号叶片";
    msg << " " << alarmTypeToString(type)
        << " 实际值:" << std::fixed << std::setprecision(3) << actual
        << " 阈值:" << threshold;
    alarm.alarm_message = msg.str();

    alarm.maintenance_suggestion = generateMaintenanceSuggestion(type, level, turbineId, bladeId);

    return alarm;
}

void AlarmManager::checkCavitation(const CavitationState& state) {
    if (isAlarmSuppressed(state.turbine_id, state.blade_id, AlarmType::CAVITATION_EXCEED)) {
        return;
    }

    float intensity = state.cavitation_intensity;
    if (intensity >= thresholds_.cavitationWarning) {
        AlarmLevel level = getAlarmLevel(AlarmType::CAVITATION_EXCEED, intensity);
        float threshold = intensity >= thresholds_.cavitationCritical ?
            thresholds_.cavitationCritical : thresholds_.cavitationWarning;

        AlarmLog alarm = createAlarm(
            state.turbine_id, state.blade_id,
            AlarmType::CAVITATION_EXCEED, level,
            threshold, intensity);

        {
            std::lock_guard<std::mutex> lock(alarmsMutex_);
            activeAlarms_.push_back(alarm);
        }

        if (alarmCallback_) alarmCallback_(alarm);
        if (clickhouseWriter_) clickhouseWriter_(alarm);
        if (enableIEC61850_) pushToIEC61850(alarm);
    }
}

void AlarmManager::checkVibration(const SpectrumFeatures& features) {
    if (isAlarmSuppressed(features.turbine_id, features.blade_id, AlarmType::VIBRATION_EXCEED)) {
        return;
    }

    float rms = features.rms_value;
    if (rms >= thresholds_.vibrationWarning) {
        AlarmLevel level = getAlarmLevel(AlarmType::VIBRATION_EXCEED, rms);
        float threshold = rms >= thresholds_.vibrationCritical ?
            thresholds_.vibrationCritical : thresholds_.vibrationWarning;

        AlarmLog alarm = createAlarm(
            features.turbine_id, features.blade_id,
            AlarmType::VIBRATION_EXCEED, level,
            threshold, rms);

        {
            std::lock_guard<std::mutex> lock(alarmsMutex_);
            activeAlarms_.push_back(alarm);
        }

        if (alarmCallback_) alarmCallback_(alarm);
        if (clickhouseWriter_) clickhouseWriter_(alarm);
        if (enableIEC61850_) pushToIEC61850(alarm);
    }
}

void AlarmManager::checkLife(const LifeAssessment& assessment) {
    if (isAlarmSuppressed(assessment.turbine_id, assessment.blade_id, AlarmType::LIFE_CRITICAL)) {
        return;
    }

    if (assessment.cumulative_damage >= thresholds_.lifeCritical) {
        AlarmLog alarm = createAlarm(
            assessment.turbine_id, assessment.blade_id,
            AlarmType::LIFE_CRITICAL, AlarmLevel::CRITICAL,
            thresholds_.lifeCritical, assessment.cumulative_damage);

        {
            std::lock_guard<std::mutex> lock(alarmsMutex_);
            activeAlarms_.push_back(alarm);
        }

        if (alarmCallback_) alarmCallback_(alarm);
        if (clickhouseWriter_) clickhouseWriter_(alarm);
        if (enableIEC61850_) pushToIEC61850(alarm);
    }
}

void AlarmManager::acknowledgeAlarm(const std::string& alarmId, const std::string& user) {
    std::lock_guard<std::mutex> lock(alarmsMutex_);

    for (auto& alarm : activeAlarms_) {
        if (alarm.alarm_id == alarmId && !alarm.acknowledged) {
            alarm.acknowledged = true;
            alarm.acknowledged_at = currentTimestampMs();
            alarm.acknowledged_by = user;

            if (clickhouseWriter_) clickhouseWriter_(alarm);
            break;
        }
    }

    activeAlarms_.erase(
        std::remove_if(activeAlarms_.begin(), activeAlarms_.end(),
            [](const AlarmLog& a) { return a.acknowledged; }),
        activeAlarms_.end());
}

void AlarmManager::suppressAlarm(uint8_t turbineId, uint8_t bladeId,
                                 AlarmType type, uint32_t durationMs) {
    std::lock_guard<std::mutex> lock(alarmsMutex_);

    AlarmSuppression suppression{
        turbineId, bladeId, type,
        std::chrono::steady_clock::now() + std::chrono::milliseconds(durationMs)
    };

    suppressedAlarms_.erase(
        std::remove_if(suppressedAlarms_.begin(), suppressedAlarms_.end(),
            [turbineId, bladeId, type](const AlarmSuppression& s) {
                return s.turbineId == turbineId &&
                       s.bladeId == bladeId &&
                       s.alarmType == type;
            }),
        suppressedAlarms_.end());

    suppressedAlarms_.push_back(suppression);
}

bool AlarmManager::isAlarmSuppressed(uint8_t turbineId, uint8_t bladeId, AlarmType type) {
    std::lock_guard<std::mutex> lock(alarmsMutex_);

    auto now = std::chrono::steady_clock::now();
    for (const auto& s : suppressedAlarms_) {
        if (s.turbineId == turbineId &&
            s.bladeId == bladeId &&
            s.alarmType == type &&
            s.suppressUntil > now) {
            return true;
        }
    }
    return false;
}

void AlarmManager::cleanupExpiredSuppressions() {
    std::lock_guard<std::mutex> lock(alarmsMutex_);

    auto now = std::chrono::steady_clock::now();
    suppressedAlarms_.erase(
        std::remove_if(suppressedAlarms_.begin(), suppressedAlarms_.end(),
            [now](const AlarmSuppression& s) { return s.suppressUntil <= now; }),
        suppressedAlarms_.end());
}

void AlarmManager::pushToIEC61850(const AlarmLog& alarm) {
    if (!iecClient_ || !iecClient_->isConnected()) {
        const_cast<AlarmLog&>(alarm).iec61850_pushed = false;
        return;
    }

    bool success = iecClient_->sendAlarm(alarm);
    const_cast<AlarmLog&>(alarm).iec61850_pushed = success;
}

void AlarmManager::checkLoop(uint32_t intervalMs) {
    while (running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
        cleanupExpiredSuppressions();
    }
}

std::vector<AlarmLog> AlarmManager::getActiveAlarms(uint8_t turbineId) {
    std::lock_guard<std::mutex> lock(alarmsMutex_);

    std::vector<AlarmLog> result;
    for (const auto& alarm : activeAlarms_) {
        if (turbineId == 0 || alarm.turbine_id == turbineId) {
            result.push_back(alarm);
        }
    }
    return result;
}

std::vector<AlarmLog> AlarmManager::getAlarmHistory(
    uint64_t startTime, uint64_t endTime, uint8_t turbineId) {
    std::vector<AlarmLog> result;
    return result;
}

}
