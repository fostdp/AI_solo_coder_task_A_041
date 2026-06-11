#pragma once

#include "data_structures.h"
#include "config.h"
#include "udp_server.h"
#include "clickhouse_client.h"
#include "signal_processor.h"
#include "cavitation_detector.h"
#include "life_assessor.h"
#include "alarm_manager.h"
#include "ipc_queue.h"
#include <memory>
#include <string>

namespace turbine_monitor {

class ServiceBase {
public:
    ServiceBase(const std::string& name, uint32_t instanceId = 0)
        : name_(name), instanceId_(instanceId), running_(false) {}
    virtual ~ServiceBase() = default;

    virtual bool init(const Config& config) = 0;
    virtual bool start() { running_ = true; return true; }
    virtual void stop() { running_ = false; }
    virtual void join() = 0;

    bool isRunning() const { return running_; }
    const std::string& name() const { return name_; }

protected:
    std::string name_;
    uint32_t instanceId_;
    std::atomic<bool> running_;
};

}
