"""Validación y procesamiento de lecturas del sensor.

Formato principal (NodoCentral v2): un *sobre* con la trama original del
NodoSensor1 sin modificar, más datos de integridad:

    {"gateway_id": "NodoSensor2", "message_id": "<16 hex>",
     "frame_length": 230, "frame_crc32": "<8 hex>",
     "gateway_time": "AAAA-MM-DD HH:MM:SS" | null,
     "central_ack": true, "duplicates": 0,
     "frame": "{\"node_id\":\"NodoSensor1\",\"message_id\":\"...\", ...}"}

La trama (`frame`) conserva exactamente las claves que arma el sensor:
node_id, message_id, timestamp, type, value, unit, status, hx710b_raw,
pressure_kpa, water_level_cm, net_flow_l_min (battery solo en tramas viejas).

También se aceptan los dos formatos anteriores para no perder lecturas que ya
estén en la cola de la microSD de la Central al actualizar el firmware.
"""
import json
import re
import zlib
from dataclasses import dataclass, field
from datetime import datetime
from decimal import Decimal, InvalidOperation
from typing import Optional

import alert_service
import config
import database


class ValidationError(Exception):
    def __init__(self, message: str, status: int):
        super().__init__(message)
        self.message = message
        self.status = status


@dataclass
class Lectura:
    node_id: str
    timestamp: str                 # texto mostrado en mensajes
    sensor_time: datetime          # hora local naive guardada en MariaDB
    sensor_ok: bool
    distance: Optional[Decimal]
    message_id: Optional[str] = None
    timestamp_source: str = "sensor"
    hx710b_raw: Optional[int] = None
    pressure_kpa: Optional[Decimal] = None
    water_level_cm: Optional[Decimal] = None
    net_flow_l_min: Optional[Decimal] = None
    gateway_id: Optional[str] = None
    gateway_time: Optional[datetime] = None
    central_ack: Optional[bool] = None
    duplicates: Optional[int] = None
    frame_length: Optional[int] = None
    frame_crc32: Optional[str] = None
    raw_frame: Optional[str] = None


@dataclass
class Resultado:
    estado: str
    duplicada: bool = False
    notificaciones: list = field(default_factory=list)


_HEX16 = re.compile(r"^[0-9A-Fa-f]{16}$")
_HEX8 = re.compile(r"^[0-9A-Fa-f]{8}$")
INT32_MIN, INT32_MAX = -(2 ** 31), 2 ** 31 - 1


def parse_timestamp(value):
    if not isinstance(value, str):
        return None
    try:
        if value.endswith("Z"):
            return datetime.strptime(value, "%Y-%m-%dT%H:%M:%SZ")
        return datetime.strptime(value, "%Y-%m-%d %H:%M:%S")
    except ValueError:
        return None


def _vacio(value) -> bool:
    """null, "" o clave ausente significan "sin dato"."""
    return value is None or value == ""


def _numero_opcional(data: dict, clave: str) -> Optional[Decimal]:
    value = data.get(clave)
    if _vacio(value):
        return None
    if isinstance(value, bool) or not isinstance(value, (int, float, str)):
        raise ValidationError(f"Campo {clave} inválido", 422)
    try:
        numero = Decimal(str(value))
    except InvalidOperation:
        raise ValidationError(f"Campo {clave} inválido", 422)
    if not numero.is_finite():
        raise ValidationError(f"Campo {clave} no finito", 422)
    return numero


def _entero_opcional(data: dict, clave: str) -> Optional[int]:
    value = data.get(clave)
    if _vacio(value):
        return None
    if isinstance(value, bool) or not isinstance(value, int) or not INT32_MIN <= value <= INT32_MAX:
        raise ValidationError(f"Campo {clave} inválido", 422)
    return value


def _distancia(sensor_ok: bool, value) -> Optional[Decimal]:
    if not sensor_ok:
        return None
    if _vacio(value) or isinstance(value, bool) or not isinstance(value, (int, float, str)):
        raise ValidationError("Distancia inválida", 422)
    try:
        distancia = Decimal(str(value))
    except InvalidOperation:
        raise ValidationError("Distancia inválida", 422)
    if not distancia.is_finite() or not config.SENSOR_MIN_CM <= distancia <= config.SENSOR_MAX_CM:
        raise ValidationError(
            f"Distancia fuera del rango válido del sensor "
            f"({config.SENSOR_MIN_CM}-{config.SENSOR_MAX_CM} cm)", 422)
    return distancia


