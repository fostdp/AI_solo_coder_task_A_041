#!/usr/bin/env python3
"""
PXI 采集卡模拟器 v2.0
======================
模拟 6 台混流式水轮机，每台 12 水听器 + 8 加速度计 = 20 传感器
1ms 间隔 UDP 数据流，可注入不同阶段空化特征信号

空化阶段:
  normal    (0) - 正常运行，仅背景噪声+轴频谐波
  incipient (1) - 初生空化，5-15kHz 微弱宽带噪声
  critical  (2) - 临界空化，15-30kHz 强噪声+冲击脉冲
  developed (3) - 发展空化，全频段高噪声+密集冲击+谐波畸变

用法:
  python pxi_simulator.py -H pxi_collector -p 9000 \
      --inject-cavitation 1:incipient 3:critical 5:developed
"""

import socket
import struct
import time
import random
import math
import threading
import argparse
import signal as sig_module
import sys
import json
from dataclasses import dataclass, field
from enum import IntEnum
from pathlib import Path
import numpy as np


class CavitationStage(IntEnum):
    NORMAL = 0
    INCIPIENT = 1
    CRITICAL = 2
    DEVELOPED = 3


class SensorType(IntEnum):
    HYDROPHONE = 0
    ACCELEROMETER = 1


class SensorPosition(IntEnum):
    SPIRAL_CASE_INLET = 0
    GUIDE_VANE = 1
    DRAFT_TUBE = 2
    RUNNER_BLADE = 3
    GUIDE_BEARING = 4
    THRUST_BEARING = 5


TURBINE_COUNT = 6
HYDROPHONE_COUNT = 12
ACCELEROMETER_COUNT = 8
BLADE_COUNT = 15
SENSORS_PER_TURBINE = HYDROPHONE_COUNT + ACCELEROMETER_COUNT

HYDROPHONE_SAMPLE_RATE = 51200
ACCELEROMETER_SAMPLE_RATE = 25600
SAMPLES_PER_PACKET = 128
PACKET_INTERVAL = 0.001

HYDROPHONE_POSITIONS = [
    SensorPosition.SPIRAL_CASE_INLET,
    SensorPosition.SPIRAL_CASE_INLET,
    SensorPosition.SPIRAL_CASE_INLET,
    SensorPosition.SPIRAL_CASE_INLET,
    SensorPosition.GUIDE_VANE,
    SensorPosition.GUIDE_VANE,
    SensorPosition.GUIDE_VANE,
    SensorPosition.GUIDE_VANE,
    SensorPosition.DRAFT_TUBE,
    SensorPosition.DRAFT_TUBE,
    SensorPosition.DRAFT_TUBE,
    SensorPosition.DRAFT_TUBE,
]

ACCELEROMETER_POSITIONS = [SensorPosition.RUNNER_BLADE] * ACCELEROMETER_COUNT

CAVITATION_PROFILES = {
    CavitationStage.NORMAL: {
        "broadband_noise_amp": 0.3,
        "cavitation_freq_range": (0, 0),
        "cavitation_amp_factor": 0.0,
        "impact_count": 0,
        "impact_amp": 0.0,
        "harmonic_distortion": 0.0,
        "description": "正常运行 - 背景噪声+轴频谐波",
    },
    CavitationStage.INCIPIENT: {
        "broadband_noise_amp": 0.8,
        "cavitation_freq_range": (5000, 15000),
        "cavitation_amp_factor": 1.5,
        "impact_count": 1,
        "impact_amp": 2.0,
        "harmonic_distortion": 0.05,
        "description": "初生空化 - 5~15kHz微弱宽带噪声",
    },
    CavitationStage.CRITICAL: {
        "broadband_noise_amp": 2.0,
        "cavitation_freq_range": (15000, 30000),
        "cavitation_amp_factor": 4.0,
        "impact_count": 4,
        "impact_amp": 5.0,
        "harmonic_distortion": 0.15,
        "description": "临界空化 - 15~30kHz强噪声+冲击脉冲",
    },
    CavitationStage.DEVELOPED: {
        "broadband_noise_amp": 5.0,
        "cavitation_freq_range": (5000, 45000),
        "cavitation_amp_factor": 8.0,
        "impact_count": 10,
        "impact_amp": 10.0,
        "harmonic_distortion": 0.35,
        "description": "发展空化 - 全频段高噪声+密集冲击+谐波畸变",
    },
}


@dataclass
class UDPDataPacket:
    timestamp: int
    batch_id: int
    turbine_id: int
    sensor_type: int
    sensor_id: int
    sensor_position: int
    blade_id: int
    sample_count: int
    sample_rate: int
    amplitude: float
    data: list

    def to_bytes(self):
        header = struct.pack(
            '<QIBBBBBHHf',
            self.timestamp, self.batch_id, self.turbine_id,
            self.sensor_type, self.sensor_id, self.sensor_position,
            self.blade_id, self.sample_count, self.sample_rate, self.amplitude
        )
        data_bytes = struct.pack('<' + 'f' * len(self.data), *self.data)
        return header + data_bytes


