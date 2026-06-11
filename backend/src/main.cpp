#include <iostream>
#include <memory>
#include <csignal>
#include <thread>
#include <chrono>
#include "config.h"
#include "data_pipeline.h"

using namespace turbine_monitor;

std::atomic<bool> g_running(true);

void signalHandler(int signal) {
    std::cout << "\nReceived signal " << signal << ", shutting down..." << std::endl;
    g_running = false;
}

void printUsage(const char* program) {
    std::cout << "Usage: " << program << " [options]" << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  -c, --config <file>   Path to configuration file" << std::endl;
    std::cout << "  -h, --help            Show this help message" << std::endl;
}

int main(int argc, char* argv[]) {
    std::string configFile;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "-c" || arg == "--config") && i + 1 < argc) {
            configFile = argv[++i];
        } else if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return 0;
        }
    }

    if (!configFile.empty()) {
        if (!loadConfig(configFile)) {
            std::cerr << "Warning: Failed to load config file, using defaults" << std::endl;
        }
    }

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    const Config& config = getConfig();

    std::cout << "========================================" << std::endl;
    std::cout << "水轮机空化噪声监测与寿命评估系统" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "UDP Server:   " << config.udp_host << ":" << config.udp_port << std::endl;
    std::cout << "API Server:   " << config.api_host << ":" << config.api_port << std::endl;
    std::cout << "ClickHouse:   " << config.clickhouse_host << ":" << config.clickhouse_port << std::endl;
    std::cout << "IEC 61850:    " << (config.enable_iec61850_push ? "Enabled" : "Disabled") << std::endl;
    std::cout << "Turbines:     " << TURBINE_COUNT << std::endl;
    std::cout << "Hydrophones:  " << HYDROPHONE_COUNT << "/turbine" << std::endl;
    std::cout << "Accelerometers: " << ACCELEROMETER_COUNT << "/turbine" << std::endl;
    std::cout << "========================================" << std::endl;

    auto pipeline = std::make_shared<DataPipeline>(config);

    if (!pipeline->start()) {
        std::cerr << "Failed to start data pipeline" << std::endl;
        return 1;
    }

    std::cout << "System started. Press Ctrl+C to stop." << std::endl;

    uint64_t lastStatsPrint = 0;
    while (g_running && pipeline->isRunning()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        uint64_t now = currentTimestampMs();
        if (now - lastStatsPrint >= 5000) {
            DataPipelineStats stats = pipeline->getStats();
            std::cout << "\r[Stats] Recv: " << stats.totalPacketsReceived
                      << " | Processed: " << stats.totalPacketsProcessed
                      << " | Queue: " << stats.queueSize
                      << " | Latency: " << stats.processingLatencyMs << "ms"
                      << " | Dropped: " << stats.droppedPackets
                      << " | Alarms: " << stats.alarmsGenerated
                      << std::flush;
            lastStatsPrint = now;
        }
    }

    std::cout << std::endl << "Stopping system..." << std::endl;
    pipeline->stop();

    DataPipelineStats stats = pipeline->getStats();
    std::cout << "========================================" << std::endl;
    std::cout << "Final Statistics:" << std::endl;
    std::cout << "  Total packets received:  " << stats.totalPacketsReceived << std::endl;
    std::cout << "  Total packets processed: " << stats.totalPacketsProcessed << std::endl;
    std::cout << "  Total bytes received:    " << stats.totalBytesReceived << std::endl;
    std::cout << "  Raw data inserted:       " << stats.rawDataInserted << std::endl;
    std::cout << "  Features extracted:      " << stats.featuresExtracted << std::endl;
    std::cout << "  Cavitation detections:   " << stats.cavitationDetections << std::endl;
    std::cout << "  Life assessments:        " << stats.lifeAssessments << std::endl;
    std::cout << "  Alarms generated:        " << stats.alarmsGenerated << std::endl;
    std::cout << "  Packets dropped:         " << stats.droppedPackets << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "System shutdown complete." << std::endl;

    return 0;
}
