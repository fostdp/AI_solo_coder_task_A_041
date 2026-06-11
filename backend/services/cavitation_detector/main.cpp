#include <iostream>
#include <memory>
#include <csignal>
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>
#include <mutex>

#include "../../include/config.h"
#include "../../include/cavitation_detector.h"
#include "../../include/clickhouse_client.h"
#include "../../../common/include/ipc_queue.h"
#include "../../../common/include/service_base.h"

using namespace turbine_monitor;

std::atomic<bool> g_running(true);

void signalHandler(int) {
    std::cout << "\n[cavitation_detector] Received shutdown signal" << std::endl;
    g_running = false;
}

class CavitationDetectorService : public ServiceBase {
public:
    CavitationDetectorService() : ServiceBase("cavitation_detector", 2) {}

    bool init(const Config& config) override {
        config_ = config;

        cavitationDetector_ = std::make_unique<CavitationDetector>(
            config_.enable_autoencoder,
            config_.enable_isolation_forest,
            config_.autoencoder_model_path,
            config_.isolation_forest_model_path);

        if (!cavitationDetector_->loadModels()) {
            std::cerr << "[" << name_ << "] Warning: Model load skipped, using defaults" << std::endl;
        }
        cavitationDetector_->setThresholds(
            config_.cavitation_threshold_warning * 0.5f,
            config_.cavitation_threshold_warning,
            config_.cavitation_threshold_critical);

        clickhouseClient_ = std::make_unique<ClickHouseClient>(
            config_.clickhouse_host, config_.clickhouse_port,
            config_.clickhouse_user, config_.clickhouse_pass,
            config_.clickhouse_db);

        featuresQueue_ = std::make_unique<SharedMemorySPSC<IPCMessageFeatures>>(
            IPCChannel::FEATURES, IPC_DEFAULT_CAPACITY, false);
        if (!featuresQueue_->open()) {
            std::cerr << "[" << name_ << "] Failed to open FEATURES IPC (consumer)" << std::endl;
            return false;
        }

        cavitationQueue_ = std::make_unique<SharedMemorySPSC<IPCMessageCavitation>>(
            IPCChannel::CAVITATION, IPC_DEFAULT_CAPACITY, true);
        if (!cavitationQueue_->open()) {
            std::cerr << "[" << name_ << "] Failed to open CAVITATION IPC (producer)" << std::endl;
            return false;
        }

        bladeStateCache_.resize((TURBINE_COUNT + 1) * (BLADE_COUNT + 1));
        return true;
    }

    bool start() override {
        if (!clickhouseClient_->connect()) {
            std::cerr << "[" << name_ << "] ClickHouse connect failed, running without DB" << std::endl;
        }
        clickhouseClient_->startBatchWriter(BATCH_WRITE_SIZE, 1000);

        detectorThread_ = std::thread([this]() { this->detectorLoop(); });
        statsThread_ = std::thread([this]() { this->statsLoop(); });

        std::cout << "========================================" << std::endl;
        std::cout << "[cavitation_detector] Started successfully" << std::endl;
        std::cout << "  IPC FEATURES -> channel 1 (consumer)" << std::endl;
        std::cout << "  IPC CAVITATION -> channel 2 (producer)" << std::endl;
        std::cout << "  AutoEncoder: " << (config_.enable_autoencoder ? "ON" : "OFF") << std::endl;
        std::cout << "  IsolationForest: " << (config_.enable_isolation_forest ? "ON" : "OFF") << std::endl;
        std::cout << "========================================" << std::endl;

        return ServiceBase::start();
    }

    void stop() override {
        ServiceBase::stop();
        if (detectorThread_.joinable()) detectorThread_.join();
        if (statsThread_.joinable()) statsThread_.join();
        clickhouseClient_->stopBatchWriter();
        clickhouseClient_->disconnect();
        featuresQueue_->close();
        cavitationQueue_->close();
    }

