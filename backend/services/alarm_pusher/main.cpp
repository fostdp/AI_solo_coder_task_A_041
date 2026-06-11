#include <iostream>
#include <memory>
#include <csignal>
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>
#include <mutex>
#include <unordered_map>

#include "../../include/config.h"
#include "../../include/alarm_manager.h"
#include "../../include/clickhouse_client.h"
#include "../../../common/include/ipc_queue.h"
#include "../../../common/include/service_base.h"

using namespace turbine_monitor;

std::atomic<bool> g_running(true);

void signalHandler(int) {
    std::cout << "\n[alarm_pusher] Received shutdown signal" << std::endl;
    g_running = false;
}

class AlarmPusherService : public ServiceBase {
public:
    AlarmPusherService() : ServiceBase("alarm_pusher", 4) {}

    bool init(const Config& config) override {
        config_ = config;

        ThresholdConfig thresholds{
            config_.cavitation_threshold_warning,
            config_.cavitation_threshold_critical,
            config_.vibration_threshold_warning,
            config_.vibration_threshold_critical,
            config_.life_threshold_critical
        };

        alarmManager_ = std::make_unique<AlarmManager>(
            thresholds,
            config_.enable_iec61850_push,
            config_.iec61850_server,
            config_.iec61850_port);

        clickhouseClient_ = std::make_unique<ClickHouseClient>(
            config_.clickhouse_host, config_.clickhouse_port,
            config_.clickhouse_user, config_.clickhouse_pass,
            config_.clickhouse_db);

        cavitationQueue_ = std::make_unique<SharedMemorySPSC<IPCMessageCavitation>>(
            IPCChannel::CAVITATION, IPC_DEFAULT_CAPACITY, false);
        if (!cavitationQueue_->open()) {
            std::cerr << "[" << name_ << "] Failed to open CAVITATION IPC" << std::endl;
            return false;
        }

        stressQueue_ = std::make_unique<SharedMemorySPSC<IPCMessageStress>>(
            IPCChannel::STRESS, IPC_DEFAULT_CAPACITY, false);
        if (!stressQueue_->open()) {
            std::cerr << "[" << name_ << "] Failed to open STRESS IPC" << std::endl;
            return false;
        }

        lifeQueue_ = std::make_unique<SharedMemorySPSC<IPCMessageLife>>(
            IPCChannel::LIFE, IPC_DEFAULT_CAPACITY, false);
        if (!lifeQueue_->open()) {
            std::cerr << "[" << name_ << "] Failed to open LIFE IPC" << std::endl;
            return false;
        }

        alarmQueue_ = std::make_unique<SharedMemorySPSC<IPCMessageAlarm>>(
            IPCChannel::ALARM, IPC_DEFAULT_CAPACITY, true);
        if (!alarmQueue_->open()) {
            std::cerr << "[" << name_ << "] Failed to open ALARM IPC" << std::endl;
            return false;
        }

        cooldownUntil_.resize((TURBINE_COUNT + 1) * (BLADE_COUNT + 1), 0);
        return true;
    }

    bool start() override {
        if (!clickhouseClient_->connect()) {
            std::cerr << "[" << name_ << "] ClickHouse connect failed" << std::endl;
        }
        clickhouseClient_->startBatchWriter(BATCH_WRITE_SIZE, 1000);

        alarmManager_->setClickHouseWriter([this](const AlarmLog& alarm) {
            clickhouseClient_->insertAlarmLog(alarm);
            IPCMessageAlarm msg{};
            msg.timestamp = alarm.timestamp;
            msg.turbine_id = alarm.turbine_id;
            msg.blade_id = alarm.blade_id;
            msg.alarm_type = static_cast<uint8_t>(alarm.alarm_type);
            msg.alarm_level = static_cast<uint8_t>(alarm.alarm_level);
            msg.acknowledged = alarm.acknowledged ? 1 : 0;
            msg.iec61850_pushed = alarm.iec61850_pushed ? 1 : 0;
            msg.acknowledged_at = alarm.acknowledged_at;
            msg.threshold_value = alarm.threshold_value;
            msg.actual_value = alarm.actual_value;
            strncpy_s(msg.alarm_id, sizeof(msg.alarm_id), alarm.alarm_id.c_str(), alarm.alarm_id.size());
            strncpy_s(msg.alarm_message, sizeof(msg.alarm_message), alarm.alarm_message.c_str(), alarm.alarm_message.size());
            strncpy_s(msg.maintenance_suggestion, sizeof(msg.maintenance_suggestion),
                      alarm.maintenance_suggestion.c_str(), alarm.maintenance_suggestion.size());
            strncpy_s(msg.acknowledged_by, sizeof(msg.acknowledged_by),
                      alarm.acknowledged_by.c_str(), alarm.acknowledged_by.size());
            alarmQueue_->push(msg);
        });

        alarmManager_->start(config_.alarm_check_interval_ms);

        cavThread_ = std::thread([this]() { this->cavitationLoop(); });
        stressThread_ = std::thread([this]() { this->stressLoop(); });
        lifeThread_ = std::thread([this]() { this->lifeLoop(); });
        statsThread_ = std::thread([this]() { this->statsLoop(); });

        std::cout << "========================================" << std::endl;
        std::cout << "[alarm_pusher] Started successfully" << std::endl;
        std::cout << "  IPC CAVITATION -> channel 2 (consumer)" << std::endl;
        std::cout << "  IPC STRESS -> channel 3 (consumer)" << std::endl;
        std::cout << "  IPC LIFE -> channel 4 (consumer)" << std::endl;
        std::cout << "  IPC ALARM -> channel 5 (producer)" << std::endl;
        std::cout << "  IEC61850: " << (config_.enable_iec61850_push ? "ON" : "OFF") << std::endl;
        std::cout << "========================================" << std::endl;

        return ServiceBase::start();
    }

