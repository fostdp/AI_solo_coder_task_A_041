#pragma once

#include <memory>
#include <string>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/fmt/fmt.h>
#include <prometheus/counter.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <prometheus/registry.h>
#include <prometheus/exposer.h>

namespace turbine_monitor {

class MetricsRegistry {
public:
    static MetricsRegistry& instance() {
        static MetricsRegistry inst;
        return inst;
    }

    void init(const std::string& bind_address = "0.0.0.0:9100") {
        exposer_ = std::make_unique<prometheus::Exposer>(bind_address);
        registry_ = std::make_shared<prometheus::Registry>();
        exposer_->RegisterCollectable(registry_);

        auto& counter_fam = prometheus::BuildCounter()
            .Name("turbine_packets_total")
            .Help("Total packets processed")
            .Register(*registry_);
        packets_total_ = &counter_fam.Add({});

        auto& dropped_fam = prometheus::BuildCounter()
            .Name("turbine_packets_dropped_total")
            .Help("Total packets dropped")
            .Register(*registry_);
        packets_dropped_ = &dropped_fam.Add({});

        auto& ipc_sent_fam = prometheus::BuildCounter()
            .Name("turbine_ipc_sent_total")
            .Help("Total IPC messages sent")
            .Register(*registry_);
        ipc_sent_total_ = &ipc_sent_fam.Add({});

        auto& ipc_recv_fam = prometheus::BuildCounter()
            .Name("turbine_ipc_received_total")
            .Help("Total IPC messages received")
            .Register(*registry_);
        ipc_recv_total_ = &ipc_recv_fam.Add({});

        auto& cavitation_fam = prometheus::BuildCounter()
            .Name("turbine_cavitation_detected_total")
            .Help("Cavitation detections by stage")
            .Register(*registry_);
        cavitation_normal_ = &cavitation_fam.Add({{"stage", "normal"}});
        cavitation_incipient_ = &cavitation_fam.Add({{"stage", "incipient"}});
        cavitation_critical_ = &cavitation_fam.Add({{"stage", "critical"}});
        cavitation_developed_ = &cavitation_fam.Add({{"stage", "developed"}});

        auto& alarm_fam = prometheus::BuildCounter()
            .Name("turbine_alarms_total")
            .Help("Alarms by level")
            .Register(*registry_);
        alarms_warning_ = &alarm_fam.Add({{"level", "warning"}});
        alarms_critical_ = &alarm_fam.Add({{"level", "critical"}});
        alarms_emergency_ = &alarm_fam.Add({{"level", "emergency"}});

        auto& damage_gauge_fam = prometheus::BuildGauge()
            .Name("turbine_blade_cumulative_damage")
            .Help("Cumulative fatigue damage per blade")
            .Register(*registry_);
        blade_damage_gauge_ = &damage_gauge_fam;

        auto& life_gauge_fam = prometheus::BuildGauge()
            .Name("turbine_blade_remaining_life_hours")
            .Help("Remaining life hours per blade")
            .Register(*registry_);
        blade_life_gauge_ = &life_gauge_fam;

        auto& latency_fam = prometheus::BuildHistogram()
            .Name("turbine_processing_latency_seconds")
            .Help("Processing latency")
            .Register(*registry_);
        processing_latency_ = &latency_fam.Add({}, prometheus::Histogram::BucketBoundaries{
            0.0001, 0.0005, 0.001, 0.005, 0.01, 0.05, 0.1, 0.5, 1.0});

        auto& ch_insert_fam = prometheus::BuildCounter()
            .Name("turbine_clickhouse_inserts_total")
            .Help("ClickHouse insert operations")
            .Register(*registry_);
        ch_inserts_ = &ch_insert_fam.Add({});

        auto& ch_errors_fam = prometheus::BuildCounter()
            .Name("turbine_clickhouse_errors_total")
            .Help("ClickHouse error count")
            .Register(*registry_);
        ch_errors_ = &ch_errors_fam.Add({});

        spdlog::info("MetricsRegistry initialized on {}", bind_address);
    }

    prometheus::Counter& packets_total() { return *packets_total_; }
    prometheus::Counter& packets_dropped() { return *packets_dropped_; }
    prometheus::Counter& ipc_sent() { return *ipc_sent_total_; }
    prometheus::Counter& ipc_recv() { return *ipc_recv_total_; }

    void record_cavitation(int stage) {
        switch (stage) {
            case 0: cavitation_normal_->Increment(); break;
            case 1: cavitation_incipient_->Increment(); break;
            case 2: cavitation_critical_->Increment(); break;
            case 3: cavitation_developed_->Increment(); break;
            default: break;
        }
    }

    void record_alarm(int level) {
        switch (level) {
            case 1: alarms_warning_->Increment(); break;
            case 2: alarms_critical_->Increment(); break;
            case 3: alarms_emergency_->Increment(); break;
            default: break;
        }
    }

    void set_blade_damage(uint8_t turbine, uint8_t blade, double damage) {
        blade_damage_gauge_->Add({{"turbine", std::to_string(turbine)}, {"blade", std::to_string(blade)}}).Set(damage);
    }

    void set_blade_life(uint8_t turbine, uint8_t blade, double hours) {
        blade_life_gauge_->Add({{"turbine", std::to_string(turbine)}, {"blade", std::to_string(blade)}}).Set(hours);
    }

    prometheus::Histogram& processing_latency() { return *processing_latency_; }
    prometheus::Counter& ch_inserts() { return *ch_inserts_; }
    prometheus::Counter& ch_errors() { return *ch_errors_; }

private:
    MetricsRegistry() = default;
    std::unique_ptr<prometheus::Exposer> exposer_;
    std::shared_ptr<prometheus::Registry> registry_;

    prometheus::Counter* packets_total_;
    prometheus::Counter* packets_dropped_;
    prometheus::Counter* ipc_sent_total_;
    prometheus::Counter* ipc_recv_total_;
    prometheus::Counter* cavitation_normal_;
    prometheus::Counter* cavitation_incipient_;
    prometheus::Counter* cavitation_critical_;
    prometheus::Counter* cavitation_developed_;
    prometheus::Counter* alarms_warning_;
    prometheus::Counter* alarms_critical_;
    prometheus::Counter* alarms_emergency_;
    prometheus::Family<prometheus::Gauge>* blade_damage_gauge_;
    prometheus::Family<prometheus::Gauge>* blade_life_gauge_;
    prometheus::Histogram* processing_latency_;
    prometheus::Counter* ch_inserts_;
    prometheus::Counter* ch_errors_;
};

inline void init_logging(const std::string& service_name,
                         const std::string& log_level = "info",
                         const std::string& log_dir = "/var/log/turbine_monitor") {
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] %v");

    std::vector<spdlog::sink_ptr> sinks{console_sink};

    try {
        auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            log_dir + "/" + service_name + ".log", 100 * 1024 * 1024, 10);
        file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%l] [thread %t] %v");
        sinks.push_back(file_sink);
    } catch (const std::exception& e) {
        std::cerr << "File log init failed: " << e.what() << std::endl;
    }

    auto logger = std::make_shared<spdlog::logger>(service_name, sinks.begin(), sinks.end());
    logger->set_level(spdlog::level::from_str(log_level));
    logger->flush_on(spdlog::level::warn);
    spdlog::set_default_logger(logger);
    spdlog::info("Logging initialized: service={}, level={}", service_name, log_level);
}

}
