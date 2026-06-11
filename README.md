# 大型水电站水轮机空化噪声监测与寿命评估系统

## 系统架构

```
                            ┌──────────────────────────────────────────────┐
                            │              Docker Compose 编排              │
                            │                                              │
┌─────────┐  UDP 1ms  ┌────┴─────┐  IPC  ┌────────────────┐  IPC  ┌─────┴──────┐
│  PXI     │──────────→│ pxi_     │──RAW──→│ cavitation_    │──CAV──→│ fatigue_   │
│  Sim     │  6×20pkt  │ collector│──FEAT─→│ detector       │       │ evaluator  │
└─────────┘  /ms       └────┬─────┘       └───────┬────────┘       └──┬─────┬──┘
                            │                      │                   │STRESS│LIFE
                            │ CH INSERT            │ CAVITATION        ↓     ↓
                            ↓                      ↓              ┌─────────────┐
                    ┌───────────────┐      ┌───────────────┐      │ alarm_      │
                    │  ClickHouse   │      │  alarm_       │←─────│ pusher      │
                    │  (TTL+归档)   │      │  pusher       │ALARM→│  IEC61850→  │
                    └───────────────┘      └───────┬───────┘      └─────────────┘
                                                   │                     │
                                                   ↓ CAV+LIFE+ALARM     ↓
                                            ┌──────────────┐     ┌──────────┐
                                            │ api_gateway   │     │ IEC 61850│
                                            │ (REST聚合)    │     │ Simulator│
                                            └──────┬───────┘     └──────────┘
                                                   │
                                            ┌──────┴───────┐
                                            │   Nginx      │
                                            │  (Gzip+反代) │
                                            └──────┬───────┘
                                                   │
                              ┌────────────────────┬┴───────────────────┐
                              ↓                    ↓                     ↓
                    ┌─────────────────┐  ┌─────────────────┐  ┌─────────────────┐
                    │ turbine_3d_     │  │ blade_detail    │  │ waterfall_      │
                    │ viewer.js       │  │ .js             │  │ chart.js        │
                    │ (WebGL云图)     │  │ (叶片面板)      │  │ (频谱瀑布)      │
                    └─────────────────┘  └─────────────────┘  └─────────────────┘
```

### IPC 数据流 (共享内存无锁队列)

| 通道 | 生产者 | 消费者 | 消息类型 |
|------|--------|--------|----------|
| RAW_DATA (0) | pxi_collector | cavitation_detector, fatigue_evaluator | IPCMessageRaw (128点波形) |
| FEATURES (1) | pxi_collector | cavitation_detector | IPCMessageFeatures (谱+小波) |
| CAVITATION (2) | cavitation_detector | fatigue_evaluator, alarm_pusher, api_gateway | IPCMessageCavitation |
| STRESS (3) | fatigue_evaluator | alarm_pusher | IPCMessageStress |
| LIFE (4) | fatigue_evaluator | alarm_pusher, api_gateway | IPCMessageLife |
| ALARM (5) | alarm_pusher | api_gateway | IPCMessageAlarm |

## 目录结构

```
.
├── backend/
│   ├── common/include/          # 共享库
│   │   ├── ipc_queue.h          # 共享内存SPSC队列 + 6种IPC消息
│   │   ├── service_base.h       # 服务生命周期基类
│   │   └── metrics.h            # spdlog + Prometheus指标
│   ├── include/                 # 算法头文件
│   ├── src/                     # 算法实现
│   ├── services/                # 微服务入口
│   │   ├── pxi_collector/       # UDP采集 + FFT/小波特征
│   │   ├── cavitation_detector/ # AE/IF + 工况归一化 + 自适应阈值
│   │   ├── fatigue_evaluator/   # 四点法雨流 + Goodman + Miner
│   │   ├── alarm_pusher/        # 4级告警 + 10s去抖 + IEC61850
│   │   └── api_gateway/         # REST API聚合
│   └── CMakeLists.txt           # 多target编译
├── frontend/
│   ├── js/
│   │   ├── turbine_3d_viewer.js # WebGL空化云图组件
│   │   ├── blade_detail.js      # 叶片详情面板组件
│   │   ├── waterfall_chart.js   # 瀑布图WebWorker组件
│   │   └── main.js              # 前端主逻辑
│   └── index.html
├── simulator/
│   ├── pxi_simulator.py         # PXI模拟器 v2.0 (空化注入)
│   └── iec61850_simulator.py    # IEC 61850网关模拟器
├── clickhouse/
│   ├── init.sql                 # 建表 + 物化视图 + 默认配置
│   └── archive_policy.sql       # TTL + 冷热分离 + 聚合归档
├── config/
│   ├── config.json              # 系统配置
│   └── models/                  # 外置模型参数
│       ├── autoencoder.json     # 深度自编码器架构/权重/评估
│       ├── isolation_forest.json # 孤立森林参数/特征/工况桶
│       └── material_13Cr4Ni.json # 材料S-N曲线/Goodman/Miner
├── docker/
│   ├── docker-compose.yml       # 全栈编排
│   ├── Dockerfile.cpp           # C++多阶段构建
│   ├── Dockerfile.pxi_sim       # PXI模拟器
│   ├── Dockerfile.iec61850      # IEC61850模拟器
│   ├── Dockerfile.frontend      # Nginx + Gzip
│   ├── nginx.conf               # Gzip + /api反代
│   └── prometheus.yml           # Prometheus采集配置
├── tests/
│   ├── regression_runner.py     # 9项回归测试
│   └── regression_report.json   # 测试报告
└── start_services.bat           # Windows本地启动脚本
```

