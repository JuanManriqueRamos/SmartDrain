CREATE DATABASE IF NOT EXISTS nodo_sensor CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
USE nodo_sensor;

CREATE TABLE IF NOT EXISTS lecturas_lora (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    node_id VARCHAR(32) NOT NULL,
    sensor_timestamp DATETIME NOT NULL,
    distance_cm DECIMAL(7,2) NULL,
    sensor_ok TINYINT(1) NOT NULL,
    received_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (id),
    INDEX idx_node_timestamp (node_id, sensor_timestamp)
);

CREATE TABLE IF NOT EXISTS alert_config (
    id TINYINT UNSIGNED NOT NULL PRIMARY KEY,
    min_distance_cm DECIMAL(7,2) NOT NULL DEFAULT 10.00,
    max_distance_cm DECIMAL(7,2) NOT NULL DEFAULT 100.00,
    alert_sensor_error TINYINT(1) NOT NULL DEFAULT 1,
    alert_recovery TINYINT(1) NOT NULL DEFAULT 1,
    CHECK (min_distance_cm >= 5 AND max_distance_cm <= 400 AND min_distance_cm < max_distance_cm)
);

CREATE TABLE IF NOT EXISTS alert_state (
    id TINYINT UNSIGNED NOT NULL PRIMARY KEY,
    current_state ENUM('normal','urgency','sensor_error') NOT NULL DEFAULT 'normal',
    changed_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP
);

INSERT IGNORE INTO alert_config (id, min_distance_cm, max_distance_cm, alert_sensor_error, alert_recovery)
VALUES (1, 10.00, 100.00, 1, 1);
INSERT IGNORE INTO alert_state (id, current_state) VALUES (1, 'normal');

-- ===========================================================================
-- Migración v2 (reenvío íntegro de la trama + watchdog). Idempotente: se puede
-- ejecutar varias veces. Las filas antiguas quedan con NULL en las columnas nuevas.
-- ===========================================================================
ALTER TABLE lecturas_lora
    ADD COLUMN IF NOT EXISTS message_id CHAR(16) NULL AFTER node_id,
    ADD COLUMN IF NOT EXISTS hx710b_raw INT NULL AFTER sensor_ok,
    ADD COLUMN IF NOT EXISTS pressure_kpa DECIMAL(9,3) NULL AFTER hx710b_raw,
    ADD COLUMN IF NOT EXISTS water_level_cm DECIMAL(7,2) NULL AFTER pressure_kpa,
    ADD COLUMN IF NOT EXISTS net_flow_l_min DECIMAL(9,2) NULL AFTER water_level_cm,
    ADD COLUMN IF NOT EXISTS timestamp_source ENUM('sensor','gateway','cloud') NULL AFTER sensor_timestamp,
    ADD COLUMN IF NOT EXISTS gateway_id VARCHAR(32) NULL,
    ADD COLUMN IF NOT EXISTS gateway_time DATETIME NULL,
    ADD COLUMN IF NOT EXISTS central_ack TINYINT(1) NULL,
    ADD COLUMN IF NOT EXISTS duplicates INT UNSIGNED NULL,
    ADD COLUMN IF NOT EXISTS frame_length SMALLINT UNSIGNED NULL,
    ADD COLUMN IF NOT EXISTS frame_crc32 CHAR(8) NULL,
    ADD COLUMN IF NOT EXISTS raw_frame VARCHAR(255) NULL;

-- Un message_id solo puede guardarse una vez (reintentos HTTP/LoRa duplicados).
-- Varias filas antiguas con NULL no chocan con este índice.
CREATE UNIQUE INDEX IF NOT EXISTS uq_message_id ON lecturas_lora (message_id);
CREATE INDEX IF NOT EXISTS idx_received_at ON lecturas_lora (received_at);

-- Estado del nodo para avisos de "sensor faltante" (sin reportes / HX710B ausente).
CREATE TABLE IF NOT EXISTS node_watchdog (
    node_id VARCHAR(32) NOT NULL PRIMARY KEY,
    last_seen TIMESTAMP NULL,
    offline TINYINT(1) NOT NULL DEFAULT 0,
    hx_missing TINYINT(1) NOT NULL DEFAULT 0,
    hx_streak TINYINT UNSIGNED NOT NULL DEFAULT 0,
    changed_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP
);
-- Por si node_watchdog se creó con una versión previa de este script.
ALTER TABLE node_watchdog ADD COLUMN IF NOT EXISTS hx_streak TINYINT UNSIGNED NOT NULL DEFAULT 0 AFTER hx_missing;
