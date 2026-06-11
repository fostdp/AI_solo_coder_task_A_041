#include <iostream>
#include <memory>
#include <csignal>
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>
#include <mutex>

#include "../../include/config.h"
#include "../../include/life_assessor.h"
#include "../../include/clickhouse_client.h"
#include "../../../common/include/ipc_queue.h"
#include "../../../common/include/service_base.h"

using namespace turbine_monitor;

std::atomic<bool> g_running(true);

void signalHandler(int) {
    std::cout << "\n[fatigue_evaluator] Received shutdown signal" << std::endl;
    g_running = false;
}

class FatigueEvaluatorService : public ServiceBase {
public:
    FatigueEvaluatorService() : ServiceBase("fatigue_evaluator", 3) {}

    bool init(const Config& config) override {
        config_ = config;

        lifeAssessor_ = std::make_unique<LifeAssessor>();
        MaterialProperties props;
        props.name = "13Cr4Ni";
        props.ultimateTensileStrength = 750.0f;
        props.fatigueLimit = 250.0f;
        props.fractureToughness = 60.0f;
        props.k = 5.0e-12f;
        props.m = 3.0f;
        lifeAssessor_->setMaterialProperties(props);
        lifeAssessor_->setExpectedLifeHours(config_.expected_life_hours);

        clickhouseClient_ = std::make_unique<ClickHouseClient>(
            config_.clickhouse_host, config_.clickhouse_port,
            config_.clickhouse_user, config_.clickhouse_pass,
            config_.clickhouse_db);

        rawQueue_ = std::make_unique<SharedMemorySPSC<IPCMessageRaw>>(
            IPCChannel::RAW_DATA, IPC_DEFAULT_CAPACITY, false);
        if (!rawQueue_->open()) {
            std::cerr << "[" << name_ << "] Failed to open RAW_DATA IPC (consumer)" << std::endl;
            return false;
        }

        cavitationQueue_ = std::make_unique<SharedMemorySPSC<IPCMessageCavitation>>(
            IPCChannel::CAVITATION, IPC_DEFAULT_CAPACITY, false);
        if (!cavitationQueue_->open()) {
            std::cerr << "[" << name_ << "] Failed to open CAVITATION IPC (consumer)" << std::endl;
            return false;
        }

        stressQueue_ = std::make_unique<SharedMemorySPSC<IPCMessageStress>>(
            IPCChannel::STRESS, IPC_DEFAULT_CAPACITY, true);
        if (!stressQueue_->open()) {
            std::cerr << "[" << name_ << "] Failed to open STRESS IPC (producer)" << std::endl;
            return false;
        }

        lifeQueue_ = std::make_unique<SharedMemorySPSC<IPCMessageLife>>(
            IPCChannel::LIFE, IPC_DEFAULT_CAPACITY, true);
        if (!lifeQueue_->open()) {
            std::cerr << "[" << name_ << "] Failed to open LIFE IPC (producer)" << std::endl;
            return false;
        }

        vibrationBuffer_.resize((TURBINE_COUNT + 1) * (BLADE_COUNT + 1));
        lastCavitationCache_.resize((TURBINE_COUNT + 1) * (BLADE_COUNT + 1));

        return true;
    }

    bool start() override {
        if (!clickhouseClient_->connect()) {
            std::cerr << "[" << name_ << "] ClickHouse connect failed, running without DB" << std::endl;
        }
        clickhouseClient_->startBatchWriter(BATCH_WRITE_SIZE, 1000);

        vibrationThread_ = std::thread([this]() { this->vibrationConsumeLoop(); });
        cavitationThread_ = std::thread([this]() { this->cavitationConsumeLoop(); });
        evaluateThread_ = std::thread([this]() { this->evaluateLoop(); });
        statsThread_ = std::thread([this]() { this->statsLoop(); });

        std::cout << "========================================" << std::endl;
        std::cout << "[fatigue_evaluator] Started successfully" << std::endl;
        std::cout << "  IPC RAW_DATA -> channel 0 (consumer)" << std::endl;
        std::cout << "  IPC CAVITATION -> channel 2 (consumer)" << std::endl;
        std::cout << "  IPC STRESS -> channel 3 (producer)" << std::endl;
        std::cout << "  IPC LIFE -> channel 4 (producer)" << std::endl;
        std::cout << "  Material: 13Cr4Ni, k=5e-12, m=3" << std::endl;
        std::cout << "========================================" << std::endl;

        return ServiceBase::start();
    }

