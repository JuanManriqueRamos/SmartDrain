# SmartDrain

Prototipo de monitoreo de condiciones hidráulicas en una estructura de alcantarillado.

Este repositorio conserva el código fuente de los tres componentes del proyecto:

- **NodoSensor1:** adquisición ultrasónica y lectura HX710B, generación de tramas y transmisión LoRa.
- **NodoCentral / NodoSensor2:** recepción LoRa, confirmación ACK, almacenamiento intermedio y reenvío HTTP.
- **NodoCloud:** API Flask, persistencia MariaDB, reglas de estado, dashboard y notificaciones Discord.

## Estructura

```text
SmartDrain/
├── NodoSensor1_1/NodoSensor1/       # Firmware de adquisición (ESP32-S3 / ESP-IDF)
├── NodoCentral_NodoSensor2/NodoSensor2/  # Gateway LoRa y envío HTTP
├── NodoCloud/nodo-cloud/            # Backend Flask y dashboard
├── .gitignore
└── README.md
```

## Seguridad: configuración obligatoria

Por seguridad, las credenciales reales fueron retiradas del código publicado. Antes de compilar o ejecutar:

1. En `NodoSensor1_1/NodoSensor1/main/node_config.h`, configura `NODE_WIFI_SSID` y `NODE_WIFI_PASSWORD`.
2. En `NodoCentral_NodoSensor2/NodoSensor2/main/config.h`, configura `NODO_WIFI_SSID`, `NODO_WIFI_PASSWORD`, `NODO_API_URL` y `NODO_API_TOKEN`.
3. En `NodoCloud/nodo-cloud/`, copia `.env.example` como `.env` y completa los valores de MariaDB, API, dashboard y Discord.
4. Usa el mismo token API en el NodoCentral y en el NodoCloud.
5. No subas `.env`, credenciales reales, tokens ni webhooks al repositorio.

Los valores `192.0.2.1` incluidos como URL de ejemplo pertenecen a un rango reservado para documentación; reemplázalos por la dirección real del NodoCloud.

**Importante:** si las credenciales anteriores fueron compartidas o estuvieron publicadas, cámbialas en el servicio correspondiente. Sustituirlas en el código no las invalida.

## Firmware ESP-IDF

Cada firmware es un proyecto ESP-IDF independiente. Abre una terminal con ESP-IDF configurado y ejecuta desde la carpeta correspondiente:

```bash
idf.py set-target esp32s3
idf.py build
```

Para cargarlo a la placa:

```bash
idf.py -p PUERTO flash monitor
```

Reemplaza `PUERTO` por el puerto serial de la placa.

## NodoCloud

Desde `NodoCloud/nodo-cloud/`:

```bash
python3 -m venv .venv
# Windows: .venv\Scripts\activate
# Linux/macOS: source .venv/bin/activate
pip install -r requirements.txt
cp .env.example .env
```

Completa `.env`, prepara MariaDB ejecutando `schema_update.sql` con un usuario autorizado e inicia la aplicación:

```bash
python app.py
```

## Alcance

Este repositorio contiene el software del prototipo. La presencia del código no demuestra por sí sola precisión de sensores, alcance LoRa en campo ni validación en infraestructura urbana. Consulta los documentos técnicos del proyecto para conocer las pruebas y limitaciones reportadas.
