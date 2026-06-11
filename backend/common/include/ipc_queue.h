#pragma once

#include <string>
#include <vector>
#include <atomic>
#include <memory>
#include <cstring>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace turbine_monitor {

enum class IPCChannel : uint32_t {
    RAW_DATA = 0,
    FEATURES = 1,
    CAVITATION = 2,
    STRESS = 3,
    LIFE = 4,
    ALARM = 5,
    COUNT
};

static const char* IPC_CHANNEL_NAMES[] = {
    "/turbine_raw_data",
    "/turbine_features",
    "/turbine_cavitation",
    "/turbine_stress",
    "/turbine_life",
    "/turbine_alarm"
};

static constexpr size_t IPC_DEFAULT_CAPACITY = 65536;
static constexpr size_t IPC_SLOT_SIZE = 4096;
static constexpr size_t IPC_MAX_MESSAGE = IPC_SLOT_SIZE - sizeof(uint32_t);

template<typename MessageType>
class SharedMemorySPSC {
public:
    SharedMemorySPSC(IPCChannel channel,
                     size_t capacity = IPC_DEFAULT_CAPACITY,
                     bool isProducer = false)
        : channel_(channel), capacity_(capacity), isProducer_(isProducer),
          mapped_(nullptr), mapSize_(0),
          shmHandle_(IPC_INVALID_HANDLE) {
        name_ = IPC_CHANNEL_NAMES[static_cast<uint32_t>(channel)];
        slotSize_ = sizeof(SlotHeader) + sizeof(MessageType);
        mapSize_ = sizeof(ShmHeader) + capacity_ * slotSize_;
    }

    ~SharedMemorySPSC() {
        close();
    }

    bool open() {
#ifdef _WIN32
        shmHandle_ = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name_.c_str());
        if (shmHandle_ == NULL) {
            if (!isProducer_) return false;
            shmHandle_ = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL,
                PAGE_READWRITE, 0, static_cast<DWORD>(mapSize_), name_.c_str());
            if (shmHandle_ == NULL) return false;
        }
        mapped_ = MapViewOfFile(shmHandle_, FILE_MAP_ALL_ACCESS, 0, 0, mapSize_);
        if (mapped_ == NULL) {
            CloseHandle(shmHandle_);
            shmHandle_ = IPC_INVALID_HANDLE;
            return false;
        }
