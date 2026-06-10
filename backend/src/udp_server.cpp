#include "udp_server.h"
#include <iostream>
#include <cstring>
#include <chrono>

#ifndef _WIN32
#include <linux/if_packet.h>
#include <linux/if_link.h>
#include <linux/if_xdp.h>
#include <net/if.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <poll.h>
#ifdef HAS_RECVMMSG
#include <sys/socket.h>
#endif
#endif

namespace turbine_monitor {

UDPServer::UDPServer(const std::string& host, uint16_t port,
                     size_t threadPoolSize, ReceiveMode mode)
    : host_(host), port_(port), socket_(INVALID_SOCKET), running_(false),
      receiveMode_(mode), callbackSet_(false),
      xdpProgramFd_(-1), xdpMapFd_(-1) {
    processThreads_.reserve(threadPoolSize);
}

UDPServer::~UDPServer() {
    stop();
}

void UDPServer::cleanup() {
#ifdef _WIN32
    if (socket_ != INVALID_SOCKET) {
        closesocket(socket_);
    }
    WSACleanup();
#else
    if (socket_ != INVALID_SOCKET) {
        close(socket_);
    }
    if (xdpProgramFd_ >= 0) {
        close(xdpProgramFd_);
    }
    if (xdpMapFd_ >= 0) {
        close(xdpMapFd_);
    }
#endif
    socket_ = INVALID_SOCKET;
}

bool UDPServer::setupSocket() {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed" << std::endl;
        return false;
    }
#endif

    socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_ == INVALID_SOCKET) {
        std::cerr << "Failed to create socket" << std::endl;
        cleanup();
        return false;
    }

    int opt = 1;
    setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&opt), sizeof(opt));

    int rcvbuf = 16 * 1024 * 1024;
    if (setsockopt(socket_, SOL_SOCKET, SO_RCVBUF,
                   reinterpret_cast<const char*>(&rcvbuf), sizeof(rcvbuf)) == SOCKET_ERROR) {
        std::cerr << "Warning: Failed to set SO_RCVBUF to 16MB" << std::endl;
    }

#ifndef _WIN32
    if (receiveMode_ == ReceiveMode::RECVMMSG) {
        int val = 1;
        setsockopt(socket_, SOL_SOCKET, SO_TIMESTAMPNS, &val, sizeof(val));
    }
#endif

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port_);
    if (host_ == "0.0.0.0" || host_.empty()) {
        serverAddr.sin_addr.s_addr = INADDR_ANY;
    } else {
        inet_pton(AF_INET, host_.c_str(), &serverAddr.sin_addr);
    }

    if (bind(socket_, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        std::cerr << "Failed to bind socket to " << host_ << ":" << port_ << std::endl;
        cleanup();
        return false;
    }

    return true;
}

bool UDPServer::setupXDP() {
#ifndef _WIN32
    std::cout << "AF_XDP kernel bypass mode requested" << std::endl;
    std::cout << "Note: AF_XDP requires root, XDP-capable NIC, and BPF program." << std::endl;
    std::cout << "Falling back to recvmmsg mode. See xdp_loader.c for full XDP setup." << std::endl;
    receiveMode_ = ReceiveMode::RECVMMSG;
#else
    std::cerr << "AF_XDP not supported on Windows, falling back to recvfrom" << std::endl;
    receiveMode_ = ReceiveMode::RECVFROM;
#endif
    return setupSocket();
}

bool UDPServer::start() {
    if (receiveMode_ == ReceiveMode::AF_XDP) {
        if (!setupXDP()) {
            return false;
        }
    } else {
        if (!setupSocket()) {
            return false;
        }
    }

    running_ = true;

    switch (receiveMode_) {
    case ReceiveMode::RECVMMSG:
#ifndef _WIN32
        receiveThread_ = std::thread(&UDPServer::receiveLoopRecvmmsg, this);
        std::cout << "UDP Server started with recvmmsg batch mode on "
                  << host_ << ":" << port_ << std::endl;
        break;
#else
        receiveThread_ = std::thread(&UDPServer::receiveLoopRecvfrom, this);
        std::cout << "UDP Server started with recvfrom mode on "
                  << host_ << ":" << port_ << std::endl;
        break;
#endif
    case ReceiveMode::AF_XDP:
        receiveThread_ = std::thread(&UDPServer::receiveLoopXDP, this);
        std::cout << "UDP Server started with AF_XDP kernel bypass on "
                  << host_ << ":" << port_ << std::endl;
        break;
    default:
        receiveThread_ = std::thread(&UDPServer::receiveLoopRecvfrom, this);
        std::cout << "UDP Server started with recvfrom mode on "
                  << host_ << ":" << port_ << std::endl;
        break;
    }

    for (size_t i = 0; i < processThreads_.capacity(); ++i) {
        processThreads_.emplace_back(&UDPServer::processLoop, this);
    }

    return true;
}