    void stop() override {
        ServiceBase::stop();
        alarmManager_->stop();
        if (cavThread_.joinable()) cavThread_.join();
        if (stressThread_.joinable()) stressThread_.join();
        if (lifeThread_.joinable()) lifeThread_.join();
        if (statsThread_.joinable()) statsThread_.join();
        clickhouseClient_->stopBatchWriter();
        clickhouseClient_->disconnect();
        cavitationQueue_->close();
        stressQueue_->close();
        lifeQueue_->close();
        alarmQueue_->close();
    }

    void join() override {
        while (running_ && g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        stop();
    }

private:
    void cavitationLoop() {
        IPCMessageCavitation msg{};
        while (g_running && running_) {
            size_t processed = 0;
            while (processed < 300 && cavitationQueue_->pop(msg) && g_running) {
                processed++;
                {
                    std::lock_guard<std::mutex> lk(statsMutex_);
                    stats_.cavitationConsumed++;
                }

                if (msg.cavitation_intensity < config_.cavitation_threshold_warning) continue;
                size_t key = (msg.turbine_id - 1) * BLADE_COUNT + (msg.blade_id - 1);
                uint64_t now = currentTimestampMs();
                if (now < cooldownUntil_[key]) continue;

                AlarmType type = AlarmType::CAVITATION_EXCEED;
                AlarmLevel level;
                float threshold;
                if (msg.cavitation_intensity >= config_.cavitation_threshold_critical) {
                    level = AlarmLevel::EMERGENCY;
                    threshold = config_.cavitation_threshold_critical;
                } else if (msg.cavitation_intensity >= config_.cavitation_threshold_warning) {
                    level = AlarmLevel::CRITICAL;
                    threshold = config_.cavitation_threshold_warning;
                } else {
                    level = AlarmLevel::WARNING;
                    threshold = config_.cavitation_threshold_warning;
                }

                AlarmLog alarm = alarmManager_->createAlarm(
                    msg.turbine_id, msg.blade_id, type, level,
                    threshold, msg.cavitation_intensity);
                alarmManager_->pushAlarm(alarm);
                cooldownUntil_[key] = now + config_.alarm_cooldown_ms;

                {
                    std::lock_guard<std::mutex> lk(statsMutex_);
                    stats_.cavitationAlarms++;
                }
            }
            if (processed == 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(500));
            }
        }
    }

