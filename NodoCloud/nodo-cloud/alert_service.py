"""Reglas de alertas: cálculo de estado, mensajes y envío a Discord."""
from decimal import Decimal, InvalidOperation

import config
import discord_notify

NORMAL = "normal"
URGENCY = "urgency"
SENSOR_ERROR = "sensor_error"


def calcular_estado(sensor_ok, distance, cfg):
    if not sensor_ok:
        return SENSOR_ERROR
    if distance < Decimal(str(cfg["min_distance_cm"])) or distance > Decimal(str(cfg["max_distance_cm"])):
        return URGENCY
    return NORMAL


def construir_notificacion(previous, current, node_id, timestamp, distance, cfg):
    """Devuelve (titulo, mensaje) si el cambio de estado debe notificarse; si no, None."""
    if current == previous:
        return None
    rango = f"{cfg['min_distance_cm']}–{cfg['max_distance_cm']} cm"

    if current == SENSOR_ERROR and cfg["alert_sensor_error"]:
        return (
            "ERROR DEL SENSOR",
            f"Nodo: {node_id}\nHora del sensor: {timestamp}\n"
            "El sensor ultrasónico no responde o está fuera de rango (status = sensor_error).",
        )
    if current == URGENCY:
        return (
            "🚨 URGENCIA DE DISTANCIA",
            f"Nodo: {node_id}\nDistancia: {distance} cm\nLímite configurado: {rango}\n"
            f"Hora del sensor: {timestamp}",
        )
    if current == NORMAL and cfg["alert_recovery"]:
        dato = distance if distance is not None else "Sin dato"
        return (
            "✅ SISTEMA RECUPERADO",
            f"Nodo: {node_id}\nDistancia actual: {dato} cm\n"
            f"El sistema volvió al rango normal ({rango}).",
        )
    return None


def enviar_alerta(titulo, mensaje):
    """Encola la alerta para Discord (envío asíncrono con reintentos)."""
    return discord_notify.encolar(titulo, mensaje)


def _duracion(segundos):
    if segundos is None:
        return "desconocido"
    minutos, seg = divmod(int(segundos), 60)
    horas, minutos = divmod(minutos, 60)
    return f"{horas} h {minutos} min" if horas else f"{minutos} min {seg} s"


def aviso_nodo_silencioso(node_id, silencio_s):
    """Sensor faltante: no llegan tramas del nodo (sensor, LoRa, Central o Wi-Fi)."""
    return (
        "📡 SENSOR SIN REPORTAR",
        f"Nodo: {node_id}\nNo llegan datos hace {_duracion(silencio_s)} "
        f"(límite {_duracion(config.SENSOR_OFFLINE_SECONDS)}).\n"
        "Revisa alimentación del NodoSensor1, enlace LoRa y Wi-Fi del NodoCentral.",
    )


def avisos_presencia(node_id, timestamp, info):
    """Avisos al recibir una lectura: el nodo volvió y/o cambió (con antirrebote)
    la presencia del HX710B. info = resultado de database.touch_watchdog()."""
    avisos = []
    if info["was_offline"]:
        avisos.append((
            "✅ SENSOR REPORTANDO DE NUEVO",
            f"Nodo: {node_id}\nVolvió a enviar datos tras {_duracion(info['silencio_s'])} "
            f"sin reportar.\nHora de la lectura: {timestamp}",
        ))
    if config.ALERT_HX710B_MISSING and info["hx_change"] == "missing":
        avisos.append((
            "⚠️ SENSOR DE PRESIÓN (HX710B) SIN LECTURA",
            f"Nodo: {node_id}\nLas últimas {config.HX710B_CONFIRM_READINGS} tramas llegaron con "
            f"hx710b_raw = null: el HX710B no responde o no está conectado.\n"
            f"Hora de la lectura: {timestamp}",
        ))
    elif config.ALERT_HX710B_MISSING and info["hx_change"] == "restored":
        avisos.append((
            "✅ SENSOR DE PRESIÓN (HX710B) RECUPERADO",
            f"Nodo: {node_id}\nEl HX710B volvió a entregar lecturas.\n"
            f"Hora de la lectura: {timestamp}",
        ))
    return avisos


def validar_limites(min_raw, max_raw):
    """Valida los límites del formulario de configuración. Lanza ValueError si son inválidos."""
    try:
        minimo = Decimal(min_raw)
        maximo = Decimal(max_raw)
    except InvalidOperation:
        raise ValueError("Valores inválidos.")
    if (
        not minimo.is_finite()
        or not maximo.is_finite()
        or minimo < config.SENSOR_MIN_CM
        or maximo > config.SENSOR_MAX_CM
        or minimo >= maximo
    ):
        raise ValueError(
            f"Usa límites entre {config.SENSOR_MIN_CM} y {config.SENSOR_MAX_CM} cm "
            "y asegúrate de que el mínimo sea menor que el máximo."
        )
    return minimo, maximo
