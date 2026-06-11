#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <vector>
#include <functional>
#include <memory>
#include <array>
#include "data_structures.h"

#ifdef _WIN32
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
typedef int SOCKET;
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#endif

namespace turbine_monitor {

using DataCallback = std::function<void(const RawSensorData&)>;

static constexpr size_t SPSC_RING_CAPACITY = 131072;
static constexpr size_t RECVMMSG_BATCH_SIZE = 32;
static constexpr size_t RX_RING_SIZE = 4096;
static constexpr size_t XDP_FRAME_SIZE = 2048;

template<typename T, size_t Cap>
class SPSCRingBuffer {
public:
    SPSCRingBuffer() : head_(0), tail_(0) {
        for (size_t i = 0; i < Cap; ++i) {
            slots_[i].store(T{});
        }
    }

    bool push(const T& item) {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t next = (head + 1) % Cap;
        if (next == tail_.load(std::memory_order_acquire)) {
            return false;
        }
        slots_[head].store(item, std::memory_order_relaxed);
        head_.store(next, std::memory_order_release);
        return true;
    }

    bool pop(T& item) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) {
            return false;
        }
        item = slots_[tail].load(std::memory_order_relaxed);
        tail_.store((tail + 1) % Cap, std::memory_order_release);
        return true;
    }

    size_t size() const {
        size_t h = head_.load(std::memory_order_relaxed);
        size_t t = tail_.load(std::memory_order_relaxed);
        return (h >= t) ? (h - t) : (Cap - t + h);
    }

    bool empty() const {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

private:
    std::array<std::atomic<T>, Cap> slots_;
    alignas(64) std::atomic<size_t> head_;
    alignas(64) std::atomic<size_t> tail_;
};

struct UDPStats {
    std::atomic<uint64_t> packetsReceived{0};
    std::atomic<uint64_t> packetsDropped{0};
    std::atomic<uint64_t> batchRecvCalls{0};
    std::atomic<uint64_t> avgBatchSize{0};
};

class UDPServer {
public:
    enum class ReceiveMode {
        RECVFROM,
        RECVMMSG,
        AF_XDP
    };

    UDPServer(const std::string& host, uint16_t port,
              size_t threadPoolSize = 8,
              ReceiveMode mode = ReceiveMode::RECVMMSG);
    ~UDPServer();

    bool start();
    void stop();
    void setDataCallback(DataCallback callback);
    UDPStats getStats() const;

private:
    void receiveLoopRecvfrom();
    void receiveLoopRecvmmsg();
    void receiveLoopXDP();
    void processLoop();
    bool parsePacket(const UDPDataPacket* packet, RawSensorData& data);
    void cleanup();
    bool setupSocket();
    bool setupXDP();

    std::string     host_;
    uint16_t        port_;
    SOCKET          socket_;
    std::atomic<bool> running_;
    ReceiveMode     receiveMode_;

    std::thread     receiveThread_;
    std::vector<std::thread> processThreads_;

    SPSCRingBuffer<RawSensorData, SPSC_RING_CAPACITY> ringBuffer_;

    DataCallback  dataCallback_;
    std::atomic<bool> callbackSet_;

    UDPStats stats_;

    int xdpProgramFd_;
    int xdpMapFd_;

    static constexpr size_t NUMA_NODE = 0;
};

}