#else
        int fd = shm_open(name_.c_str(), O_RDWR, 0666);
        if (fd < 0) {
            if (!isProducer_) return false;
            fd = shm_open(name_.c_str(), O_CREAT | O_RDWR, 0666);
            if (fd < 0) return false;
            if (ftruncate(fd, mapSize_) < 0) {
                close(fd);
                shm_unlink(name_.c_str());
                return false;
            }
        }
        mapped_ = mmap(nullptr, mapSize_, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (mapped_ == MAP_FAILED) {
            mapped_ = nullptr;
            ::close(fd);
            return false;
        }
        ::close(fd);
#endif

        header_ = reinterpret_cast<ShmHeader*>(mapped_);
        slots_ = reinterpret_cast<uint8_t*>(mapped_) + sizeof(ShmHeader);

        if (isProducer_ && header_->magic.load(std::memory_order_relaxed) != SHM_MAGIC) {
            std::memset(mapped_, 0, mapSize_);
            header_->magic.store(SHM_MAGIC, std::memory_order_relaxed);
            header_->slotSize.store(slotSize_, std::memory_order_relaxed);
            header_->capacity.store(capacity_, std::memory_order_relaxed);
            header_->producerPid.store(getCurrentPid(), std::memory_order_relaxed);
            header_->head.store(0, std::memory_order_release);
            header_->tail.store(0, std::memory_order_release);
            header_->initDone.store(true, std::memory_order_release);
        }

        while (isProducer_ == false &&
               header_->initDone.load(std::memory_order_acquire) == false) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        return true;
    }

    void close() {
        if (mapped_) {
#ifdef _WIN32
            UnmapViewOfFile(mapped_);
            if (shmHandle_ != IPC_INVALID_HANDLE) CloseHandle(shmHandle_);
#else
            munmap(mapped_, mapSize_);
            if (isProducer_) shm_unlink(name_.c_str());
#endif
            mapped_ = nullptr;
            shmHandle_ = IPC_INVALID_HANDLE;
        }
    }

    bool push(const MessageType& msg) {
        if (!mapped_ || !isProducer_) return false;

        const size_t head = header_->head.load(std::memory_order_relaxed);
        const size_t next = (head + 1) % capacity_;
        if (next == header_->tail.load(std::memory_order_acquire)) {
            header_->dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        void* slotAddr = slots_ + head * slotSize_;
        SlotHeader* slotHdr = reinterpret_cast<SlotHeader*>(slotAddr);
        MessageType* slotMsg = reinterpret_cast<MessageType*>(slotHdr + 1);

        slotHdr->length = static_cast<uint32_t>(sizeof(MessageType));
        slotHdr->sequence = header_->sequence.fetch_add(1, std::memory_order_relaxed);
        slotHdr->timestamp = currentTimestampMs();
        *slotMsg = msg;

        header_->head.store(next, std::memory_order_release);
        header_->produced.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    bool pop(MessageType& msg) {
        if (!mapped_) return false;

        const size_t tail = header_->tail.load(std::memory_order_relaxed);
        if (tail == header_->head.load(std::memory_order_acquire)) {
            return false;
        }

        void* slotAddr = slots_ + tail * slotSize_;
        SlotHeader* slotHdr = reinterpret_cast<SlotHeader*>(slotAddr);
        MessageType* slotMsg = reinterpret_cast<MessageType*>(slotHdr + 1);

        if (slotHdr->length > 0) {
            msg = *slotMsg;
            header_->consumed.fetch_add(1, std::memory_order_relaxed);
        }

        header_->tail.store((tail + 1) % capacity_, std::memory_order_release);
        return slotHdr->length > 0;
    }

    size_t size() const {
        if (!mapped_) return 0;
        size_t h = header_->head.load(std::memory_order_relaxed);
        size_t t = header_->tail.load(std::memory_order_relaxed);
        return (h >= t) ? (h - t) : (capacity_ - t + h);
    }

    bool empty() const {
        if (!mapped_) return true;
        return header_->head.load(std::memory_order_acquire) ==
               header_->tail.load(std::memory_order_acquire);
    }

    struct Stats {
        uint64_t produced;
        uint64_t consumed;
        uint64_t dropped;
        size_t   currentSize;
        uint64_t sequence;
    };

    Stats getStats() const {
        Stats s{};
        if (!mapped_) return s;
        s.produced = header_->produced.load(std::memory_order_relaxed);
        s.consumed = header_->consumed.load(std::memory_order_relaxed);
        s.dropped = header_->dropped.load(std::memory_order_relaxed);
        s.currentSize = size();
        s.sequence = header_->sequence.load(std::memory_order_relaxed);
        return s;
    }

private:
    static constexpr uint32_t SHM_MAGIC = 0x54424D4E;

#ifdef _WIN32
    using IPC_HANDLE = HANDLE;
    static constexpr IPC_HANDLE IPC_INVALID_HANDLE = NULL;
#else
    using IPC_HANDLE = int;
    static constexpr IPC_HANDLE IPC_INVALID_HANDLE = -1;
#endif

    struct SlotHeader {
        uint32_t length;
        uint32_t sequence;
        uint64_t timestamp;
    };

    struct alignas(64) ShmHeader {
        std::atomic<uint32_t> magic;
        std::atomic<uint32_t> slotSize;
        std::atomic<uint32_t> capacity;
        std::atomic<uint32_t> producerPid;
        std::atomic<bool>     initDone;
        alignas(64) std::atomic<size_t> head;
        alignas(64) std::atomic<size_t> tail;
        alignas(64) std::atomic<uint64_t> produced;
        std::atomic<uint64_t> consumed;
        std::atomic<uint64_t> dropped;
        std::atomic<uint64_t> sequence;
    };

    IPCChannel channel_;
    std::string name_;
    size_t capacity_;
    size_t slotSize_;
    size_t mapSize_;
    bool isProducer_;

    void* mapped_;
    IPC_HANDLE shmHandle_;
    ShmHeader* header_;
    uint8_t* slots_;

    static uint32_t getCurrentPid() {
#ifdef _WIN32
        return static_cast<uint32_t>(GetCurrentProcessId());
#else
        return static_cast<uint32_t>(getpid());
#endif
    }
};

struct IPCMessageRaw {
    uint8_t  turbine_id;
    uint8_t  sensor_type;
    uint8_t  sensor_id;
    uint8_t  blade_id;
    uint8_t  sensor_position;
    uint8_t  _pad[3];
    float    amplitude;
    uint32_t sample_rate;
    uint32_t batch_id;
    uint64_t timestamp;
    uint32_t sample_count;
    float    data[128];
};

struct IPCMessageFeatures {
    uint8_t  turbine_id;
    uint8_t  sensor_type;
    uint8_t  sensor_id;
    uint8_t  blade_id;
    uint8_t  _pad[4];
    uint64_t timestamp;
    float    spectrum[11];
    float    wavelet_energy[16];
    float    wavelet_entropy;
    float    wavelet_total;
    float    rms_value;
    float    _reserved[6];
};

struct IPCMessageCavitation {
    uint8_t  turbine_id;
    uint8_t  blade_id;
    uint8_t  cavitation_stage;
    uint8_t  model_type;
    uint8_t  _pad[4];
    uint64_t timestamp;
    float    cavitation_intensity;
    float    confidence;
    float    anomaly_score;
    float    reconstruction_error;
    float    feature_vector[32];
};

struct IPCMessageStress {
    uint8_t  turbine_id;
    uint8_t  blade_id;
    uint8_t  _pad[6];
    uint64_t timestamp;
    float    mean_stress;
    float    max_stress;
    float    min_stress;
    float    stress_amplitude;
    float    combined_stress;
    float    vibration_stress;
    float    cavitation_stress;
    uint32_t stress_cycles;
    float    rainflow_summary[96];
};

struct IPCMessageLife {
    uint8_t  turbine_id;
    uint8_t  blade_id;
    uint8_t  _pad[6];
    uint64_t timestamp;
    float    cumulative_damage;
    float    remaining_life_hours;
    float    remaining_life_days;
    float    miner_sum;
    float    fatigue_damage;
    float    cavitation_damage;
    float    material_k;
    float    material_m;
    float    stress_range;
    uint32_t cycle_count;
};

struct IPCMessageAlarm {
    uint8_t  turbine_id;
    uint8_t  blade_id;
    uint8_t  alarm_type;
    uint8_t  alarm_level;
    uint8_t  acknowledged;
    uint8_t  iec61850_pushed;
    uint8_t  _pad[2];
    uint64_t timestamp;
    uint64_t acknowledged_at;
    float    threshold_value;
    float    actual_value;
    char     alarm_id[40];
    char     alarm_message[256];
    char     maintenance_suggestion[512];
    char     acknowledged_by[32];
};

}