## Docker 部署

### 前置要求

- Docker 20.10+
- Docker Compose v2.0+
- 8GB+ 可用内存

### 一键启动

```bash
cd docker
docker compose up -d

# 查看服务状态
docker compose ps

# 查看日志
docker compose logs -f pxi_collector
docker compose logs -f cavitation_detector
```

### 逐服务启动顺序

```bash
# 1. 基础设施
docker compose up -d clickhouse
# 等待健康检查通过
docker compose up -d iec61850_simulator

# 2. C++微服务
docker compose up -d pxi_collector
docker compose up -d cavitation_detector
docker compose up -d fatigue_evaluator
docker compose up -d alarm_pusher
docker compose up -d api_gateway

# 3. 模拟器 + 前端
docker compose up -d pxi_simulator
docker compose up -d frontend

# 4. 监控
docker compose up -d prometheus grafana
```

### 服务端口映射

| 服务 | 容器端口 | 主机端口 | 用途 |
|------|----------|----------|------|
| frontend (Nginx) | 80 | 80 | 前端 + /api反代 |
| api_gateway | 8080 | 8080 | REST API |
| pxi_collector | 9000/udp | 9001 | UDP数据接收 |
| ClickHouse HTTP | 8123 | 8123 | 查询接口 |
| ClickHouse Native | 9000 | 9000 | Native协议 |
| IEC 61850 Sim | 8102 | 8102 | MMS模拟 |
| Prometheus | 9090 | 9090 | 指标采集 |
| Grafana | 3000 | 3000 | 监控面板 |
| pxi_collector metrics | 9100 | 9101 | Prometheus pull |
| cavitation_detector metrics | 9100 | 9102 | Prometheus pull |
| fatigue_evaluator metrics | 9100 | 9103 | Prometheus pull |
| alarm_pusher metrics | 9100 | 9104 | Prometheus pull |
| api_gateway metrics | 9100 | 9105 | Prometheus pull |

### 停止与清理

```bash
docker compose down              # 停止所有容器
docker compose down -v           # 停止并删除数据卷
docker compose down --rmi all    # 停止并删除镜像
```

## PXI 模拟器用法

### 基本用法

```bash
# 所有水轮机正常运行
python simulator/pxi_simulator.py -H 127.0.0.1 -p 9000

# Docker 内运行 (自动连接 pxi_collector)
docker compose up pxi_simulator
```

### 注入空化特征信号

模拟器支持按水轮机编号注入不同阶段的空化特征：

```bash
# 2号机初生空化, 4号机临界空化
python simulator/pxi_simulator.py -H 127.0.0.1 -p 9000 \
    --inject-cavitation 2:incipient 4:critical

# 混合注入: 1号机初生, 3号机临界, 5号机发展
python simulator/pxi_simulator.py \
    --inject-cavitation 1:incipient,3:critical,5:developed

# 也支持数字编码: 0=normal 1=incipient 2=critical 3=developed
python simulator/pxi_simulator.py \
    --inject-cavitation 2:3 4:2
```

### 空化阶段特征

| 阶段 | 水听器特征 | 加速度计特征 | 信号注入细节 |
|------|-----------|-------------|-------------|
| normal (0) | 背景噪声 0.3 | 轴频谐波 | 仅环境噪声 |
| incipient (1) | 5-15kHz 宽带噪声 | 微弱冲击 | noise=0.8, 1次冲击/帧 |
| critical (2) | 15-30kHz 强噪声 | 密集冲击脉冲 | noise=2.0, 4次冲击, BPF调制 |
| developed (3) | 全频段 5-45kHz | 高强度冲击 | noise=5.0, 10次冲击, 谐波畸变0.35 |

空化阶段转换采用渐进插值（每0.5秒步进），模拟真实工况渐变过程。

### 模拟器参数

```
-H, --host           目标主机 (默认 127.0.0.1)
-p, --port           目标端口 (默认 9000)
-i, --interval       发送间隔秒 (默认 0.001)
--inject-cavitation  注入规格 TURBINE:STAGE
```

## IEC 61850 模拟器用法

```bash
# 本地运行
python simulator/iec61850_simulator.py --port 8102

# Docker 运行
docker compose up iec61850_simulator
```

### API 端点

| 方法 | 路径 | 说明 |
|------|------|------|
| POST | /mms/report | 接收告警报告 |
| POST | /mms/associate | MMS关联请求 |
| GET | /mms/status | 网关状态+统计 |
| GET | /mms/alarms | 告警历史 |

