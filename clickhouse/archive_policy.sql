-- =========================================
-- ClickHouse 自动归档策略
-- =========================================
-- 原始数据: 30天热数据 → 归档到冷存储
-- 特征/空化: 90天 → 年度聚合后归档
-- 疲劳/寿命: 365天 → 保留
-- 告警: 730天 → 保留
-- =========================================

USE turbine_monitor;

-- =========================================
-- 1. 冷热数据移动策略 (TIERED STORAGE)
-- =========================================

-- 注意: 以下 ALTER TABLE 操作需要 ClickHouse 23.8+ 版本
-- 如版本不支持，可改用 PARTITION MOVE 手动操作

-- raw_sensor_data: 7天后移到冷卷
ALTER TABLE raw_sensor_data
    MODIFY TTL
        toDateTime(timestamp / 1000) + INTERVAL 7 DAY TO VOLUME 'cold',
        toDateTime(timestamp / 1000) + INTERVAL 30 DAY DELETE;

-- spectrum_features: 30天后移冷卷，90天后删除
ALTER TABLE spectrum_features
    MODIFY TTL
        toDateTime(timestamp / 1000) + INTERVAL 30 DAY TO VOLUME 'cold',
        toDateTime(timestamp / 1000) + INTERVAL 90 DAY DELETE;

-- wavelet_features: 30天后移冷卷，90天后删除
ALTER TABLE wavelet_features
    MODIFY TTL
        toDateTime(timestamp / 1000) + INTERVAL 30 DAY TO VOLUME 'cold',
        toDateTime(timestamp / 1000) + INTERVAL 90 DAY DELETE;

-- cavitation_state: 90天后移冷卷，365天后删除
ALTER TABLE cavitation_state
    MODIFY TTL
        toDateTime(timestamp / 1000) + INTERVAL 90 DAY TO VOLUME 'cold',
        toDateTime(timestamp / 1000) + INTERVAL 365 DAY DELETE;

-- blade_stress: 90天后移冷卷，365天后删除
ALTER TABLE blade_stress
    MODIFY TTL
        toDateTime(timestamp / 1000) + INTERVAL 90 DAY TO VOLUME 'cold',
        toDateTime(timestamp / 1000) + INTERVAL 365 DAY DELETE;

-- life_assessment: 365天后移冷卷，1825天后删除 (5年)
ALTER TABLE life_assessment
    MODIFY TTL
        toDateTime(timestamp / 1000) + INTERVAL 365 DAY TO VOLUME 'cold',
        toDateTime(timestamp / 1000) + INTERVAL 1825 DAY DELETE;

-- alarm_logs: 730天后删除 (2年)
ALTER TABLE alarm_logs
    MODIFY TTL
        toDateTime(timestamp / 1000) + INTERVAL 730 DAY DELETE;

-- =========================================
-- 2. 物化视图自动归档聚合
-- =========================================

-- 小时级空化强度聚合 (从1秒物化视图进一步压缩)
CREATE TABLE IF NOT EXISTS cavitation_intensity_1h (
    time_1h DateTime,
    turbine_id UInt8,
    blade_id UInt8,
    avg_intensity Float32,
    max_intensity Float32,
    p95_intensity Float32,
    min_intensity Float32,
    sample_count UInt64
) ENGINE = AggregatingMergeTree()
PARTITION BY toDate(time_1h)
ORDER BY (time_1h, turbine_id, blade_id)
TTL time_1h + INTERVAL 1825 DAY;

CREATE MATERIALIZED VIEW IF NOT EXISTS cavitation_intensity_1h_mv
TO cavitation_intensity_1h
AS SELECT
    toStartOfHour(time_1s) AS time_1h,
    turbine_id,
    blade_id,
    avg(avg_intensity) AS avg_intensity,
    max(max_intensity) AS max_intensity,
    quantile(0.95)(max_intensity) AS p95_intensity,
    min(avg_intensity) AS min_intensity,
    count() AS sample_count
FROM cavitation_intensity_1s_table
GROUP BY time_1h, turbine_id, blade_id;

-- 日级叶片损伤聚合 (预聚合避免实时查询扫描大表)
CREATE TABLE IF NOT EXISTS blade_damage_daily (
    date Date,
    turbine_id UInt8,
    blade_id UInt8,
    start_damage Float32,
    end_damage Float32,
    damage_increment Float32,
    avg_stress Float32,
    max_stress Float32,
    cycle_count UInt32,
    miner_increment Float32
) ENGINE = AggregatingMergeTree()
PARTITION BY toYYYYMM(date)
ORDER BY (date, turbine_id, blade_id)
TTL date + INTERVAL 3650 DAY;

CREATE MATERIALIZED VIEW IF NOT EXISTS blade_damage_daily_mv
TO blade_damage_daily
AS SELECT
    toDate(timestamp / 1000) AS date,
    turbine_id,
    blade_id,
    min(cumulative_damage) AS start_damage,
    max(cumulative_damage) AS end_damage,
    max(cumulative_damage) - min(cumulative_damage) AS damage_increment,
    avg(combined_stress) AS avg_stress,
    max(max_stress) AS max_stress,
    sum(stress_cycles) AS cycle_count,
    max(miner_sum) - min(miner_sum) AS miner_increment
FROM blade_stress
GROUP BY date, turbine_id, blade_id;

-- 日级告警统计
CREATE TABLE IF NOT EXISTS alarm_stats_daily (
    date Date,
    turbine_id UInt8,
    alarm_type String,
    alarm_level String,
    alarm_count UInt64,
    avg_actual_value Float32,
    max_actual_value Float32
) ENGINE = AggregatingMergeTree()
PARTITION BY toYYYYMM(date)
ORDER BY (date, turbine_id, alarm_type, alarm_level)
TTL date + INTERVAL 1825 DAY;

CREATE MATERIALIZED VIEW IF NOT EXISTS alarm_stats_daily_mv
TO alarm_stats_daily
AS SELECT
    toDate(timestamp / 1000) AS date,
    turbine_id,
    toString(alarm_type) AS alarm_type,
    toString(alarm_level) AS alarm_level,
    count() AS alarm_count,
    avg(actual_value) AS avg_actual_value,
    max(actual_value) AS max_actual_value
FROM alarm_logs
GROUP BY date, turbine_id, alarm_type, alarm_level;

-- =========================================
-- 3. 存储策略配置 (需要在 config.xml 中配合)
-- =========================================
-- ClickHouse config.xml 需添加:
--
-- <storage_configuration>
--     <disks>
--         <hot>
--             <path>/var/lib/clickhouse/hot/</path>
--             <keep_free_space_bytes>1073741824</keep_free_space_bytes>
--         </hot>
--         <cold>
--             <path>/var/lib/clickhouse/cold/</path>
--         </cold>
--     </disks>
--     <policies>
--         <tiered>
--             <volumes>
--                 <hot>
--                     <disk>hot</disk>
--                     <max_data_part_size_bytes>10737418240</max_data_part_size_bytes>
--                 </hot>
--                 <cold>
--                     <disk>cold</disk>
--                 </cold>
--             </volumes>
--             <move_factor>0.2</move_factor>
--         </tiered>
--     </policies>
-- </storage_configuration>
--
-- 然后对表设置存储策略:
-- ALTER TABLE raw_sensor_data SET SETTINGS storage_policy = 'tiered';