void UDPServer::stop() {
    running_ = false;

    if (receiveThread_.joinable()) {
        receiveThread_.join();
    }

    for (auto& t : processThreads_) {
        if (t.joinable()) {
            t.join();
        }
    }

    cleanup();
}

void UDPServer::setDataCallback(DataCallback callback) {
    dataCallback_ = std::move(callback);
    callbackSet_.store(true, std::memory_order_release);
}

UDPStats UDPServer::getStats() const {
    return stats_;
}

void UDPServer::receiveLoopRecvfrom() {
    sockaddr_in clientAddr{};
#ifdef _WIN32
    int addrLen = sizeof(clientAddr);
#else
    socklen_t addrLen = sizeof(clientAddr);
#endif

    alignas(64) char buffer[sizeof(UDPDataPacket) + 64];

    while (running_) {
        int bytesReceived = recvfrom(
            socket_,
            buffer,
            sizeof(UDPDataPacket),
            0,
            reinterpret_cast<sockaddr*>(&clientAddr),
            &addrLen
        );

        if (bytesReceived == SOCKET_ERROR || bytesReceived < static_cast<int>(sizeof(UDPDataPacket))) {
            if (running_) continue;
            break;
        }

        const UDPDataPacket* packet = reinterpret_cast<const UDPDataPacket*>(buffer);
        RawSensorData data;
        if (parsePacket(packet, data)) {
            stats_.packetsReceived.fetch_add(1, std::memory_order_relaxed);
            if (!ringBuffer_.push(data)) {
                stats_.packetsDropped.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
}

#ifndef _WIN32
void UDPServer::receiveLoopRecvmmsg() {
    constexpr size_t BATCH = RECVMMSG_BATCH_SIZE;

    struct mmsghdr msgs[BATCH];
    struct iovec iovecs[BATCH];
    alignas(64) char buffers[BATCH][sizeof(UDPDataPacket) + 64];

    std::memset(msgs, 0, sizeof(msgs));
    for (size_t i = 0; i < BATCH; ++i) {
        iovecs[i].iov_base = buffers[i];
        iovecs[i].iov_len = sizeof(UDPDataPacket);
        msgs[i].msg_hdr.msg_iov = &iovecs[i];
        msgs[i].msg_hdr.msg_iovlen = 1;
        msgs[i].msg_hdr.msg_name = nullptr;
        msgs[i].msg_hdr.msg_namelen = 0;
    }

    while (running_) {
        int nrecv = recvmmsg(socket_, msgs, BATCH, MSG_DONTWAIT, nullptr);

        if (nrecv < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct timespec ts{0, 100000};
                nanosleep(&ts, nullptr);
                continue;
            }
            if (running_) continue;
            break;
        }

        if (nrecv > 0) {
            stats_.batchRecvCalls.fetch_add(1, std::memory_order_relaxed);
            stats_.avgBatchSize.store(
                (stats_.avgBatchSize.load(std::memory_order_relaxed) * 9 + nrecv) / 10,
                std::memory_order_relaxed
            );
        }

        for (int i = 0; i < nrecv; ++i) {
            if (msgs[i].msg_len < sizeof(UDPDataPacket)) {
                continue;
            }

            const UDPDataPacket* packet = reinterpret_cast<const UDPDataPacket*>(buffers[i]);
            RawSensorData data;
            if (parsePacket(packet, data)) {
                stats_.packetsReceived.fetch_add(1, std::memory_order_relaxed);
                if (!ringBuffer_.push(data)) {
                    stats_.packetsDropped.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    }
}
#else
void UDPServer::receiveLoopRecvmmsg() {
    receiveLoopRecvfrom();
}
#endif

void UDPServer::receiveLoopXDP() {
#ifndef _WIN32
    const char* ifname = "eth0";
    int ifindex = if_nametoindex(ifname);
    if (ifindex == 0) {
        std::cerr << "XDP: Interface " << ifname << " not found" << std::endl;
        return;
    }

    struct xdp_umem_reg umem_reg = {};
    size_t num_frames = RX_RING_SIZE * 2;
    size_t frame_size = XDP_FRAME_SIZE;
    umem_reg.addr = reinterpret_cast<uint64_t>(
        mmap(nullptr, num_frames * frame_size,
             PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0));
    if (umem_reg.addr == MAP_FAILED) {
        umem_reg.addr = reinterpret_cast<uint64_t>(
            mmap(nullptr, num_frames * frame_size,
                 PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    }
    if (umem_reg.addr == MAP_FAILED) {
        std::cerr << "XDP: Failed to allocate UMEM" << std::endl;
        return;
    }
    umem_reg.len = num_frames * frame_size;
    umem_reg.chunk_size = frame_size;
    umem_reg.headroom = 0;
    umem_reg.flags = 0;

    std::cout << "XDP: UMEM allocated at " << std::hex << umem_reg.addr << std::dec
              << " size=" << umem_reg.len << std::endl;

    struct sockaddr_xdp sxdp = {};
    sxdp.sxdp_family = PF_XDP;
    sxdp.sxdp_ifindex = ifindex;
    sxdp.sxdp_queue_id = 0;

    int xdp_sock = socket(AF_XDP, SOCK_RAW, 0);
    if (xdp_sock < 0) {
        std::cerr << "XDP: Failed to create XDP socket, falling back" << std::endl;
        munmap(reinterpret_cast<void*>(umem_reg.addr), umem_reg.len);
        return;
    }

    if (setsockopt(xdp_sock, SOL_XDP, XDP_UMEM_REG, &umem_reg, sizeof(umem_reg)) < 0) {
        std::cerr << "XDP: Failed to register UMEM: " << strerror(errno) << std::endl;
        close(xdp_sock);
        munmap(reinterpret_cast<void*>(umem_reg.addr), umem_reg.len);
        return;
    }

    int opt = RX_RING_SIZE;
    setsockopt(xdp_sock, SOL_XDP, XDP_RX_RING, &opt, sizeof(opt));
    setsockopt(xdp_sock, SOL_XDP, XDP_UMEM_FILL_RING, &opt, sizeof(opt));
    setsockopt(xdp_sock, SOL_XDP, XDP_UMEM_COMPLETION_RING, &opt, sizeof(opt));

    if (bind(xdp_sock, reinterpret_cast<sockaddr*>(&sxdp), sizeof(sxdp)) < 0) {
        std::cerr << "XDP: Failed to bind XDP socket: " << strerror(errno) << std::endl;
        close(xdp_sock);
        munmap(reinterpret_cast<void*>(umem_reg.addr), umem_reg.len);
        return;
    }

    struct pollfd fds[1];
    fds[0].fd = xdp_sock;
    fds[0].events = POLLIN;

    uint8_t* umem_base = reinterpret_cast<uint8_t*>(umem_reg.addr);

    while (running_) {
        int ret = poll(fds, 1, 1);
        if (ret <= 0) continue;

        uint32_t idx = 0;
        int nrecv = recvfrom(xdp_sock, nullptr, 0, MSG_DONTWAIT, nullptr, nullptr);

        if (nrecv > 0) {
            for (int i = 0; i < nrecv; ++i) {
                uint8_t* frame = umem_base + idx * frame_size;

                if (frame_size >= sizeof(UDPDataPacket) + 42) {
                    const UDPDataPacket* packet = reinterpret_cast<const UDPDataPacket*>(
                        frame + 42);
                    RawSensorData data;
                    if (parsePacket(packet, data)) {
                        stats_.packetsReceived.fetch_add(1, std::memory_order_relaxed);
                        if (!ringBuffer_.push(data)) {
                            stats_.packetsDropped.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                }

                idx = (idx + 1) % num_frames;
            }
        }
    }

    close(xdp_sock);
    munmap(reinterpret_cast<void*>(umem_reg.addr), umem_reg.len);
#else
    receiveLoopRecvfrom();
#endif
}

void UDPServer::processLoop() {
    RawSensorData data;
    while (running_) {
        if (ringBuffer_.pop(data)) {
            if (callbackSet_.load(std::memory_order_acquire) && dataCallback_) {
                dataCallback_(data);
            }
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
    }

    while (ringBuffer_.pop(data)) {
        if (callbackSet_.load(std::memory_order_acquire) && dataCallback_) {
            dataCallback_(data);
        }
    }
}

bool UDPServer::parsePacket(const UDPDataPacket* packet, RawSensorData& data) {
    if (!packet || packet->sample_count > 128) {
        return false;
    }

    data.timestamp       = packet->timestamp;
    data.turbine_id      = packet->turbine_id;
    data.sensor_type     = static_cast<SensorType>(packet->sensor_type);
    data.sensor_id       = packet->sensor_id;
    data.sensor_position = static_cast<SensorPosition>(packet->sensor_position);
    data.blade_id        = packet->blade_id;
    data.amplitude       = packet->amplitude;
    data.sample_rate     = packet->sample_rate;
    data.batch_id        = packet->batch_id;

    data.raw_data.assign(packet->data, packet->data + packet->sample_count);
    return true;
}

}