class CavitationSignalInjector:
    def __init__(self, injection_map: dict):
        self.injection_map = injection_map
        self.stage_levels = {}
        self.transition_speed = {}

        for tid in range(TURBINE_COUNT):
            target = injection_map.get(tid, CavitationStage.NORMAL)
            self.stage_levels[tid] = {
                "current": 0.0,
                "target": float(target),
                "stage": CavitationStage.NORMAL,
            }
            self.transition_speed[tid] = random.uniform(0.02, 0.05)

    def update(self):
        for tid in range(TURBINE_COUNT):
            info = self.stage_levels[tid]
            diff = info["target"] - info["current"]
            if abs(diff) > 0.01:
                info["current"] += diff * self.transition_speed[tid]
            else:
                info["current"] = info["target"]
            info["stage"] = CavitationStage(int(round(info["current"])))

    def get_effective_stage(self, turbine_id: int) -> CavitationStage:
        return self.stage_levels[turbine_id]["stage"]

    def get_transition_factor(self, turbine_id: int) -> float:
        info = self.stage_levels[turbine_id]
        base = int(info["current"])
        frac = info["current"] - base
        return frac

    def inject_cavitation_features(
        self, signal: np.ndarray, turbine_id: int,
        sensor_type: SensorType, sample_rate: int
    ) -> np.ndarray:
        stage = self.get_effective_stage(turbine_id)
        profile = CAVITATION_PROFILES[stage]
        n = len(signal)
        t = np.arange(n) / sample_rate

        # 1. 宽带噪声注入
        noise_amp = profile["broadband_noise_amp"]
        if noise_amp > 0:
            signal = signal + noise_amp * np.random.randn(n)

        # 2. 空化特征频段
        f_lo, f_hi = profile["cavitation_freq_range"]
        if f_hi > 0:
            cav_amp = profile["cavitation_amp_factor"]
            num_cav_tones = max(1, int((f_hi - f_lo) / 2000))
            for i in range(num_cav_tones):
                freq = f_lo + (f_hi - f_lo) * (i + 0.5) / num_cav_tones
                amp = cav_amp / num_cav_tones * 2
                phase = random.uniform(0, 2 * math.pi)
                signal += amp * np.sin(2 * math.pi * freq * t + phase)

        # 3. 冲击脉冲 (空化泡溃灭特征)
        impact_count = profile["impact_count"]
        if impact_count > 0:
            impact_amp = profile["impact_amp"]
            for _ in range(impact_count):
                pos = random.randint(0, n - 30)
                impact_len = min(random.randint(5, 20), n - pos)
                impact_t = np.arange(impact_len) / sample_rate
                decay = random.uniform(500, 5000)
                freq = random.uniform(20000, 40000) if sensor_type == SensorType.HYDROPHONE else random.uniform(5000, 15000)
                pulse = impact_amp * np.exp(-decay * impact_t) * np.sin(2 * math.pi * freq * impact_t)
                signal[pos:pos + impact_len] += pulse

        # 4. 谐波畸变 (发展空化时水听器信号非线性)
        hd = profile["harmonic_distortion"]
        if hd > 0 and sensor_type == SensorType.HYDROPHONE:
            fundamental = 100 + turbine_id * 50
            for h in range(2, 6):
                signal += hd * np.sin(2 * math.pi * fundamental * h * t + random.uniform(0, 2 * math.pi))

        # 5. 空化强度周期性调制 (叶片通过频率)
        if stage >= CavitationStage.CRITICAL:
            bpf = 2.0 * BLADE_COUNT
            mod = 1.0 + 0.3 * np.sin(2 * math.pi * bpf * t)
            signal = signal * mod

        return signal


