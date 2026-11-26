"""Configuración central: variables de entorno, constantes y logging."""
import logging
import os
from logging.handlers import RotatingFileHandler
from pathlib import Path

from dotenv import load_dotenv

BASE_DIR = Path(__file__).resolve().parent

# Debe cargarse antes de importar módulos que lean os.environ (p. ej. discord_notify).
load_dotenv(BASE_DIR / ".env")

# --- Seguridad ---
API_TOKEN = os.environ["API_TOKEN"]
DASHBOARD_USER = os.environ.get("DASHBOARD_USER", "admin")
DASHBOARD_PASSWORD = os.environ["DASHBOARD_PASSWORD"]

# --- Base de datos ---
DB_HOST = os.environ.get("DB_HOST", "localhost")
DB_USER = os.environ["DB_USER"]
DB_PASSWORD = os.environ["DB_PASSWORD"]
DB_NAME = os.environ.get("DB_NAME", "nodo_sensor")

# --- Flask ---
MAX_CONTENT_LENGTH = 4096
HOST = "0.0.0.0"
PORT = 5000

# --- Sensor ---
NODE_ID = "NodoSensor1"
SENSOR_MIN_CM = 5
SENSOR_MAX_CM = 400

# --- Alertas de "sensor faltante" (Discord) ---
# Sin tramas durante este tiempo => aviso "sensor sin reportar". El sensor
# envía cada 5 s, así que 120 s tolera cortes breves de Wi-Fi/LoRa.
SENSOR_OFFLINE_SECONDS = int(os.environ.get("SENSOR_OFFLINE_SECONDS", "120"))
WATCHDOG_INTERVAL_SECONDS = int(os.environ.get("WATCHDOG_INTERVAL_SECONDS", "30"))
# Aviso cuando el HX710B deja de entregar lectura (hx710b_raw = null).
ALERT_HX710B_MISSING = os.environ.get("ALERT_HX710B_MISSING", "1") == "1"
# Antirrebote: lecturas seguidas en el nuevo estado antes de avisar.
HX710B_CONFIRM_READINGS = int(os.environ.get("HX710B_CONFIRM_READINGS", "3"))

# --- Dashboard ---
DASHBOARD_HISTORY_DEFAULT = 300
DASHBOARD_HISTORY_MAX = 2000

# --- Logs ---
LOG_DIR = BASE_DIR / "logs"
LOG_FILE = LOG_DIR / "nodo-cloud.log"


def setup_logging() -> None:
    """Configura logging a consola y a archivo rotativo en logs/."""
    LOG_DIR.mkdir(exist_ok=True)
    formato = logging.Formatter("%(asctime)s %(levelname)s [%(name)s] %(message)s")

    consola = logging.StreamHandler()
    consola.setFormatter(formato)

    archivo = RotatingFileHandler(LOG_FILE, maxBytes=1_000_000, backupCount=5, encoding="utf-8")
    archivo.setFormatter(formato)

    raiz = logging.getLogger()
    raiz.setLevel(logging.INFO)
    if not raiz.handlers:
        raiz.addHandler(consola)
        raiz.addHandler(archivo)
