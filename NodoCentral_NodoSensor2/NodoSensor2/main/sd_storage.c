#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#include "config.h"
#include "sd_storage.h"

#define SD_QUEUE_PATH NODO_SD_MOUNT_POINT "/pending.log"
#define SD_TEMP_PATH NODO_SD_MOUNT_POINT "/pending.tmp"
#define SD_BACKUP_PATH NODO_SD_MOUNT_POINT "/pending.bak"
#define SD_REJECTED_PATH NODO_SD_MOUNT_POINT "/rejected.log"

/* Los buffers de linea son static para no gastar ~2 KB de pila por llamada.
 * Es seguro porque todas las llamadas se hacen con storage_mutex tomado
 * (ver wifi_app.c). */

static const char *TAG = "SD";
static sdmmc_card_t *card;
static bool mounted;

static bool flush_file(FILE *file) { return fflush(file) == 0 && fsync(fileno(file)) == 0; }

static bool parse_record(char *line, uint64_t *message_id, char **payload)
{
    char *separator = strchr(line, '|');
    if (separator == NULL) return false;
    *separator = '\0';
    char *end = NULL;
    errno = 0;
    unsigned long long id = strtoull(line, &end, 16);
    if (end == line || *end != '\0' || errno == ERANGE) return false;
    *payload = separator + 1;
    (*payload)[strcspn(*payload, "\r\n")] = '\0';
    if ((*payload)[0] == '\0') return false;
    *message_id = (uint64_t)id;
    return true;
}

static bool recover_queue_files(void)
{
    if (access(SD_QUEUE_PATH, F_OK) != 0 && access(SD_BACKUP_PATH, F_OK) == 0 &&
        rename(SD_BACKUP_PATH, SD_QUEUE_PATH) != 0) return false;
    if (access(SD_QUEUE_PATH, F_OK) == 0 && access(SD_BACKUP_PATH, F_OK) == 0 &&
        unlink(SD_BACKUP_PATH) != 0) return false;
    return unlink(SD_TEMP_PATH) == 0 || errno == ENOENT;
}

bool sd_storage_init(void)
{
    if (mounted) return true;
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = NODO_SD_SPI_HOST;
    host.max_freq_khz = NODO_SD_MAX_FREQ_KHZ;
    spi_bus_config_t bus = {.mosi_io_num = NODO_SD_MOSI_PIN, .miso_io_num = NODO_SD_MISO_PIN,
                            .sclk_io_num = NODO_SD_SCK_PIN, .quadwp_io_num = -1,
                            .quadhd_io_num = -1, .max_transfer_sz = NODO_SD_MAX_TRANSFER_SIZE};
    /* El driver SDSPI gestiona CS. Reseteamos el pin para liberar cualquier reserva previa de ESP-IDF */
    gpio_reset_pin(NODO_SD_CS_PIN);
    esp_err_t err = gpio_set_pull_mode(NODO_SD_MISO_PIN, GPIO_PULLUP_ONLY);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo activar pull-up en MISO: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "Montando microSD: SPI%d, CS=%d SCK=%d MOSI=%d MISO=%d, %d kHz",
             host.slot, NODO_SD_CS_PIN, NODO_SD_SCK_PIN, NODO_SD_MOSI_PIN,
             NODO_SD_MISO_PIN, host.max_freq_khz);
    err = spi_bus_initialize(host.slot, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) { ESP_LOGE(TAG, "No se pudo inicializar SPI SD: %s", esp_err_to_name(err)); return false; }
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = host.slot;
    slot.gpio_cs = NODO_SD_CS_PIN;
    esp_vfs_fat_mount_config_t mount = {.format_if_mount_failed = false,
                                        .max_files = NODO_SD_MAX_OPEN_FILES,
                                        .allocation_unit_size = NODO_SD_ALLOCATION_UNIT_SIZE};
    err = esp_vfs_fat_sdspi_mount(NODO_SD_MOUNT_POINT, &host, &slot, &mount, &card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo montar microSD: %s", esp_err_to_name(err));
        gpio_reset_pin(NODO_SD_CS_PIN);
        spi_bus_free(host.slot);
        return false;
    }
    mounted = true;
    if (!recover_queue_files()) {
        ESP_LOGE(TAG, "No se pudo recuperar cola SD");
        esp_vfs_fat_sdcard_unmount(NODO_SD_MOUNT_POINT, card);
        card = NULL;
        mounted = false;
        gpio_reset_pin(NODO_SD_CS_PIN);
        spi_bus_free(host.slot);
        return false;
    }
    ESP_LOGI(TAG, "Cola persistente lista en %s", SD_QUEUE_PATH);
    return true;
}

bool sd_storage_enqueue(uint64_t message_id, const char *payload)
{
    if (!mounted || payload == NULL || payload[0] == '\0' || strchr(payload, '\n') || strchr(payload, '\r')) return false;
    FILE *file = fopen(SD_QUEUE_PATH, "a");
    if (file == NULL) return false;
    bool ok = fprintf(file, "%016" PRIX64 "|%s\n", message_id, payload) > 0 && flush_file(file);
    return fclose(file) == 0 && ok;
}