class SignalSimulator:
    def __init__(self, injector: CavitationSignalInjector):
        self.injector = injector
        self.base_vibration = {}
        for tid in range(TURBINE_COUNT):
            self.base_vibration[tid] = random.uniform(0.05, 0.15)

    def generate_hydrophone_signal(
        self, turbine_id: int, sensor_id: int,
        sensor_position: int, n_samples: int, sample_rate: int
    ):
        t = np.arange(n_samples) / sample_rate
        signal = np.zeros(n_samples)

        base_freq = 100 + sensor_id * 50
        for harmonic in range(1, 6):
            amp = 1.0 / harmonic
            phase = random.uniform(0, 2 * math.pi)
            signal += amp * np.sin(2 * math.pi * base_freq * harmonic * t + phase)

        signal += 0.2 * np.random.randn(n_samples)

        signal = self.injector.inject_cavitation_features(
            signal, turbine_id, SensorType.HYDROPHONE, sample_rate
        )

        amplitude = float(np.max(np.abs(signal)))
        return signal.tolist(), amplitude

    def generate_accelerometer_signal(
        self, turbine_id: int, sensor_id: int, blade_id: int,
        n_samples: int, sample_rate: int
    ):
        t = np.arange(n_samples) / sample_rate
        signal = np.zeros(n_samples)

        rot_freq = 2.0
        for harmonic in range(1, 10):
            amp = 1.0 / (harmonic ** 1.5)
            phase = random.uniform(0, 2 * math.pi)
            signal += amp * np.sin(2 * math.pi * rot_freq * harmonic * t + phase)

        blade_pass_freq = rot_freq * BLADE_COUNT
        blade_amp = self.base_vibration[turbine_id] * 3
        phase = random.uniform(0, 2 * math.pi)
        signal += blade_amp * np.sin(2 * math.pi * blade_pass_freq * t + phase)

        signal = self.injector.inject_cavitation_features(
            signal, turbine_id, SensorType.ACCELEROMETER, sample_rate
        )

        amplitude = float(np.max(np.abs(signal)))
        return signal.tolist(), amplitude


class PXISimulator:
    def __init__(self, host='127.0.0.1', port=9000,
                 send_interval=PACKET_INTERVAL,
                 injection_map=None):
        self.host = host
        self.port = port
        self.send_interval = send_interval
        self.running = False

        if injection_map is None:
            injection_map = {}
        self.injector = CavitationSignalInjector(injection_map)
        self.signal_sim = SignalSimulator(self.injector)

        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.batch_id = 0
        self.stats = {'packets_sent': 0, 'bytes_sent': 0, 'start_time': 0}
        self.lock = threading.Lock()

    def send_packet(self, packet):
        data = packet.to_bytes()
        self.sock.sendto(data, (self.host, self.port))
        with self.lock:
            self.stats['packets_sent'] += 1
            self.stats['bytes_sent'] += len(data)

    def turbine_sender_thread(self, turbine_id):
        sensors = []
        for hid in range(HYDROPHONE_COUNT):
            sensors.append({
                'type': SensorType.HYDROPHONE,
                'id': hid,
                'position': HYDROPHONE_POSITIONS[hid].value,
                'blade_id': 0,
                'sample_rate': HYDROPHONE_SAMPLE_RATE,
            })
        for aid in range(ACCELEROMETER_COUNT):
            blade_id = (aid % BLADE_COUNT) + 1
            sensors.append({
                'type': SensorType.ACCELEROMETER,
                'id': aid,
                'position': ACCELEROMETER_POSITIONS[aid].value,
                'blade_id': blade_id,
                'sample_rate': ACCELEROMETER_SAMPLE_RATE,
            })

        while self.running:
            cycle_start = time.time()
            timestamp = int(time.time() * 1000)

            with self.lock:
                self.batch_id += 1
                current_batch = self.batch_id

            for sensor in sensors:
                if not self.running:
                    break
                if sensor['type'] == SensorType.HYDROPHONE:
                    data, amplitude = self.signal_sim.generate_hydrophone_signal(
                        turbine_id, sensor['id'], sensor['position'],
                        SAMPLES_PER_PACKET, sensor['sample_rate']
                    )
                else:
                    data, amplitude = self.signal_sim.generate_accelerometer_signal(
                        turbine_id, sensor['id'], sensor['blade_id'],
                        SAMPLES_PER_PACKET, sensor['sample_rate']
                    )

                packet = UDPDataPacket(
                    timestamp=timestamp, batch_id=current_batch,
                    turbine_id=turbine_id, sensor_type=sensor['type'].value,
                    sensor_id=sensor['id'], sensor_position=sensor['position'],
                    blade_id=sensor['blade_id'], sample_count=SAMPLES_PER_PACKET,
                    sample_rate=sensor['sample_rate'], amplitude=amplitude, data=data,
                )
                self.send_packet(packet)

            cycle_time = time.time() - cycle_start
            sleep_time = max(0, self.send_interval - cycle_time)
            if sleep_time > 0:
                time.sleep(sleep_time)

    def level_updater_thread(self):
        while self.running:
            self.injector.update()
            time.sleep(0.5)

    def stats_printer_thread(self):
        self.stats['start_time'] = time.time()
        while self.running:
            time.sleep(5)
            with self.lock:
                elapsed = time.time() - self.stats['start_time']
                pps = self.stats['packets_sent'] / elapsed if elapsed > 0 else 0
                mbps = self.stats['bytes_sent'] * 8 / elapsed / 1e6 if elapsed > 0 else 0
                stages = {}
                for tid in range(TURBINE_COUNT):
                    s = self.injector.get_effective_stage(tid)
                    stages[tid] = CavitationStage(s).name

            print(f"\r[PXI Sim] PPS:{pps:.0f} | {mbps:.2f}Mbps | "
                  f"Total:{self.stats['packets_sent']}pkt | "
                  f"Stages:{stages}", end='', flush=True)

    def start(self):
        self.running = True
        stage_desc = {}
        for tid in range(TURBINE_COUNT):
            s = self.injector.get_effective_stage(tid)
            stage_desc[tid] = CavitationStage(s).name

        print("=" * 60)
        print("  PXI Simulator v2.0")
        print(f"  Target: {self.host}:{self.port}")
        print(f"  Interval: {self.send_interval * 1000}ms")
        print(f"  Topology: {TURBINE_COUNT} turbines x {SENSORS_PER_TURBINE} sensors "
              f"= {TURBINE_COUNT * SENSORS_PER_TURBINE} packets/ms")
        print(f"  Injection: {stage_desc}")
        print("=" * 60)

        threads = []
        updater = threading.Thread(target=self.level_updater_thread, daemon=True)
        updater.start(); threads.append(updater)
        stats_t = threading.Thread(target=self.stats_printer_thread, daemon=True)
        stats_t.start(); threads.append(stats_t)
        for tid in range(TURBINE_COUNT):
            t = threading.Thread(target=self.turbine_sender_thread, args=(tid,), daemon=True)
            t.start(); threads.append(t)

        try:
            while self.running:
                time.sleep(1)
        except KeyboardInterrupt:
            print("\nStopping...")
            self.stop()

    def stop(self):
        self.running = False
        time.sleep(0.5)
        self.sock.close()
        with self.lock:
            elapsed = time.time() - self.stats['start_time'] if self.stats['start_time'] > 0 else 1
            print("\n" + "=" * 60)
            print("  PXI Simulator Final Stats:")
            print(f"  Packets: {self.stats['packets_sent']}")
            print(f"  Bytes:   {self.stats['bytes_sent'] / 1e9:.2f} GB")
            print(f"  Duration:{elapsed:.1f}s")
            print(f"  Rate:    {self.stats['packets_sent'] / elapsed:.0f} pkt/s")
            print(f"  Throughput: {self.stats['bytes_sent'] * 8 / elapsed / 1e6:.2f} Mbps")
            print("=" * 60)