    void join() override {
        while (running_ && g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        stop();
    }

private:
    void detectorLoop() {
        IPCMessageFeatures featMsg{};

        while (g_running && running_) {
            size_t processedInBatch = 0;
            const size_t maxBatch = 500;

            while (processedInBatch < maxBatch &&
                   featuresQueue_->pop(featMsg) && g_running) {
                SpectrumFeatures spectrum{};
                WaveletFeatures wavelet{};

                spectrum.timestamp = featMsg.timestamp;
                spectrum.turbine_id = featMsg.turbine_id;
                spectrum.sensor_type = static_cast<SensorType>(featMsg.sensor_type);
                spectrum.sensor_id = featMsg.sensor_id;
                spectrum.blade_id = featMsg.blade_id;
                spectrum.peak_frequency = featMsg.spectrum[0];
                spectrum.rms_value = featMsg.spectrum[1];
                spectrum.crest_factor = featMsg.spectrum[2];
                spectrum.kurtosis = featMsg.spectrum[3];
                spectrum.skewness = featMsg.spectrum[4];
                spectrum.band_energy_low = featMsg.spectrum[5];
                spectrum.band_energy_mid = featMsg.spectrum[6];
                spectrum.band_energy_high = featMsg.spectrum[7];
                spectrum.harmonic_ratio = featMsg.spectrum[8];
                spectrum.spectral_centroid = featMsg.spectrum[9];
                spectrum.spectral_bandwidth = featMsg.spectrum[10];

                wavelet.timestamp = featMsg.timestamp;
                wavelet.turbine_id = featMsg.turbine_id;
                wavelet.sensor_id = featMsg.sensor_id;
                wavelet.blade_id = featMsg.blade_id;
                wavelet.wavelet_basis = "db4";
                wavelet.decomposition_level = 5;
                wavelet.band_energy.assign(featMsg.wavelet_energy, featMsg.wavelet_energy + 16);
                wavelet.energy_entropy = featMsg.wavelet_entropy;
                wavelet.total_energy = featMsg.wavelet_total;
                wavelet.energy_ratio.resize(16, 0.0f);
                if (wavelet.total_energy > 0) {
                    for (size_t i = 0; i < 16; ++i) {
                        wavelet.energy_ratio[i] = wavelet.band_energy[i] / wavelet.total_energy;
                    }
                }

                OperatingCondition condition;
                condition.load_percent = 70.0f;
                condition.head = 120.0f;

                CavitationState cavState = cavitationDetector_->detect(
                    spectrum, wavelet, featMsg.timestamp,
                    featMsg.turbine_id, featMsg.blade_id, condition);

                clickhouseClient_->insertCavitationState(cavState);

                IPCMessageCavitation cavMsg{};
                cavMsg.timestamp = cavState.timestamp;
                cavMsg.turbine_id = cavState.turbine_id;
                cavMsg.blade_id = cavState.blade_id;
                cavMsg.cavitation_stage = static_cast<uint8_t>(cavState.cavitation_stage);
                cavMsg.model_type = static_cast<uint8_t>(cavState.model_type);
                cavMsg.cavitation_intensity = cavState.cavitation_intensity;
                cavMsg.confidence = cavState.confidence;
                cavMsg.anomaly_score = cavState.anomaly_score;
                cavMsg.reconstruction_error = cavState.reconstruction_error;
                const size_t fvCount = std::min(cavState.feature_vector.size(), size_t(32));
                for (size_t i = 0; i < fvCount; ++i) {
                    cavMsg.feature_vector[i] = cavState.feature_vector[i];
                }

                if (!cavitationQueue_->push(cavMsg)) {
                    std::lock_guard<std::mutex> lk(statsMutex_);
                    stats_.cavitationDropped++;
                }

                if (featMsg.turbine_id > 0 && featMsg.blade_id > 0 &&
                    featMsg.turbine_id <= TURBINE_COUNT && featMsg.blade_id <= BLADE_COUNT) {
                    bladeStateCache_[(featMsg.turbine_id - 1) * BLADE_COUNT + (featMsg.blade_id - 1)] = cavState;
                }

                {
                    std::lock_guard<std::mutex> lk(statsMutex_);
                    stats_.featuresConsumed++;
                    stats_.cavitationProduced++;
                }
                processedInBatch++;
            }

            if (processedInBatch == 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
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

            auto featStats = featuresQueue_->getStats();
            auto cavStats = cavitationQueue_->getStats();

            std::lock_guard<std::mutex> lk(statsMutex_);
            std::cout << "\r[cavitation_detector] "
                      << "in=" << stats_.featuresConsumed
                      << " | out=" << stats_.cavitationProduced
                      << " | feat_q=" << featStats.currentSize
                      << " | cav_q=" << cavStats.currentSize
                      << " | drop=" << stats_.cavitationDropped
                      << std::flush;
        }
    }

    Config config_;
    std::unique_ptr<CavitationDetector> cavitationDetector_;
    std::unique_ptr<ClickHouseClient> clickhouseClient_;
    std::unique_ptr<SharedMemorySPSC<IPCMessageFeatures>> featuresQueue_;
    std::unique_ptr<SharedMemorySPSC<IPCMessageCavitation>> cavitationQueue_;
    std::vector<CavitationState> bladeStateCache_;

    std::thread detectorThread_;
    std::thread statsThread_;

    struct ServiceStats {
        uint64_t featuresConsumed = 0;
        uint64_t cavitationProduced = 0;
        uint64_t cavitationDropped = 0;
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
        std::cerr << "[cavitation_detector] Warning: Failed to load config, using defaults" << std::endl;
    }

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    const Config& cfg = getConfig();

    auto service = std::make_unique<CavitationDetectorService>();
    if (!service->init(cfg)) {
        std::cerr << "[cavitation_detector] Init failed" << std::endl;
        return 1;
    }

    if (!service->start()) {
        std::cerr << "[cavitation_detector] Start failed" << std::endl;
        return 1;
    }

    service->join();
    std::cout << std::endl << "[cavitation_detector] Shutdown complete" << std::endl;
    return 0;
}