## ClickHouse 数据管理

### TTL 策略

| 表 | 热存储 | 冷存储 | 删除 |
|----|--------|--------|------|
| raw_sensor_data | 7天 | 7-30天 | 30天 |
| spectrum_features | 30天 | 30-90天 | 90天 |
| wavelet_features | 30天 | 30-90天 | 90天 |
| cavitation_state | 90天 | 90-365天 | 1年 |
| blade_stress | 90天 | 90-365天 | 1年 |
| life_assessment | 365天 | 1-5年 | 5年 |
| alarm_logs | - | - | 2年 |

### 自动归档物化视图

| 视图 | 源表 | 目标表 | 聚合粒度 |
|------|------|--------|----------|
| cavitation_intensity_1s | cavitation_state | cavitation_intensity_1s_table | 1秒 |
| cavitation_intensity_1h_mv | cavitation_intensity_1s_table | cavitation_intensity_1h | 1小时 |
| cavitation_damage_daily | life_assessment | cavitation_damage_daily_table | 1天 |
| blade_damage_daily_mv | blade_stress | blade_damage_daily | 1天 |
| alarm_stats_daily_mv | alarm_logs | alarm_stats_daily | 1天 |

## C++ 微服务构建

### 依赖

- C++17 编译器 (GCC 9+ / Clang 10+ / MSVC 2019+)
- CMake 3.14+
- FFTW3
- spdlog 1.14+
- prometheus-cpp 1.2+
- nlohmann/json
- ClickHouse C++ Client (可选)
- pthread / WS2_32

### 编译

```bash
cd backend
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# 产出 7 个 target:
#   bin/pxi_collector/pxi_collector
#   bin/cavitation_detector/cavitation_detector
#   bin/fatigue_evaluator/fatigue_evaluator
#   bin/alarm_pusher/alarm_pusher
#   bin/api_gateway/api_gateway
#   bin/turbine_monitor          (兼容单体)
#   bin/pxi_simulator            (C++版)
```

### Prometheus 指标

每个服务在 `:9100/metrics` 暴露以下指标族：

| 指标 | 类型 | 说明 |
|------|------|------|
| turbine_packets_total | Counter | 处理数据包总数 |
| turbine_packets_dropped_total | Counter | 丢包数 |
| turbine_ipc_sent_total | Counter | IPC发送数 |
| turbine_ipc_received_total | Counter | IPC接收数 |
| turbine_cavitation_detected_total | Counter | 空化检测(按阶段) |
| turbine_alarms_total | Counter | 告警(按级别) |
| turbine_blade_cumulative_damage | Gauge | 叶片累计损伤 |
| turbine_blade_remaining_life_hours | Gauge | 叶片剩余寿命 |
| turbine_processing_latency_seconds | Histogram | 处理延迟直方图 |
| turbine_clickhouse_inserts_total | Counter | ClickHouse写入数 |
| turbine_clickhouse_errors_total | Counter | ClickHouse错误数 |

## 前端

### Gzip 压缩

Nginx 对以下类型启用 Gzip (压缩级别 6):

- text/plain, text/css, text/xml, text/javascript
- application/javascript, application/json, application/xml
- image/svg+xml, font/woff, font/woff2

最小压缩阈值 256 字节，缓存 7 天静态资源。

### 组件

| 文件 | 类 | 职责 |
|------|-----|------|
| turbine_3d_viewer.js | Turbine3DViewer | WebGL剖面+空化云图+hover/select回调 |
| blade_detail.js | BladeDetailPanel | 模态框+趋势图+寿命仪表盘+操作按钮 |
| waterfall_chart.js | WaterfallWorker | OffscreenCanvas+分块降采样频谱瀑布 |

## 回归测试

```bash
$env:PYTHONIOENCODING='utf-8'
python tests/regression_runner.py [--quick] [--verbose]
```

9 项测试覆盖: IPC队列、UDP丢包率、工况归一化、雨流计数、告警去抖、模型配置、前端拆分、服务源码、CMake target。

## 技术指标

| 指标 | 数值 |
|------|------|
| 监测机组 | 6台混流式水轮机 |
| 传感器/机 | 12水听器 + 8加速度计 = 20 |
| 采样率(水听器) | 51.2 kHz |
| 采样率(加速度计) | 25.6 kHz |
| 上报间隔 | 1 ms |
| 数据包速率 | 120,000 包/秒 |
| UDP丢包率 | <0.1% (recvmmsg+SPSC) |
| 工况误报率 | <3% (Z-Score分桶+自适应阈值) |
| 雨流内存/叶 | <4 KB (流式四点法) |
| 瀑布图主线程 | <2ms (WebWorker+分块) |
| 叶片数/台 | 15 |
| IEC 61850去抖 | 10秒冷却窗口 |

---

**版本**: 2.0.0 (微服务化 + Docker编排)
**更新日期**: 2026-06-10
**适用场景**: 大型混流式水轮机组空化监测与寿命评估