def parse_injection_map(specs: list) -> dict:
    result = {}
    if not specs:
        return result
    for spec in specs:
        for item in spec.split(','):
            item = item.strip()
            if ':' not in item:
                continue
            tid_str, stage_str = item.split(':', 1)
            try:
                tid = int(tid_str)
                stage_map = {
                    'normal': CavitationStage.NORMAL,
                    'incipient': CavitationStage.INCIPIENT,
                    'critical': CavitationStage.CRITICAL,
                    'developed': CavitationStage.DEVELOPED,
                    '0': CavitationStage.NORMAL,
                    '1': CavitationStage.INCIPIENT,
                    '2': CavitationStage.CRITICAL,
                    '3': CavitationStage.DEVELOPED,
                }
                stage = stage_map.get(stage_str.lower())
                if stage is not None and 1 <= tid <= TURBINE_COUNT:
                    result[tid - 1] = stage
            except ValueError:
                pass
    return result


simulator = None


def signal_handler(signum, frame):
    global simulator
    print("\nReceived signal, shutting down...")
    if simulator:
        simulator.stop()
    sys.exit(0)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(
        description='PXI Data Acquisition Simulator v2.0',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Normal operation (all turbines normal)
  python pxi_simulator.py -H 127.0.0.1 -p 9000

  # Inject cavitation: turbine#2 developed, turbine#4 critical
  python pxi_simulator.py -H pxi_collector -p 9000 --inject-cavitation 2:developed 4:critical

  # Multiple stages
  python pxi_simulator.py --inject-cavitation 1:incipient,3:critical,5:developed,6:normal
        """
    )
    parser.add_argument('-H', '--host', default='127.0.0.1')
    parser.add_argument('-p', '--port', type=int, default=9000)
    parser.add_argument('-i', '--interval', type=float, default=PACKET_INTERVAL)
    parser.add_argument('--inject-cavitation', nargs='*', default=[],
                        help='Injection spec: TURBINE_ID:STAGE (e.g. 2:developed 4:critical)')
    args = parser.parse_args()

    injection_map = parse_injection_map(args.inject_cavitation)

    simulator = PXISimulator(args.host, args.port, args.interval, injection_map)

    sig_module.signal(sig_module.SIGINT, signal_handler)
    sig_module.signal(sig_module.SIGTERM, signal_handler)

    simulator.start()