    void stop() override {
        ServiceBase::stop();
        if (vibrationThread_.joinable()) vibrationThread_.join();
        if (cavitationThread_.joinable()) cavitationThread_.join();
        if (evaluateThread_.joinable()) evaluateThread_.join();
        if (statsThread_.joinable()) statsThread_.join();
        clickhouseClient_->stopBatchWriter();
        clickhouseClient_->disconnect();
        rawQueue_->close();
        cavitationQueue_->close();
        stressQueue_->close();
        lifeQueue_->close();
    }

    void join() override {
        while (running_ && g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        stop();
    }

private:
    void vibrationConsumeLoop() {
        IPCMessageRaw rawMsg{};
        while (g_running && running_) {
            size_t processed = 0;
            const size_t maxBatch = 500;
            while (processed < maxBatch && rawQueue_->pop(rawMsg) && g_running) {
                if (rawMsg.turbine_id > 0 && rawMsg.blade_id > 0 &&
                    rawMsg.turbine_id <= TURBINE_COUNT && rawMsg.blade_id <= BLADE_COUNT) {
                    size_t idx = (rawMsg.turbine_id - 1) * BLADE_COUNT + (rawMsg.blade_id - 1);
                    auto& buffer = vibrationBuffer_[idx];
                    buffer.insert(buffer.end(), rawMsg.data, rawMsg.data + rawMsg.sample_count);
                    if (buffer.size() > 4096) {
                        buffer.erase(buffer.begin(), buffer.begin() + buffer.size() - 4096);
                    }
                }
                {
                    std::lock_guard<std::mutex> lk(statsMutex_);
                    stats_.rawConsumed++;
                }
                processed++;
            }
            if (processed == 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        }
    }

    void cavitationConsumeLoop() {
        IPCMessageCavitation cavMsg{};
        while (g_running && running_) {
            size_t processed = 0;
            const size_t maxBatch = 200;
            while (processed < maxBatch && cavitationQueue_->pop(cavMsg) && g_running) {
                if (cavMsg.turbine_id > 0 && cavMsg.blade_id > 0 &&
                    cavMsg.turbine_id <= TURBINE_COUNT && cavMsg.blade_id <= BLADE_COUNT) {
                    size_t idx = (cavMsg.turbine_id - 1) * BLADE_COUNT + (cavMsg.blade_id - 1);
                    CavitationState state{};
                    state.timestamp = cavMsg.timestamp;
                    state.turbine_id = cavMsg.turbine_id;
                    state.blade_id = cavMsg.blade_id;
                    state.cavitation_stage = static_cast<CavitationStage>(cavMsg.cavitation_stage);
                    state.cavitation_intensity = cavMsg.cavitation_intensity;
                    state.confidence = cavMsg.confidence;
                    state.model_type = static_cast<ModelType>(cavMsg.model_type);
                    state.anomaly_score = cavMsg.anomaly_score;
                    state.reconstruction_error = cavMsg.reconstruction_error;
                    lastCavitationCache_[idx] = state;
                }
                {
                    std::lock_guard<std::mutex> lk(statsMutex_);
                    stats_.cavitationConsumed++;
                }
                processed++;
            }
            if (processed == 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(300));
            }
        }
    }

    void evaluateLoop() {
        uint64_t lastEvaluate = 0;
        while (g_running && running_) {
            uint64_t now = currentTimestampMs();
            if (now - lastEvaluate >= config_.life_assessment_interval_ms) {
                lastEvaluate = now;
                evaluateAllBlades(now);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    void evaluateAllBlades(uint64_t now) {
        for (uint8_t tid = 1; tid <= TURBINE_COUNT; ++tid) {
            for (uint8_t bid = 1; bid <= BLADE_COUNT; ++bid) {
                size_t idx = (tid - 1) * BLADE_COUNT + (bid - 1);
                auto& vib = vibrationBuffer_[idx];
                auto& cav = lastCavitationCache_[idx];

                if (vib.empty()) continue;

                std::vector<float> signal(vib.begin(), vib.end());
                std::vector<float> placeholder;
                CavitationState cavState = cav;
                cavState.timestamp = now;
                cavState.turbine_id = tid;
                cavState.blade_id = bid;

                BladeStress stress = lifeAssessor_->computeStress(
                    signal, cavState.cavitation_intensity, now, tid, bid);

                clickhouseClient_->insertBladeStress(stress);

                IPCMessageStress stressMsg{};
                stressMsg.timestamp = now;
                stressMsg.turbine_id = tid;
                stressMsg.blade_id = bid;
                stressMsg.mean_stress = stress.mean_stress;
                stressMsg.max_stress = stress.max_stress;
                stressMsg.min_stress = stress.min_stress;
                stressMsg.stress_amplitude = stress.stress_amplitude;
                stressMsg.combined_stress = stress.combined_stress;
                stressMsg.vibration_stress = stress.vibration_stress;
                stressMsg.cavitation_stress = stress.cavitation_stress;
                stressMsg.stress_cycles = stress.stress_cycles;

                if (!stressQueue_->push(stressMsg)) {
                    std::lock_guard<std::mutex> lk(statsMutex_);
                    stats_.stressDropped++;
                }

                LifeAssessment life = lifeAssessor_->assessLife(
                    stress, cavState, tid, bid);
                clickhouseClient_->insertLifeAssessment(life);

                IPCMessageLife lifeMsg{};
                lifeMsg.timestamp = now;
                lifeMsg.turbine_id = tid;
                lifeMsg.blade_id = bid;
                lifeMsg.cumulative_damage = life.cumulative_damage;
                lifeMsg.remaining_life_hours = life.remaining_life_hours;
                lifeMsg.remaining_life_days = life.remaining_life_days;
                lifeMsg.miner_sum = life.miner_sum;
                lifeMsg.fatigue_damage = life.fatigue_damage;
                lifeMsg.cavitation_damage = life.cavitation_damage;
                lifeMsg.material_constant_k = life.material_constant_k;
                lifeMsg.material_constant_m = life.material_constant_m;
                lifeMsg.stress_range = life.stress_range;
                lifeMsg.cycle_count = life.cycle_count;

                if (!lifeQueue_->push(lifeMsg)) {
                    std::lock_guard<std::mutex> lk(statsMutex_);
                    stats_.lifeDropped++;
                }

                {
                    std::lock_guard<std::mutex> lk(statsMutex_);
                    stats_.stressProduced++;
                    stats_.lifeProduced++;
                }
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

            auto stressS = stressQueue_->getStats();
            auto lifeS = lifeQueue_->getStats();

            std::lock_guard<std::mutex> lk(statsMutex_);
            std::cout << "\r[fatigue_evaluator] "
                      << "raw_in=" << stats_.rawConsumed
                      << " | cav_in=" << stats_.cavitationConsumed
                      << " | stress=" << stats_.stressProduced
                      << " | life=" << stats_.lifeProduced
                      << " | s_q=" << stressS.currentSize
                      << " | l_q=" << lifeS.currentSize
                      << " | drop=" << (stats_.stressDropped + stats_.lifeDropped)
                      << std::flush;
        }
    }

    Config config_;
    std::unique_ptr<LifeAssessor> lifeAssessor_;
    std::unique_ptr<ClickHouseClient> clickhouseClient_;

    std::unique_ptr<SharedMemorySPSC<IPCMessageRaw>> rawQueue_;
    std::unique_ptr<SharedMemorySPSC<IPCMessageCavitation>> cavitationQueue_;
    std::unique_ptr<SharedMemorySPSC<IPCMessageStress>> stressQueue_;
    std::unique_ptr<SharedMemorySPSC<IPCMessageLife>> lifeQueue_;

    std::vector<std::vector<float>> vibrationBuffer_;
    std::vector<CavitationState> lastCavitationCache_;

    std::thread vibrationThread_;
    std::thread cavitationThread_;
    std::thread evaluateThread_;
    std::thread statsThread_;

    struct ServiceStats {
        uint64_t rawConsumed = 0;
        uint64_t cavitationConsumed = 0;
        uint64_t stressProduced = 0;
        uint64_t lifeProduced = 0;
        uint64_t stressDropped = 0;
        uint64_t lifeDropped = 0;
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
        std::cerr << "[fatigue_evaluator] Warning: Failed to load config, using defaults" << std::endl;
    }

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    const Config& cfg = getConfig();

    auto service = std::make_unique<FatigueEvaluatorService>();
    if (!service->init(cfg)) {
        std::cerr << "[fatigue_evaluator] Init failed" << std::endl;
        return 1;
    }

    if (!service->start()) {
        std::cerr << "[fatigue_evaluator] Start failed" << std::endl;
        return 1;
    }

    service->join();
    std::cout << std::endl << "[fatigue_evaluator] Shutdown complete" << std::endl;
    return 0;
}