def _elegir_hora(sensor_ts, gateway_time: Optional[datetime]):
    """Hora del sensor; si viene null, la de la Central; si no, la del Cloud."""
    if not _vacio(sensor_ts):
        sensor_time = parse_timestamp(sensor_ts)
        if sensor_time is None:
            raise ValidationError("Timestamp inválido", 422)
        return sensor_ts, sensor_time, "sensor"
    if gateway_time is not None:
        return gateway_time.strftime("%Y-%m-%d %H:%M:%S"), gateway_time, "gateway"
    ahora = datetime.now().replace(microsecond=0)
    return ahora.strftime("%Y-%m-%d %H:%M:%S"), ahora, "cloud"


def _validar_trama(trama: dict) -> dict:
    """Mismas reglas que packet_parse() del NodoCentral."""
    if trama.get("node_id") != config.NODE_ID:
        raise ValidationError("node_id inválido", 400)
    message_id = trama.get("message_id")
    if not isinstance(message_id, str) or not _HEX16.match(message_id):
        raise ValidationError("message_id inválido", 400)
    if not _vacio(trama.get("type")) and trama.get("type") != "distance":
        raise ValidationError("type inválido", 422)
    if not _vacio(trama.get("unit")) and trama.get("unit") != "cm":
        raise ValidationError("unit inválida", 422)

    status = trama.get("status")
    if status == "ok":
        sensor_ok = True
    elif status == "sensor_error":
        sensor_ok = False
        valor = trama.get("value")
        if not _vacio(valor) and _numero_opcional(trama, "value") != 0:
            raise ValidationError("sensor_error debe traer value 0 o null", 422)
    else:
        raise ValidationError("status inválido", 422)

    # Campo opcional heredado: debe ser null o número, se ignora.
    _numero_opcional(trama, "battery")
    return {
        "message_id": message_id.upper(),
        "sensor_ok": sensor_ok,
        "distance": _distancia(sensor_ok, trama.get("value")),
        "timestamp": trama.get("timestamp"),
        "hx710b_raw": _entero_opcional(trama, "hx710b_raw"),
        "pressure_kpa": _numero_opcional(trama, "pressure_kpa"),
        "water_level_cm": _numero_opcional(trama, "water_level_cm"),
        "net_flow_l_min": _numero_opcional(trama, "net_flow_l_min"),
    }


def _rechazar_constante(nombre):
    """json.loads acepta NaN/Infinity por defecto; en la trama no son válidos."""
    raise ValueError(f"Constante no permitida: {nombre}")


def _validar_sobre(data: dict) -> Lectura:
    frame = data.get("frame")
    frame_length = data.get("frame_length")
    frame_crc32 = data.get("frame_crc32")
    if (
        not isinstance(frame, str)
        or isinstance(frame_length, bool) or not isinstance(frame_length, int)
        or not isinstance(frame_crc32, str) or not _HEX8.match(frame_crc32)
    ):
        raise ValidationError("Sobre de trama incompleto", 400)

    # --- Integridad: longitud y CRC32 de la trama tal como la recibió la Central ---
    try:
        frame_bytes = frame.encode("ascii")
    except UnicodeEncodeError:
        raise ValidationError("La trama contiene caracteres no ASCII", 422)
    if len(frame_bytes) != frame_length or not 0 < frame_length <= 255:
        raise ValidationError(
            f"Longitud de trama inconsistente ({len(frame_bytes)} != {frame_length})", 422)
    crc = f"{zlib.crc32(frame_bytes) & 0xFFFFFFFF:08X}"
    if crc != frame_crc32.upper():
        raise ValidationError(f"CRC32 de trama inválido ({crc} != {frame_crc32})", 422)

    try:
        trama = json.loads(frame, parse_constant=_rechazar_constante)
    except ValueError:
        raise ValidationError("La trama no es JSON válido", 422)
    if not isinstance(trama, dict):
        raise ValidationError("La trama no es un objeto JSON", 422)

    campos = _validar_trama(trama)
    if str(data.get("message_id", "")).upper() != campos["message_id"]:
        raise ValidationError("message_id del sobre y de la trama no coinciden", 422)

    gateway_time = None
    if not _vacio(data.get("gateway_time")):
        gateway_time = parse_timestamp(data.get("gateway_time"))
        if gateway_time is None:
            raise ValidationError("gateway_time inválido", 422)
    timestamp, sensor_time, fuente = _elegir_hora(campos["timestamp"], gateway_time)

    duplicates = data.get("duplicates")
    if isinstance(duplicates, bool) or not isinstance(duplicates, int) or duplicates < 0:
        duplicates = None
    gateway_id = data.get("gateway_id") if isinstance(data.get("gateway_id"), str) else None

    return Lectura(
        node_id=config.NODE_ID, timestamp=timestamp, sensor_time=sensor_time,
        sensor_ok=campos["sensor_ok"], distance=campos["distance"],
        message_id=campos["message_id"], timestamp_source=fuente,
        hx710b_raw=campos["hx710b_raw"], pressure_kpa=campos["pressure_kpa"],
        water_level_cm=campos["water_level_cm"], net_flow_l_min=campos["net_flow_l_min"],
        gateway_id=gateway_id[:32] if gateway_id else None, gateway_time=gateway_time,
        central_ack=data.get("central_ack") is True, duplicates=duplicates,
        frame_length=frame_length, frame_crc32=crc, raw_frame=frame,
    )


