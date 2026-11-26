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
#include "sd_storage.h"
#include "node_config.h"

#define SD_QUEUE_PATH NODE_SD_MOUNT_POINT "/pending.log"
#define SD_TEMP_PATH NODE_SD_MOUNT_POINT "/pending.tmp"
#define SD_BACKUP_PATH NODE_SD_MOUNT_POINT "/pending.bak"
static const char *TAG = "SD";
static sdmmc_card_t *card;
static bool mounted;

static bool flush_file(FILE *file)
{
    return fflush(file) == 0 && fsync(fileno(file)) == 0;
}

static bool parse_record(char *line, uint64_t *message_id, char **payload)
{
    char *separator = strchr(line, '|');
    if (separator == NULL) return false;

    *separator = '\0';
    char *end = NULL;
    errno = 0;
    unsigned long long parsed_id = strtoull(line, &end, 16);
    if (end == line || *end != '\0' || errno == ERANGE) return false;

    char *record_payload = separator + 1;
    record_payload[strcspn(record_payload, "\r\n")] = '\0';
    if (record_payload[0] == '\0') return false;

    *message_id = (uint64_t)parsed_id;
    *payload = record_payload;
    return true;
}

static bool recover_queue_files(void)
{
    if (access(SD_QUEUE_PATH, F_OK) != 0 &&
        access(SD_BACKUP_PATH, F_OK) == 0) {
        if (rename(SD_BACKUP_PATH, SD_QUEUE_PATH) != 0) {
            ESP_LOGE(TAG, "No se pudo recuperar la cola SD: %s", strerror(errno));
            return false;
        }
    } else if (access(SD_QUEUE_PATH, F_OK) == 0 &&
               access(SD_BACKUP_PATH, F_OK) == 0) {
        if (unlink(SD_BACKUP_PATH) != 0) {
            ESP_LOGE(TAG, "No se pudo limpiar respaldo SD: %s", strerror(errno));
            return false;
        }
    }

    if (unlink(SD_TEMP_PATH) != 0 && errno != ENOENT) {
        ESP_LOGE(TAG, "No se pudo limpiar archivo temporal SD: %s", strerror(errno));
        return false;
    }
    return true;
}

bool sd_storage_init(void)
{
    if (mounted) return true;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = NODE_SD_SPI_HOST;
    host.max_freq_khz = NODE_SD_MAX_FREQ_KHZ;
    spi_bus_config_t bus_config = {
        .mosi_io_num = NODE_SD_MOSI_PIN,
        .miso_io_num = NODE_SD_MISO_PIN,
        .sclk_io_num = NODE_SD_SCK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = NODE_SD_MAX_TRANSFER_SIZE
    };
    /* El driver SDSPI gestiona CS. Reseteamos el pin para liberar cualquier reserva previa de ESP-IDF */
    gpio_reset_pin(NODE_SD_CS_PIN);
    esp_err_t err = gpio_set_pull_mode(NODE_SD_MISO_PIN, GPIO_PULLUP_ONLY);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo activar pull-up en MISO (%s)", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "SD configurada: CS=%d SCK=%d MOSI=%d MISO=%d, %d kHz",
             NODE_SD_CS_PIN, NODE_SD_SCK_PIN, NODE_SD_MOSI_PIN, NODE_SD_MISO_PIN,
             NODE_SD_MAX_FREQ_KHZ);
    err = spi_bus_initialize(host.slot, &bus_config, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo inicializar SPI3 (%s)", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "Pull-up interno activado en MISO");

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.host_id = host.slot;
    slot_config.gpio_cs = NODE_SD_CS_PIN;

    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = NODE_SD_MAX_OPEN_FILES,
        .allocation_unit_size = NODE_SD_ALLOCATION_UNIT_SIZE
    };

    esp_log_level_set("sdmmc_cmd", ESP_LOG_DEBUG);
    err = esp_vfs_fat_sdspi_mount(NODE_SD_MOUNT_POINT, &host, &slot_config,
                                  &mount_config, &card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo montar la microSD (%s)", esp_err_to_name(err));
        gpio_reset_pin(NODE_SD_CS_PIN);
        esp_err_t free_err = spi_bus_free(host.slot);
        if (free_err != ESP_OK)
            ESP_LOGE(TAG, "No se pudo liberar SPI3 (%s)", esp_err_to_name(free_err));
        return false;
    }

    mounted = true;
    sdmmc_card_print_info(stdout, card);
    if (!recover_queue_files()) {
        esp_vfs_fat_sdcard_unmount(NODE_SD_MOUNT_POINT, card);
        card = NULL;
        mounted = false;
        gpio_reset_pin(NODE_SD_CS_PIN);
        esp_err_t free_err = spi_bus_free(host.slot);
        if (free_err != ESP_OK)
            ESP_LOGE(TAG, "No se pudo liberar SPI3 (%s)", esp_err_to_name(free_err));
        return false;
    }

    ESP_LOGI(TAG, "Cola SD lista en %s (SPI3: SCK=%d, MOSI=%d, MISO=%d, CS=%d)",
             NODE_SD_MOUNT_POINT, NODE_SD_SCK_PIN, NODE_SD_MOSI_PIN,
             NODE_SD_MISO_PIN, NODE_SD_CS_PIN);
    return true;
}

