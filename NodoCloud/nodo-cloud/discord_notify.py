"""Envío de notificaciones a Discord (webhook) en segundo plano.

Las alertas se encolan y un hilo las envía: así el endpoint /api/event responde
de inmediato a la Central (timeout HTTP de 5 s) aunque Discord tarde o limite.
"""
import logging
import os
import queue
import threading
import time

import requests

WEBHOOK_URL = os.environ["DISCORD_WEBHOOK_URL"]
MAX_INTENTOS = 4
logger = logging.getLogger("nodo-cloud.discord")

_cola: "queue.Queue[tuple[str, str]]" = queue.Queue(maxsize=100)
_hilo = None
_hilo_lock = threading.Lock()


def enviar_mensaje(evento: str, mensaje: str) -> None:
    """Envío síncrono (lanza requests.RequestException si falla)."""
    contenido = f"🔔 **Nodo Cloud Raspberry Pi**\n**Evento:** {evento}\n**Detalle:** {mensaje}"
    respuesta = requests.post(
        WEBHOOK_URL,
        json={"content": contenido[:2000], "allowed_mentions": {"parse": []}},
        timeout=15,
    )
    if respuesta.status_code == 429:
        # Discord indica cuánto esperar antes de reintentar.
        try:
            espera = float(respuesta.json().get("retry_after", 2))
        except ValueError:
            espera = 2.0
        raise _Limitado(espera)
    respuesta.raise_for_status()


class _Limitado(requests.RequestException):
    def __init__(self, espera: float):
        super().__init__(f"Discord limitó el envío; reintentar en {espera:.1f} s")
        self.espera = espera


def _trabajador():
    while True:
        evento, mensaje = _cola.get()
        for intento in range(1, MAX_INTENTOS + 1):
            try:
                enviar_mensaje(evento, mensaje)
                logger.info("Alerta enviada a Discord: %s", evento)
                break
            except _Limitado as exc:
                time.sleep(min(exc.espera, 60))
            except requests.RequestException:
                logger.warning("Fallo enviando '%s' a Discord (intento %d/%d)",
                               evento, intento, MAX_INTENTOS, exc_info=True)
                time.sleep(2 ** intento)
        else:
            logger.error("Alerta descartada tras %d intentos: %s", MAX_INTENTOS, evento)
        _cola.task_done()


def encolar(evento: str, mensaje: str) -> bool:
    """Encola una alerta; arranca el hilo de envío la primera vez (por proceso)."""
    global _hilo
    with _hilo_lock:
        if _hilo is None or not _hilo.is_alive():
            _hilo = threading.Thread(target=_trabajador, name="discord-notify", daemon=True)
            _hilo.start()
    try:
        _cola.put_nowait((evento, mensaje))
        return True
    except queue.Full:
        logger.error("Cola de alertas llena; se descarta: %s", evento)
        return False
