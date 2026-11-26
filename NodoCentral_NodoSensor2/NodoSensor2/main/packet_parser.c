#include "packet_parser.h"

#include <math.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

static void calculate_water_measurements(sensor_packet_t *packet)
{
    packet->pressure_valid = false;
    if (!packet->hx710b_valid || !NODO_HX710B_CALIBRATED) return;

    float pressure = ((float)packet->hx710b_raw - (float)NODO_HX710B_ZERO_RAW) *
                     NODO_HX710B_PA_PER_COUNT;
    if (!isfinite(pressure) || pressure < 0.0f) return;

    packet->pressure_pa = pressure;
    packet->water_level_cm = pressure /
        (NODO_WATER_DENSITY_KG_M3 * NODO_GRAVITY_M_S2) * 100.0f;
    packet->pressure_valid = isfinite(packet->water_level_cm);
}

static bool timestamp_is_valid(const char *timestamp)
{
    size_t length = strlen(timestamp);
    return (length == 20 && timestamp[4] == '-' && timestamp[7] == '-' &&
            timestamp[10] == 'T' && timestamp[13] == ':' &&
            timestamp[16] == ':' && timestamp[19] == 'Z') ||
           (length == 19 && timestamp[4] == '-' && timestamp[7] == '-' &&
            timestamp[10] == ' ' && timestamp[13] == ':' &&
            timestamp[16] == ':');
}

static bool nullable_number_is_valid(const char *value)
{
    if (strcmp(value, "null") == 0) return true;
    char *end = NULL;
    errno = 0;
    float parsed = strtof(value, &end);
    return errno != ERANGE && end != value && *end == '\0' && isfinite(parsed);
}

/* ---- Parser de trama: objeto JSON plano, por clave (no por posicion) ----
 * - Acepta campos en cualquier orden y espacios opcionales.
 * - null, "" y campos opcionales ausentes se tratan como "sin dato" sin
 *   desplazar ni truncar el resto de la trama.
 * - Nunca usa %[...] de sscanf (falla con campos vacios) ni depende de que el
 *   ultimo campo consuma la llave de cierre.
 */
#define FRAME_TOKEN_MAX 32

typedef enum { FRAME_VAL_NULL, FRAME_VAL_STRING, FRAME_VAL_NUMBER } frame_val_kind_t;

static const char *frame_skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

/* Copia una cadena entre comillas (sin escapes) a dst. Devuelve el puntero
 * posterior a la comilla de cierre o NULL si no cabe o esta mal formada. */
static const char *frame_read_string(const char *p, char *dst, size_t dst_size)
{
    if (*p != '"') return NULL;
    p++;
    size_t n = 0;
    while (*p != '"') {
        if (*p == '\0' || *p == '\\' || (unsigned char)*p < 0x20) return NULL;
        if (n + 1 >= dst_size) return NULL;
        dst[n++] = *p++;
    }
    dst[n] = '\0';
    return p + 1;
}

/* Lee un valor: "cadena", null o un token numerico. */
static const char *frame_read_value(const char *p, char *dst, size_t dst_size,
                                    frame_val_kind_t *kind)
{
    if (*p == '"') {
        *kind = FRAME_VAL_STRING;
        return frame_read_string(p, dst, dst_size);
    }
    if (strncmp(p, "null", 4) == 0) {
        *kind = FRAME_VAL_NULL;
        dst[0] = '\0';
        return p + 4;
    }
    size_t n = 0;
    while (*p == '-' || *p == '+' || *p == '.' || *p == 'e' || *p == 'E' ||
           (*p >= '0' && *p <= '9')) {
        if (n + 1 >= dst_size) return NULL;
        dst[n++] = *p++;
    }
    if (n == 0) return NULL;
    dst[n] = '\0';
    *kind = FRAME_VAL_NUMBER;
    return p;
}

/* Numero opcional: null, "" o ausente => sin dato (true, *has_value=false). */
static bool frame_optional_float(frame_val_kind_t kind, const char *raw,
                                 bool *has_value, float *value)
{
    *has_value = false;
    if (kind == FRAME_VAL_NULL || raw[0] == '\0') return true;
    if (!nullable_number_is_valid(raw)) return false;
    if (value) *value = strtof(raw, NULL);
    *has_value = true;
    return true;
}

enum {
    F_NODE_ID, F_MESSAGE_ID, F_TIMESTAMP, F_TYPE, F_VALUE, F_UNIT, F_STATUS,
    F_HX710B, F_PRESSURE, F_LEVEL, F_FLOW, F_BATTERY, F_COUNT
};
static const char *const frame_keys[F_COUNT] = {
    "node_id", "message_id", "timestamp", "type", "value", "unit", "status",
    "hx710b_raw", "pressure_kpa", "water_level_cm", "net_flow_l_min", "battery"
};

