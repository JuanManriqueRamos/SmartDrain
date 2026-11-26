"""Conexión y consultas a MariaDB."""
from contextlib import contextmanager

import pymysql

import config


def _connect():
    return pymysql.connect(
        host=config.DB_HOST,
        user=config.DB_USER,
        password=config.DB_PASSWORD,
        database=config.DB_NAME,
        charset="utf8mb4",
        cursorclass=pymysql.cursors.DictCursor,
        autocommit=False,
    )


@contextmanager
def get_connection():
    """Entrega una conexión; hace rollback si hay error y siempre la cierra."""
    conn = _connect()
    try:
        yield conn
    except Exception:
        conn.rollback()
        raise
    finally:
        conn.close()


# --- Configuración de alertas ---
def get_config(conn):
    with conn.cursor() as cur:
        cur.execute(
            "SELECT min_distance_cm, max_distance_cm, alert_sensor_error, alert_recovery "
            "FROM alert_config WHERE id=1"
        )
        row = cur.fetchone()
    if not row:
        raise RuntimeError("No existe la configuración inicial en alert_config")
    return row


def update_config(conn, minimo, maximo, alert_error, alert_recovery):
    with conn.cursor() as cur:
        cur.execute(
            "UPDATE alert_config SET min_distance_cm=%s, max_distance_cm=%s, "
            "alert_sensor_error=%s, alert_recovery=%s WHERE id=1",
            (minimo, maximo, alert_error, alert_recovery),
        )


# --- Estado de alertas ---
def get_state_for_update(conn):
    """Bloquea la fila de estado para serializar POST simultáneos."""
    with conn.cursor() as cur:
        cur.execute("SELECT current_state FROM alert_state WHERE id=1 FOR UPDATE")
        row = cur.fetchone()
    if not row:
        raise RuntimeError("Falta fila inicial en alert_state")
    return row["current_state"]


def update_state(conn, new_state):
    with conn.cursor() as cur:
        cur.execute(
            "UPDATE alert_state SET current_state=%s, changed_at=CURRENT_TIMESTAMP WHERE id=1",
            (new_state,),
        )


# --- Lecturas ---
_COLUMNAS_LECTURA = (
    "id, node_id, message_id, sensor_timestamp, timestamp_source, distance_cm, sensor_ok, "
    "hx710b_raw, pressure_kpa, water_level_cm, net_flow_l_min, received_at, gateway_id, "
    "gateway_time, central_ack, duplicates, frame_length, frame_crc32"
)


def reading_exists(conn, message_id):
    with conn.cursor() as cur:
        cur.execute("SELECT 1 FROM lecturas_lora WHERE message_id=%s LIMIT 1", (message_id,))
        return cur.fetchone() is not None


def insert_reading(conn, lectura):
    """Guarda TODAS las variables de la trama (los nulos quedan como NULL)."""
    with conn.cursor() as cur:
        cur.execute(
            "INSERT INTO lecturas_lora (node_id, message_id, sensor_timestamp, timestamp_source, "
            "distance_cm, sensor_ok, hx710b_raw, pressure_kpa, water_level_cm, net_flow_l_min, "
            "gateway_id, gateway_time, central_ack, duplicates, frame_length, frame_crc32, raw_frame) "
            "VALUES (%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s)",
            (
                lectura.node_id, lectura.message_id, lectura.sensor_time, lectura.timestamp_source,
                lectura.distance, int(lectura.sensor_ok), lectura.hx710b_raw, lectura.pressure_kpa,
                lectura.water_level_cm, lectura.net_flow_l_min, lectura.gateway_id,
                lectura.gateway_time,
                None if lectura.central_ack is None else int(lectura.central_ack),
                lectura.duplicates, lectura.frame_length, lectura.frame_crc32, lectura.raw_frame,
            ),
        )


