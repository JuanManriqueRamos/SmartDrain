"""API Flask y rutas HTTP."""
import config  # debe ir primero: carga .env antes de importar discord_notify

import hmac
import logging
import threading
import time
from datetime import date, datetime
from decimal import Decimal

from flask import Flask, Response, jsonify, render_template, request

import alert_service
import database
import sensor_service
from sensor_service import ValidationError

config.setup_logging()
logger = logging.getLogger("nodo-cloud")

app = Flask(__name__)
app.config["MAX_CONTENT_LENGTH"] = config.MAX_CONTENT_LENGTH


# --- Watchdog "sensor faltante" (un hilo por proceso; MariaDB evita avisos dobles) ---
_watchdog_lock = threading.Lock()
_watchdog_iniciado = False


def _bucle_watchdog():
    while True:
        time.sleep(config.WATCHDOG_INTERVAL_SECONDS)
        try:
            aviso = sensor_service.revisar_nodo_silencioso()
            if aviso:
                logger.warning("%s", aviso[0])
                alert_service.enviar_alerta(*aviso)
        except Exception:
            logger.exception("Fallo revisando si el sensor dejó de reportar")


def iniciar_watchdog():
    global _watchdog_iniciado
    with _watchdog_lock:
        if not _watchdog_iniciado:
            threading.Thread(target=_bucle_watchdog, name="watchdog", daemon=True).start()
            _watchdog_iniciado = True


@app.before_request
def _asegurar_watchdog():
    # Con gunicorn cada worker lo arranca en su primera petición.
    iniciar_watchdog()


# --- Autenticación ---
def autorizado_api():
    cabecera = request.headers.get("Authorization", "")
    prefijo = "Bearer "
    return cabecera.startswith(prefijo) and hmac.compare_digest(cabecera[len(prefijo):], config.API_TOKEN)


def proteger_configuracion():
    auth = request.authorization
    valido = (
        auth
        and hmac.compare_digest(auth.username, config.DASHBOARD_USER)
        and hmac.compare_digest(auth.password, config.DASHBOARD_PASSWORD)
    )
    if not valido:
        return Response(
            "Se requiere autenticación",
            401,
            {"WWW-Authenticate": 'Basic realm="Configuración de urgencias"'},
        )
    return None


# --- Rutas ---
@app.get("/health")
def health():
    return jsonify({"nodo": "raspberry-pi-3-b-plus", "estado": "activo"}), 200


@app.post("/api/event")
def recibir_evento():
    if not autorizado_api():
        return jsonify({"ok": False, "error": "No autorizado"}), 401

    try:
        lectura = sensor_service.validar_payload(request.get_json(silent=True))
    except ValidationError as exc:
        return jsonify({"ok": False, "error": exc.message}), exc.status

    try:
        resultado = sensor_service.procesar_lectura(lectura)
    except Exception:
        logger.exception("No se pudo guardar/procesar la lectura")
        return jsonify({"ok": False, "error": "No se pudo guardar la lectura"}), 500

    if resultado.duplicada:
        # Reenvío de una lectura ya guardada: 200 para que la Central la retire de su cola.
        logger.info("Lectura %s duplicada; ya estaba guardada", lectura.message_id)
        return jsonify({"ok": True, "guardada": False, "duplicada": True}), 200

    for titulo, mensaje in resultado.notificaciones:
        alert_service.enviar_alerta(titulo, mensaje)

    return jsonify({
        "ok": True,
        "guardada": True,
        "duplicada": False,
        "estado": resultado.estado,
        "notificaciones": len(resultado.notificaciones),
    }), 200


@app.get("/")
def dashboard():
    # La página no consulta MariaDB: los datos llegan por /api/dashboard, así la
    # interfaz carga siempre y muestra el error de conexión si la BD falla.
    return render_template(
        "dashboard.html",
        node_id=config.NODE_ID,
        offline_seconds=config.SENSOR_OFFLINE_SECONDS,
    )


def _json(valor):
    """Decimal/fecha -> tipos JSON; NaN/Inf nunca llegan al navegador."""
    if isinstance(valor, Decimal):
        return float(valor) if valor.is_finite() else None
    if isinstance(valor, (datetime, date)):
        return valor.strftime("%Y-%m-%d %H:%M:%S")
    return valor