    void stressLoop() {
        IPCMessageStress msg{};
        while (g_running && running_) {
            size_t processed = 0;
            while (processed < 300 && stressQueue_->pop(msg) && g_running) {
                processed++;
                {
                    std::lock_guard<std::mutex> lk(statsMutex_);
                    stats_.stressConsumed++;
                }

                float vib = msg.vibration_stress;
                if (vib < config_.vibration_threshold_warning) continue;
                size_t key = (msg.turbine_id - 1) * BLADE_COUNT + (msg.blade_id - 1) + 9999;
                uint64_t now = currentTimestampMs();
                if (now < cooldownUntil_[key % cooldownUntil_.size()]) continue;

                AlarmType type = AlarmType::VIBRATION_EXCEED;
                AlarmLevel level;
                float threshold;
                if (vib >= config_.vibration_threshold_critical) {
                    level = AlarmLevel::EMERGENCY;
                    threshold = config_.vibration_threshold_critical;
                } else {
                    level = AlarmLevel::CRITICAL;
                    threshold = config_.vibration_threshold_warning;
                }

                AlarmLog alarm = alarmManager_->createAlarm(
                    msg.turbine_id, msg.blade_id, type, level, threshold, vib);
                alarmManager_->pushAlarm(alarm);
                cooldownUntil_[key % cooldownUntil_.size()] = now + config_.alarm_cooldown_ms;

                {
                    std::lock_guard<std::mutex> lk(statsMutex_);
                    stats_.vibrationAlarms++;
                }
            }
            if (processed == 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(500));
            }
        }
    }

    void lifeLoop() {
        IPCMessageLife msg{};
        while (g_running && running_) {
            size_t processed = 0;
            while (processed < 100 && lifeQueue_->pop(msg) && g_running) {
                processed++;
                {
                    std::lock_guard<std::mutex> lk(statsMutex_);
                    stats_.lifeConsumed++;
                }

                float lifePct = msg.remaining_life_hours / config_.expected_life_hours * 100.0f;
                if (lifePct > config_.life_threshold_critical) continue;
                size_t key = (msg.turbine_id - 1) * BLADE_COUNT + (msg.blade_id - 1) + 5555;
                uint64_t now = currentTimestampMs();
                if (now < cooldownUntil_[key % cooldownUntil_.size()]) continue;

                AlarmLog alarm = alarmManager_->createAlarm(
                    msg.turbine_id, msg.blade_id,
                    AlarmType::LIFE_CRITICAL, AlarmLevel::CRITICAL,
                    config_.life_threshold_critical, lifePct);
                alarmManager_->pushAlarm(alarm);
                cooldownUntil_[key % cooldownUntil_.size()] = now + config_.alarm_cooldown_ms * 10;

                {
                    std::lock_guard<std::mutex> lk(statsMutex_);
                    stats_.lifeAlarms++;
                }
            }
            if (processed == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
    }

    void statsLoop() {
        uint64_t lastPrint = 0;
        while (g_running && running_) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            uint64_t now = currentTimestampMs();
            if (now - lastPrint < 5000) continue;
            lastPrint = now;

            auto alarmS = alarmQueue_->getStats();
            std::lock_guard<std::mutex> lk(statsMutex_);
            std::cout << "\r[alarm_pusher] "
                      << "cav_in=" << stats_.cavitationConsumed
                      << " | str_in=" << stats_.stressConsumed
                      << " | life_in=" << stats_.lifeConsumed
                      << " | cav_a=" << stats_.cavitationAlarms
                      << " | vib_a=" << stats_.vibrationAlarms
                      << " | life_a=" << stats_.lifeAlarms
                      << " | q=" << alarmS.currentSize
                      << std::flush;
        }
    }

    Config config_;
    std::unique_ptr<AlarmManager> alarmManager_;
    std::unique_ptr<ClickHouseClient> clickhouseClient_;

    std::unique_ptr<SharedMemorySPSC<IPCMessageCavitation>> cavitationQueue_;
    std::unique_ptr<SharedMemorySPSC<IPCMessageStress>> stressQueue_;
    std::unique_ptr<SharedMemorySPSC<IPCMessageLife>> lifeQueue_;
    std::unique_ptr<SharedMemorySPSC<IPCMessageAlarm>> alarmQueue_;

    std::vector<uint64_t> cooldownUntil_;

    std::thread cavThread_;
    std::thread stressThread_;
    std::thread lifeThread_;
    std::thread statsThread_;

    struct ServiceStats {
        uint64_t cavitationConsumed = 0;
        uint64_t stressConsumed = 0;
        uint64_t lifeConsumed = 0;
        uint64_t cavitationAlarms = 0;
        uint64_t vibrationAlarms = 0;
        uint64_t lifeAlarms = 0;
    };
    ServiceStats stats_;
    std::mutex statsMutex_;
};

int main(int argc, char* argv[]) {
    std::string configFile = "../../../config/config.json";
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "-c" || arg == "--config") && i + 1 < argc) {
            configFile = argv[++i];
        }
    }

    if (!loadConfig(configFile)) {
        std::cerr << "[alarm_pusher] Warning: Failed to load config, using defaults" << std::endl;
    }

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    const Config& cfg = getConfig();

    auto service = std::make_unique<AlarmPusherService>();
    if (!service->init(cfg)) {
        std::cerr << "[alarm_pusher] Init failed" << std::endl;
        return 1;
    }

    if (!service->start()) {
        std::cerr << "[alarm_pusher] Start failed" << std::endl;
        return 1;
    }

    service->join();
    std::cout << std::endl << "[alarm_pusher] Shutdown complete" << std::endl;
    return 0;
}