def get_dashboard_data(conn, node_id, history_limit):
    """Datos crudos para /api/dashboard (la serialización JSON la hace app.py)."""
    with conn.cursor() as cur:
        cur.execute(
            "SELECT COUNT(*) AS total, COALESCE(SUM(sensor_ok=1),0) AS ok_count, "
            "COALESCE(SUM(sensor_ok=0),0) AS error_count, MAX(received_at) AS last_received, "
            "TIMESTAMPDIFF(SECOND, MAX(received_at), NOW()) AS seconds_since_last "
            "FROM lecturas_lora WHERE node_id=%s",
            (node_id,),
        )
        summary = cur.fetchone()
        # Últimas N lecturas, devueltas en orden cronológico para los gráficos.
        cur.execute(
            f"SELECT * FROM (SELECT {_COLUMNAS_LECTURA} FROM lecturas_lora WHERE node_id=%s "
            "ORDER BY received_at DESC, id DESC LIMIT %s) AS ultimas ORDER BY received_at, id",
            (node_id, int(history_limit)),
        )
        readings = cur.fetchall()
        cur.execute("SELECT current_state, changed_at FROM alert_state WHERE id=1")
        state = cur.fetchone()
        cur.execute("SELECT offline, hx_missing FROM node_watchdog WHERE node_id=%s", (node_id,))
        watchdog = cur.fetchone()
    return summary, readings, state, watchdog


# --- Watchdog de "sensor faltante" ---
def touch_watchdog(conn, node_id, hx_missing_now, confirm_readings):
    """Registra que el nodo reportó y aplica antirrebote al estado del HX710B.

    Devuelve {"was_offline", "silencio_s", "hx_change"} donde hx_change es
    None, "missing" o "restored" (solo tras confirm_readings lecturas seguidas)."""
    with conn.cursor() as cur:
        cur.execute(
            "SELECT offline, hx_missing, hx_streak, "
            "TIMESTAMPDIFF(SECOND, last_seen, NOW()) AS silencio_s "
            "FROM node_watchdog WHERE node_id=%s FOR UPDATE",
            (node_id,),
        )
        row = cur.fetchone()
        estado_hx = bool(row["hx_missing"]) if row else False
        racha = row["hx_streak"] if row else 0
        cambio = None
        if bool(hx_missing_now) == estado_hx:
            racha = 0
        else:
            racha += 1
            if racha >= confirm_readings:
                estado_hx = bool(hx_missing_now)
                cambio = "missing" if estado_hx else "restored"
                racha = 0
        cur.execute(
            "INSERT INTO node_watchdog (node_id, last_seen, offline, hx_missing, hx_streak) "
            "VALUES (%s, NOW(), 0, %s, %s) "
            "ON DUPLICATE KEY UPDATE last_seen=NOW(), offline=0, "
            "hx_missing=VALUES(hx_missing), hx_streak=VALUES(hx_streak)",
            (node_id, int(estado_hx), min(racha, 255)),
        )
    return {
        "was_offline": bool(row and row["offline"]),
        "silencio_s": row["silencio_s"] if row else None,
        "hx_change": cambio,
    }


def mark_offline_if_silent(conn, node_id, offline_seconds):
    """Marca offline=1 si el nodo lleva más de offline_seconds sin reportar.
    Devuelve los segundos de silencio si se acaba de marcar, si no None.
    FOR UPDATE evita avisos dobles con varios workers de gunicorn."""
    with conn.cursor() as cur:
        cur.execute(
            "SELECT offline, TIMESTAMPDIFF(SECOND, last_seen, NOW()) AS silencio_s "
            "FROM node_watchdog WHERE node_id=%s FOR UPDATE",
            (node_id,),
        )
        row = cur.fetchone()
        if not row or row["offline"] or row["silencio_s"] is None \
                or row["silencio_s"] < offline_seconds:
            return None
        cur.execute("UPDATE node_watchdog SET offline=1 WHERE node_id=%s", (node_id,))
        return row["silencio_s"]