def _lectura_json(r):
    """Fila de MariaDB -> objeto con las MISMAS claves de la trama del sensor."""
    return {
        "id": r["id"],
        "node_id": r["node_id"],
        "message_id": r["message_id"],
        "timestamp": _json(r["sensor_timestamp"]),
        "timestamp_source": r["timestamp_source"],
        "type": "distance",
        "value": _json(r["distance_cm"]),
        "unit": "cm",
        "status": "ok" if r["sensor_ok"] else "sensor_error",
        "hx710b_raw": r["hx710b_raw"],
        "pressure_kpa": _json(r["pressure_kpa"]),
        "water_level_cm": _json(r["water_level_cm"]),
        "net_flow_l_min": _json(r["net_flow_l_min"]),
        "received_at": _json(r["received_at"]),
        "gateway_id": r["gateway_id"],
        "gateway_time": _json(r["gateway_time"]),
        "central_ack": None if r["central_ack"] is None else bool(r["central_ack"]),
        "duplicates": r["duplicates"],
        "frame_length": r["frame_length"],
        "frame_crc32": r["frame_crc32"],
    }


@app.get("/api/dashboard")
def api_dashboard():
    try:
        limite = int(request.args.get("limit", config.DASHBOARD_HISTORY_DEFAULT))
    except ValueError:
        limite = config.DASHBOARD_HISTORY_DEFAULT
    limite = max(1, min(limite, config.DASHBOARD_HISTORY_MAX))
    try:
        with database.get_connection() as conn:
            summary, readings, state, watchdog = database.get_dashboard_data(
                conn, config.NODE_ID, limite)
            cfg = database.get_config(conn)
    except Exception:
        logger.exception("Error consultando datos del dashboard")
        return jsonify({"ok": False, "error": "No se pudo consultar MariaDB"}), 503

    lecturas = [_lectura_json(r) for r in readings]
    segundos = summary["seconds_since_last"]
    respuesta = jsonify({
        "ok": True,
        "server_time": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
        "node_id": config.NODE_ID,
        "latest": lecturas[-1] if lecturas else None,
        "readings": lecturas,
        "summary": {
            "total": int(summary["total"] or 0),
            "ok_count": int(summary["ok_count"] or 0),
            "error_count": int(summary["error_count"] or 0),
            "last_received": _json(summary["last_received"]),
        },
        "state": {
            "current_state": state["current_state"] if state else "normal",
            "changed_at": _json(state["changed_at"]) if state else None,
        },
        "config": {k: _json(v) for k, v in cfg.items()},
        "link": {
            "online": segundos is not None and segundos < config.SENSOR_OFFLINE_SECONDS,
            "seconds_since_last": segundos,
            "offline_after_s": config.SENSOR_OFFLINE_SECONDS,
            "hx_missing": bool(watchdog["hx_missing"]) if watchdog else None,
        },
    })
    respuesta.headers["Cache-Control"] = "no-store"
    return respuesta


@app.route("/config", methods=["GET", "POST"])
def configuracion():
    auth_error = proteger_configuracion()
    if auth_error:
        return auth_error

    mensaje = None
    error = None
    try:
        with database.get_connection() as conn:
            if request.method == "POST":
                try:
                    minimo, maximo = alert_service.validar_limites(
                        request.form.get("min_distance_cm", ""),
                        request.form.get("max_distance_cm", ""),
                    )
                    alert_error = 1 if request.form.get("alert_sensor_error") == "on" else 0
                    alert_recovery = 1 if request.form.get("alert_recovery") == "on" else 0
                    database.update_config(conn, minimo, maximo, alert_error, alert_recovery)
                    conn.commit()
                    mensaje = "Configuración guardada correctamente."
                except ValueError as exc:
                    conn.rollback()
                    error = str(exc) or "Valores inválidos."
            cfg = database.get_config(conn)
        return render_template("config.html", config=cfg, mensaje=mensaje, error=error)
    except Exception:
        logger.exception("Error guardando configuración")
        return "No se pudo guardar la configuración en MariaDB.", 500


if __name__ == "__main__":
    app.run(host=config.HOST, port=config.PORT, debug=False)
