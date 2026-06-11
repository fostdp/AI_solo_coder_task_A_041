#include <iostream>
#include <memory>
#include <csignal>
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>

#include "../../include/config.h"
#include "../../include/udp_server.h"
#include "../../include/signal_processor.h"
#include "../../include/clickhouse_client.h"
#include "../../../common/include/ipc_queue.h"
#include "../../../common/include/service_base.h"

using namespace turbine_monitor;

std::atomic<bool> g_running(true);

void signalHandler(int) {
    std::cout << "\n[pxi_collector] Received shutdown signal" << std::endl;
    g_running = false;
}

class PXICollectorService : public ServiceBase {
public:
    PXICollectorService() : ServiceBase("pxi_collector", 1) {}

    bool init(const Config& config) override {
        config_ = config;

        udpServer_ = std::make_unique<UDPServer>(
            config_.udp_host, config_.udp_port, PROCESS_THREAD_COUNT,
            UDPServer::ReceiveMode::RECVMMSG);

        signalProcessor_ = std::make_unique<SignalProcessor>();

        clickhouseClient_ = std::make_unique<ClickHouseClient>(
            config_.clickhouse_host, config_.clickhouse_port,
            config_.clickhouse_user, config_.clickhouse_pass,
            config_.clickhouse_db);

        rawQueue_ = std::make_unique<SharedMemorySPSC<IPCMessageRaw>>(
            IPCChannel::RAW_DATA, IPC_DEFAULT_CAPACITY, true);
        if (!rawQueue_->open()) {
            std::cerr << "[" << name_ << "] Failed to open RAW_DATA IPC" << std::endl;
            return false;
        }

        featuresQueue_ = std::make_unique<SharedMemorySPSC<IPCMessageFeatures>>(
            IPCChannel::FEATURES, IPC_DEFAULT_CAPACITY, true);
        if (!featuresQueue_->open()) {
            std::cerr << "[" << name_ << "] Failed to open FEATURES IPC" << std::endl;
            return false;
        }

        return true;
    }

    bool start() override {
        if (!clickhouseClient_->connect()) {
            std::cerr << "[" << name_ << "] ClickHouse connect failed, running without DB" << std::endl;
        }
        clickhouseClient_->startBatchWriter(BATCH_WRITE_SIZE, 1000);

        udpServer_->setDataCallback([this](const RawSensorData& data) {
            this->onDataReceived(data);
        });

        if (!udpServer_->start()) {
            std::cerr << "[" << name_ << "] UDP server start failed" << std::endl;
            return false;
        }

        statsThread_ = std::thread([this]() { this->statsLoop(); });

        std::cout << "========================================" << std::endl;
        std::cout << "[pxi_collector] Started successfully" << std::endl;
        std::cout << "  UDP: " << config_.udp_host << ":" << config_.udp_port << std::endl;
        std::cout << "  IPC RAW -> channel 0 (producer)" << std::endl;
        std::cout << "  IPC FEATURES -> channel 1 (producer)" << std::endl;
        std::cout << "========================================" << std::endl;

        return ServiceBase::start();
    }

    void stop() override {
        ServiceBase::stop();
        udpServer_->stop();
        clickhouseClient_->stopBatchWriter();
        clickhouseClient_->disconnect();
        if (statsThread_.joinable()) statsThread_.join();
        rawQueue_->close();
        featuresQueue_->close();
    }

