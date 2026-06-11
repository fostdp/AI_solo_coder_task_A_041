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
#include "../../include/data_structures.h"
#include "../../include/api_server.h"
#include "../../../common/include/ipc_queue.h"
#include "../../../common/include/service_base.h"

using namespace turbine_monitor;

std::atomic<bool> g_running(true);

void signalHandler(int) {
    std::cout << "\n[api_gateway] Received shutdown signal" << std::endl;
    g_running = false;
}

class GatewayDataProvider : public DataProvider {
public:
    bool init() {
        cavitationQueue_ = std::make_unique<SharedMemorySPSC<IPCMessageCavitation>>(
            IPCChannel::CAVITATION, IPC_DEFAULT_CAPACITY, false);
        lifeQueue_ = std::make_unique<SharedMemorySPSC<IPCMessageLife>>(
            IPCChannel::LIFE, IPC_DEFAULT_CAPACITY, false);
        alarmQueue_ = std::make_unique<SharedMemorySPSC<IPCMessageAlarm>>(
            IPCChannel::ALARM, IPC_DEFAULT_CAPACITY, false);

        if (!cavitationQueue_->open() || !lifeQueue_->open() || !alarmQueue_->open()) {
            std::cerr << "[GatewayDataProvider] Failed to open IPC queues" << std::endl;
            return false;
        }

        turbineConfigs_ = buildTurbineConfigs();
        running_ = true;
        pollThread_ = std::thread([this]() { this->pollIPC(); });
        return true;
    }

    void stop() {
        running_ = false;
        if (pollThread_.joinable()) pollThread_.join();
        if (cavitationQueue_) cavitationQueue_->close();
        if (lifeQueue_) lifeQueue_->close();
        if (alarmQueue_) alarmQueue_->close();
    }

    std::vector<TurbineConfig> getTurbineConfigs() const override {
        return turbineConfigs_;
    }

    std::vector<CavitationState> getCavitationState(uint8_t turbineId) const override {
        std::lock_guard<std::mutex> lk(dataMutex_);
        std::vector<CavitationState> result;
        for (uint8_t bid = 1; bid <= BLADE_COUNT; ++bid) {
            size_t idx = (turbineId - 1) * BLADE_COUNT + (bid - 1);
            if (idx < cavitationCache_.size()) {
                result.push_back(cavitationCache_[idx]);
            }
        }
        return result;
    }

    std::vector<LifeAssessment> getLifeAssessment(uint8_t turbineId, uint8_t bladeId) const override {
        std::lock_guard<std::mutex> lk(dataMutex_);
        std::vector<LifeAssessment> result;
        size_t idx = (turbineId - 1) * BLADE_COUNT + std::max(bladeId, uint8_t(1)) - 1;
        if (idx < lifeCache_.size()) {
            result.push_back(lifeCache_[idx]);
        }
        return result;
    }

    std::vector<AlarmLog> getActiveAlarms(uint8_t turbineId) const override {
        std::lock_guard<std::mutex> lk(dataMutex_);
        std::vector<AlarmLog> result;
        for (const auto& a : alarmCache_) {
            if (turbineId == 0 || a.turbine_id == turbineId) {
                if (!a.acknowledged) result.push_back(a);
            }
        }
        return result;
    }

    bool acknowledgeAlarm(const std::string& alarmId, const std::string& user) override {
        std::lock_guard<std::mutex> lk(dataMutex_);
        for (auto& a : alarmCache_) {
            if (a.alarm_id == alarmId) {
                a.acknowledged = true;
                a.acknowledged_at = currentTimestampMs();
                a.acknowledged_by = user;
                return true;
            }
        }
        return false;
    }