bool packet_parse(const char *text, sensor_packet_t *out)
{
    if (!text || !out) return false;
    memset(out, 0, sizeof(*out));
    out->hx710b_valid = false;
    out->pressure_valid = false;

    char val[F_COUNT][FRAME_TOKEN_MAX];
    frame_val_kind_t kind[F_COUNT];
    bool seen[F_COUNT] = { false };
    memset(val, 0, sizeof(val));

    const char *p = frame_skip_ws(text);
    if (*p != '{') return false;
    p = frame_skip_ws(p + 1);
    if (*p != '}') {
        while (true) {
            char key[24];
            p = frame_read_string(p, key, sizeof(key));
            if (!p) return false;
            p = frame_skip_ws(p);
            if (*p != ':') return false;
            p = frame_skip_ws(p + 1);

            int idx = -1;
            for (int i = 0; i < F_COUNT; i++)
                if (strcmp(key, frame_keys[i]) == 0) { idx = i; break; }

            char scratch[FRAME_TOKEN_MAX];
            frame_val_kind_t scratch_kind;
            if (idx >= 0) {
                if (seen[idx]) return false;            /* clave duplicada */
                p = frame_read_value(p, val[idx], sizeof(val[idx]), &kind[idx]);
                seen[idx] = true;
            } else {
                p = frame_read_value(p, scratch, sizeof(scratch), &scratch_kind);
            }
            if (!p) return false;

            p = frame_skip_ws(p);
            if (*p == ',') { p = frame_skip_ws(p + 1); continue; }
            if (*p == '}') break;
            return false;
        }
    }
    p = frame_skip_ws(p + 1);
    if (*p != '\0') return false;                       /* basura tras '}' */

    /* node_id y message_id: obligatorios y no vacios. */
    if (!seen[F_NODE_ID] || kind[F_NODE_ID] != FRAME_VAL_STRING ||
        strlen(val[F_NODE_ID]) >= sizeof(out->node_id) ||
        strcmp(val[F_NODE_ID], "NodoSensor1") != 0) return false;
    memcpy(out->node_id, val[F_NODE_ID], strlen(val[F_NODE_ID]) + 1);

    if (!seen[F_MESSAGE_ID] || kind[F_MESSAGE_ID] != FRAME_VAL_STRING ||
        strlen(val[F_MESSAGE_ID]) != 16 ||
        strspn(val[F_MESSAGE_ID], "0123456789ABCDEFabcdef") != 16) return false;
    char *message_id_end = NULL;
    errno = 0;
    unsigned long long parsed_message_id = strtoull(val[F_MESSAGE_ID], &message_id_end, 16);
    if (errno == ERANGE || *message_id_end != '\0') return false;
    out->message_id = (uint64_t)parsed_message_id;

    /* timestamp: null, "" o ausente => vacio (main.c pone la hora del RTC). */
    if (seen[F_TIMESTAMP] && kind[F_TIMESTAMP] == FRAME_VAL_STRING &&
        val[F_TIMESTAMP][0] != '\0') {
        if (strlen(val[F_TIMESTAMP]) >= sizeof(out->timestamp) ||
            !timestamp_is_valid(val[F_TIMESTAMP])) return false;
        memcpy(out->timestamp, val[F_TIMESTAMP], strlen(val[F_TIMESTAMP]) + 1);
    } else if (seen[F_TIMESTAMP] && kind[F_TIMESTAMP] == FRAME_VAL_NUMBER) {
        return false;
    }

    /* Constantes opcionales: si vienen con dato, deben coincidir. */
    if (seen[F_TYPE] && kind[F_TYPE] != FRAME_VAL_NULL && val[F_TYPE][0] != '\0' &&
        strcmp(val[F_TYPE], "distance") != 0) return false;
    if (seen[F_UNIT] && kind[F_UNIT] != FRAME_VAL_NULL && val[F_UNIT][0] != '\0' &&
        strcmp(val[F_UNIT], "cm") != 0) return false;

    /* Campos informativos opcionales (el central recalcula desde hx710b_raw). */
    bool has;
    if (seen[F_PRESSURE] && !frame_optional_float(kind[F_PRESSURE], val[F_PRESSURE], &has, NULL)) return false;
    if (seen[F_LEVEL] && !frame_optional_float(kind[F_LEVEL], val[F_LEVEL], &has, NULL)) return false;
    if (seen[F_FLOW] && !frame_optional_float(kind[F_FLOW], val[F_FLOW], &has, NULL)) return false;
    if (seen[F_BATTERY] && !frame_optional_float(kind[F_BATTERY], val[F_BATTERY], &has, NULL)) return false;

    /* status + value. */
    if (!seen[F_STATUS] || kind[F_STATUS] != FRAME_VAL_STRING) return false;
    bool has_distance = false;
    float distance = 0.0f;
    if (seen[F_VALUE] && !frame_optional_float(kind[F_VALUE], val[F_VALUE], &has_distance, &distance))
        return false;
    if (strcmp(val[F_STATUS], "ok") == 0) {
        if (!has_distance || distance < 5.0f || distance > 400.0f) return false;
        out->sensor_ok = true;
        out->distance_cm = distance;
    } else if (strcmp(val[F_STATUS], "sensor_error") == 0) {
        if (has_distance && distance != 0.0f) return false;
        out->sensor_ok = false;
        out->distance_cm = 0.0f;
    } else {
        return false;
    }

    /* hx710b_raw: null, "" o ausente => hx710b_valid=false. */
    if (!seen[F_HX710B] || kind[F_HX710B] == FRAME_VAL_NULL || val[F_HX710B][0] == '\0') {
        out->hx710b_valid = false;
        return true;
    }
    if (kind[F_HX710B] != FRAME_VAL_NUMBER) return false;
    char *end = NULL;
    errno = 0;
    long long value = strtoll(val[F_HX710B], &end, 10);
    if (errno == ERANGE || end == val[F_HX710B] || *end != '\0' ||
        value < INT32_MIN || value > INT32_MAX) return false;
    out->hx710b_raw = (int32_t)value;
    out->hx710b_valid = true;
    calculate_water_measurements(out);
    return true;
}