    void join() override {
        while (running_ && g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        stop();
    }

private:
    void onDataReceived(const RawSensorData& data) {
        {
            std::lock_guard<std::mutex> lk(statsMutex_);
            stats_.totalPackets++;
            stats_.totalBytes += data.raw_data.size() * sizeof(float) + sizeof(RawSensorData);
        }

        clickhouseClient_->insertRawData(data);

        IPCMessageRaw rawMsg{};
        rawMsg.timestamp = data.timestamp;
        rawMsg.turbine_id = data.turbine_id;
        rawMsg.sensor_type = static_cast<uint8_t>(data.sensor_type);
        rawMsg.sensor_id = data.sensor_id;
        rawMsg.sensor_position = static_cast<uint8_t>(data.sensor_position);
        rawMsg.blade_id = data.blade_id;
        rawMsg.amplitude = data.amplitude;
        rawMsg.sample_rate = data.sample_rate;
        rawMsg.batch_id = data.batch_id;
        rawMsg.sample_count = static_cast<uint32_t>(data.raw_data.size());
        const size_t copyN = std::min(data.raw_data.size(), size_t(128));
        std::memcpy(rawMsg.data, data.raw_data.data(), copyN * sizeof(float));

        if (!rawQueue_->push(rawMsg)) {
            std::lock_guard<std::mutex> lk(statsMutex_);
            stats_.rawDropped++;
        }

        auto spectrum = signalProcessor_->extractSpectrumFeatures(
            data.raw_data, data.sample_rate, data.timestamp,
            data.turbine_id, data.sensor_type, data.sensor_id, data.blade_id);

        auto wavelet = signalProcessor_->extractWaveletFeatures(
            data.raw_data, data.sample_rate, data.timestamp,
            data.turbine_id, data.sensor_type, data.sensor_id, data.blade_id, 5);

        clickhouseClient_->insertSpectrumFeatures(spectrum);
        clickhouseClient_->insertWaveletFeatures(wavelet);

        IPCMessageFeatures featMsg{};
        featMsg.timestamp = data.timestamp;
        featMsg.turbine_id = data.turbine_id;
        featMsg.sensor_type = static_cast<uint8_t>(data.sensor_type);
        featMsg.sensor_id = data.sensor_id;
        featMsg.blade_id = data.blade_id;
        featMsg.rms_value = spectrum.rms_value;

        featMsg.spectrum[0] = spectrum.peak_frequency;
        featMsg.spectrum[1] = spectrum.rms_value;
        featMsg.spectrum[2] = spectrum.crest_factor;
        featMsg.spectrum[3] = spectrum.kurtosis;
        featMsg.spectrum[4] = spectrum.skewness;
        featMsg.spectrum[5] = spectrum.band_energy_low;
        featMsg.spectrum[6] = spectrum.band_energy_mid;
        featMsg.spectrum[7] = spectrum.band_energy_high;
        featMsg.spectrum[8] = spectrum.harmonic_ratio;
        featMsg.spectrum[9] = spectrum.spectral_centroid;
        featMsg.spectrum[10] = spectrum.spectral_bandwidth;

        const size_t wCount = std::min(wavelet.band_energy.size(), size_t(16));
        std::memcpy(featMsg.wavelet_energy, wavelet.band_energy.data(), wCount * sizeof(float));
        featMsg.wavelet_entropy = wavelet.energy_entropy;
        featMsg.wavelet_total = wavelet.total_energy;

        if (!featuresQueue_->push(featMsg)) {
            std::lock_guard<std::mutex> lk(statsMutex_);
            stats_.featuresDropped++;
        }

        {
            std::lock_guard<std::mutex> lk(statsMutex_);
            stats_.featuresExtracted++;
        }
    }

    void statsLoop() {
        uint64_t lastPrint = 0;
        while (g_running && running_) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            uint64_t now = currentTimestampMs();
            if (now - lastPrint < 5000) continue;
            lastPrint = now;

            std::lock_guard<std::mutex> lk(statsMutex_);
            auto rawStats = rawQueue_->getStats();
            auto featStats = featuresQueue_->getStats();

            std::cout << "\r[pxi_collector] "
                      << "pkts=" << stats_.totalPackets
                      << " | bytes=" << (stats_.totalBytes / 1048576) << "MB"
                      << " | features=" << stats_.featuresExtracted
                      << " | raw_q=" << rawStats.currentSize
                      << " | feat_q=" << featStats.currentSize
                      << " | dropped=" << (stats_.rawDropped + stats_.featuresDropped)
                      << std::flush;
        }
    }

    Config config_;
    std::unique_ptr<UDPServer> udpServer_;
    std::unique_ptr<SignalProcessor> signalProcessor_;
    std::unique_ptr<ClickHouseClient> clickhouseClient_;
    std::unique_ptr<SharedMemorySPSC<IPCMessageRaw>> rawQueue_;
    std::unique_ptr<SharedMemorySPSC<IPCMessageFeatures>> featuresQueue_;
    std::thread statsThread_;

    struct ServiceStats {
        uint64_t totalPackets = 0;
        uint64_t totalBytes = 0;
        uint64_t featuresExtracted = 0;
        uint64_t rawDropped = 0;
        uint64_t featuresDropped = 0;
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
        std::cerr << "[pxi_collector] Warning: Failed to load config, using defaults" << std::endl;
    }

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    const Config& cfg = getConfig();

    auto service = std::make_unique<PXICollectorService>();
    if (!service->init(cfg)) {
        std::cerr << "[pxi_collector] Init failed" << std::endl;
        return 1;
    }

    if (!service->start()) {
        std::cerr << "[pxi_collector] Start failed" << std::endl;
        return 1;
    }

    service->join();
    std::cout << std::endl << "[pxi_collector] Shutdown complete" << std::endl;
    return 0;
}