def _validar_legado(data: dict) -> Lectura:
    """Formatos anteriores (registros ya encolados en la SD de la Central)."""
    if data.get("node_id") != config.NODE_ID:
        raise ValidationError("Payload inválido", 400)

    if "status" in data:            # firmware Central v1: value/status/pressure_pa
        if data.get("status") not in ("ok", "sensor_error"):
            raise ValidationError("status inválido", 422)
        sensor_ok = data["status"] == "ok"
        distancia = _distancia(sensor_ok, data.get("value"))
        pressure_pa = _numero_opcional(data, "pressure_pa")
        pressure_kpa = pressure_pa / 1000 if pressure_pa is not None else None
        hx = _entero_opcional(data, "hx710b_raw")
        nivel = _numero_opcional(data, "water_level_cm")
    elif isinstance(data.get("sensor_ok"), bool) and "distance_cm" in data:   # formato inicial
        sensor_ok = data["sensor_ok"]
        distancia = _distancia(sensor_ok, data.get("distance_cm"))
        pressure_kpa = hx = nivel = None
    else:
        raise ValidationError("Payload inválido", 400)

    timestamp, sensor_time, fuente = _elegir_hora(data.get("timestamp"), None)
    return Lectura(
        node_id=config.NODE_ID, timestamp=timestamp, sensor_time=sensor_time,
        sensor_ok=sensor_ok, distance=distancia, timestamp_source=fuente,
        hx710b_raw=hx, pressure_kpa=pressure_kpa, water_level_cm=nivel,
    )


def validar_payload(data) -> Lectura:
    if not isinstance(data, dict):
        raise ValidationError("Se requiere JSON", 400)
    if "frame" in data:
        return _validar_sobre(data)
    return _validar_legado(data)


def procesar_lectura(lectura: Lectura) -> Resultado:
    """Guarda la lectura, actualiza estados y decide qué notificar.

    No envía notificaciones: las devuelve para que se encolen ya con la
    transacción cerrada (el envío a Discord es asíncrono).
    """
    with database.get_connection() as conn:
        # Bloquea alert_state: serializa POST simultáneos (y el chequeo de duplicado).
        previous = database.get_state_for_update(conn)

        if lectura.message_id and database.reading_exists(conn, lectura.message_id):
            conn.rollback()
            return Resultado(estado=previous, duplicada=True)

        cfg = database.get_config(conn)
        current = alert_service.calcular_estado(lectura.sensor_ok, lectura.distance, cfg)
        database.insert_reading(conn, lectura)

        notificaciones = []
        if current != previous:
            aviso = alert_service.construir_notificacion(
                previous, current, lectura.node_id, lectura.timestamp, lectura.distance, cfg)
            if aviso:
                notificaciones.append(aviso)
            database.update_state(conn, current)

        # Watchdog: el nodo reporta de nuevo / cambia la presencia del HX710B.
        info = database.touch_watchdog(conn, lectura.node_id, lectura.hx710b_raw is None,
                                       config.HX710B_CONFIRM_READINGS)
        notificaciones.extend(alert_service.avisos_presencia(lectura.node_id, lectura.timestamp, info))
        conn.commit()

    return Resultado(estado=current, notificaciones=notificaciones)


def revisar_nodo_silencioso() -> Optional[tuple]:
    """Llamado por el hilo watchdog: marca el nodo como caído si lleva
    SENSOR_OFFLINE_SECONDS sin reportar. Devuelve la notificación o None."""
    with database.get_connection() as conn:
        silencio = database.mark_offline_if_silent(
            conn, config.NODE_ID, config.SENSOR_OFFLINE_SECONDS)
        conn.commit()
    if silencio is None:
        return None
    return alert_service.aviso_nodo_silencioso(config.NODE_ID, silencio)