bool sd_storage_enqueue(uint64_t message_id, const char *payload)
{
    if (!mounted || payload == NULL || payload[0] == '\0' ||
        strchr(payload, '\n') != NULL || strchr(payload, '\r') != NULL) {
        ESP_LOGE(TAG, "No se pudo guardar el paquete en SD");
        return false;
    }

    FILE *file = fopen(SD_QUEUE_PATH, "a");
    if (file == NULL) {
        ESP_LOGE(TAG, "No se pudo abrir la cola SD: %s", strerror(errno));
        return false;
    }

    bool ok = fprintf(file, "%016" PRIX64 "|%s\n", message_id, payload) > 0 &&
              flush_file(file);
    if (fclose(file) != 0) ok = false;
    if (!ok) ESP_LOGE(TAG, "Error escribiendo paquete en la cola SD");
    return ok;
}

static void discard_corrupted_first_line(void)
{
    FILE *source = fopen(SD_QUEUE_PATH, "r");
    if (source == NULL) return;
    FILE *temporary = fopen(SD_TEMP_PATH, "w");
    if (temporary == NULL) {
        fclose(source);
        return;
    }

    /* Saltar caracteres de la primera linea rota hasta el proximo '\n' o EOF */
    int c;
    while ((c = fgetc(source)) != EOF) {
        if (c == '\n') break;
    }

    /* Copiar el resto de las lineas validas */
    char buf[256];
    size_t n;
    bool copy_ok = true;
    while ((n = fread(buf, 1, sizeof(buf), source)) > 0) {
        if (fwrite(buf, 1, n, temporary) != n) {
            copy_ok = false;
            break;
        }
    }

    bool ok = copy_ok && !ferror(source) && flush_file(temporary) &&
              fclose(source) == 0 && fclose(temporary) == 0;
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

sd_queue_result_t sd_storage_peek(uint64_t *message_id, char *payload,
                                  size_t payload_size)
{
    if (!mounted || message_id == NULL || payload == NULL || payload_size == 0) {
        ESP_LOGE(TAG, "Parametros invalidos al leer la cola SD");
        return SD_QUEUE_ERROR;
    }

    FILE *file = fopen(SD_QUEUE_PATH, "r");
    if (file == NULL) {
        if (errno == ENOENT) return SD_QUEUE_EMPTY;
        ESP_LOGE(TAG, "No se pudo leer la cola SD: %s", strerror(errno));
        return SD_QUEUE_ERROR;
    }

    static char line[NODE_SD_QUEUE_LINE_MAX];
    if (fgets(line, sizeof(line), file) == NULL) {
        bool read_error = ferror(file);
        fclose(file);
        if (read_error) {
            ESP_LOGE(TAG, "Error leyendo la cola SD");
            return SD_QUEUE_ERROR;
        }
        return SD_QUEUE_EMPTY;
    }
    bool complete_line = strchr(line, '\n') != NULL;
    fclose(file);

    if (!complete_line) {
        ESP_LOGE(TAG, "Registro incompleto o demasiado largo en la cola SD (se descartara)");
        discard_corrupted_first_line();
        return SD_QUEUE_ERROR;
    }

    char *record_payload = NULL;
    uint64_t record_id;
    if (!parse_record(line, &record_id, &record_payload)) {
        ESP_LOGE(TAG, "Registro invalido en la cola SD (se descartara)");
        discard_corrupted_first_line();
        return SD_QUEUE_ERROR;
    }
    size_t record_length = strlen(record_payload);
    if (record_length >= payload_size) {
        ESP_LOGE(TAG, "El buffer no alcanza para el registro SD (se descartara)");
        discard_corrupted_first_line();
        return SD_QUEUE_ERROR;
    }

    memcpy(payload, record_payload, record_length + 1);
    *message_id = record_id;
    return SD_QUEUE_PENDING;
}

bool sd_storage_ack(uint64_t message_id)
{
    if (!mounted) {
        ESP_LOGE(TAG, "No se puede confirmar un registro sin SD montada");
        return false;
    }

    FILE *source = fopen(SD_QUEUE_PATH, "r");
    if (source == NULL) {
        ESP_LOGE(TAG, "No se pudo abrir la cola SD: %s", strerror(errno));
        return false;
    }
    FILE *temporary = fopen(SD_TEMP_PATH, "w");
    if (temporary == NULL) {
        ESP_LOGE(TAG, "No se pudo crear temporal SD: %s", strerror(errno));
        fclose(source);
        return false;
    }

    bool found = false;
    bool ok = true;
    static char line[NODE_SD_QUEUE_LINE_MAX];
    while (fgets(line, sizeof(line), source) != NULL) {
        if (strchr(line, '\n') == NULL) {
            ok = false;
            break;
        }
        static char copy[NODE_SD_QUEUE_LINE_MAX];
        memcpy(copy, line, strlen(line) + 1);
        uint64_t record_id;
        char *record_payload;
        if (!parse_record(copy, &record_id, &record_payload)) {
            ok = false;
            break;
        }

        if (!found && record_id == message_id) {
            found = true;
        } else if (fputs(line, temporary) == EOF) {
            ok = false;
            break;
        }
    }
    if (ferror(source)) ok = false;
    if (!flush_file(temporary)) ok = false;
    if (fclose(source) != 0) ok = false;
    if (fclose(temporary) != 0) ok = false;

    if (!ok || !found) {
        unlink(SD_TEMP_PATH);
        ESP_LOGE(TAG, "%s al actualizar cola SD",
                 found ? "Error" : "No se encontro el ACK");
        return false;
    }

    if (unlink(SD_BACKUP_PATH) != 0 && errno != ENOENT) {
        ESP_LOGE(TAG, "No se pudo preparar respaldo SD: %s", strerror(errno));
        unlink(SD_TEMP_PATH);
        return false;
    }
    if (rename(SD_QUEUE_PATH, SD_BACKUP_PATH) != 0) {
        ESP_LOGE(TAG, "No se pudo respaldar la cola SD: %s", strerror(errno));
        unlink(SD_TEMP_PATH);
        return false;
    }
    if (rename(SD_TEMP_PATH, SD_QUEUE_PATH) != 0) {
        ESP_LOGE(TAG, "No se pudo reemplazar la cola SD: %s", strerror(errno));
        if (rename(SD_BACKUP_PATH, SD_QUEUE_PATH) != 0)
            ESP_LOGE(TAG, "No se pudo restaurar la cola SD: %s", strerror(errno));
        return false;
    }
    if (unlink(SD_BACKUP_PATH) != 0) {
        ESP_LOGE(TAG, "Cola actualizada, pero no se limpio el respaldo: %s",
                 strerror(errno));
        return false;
    }

    return true;
}