    bool suppressAlarm(const std::string& alarmId, uint32_t seconds) override {
        return acknowledgeAlarm(alarmId, "system-supress");
    }

private:
    void pollIPC() {
        while (running_ && g_running) {
            IPCMessageCavitation cavMsg{};
            size_t cCount = 0;
            while (cavitationQueue_ && cavitationQueue_->pop(cavMsg) && cCount < 500) {
                if (cavMsg.turbine_id > 0 && cavMsg.turbine_id <= TURBINE_COUNT &&
                    cavMsg.blade_id > 0 && cavMsg.blade_id <= BLADE_COUNT) {
                    size_t idx = (cavMsg.turbine_id - 1) * BLADE_COUNT + (cavMsg.blade_id - 1);
                    std::lock_guard<std::mutex> lk(dataMutex_);
                    if (idx >= cavitationCache_.size()) cavitationCache_.resize(idx + 1);
                    CavitationState& s = cavitationCache_[idx];
                    s.timestamp = cavMsg.timestamp;
                    s.turbine_id = cavMsg.turbine_id;
                    s.blade_id = cavMsg.blade_id;
                    s.cavitation_stage = static_cast<CavitationStage>(cavMsg.cavitation_stage);
                    s.cavitation_intensity = cavMsg.cavitation_intensity;
                    s.confidence = cavMsg.confidence;
                    s.model_type = static_cast<ModelType>(cavMsg.model_type);
                    s.anomaly_score = cavMsg.anomaly_score;
                    s.reconstruction_error = cavMsg.reconstruction_error;
                }
                cCount++;
            }

            IPCMessageLife lifeMsg{};
            size_t lCount = 0;
            while (lifeQueue_ && lifeQueue_->pop(lifeMsg) && lCount < 500) {
                if (lifeMsg.turbine_id > 0 && lifeMsg.turbine_id <= TURBINE_COUNT &&
                    lifeMsg.blade_id > 0 && lifeMsg.blade_id <= BLADE_COUNT) {
                    size_t idx = (lifeMsg.turbine_id - 1) * BLADE_COUNT + (lifeMsg.blade_id - 1);
                    std::lock_guard<std::mutex> lk(dataMutex_);
                    if (idx >= lifeCache_.size()) lifeCache_.resize(idx + 1);
                    LifeAssessment& l = lifeCache_[idx];
                    l.timestamp = lifeMsg.timestamp;
                    l.turbine_id = lifeMsg.turbine_id;
                    l.blade_id = lifeMsg.blade_id;
                    l.cumulative_damage = lifeMsg.cumulative_damage;
                    l.remaining_life_hours = lifeMsg.remaining_life_hours;
                    l.remaining_life_days = lifeMsg.remaining_life_days;
                    l.miner_sum = lifeMsg.miner_sum;
                    l.fatigue_damage = lifeMsg.fatigue_damage;
                    l.cavitation_damage = lifeMsg.cavitation_damage;
                }
                lCount++;
            }

            IPCMessageAlarm alarmMsg{};
            size_t aCount = 0;
            while (alarmQueue_ && alarmQueue_->pop(alarmMsg) && aCount < 100) {
                std::lock_guard<std::mutex> lk(dataMutex_);
                AlarmLog a;
                a.timestamp = alarmMsg.timestamp;
                a.alarm_id = alarmMsg.alarm_id;
                a.turbine_id = alarmMsg.turbine_id;
                a.blade_id = alarmMsg.blade_id;
                a.alarm_type = static_cast<AlarmType>(alarmMsg.alarm_type);
                a.alarm_level = static_cast<AlarmLevel>(alarmMsg.alarm_level);
                a.alarm_message = alarmMsg.alarm_message;
                a.threshold_value = alarmMsg.threshold_value;
                a.actual_value = alarmMsg.actual_value;
                a.iec61850_pushed = alarmMsg.iec61850_pushed > 0;
                a.acknowledged = alarmMsg.acknowledged > 0;
                a.maintenance_suggestion = alarmMsg.maintenance_suggestion;
                a.acknowledged_at = alarmMsg.acknowledged_at;
                a.acknowledged_by = alarmMsg.acknowledged_by;
                auto it = std::find_if(alarmCache_.begin(), alarmCache_.end(),
                    [&a](const AlarmLog& x) { return x.alarm_id == a.alarm_id; });
                if (it == alarmCache_.end()) {
                    alarmCache_.push_back(a);
                    if (alarmCache_.size() > 1000) alarmCache_.erase(alarmCache_.begin());
                }
                aCount++;
            }

            if (cCount == 0 && lCount == 0 && aCount == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
    }

    std::vector<TurbineConfig> buildTurbineConfigs() {
        std::vector<TurbineConfig> configs;
        for (uint8_t i = 1; i <= TURBINE_COUNT; ++i) {
            TurbineConfig tc{};
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
            tc.cavitation_threshold = 0.3f;
            tc.vibration_threshold = 0.3f;
            tc.expected_life_hours = 200000.0f;
            configs.push_back(tc);
        }
        return configs;
    }

    std::vector<TurbineConfig> turbineConfigs_;
    mutable std::mutex dataMutex_;
    std::vector<CavitationState> cavitationCache_;
    std::vector<LifeAssessment> lifeCache_;
    std::vector<AlarmLog> alarmCache_;

    std::unique_ptr<SharedMemorySPSC<IPCMessageCavitation>> cavitationQueue_;
    std::unique_ptr<SharedMemorySPSC<IPCMessageLife>> lifeQueue_;
    std::unique_ptr<SharedMemorySPSC<IPCMessageAlarm>> alarmQueue_;

    std::atomic<bool> running_{false};
    std::thread pollThread_;
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
        std::cerr << "[api_gateway] Warning: Failed to load config" << std::endl;
    }

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    const Config& cfg = getConfig();

    auto provider = std::make_shared<GatewayDataProvider>();
    if (!provider->init()) {
        std::cerr << "[api_gateway] Init data provider failed" << std::endl;
        return 1;
    }

    auto apiServer = std::make_unique<APIServer>(cfg.api_host, cfg.api_port, provider);
    if (!apiServer->start()) {
        std::cerr << "[api_gateway] API server start failed" << std::endl;
        return 1;
    }

    std::cout << "========================================" << std::endl;
    std::cout << "[api_gateway] Started successfully" << std::endl;
    std::cout << "  API: http://" << cfg.api_host << ":" << cfg.api_port << std::endl;
    std::cout << "  IPC CAVITATION -> channel 2 (consumer)" << std::endl;
    std::cout << "  IPC LIFE -> channel 4 (consumer)" << std::endl;
    std::cout << "  IPC ALARM -> channel 5 (consumer)" << std::endl;
    std::cout << "========================================" << std::endl;

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    provider->stop();
    apiServer->stop();
    std::cout << std::endl << "[api_gateway] Shutdown complete" << std::endl;
    return 0;
}