static void discard_corrupted_first_line(void)
{
    FILE *source = fopen(SD_QUEUE_PATH, "r");
    if (source == NULL) return;
    FILE *temp = fopen(SD_TEMP_PATH, "w");
    if (temp == NULL) {
        fclose(source);
        return;
    }

    int c;
    while ((c = fgetc(source)) != EOF) {
        if (c == '\n') break;
    }

    char buf[256];
    size_t n;
    bool copy_ok = true;
    while ((n = fread(buf, 1, sizeof(buf), source)) > 0) {
        if (fwrite(buf, 1, n, temp) != n) {
            copy_ok = false;
            break;
        }
    }

    bool ok = copy_ok && !ferror(source) && flush_file(temp) &&
              fclose(source) == 0 && fclose(temp) == 0;
    if (!ok) {
        unlink(SD_TEMP_PATH);
        return;
    }

    unlink(SD_BACKUP_PATH);
    if (rename(SD_QUEUE_PATH, SD_BACKUP_PATH) != 0 ||
        rename(SD_TEMP_PATH, SD_QUEUE_PATH) != 0) {
        rename(SD_BACKUP_PATH, SD_QUEUE_PATH);
        return;
    }
    unlink(SD_BACKUP_PATH);
    ESP_LOGW(TAG, "Registro corrupto descartado de la cola SD para desbloquear envios");
}

sd_queue_result_t sd_storage_peek(uint64_t *message_id, char *payload, size_t payload_size)
{
    if (!mounted || !message_id || !payload || payload_size == 0) return SD_QUEUE_ERROR;
    FILE *file = fopen(SD_QUEUE_PATH, "r");
    if (file == NULL) return errno == ENOENT ? SD_QUEUE_EMPTY : SD_QUEUE_ERROR;
    static char line[NODO_SD_QUEUE_LINE_MAX];
    if (fgets(line, sizeof(line), file) == NULL) { bool error = ferror(file); fclose(file); return error ? SD_QUEUE_ERROR : SD_QUEUE_EMPTY; }
    fclose(file);
    if (strchr(line, '\n') == NULL) {
        ESP_LOGE(TAG, "Registro incompleto o demasiado largo en la cola SD (se descartara)");
        discard_corrupted_first_line();
        return SD_QUEUE_ERROR;
    }
    char *record_payload;
    if (!parse_record(line, message_id, &record_payload) || strlen(record_payload) >= payload_size) {
        ESP_LOGE(TAG, "Registro invalido en la cola SD (se descartara)");
        discard_corrupted_first_line();
        return SD_QUEUE_ERROR;
    }
    strcpy(payload, record_payload);
    return SD_QUEUE_PENDING;
}

bool sd_storage_ack(uint64_t message_id)
{
    if (!mounted) return false;
    FILE *source = fopen(SD_QUEUE_PATH, "r");
    FILE *temp = source ? fopen(SD_TEMP_PATH, "w") : NULL;
    if (!source || !temp) { if (source) fclose(source); if (temp) fclose(temp); return false; }
    bool found = false, ok = true;
    static char line[NODO_SD_QUEUE_LINE_MAX];
    static char copy[NODO_SD_QUEUE_LINE_MAX];
    while (fgets(line, sizeof(line), source)) {
        char *record_payload;
        uint64_t id;
        if (strchr(line, '\n') == NULL) { ok = false; break; }
        strcpy(copy, line);
        if (!parse_record(copy, &id, &record_payload)) { ok = false; break; }
        if (!found && id == message_id) found = true;
        else if (fputs(line, temp) == EOF) { ok = false; break; }
    }
    if (ferror(source)) ok = false;
    if (!flush_file(temp)) ok = false;
    if (fclose(source) != 0) ok = false;
    if (fclose(temp) != 0) ok = false;

    if (!ok || !found) {
        unlink(SD_TEMP_PATH);
        ESP_LOGE(TAG, "%s al actualizar cola SD", found ? "Error" : "No se encontro el ACK");
        return false;
    }
    unlink(SD_BACKUP_PATH);
    if (rename(SD_QUEUE_PATH, SD_BACKUP_PATH) != 0 || rename(SD_TEMP_PATH, SD_QUEUE_PATH) != 0) {
        rename(SD_BACKUP_PATH, SD_QUEUE_PATH); return false;
    }
    unlink(SD_BACKUP_PATH);
    return true;
}

/* Guarda (no borra) un registro que el Cloud rechazo de forma definitiva. */
bool sd_storage_archive_rejected(uint64_t message_id, const char *payload)
{
    if (!mounted || payload == NULL) return false;
    FILE *file = fopen(SD_REJECTED_PATH, "a");
    if (file == NULL) return false;
    bool ok = fprintf(file, "%016" PRIX64 "|%s\n", message_id, payload) > 0 && flush_file(file);
    return fclose(file) == 0 && ok;
}
