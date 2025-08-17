#include "xz_board_info.h"
#include <esp_log.h>
#include <esp_random.h>
#include <esp_flash.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <esp_partition.h>
#include <esp_chip_info.h>
#include <esp_app_desc.h>
#include <esp_ota_ops.h>
#include <esp_netif.h>
#include <esp_mac.h>
#include <esp_check.h>
#include <nvs_flash.h>

#define NVS_PART_NAME CONFIG_XZ_NVS_PART_NAME_TO_SAVE_BOARD_INFO

#define XZ_NS "xz"
#define XZ_UUID "xz_uuid"

const static char* const TAG = "xz_board_info";

const char* xz_board_info_mac() {
    static char mac_str[18];
    if(*mac_str == 0) {
        uint8_t mac[6];
        esp_efuse_mac_get_default(mac);
        sprintf(mac_str, MACSTR, MAC2STR(mac));
    }
    return mac_str;
}

static size_t gen_uuid(char* uuid_str, size_t len) {
    uint8_t uuid[16];
    esp_fill_random(uuid, sizeof(uuid));

    // 设置版本 (版本 4) 和变体位
    uuid[6] = (uuid[6] & 0x0F) | 0x40;    // 版本 4
    uuid[8] = (uuid[8] & 0x3F) | 0x80;    // 变体 1

    // 将字节转换为标准的 UUID 字符串格式
    return snprintf(uuid_str, len,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        uuid[0], uuid[1], uuid[2], uuid[3],
        uuid[4], uuid[5], uuid[6], uuid[7],
        uuid[8], uuid[9], uuid[10], uuid[11],
        uuid[12], uuid[13], uuid[14], uuid[15]);
}

static char uuid_str[37]; // 36 + NULL
esp_err_t xz_board_info_load() {
    if(*uuid_str) return ESP_OK;
    nvs_handle_t hl;
    esp_err_t ret;
    size_t n;
    ESP_RETURN_ON_ERROR(nvs_open_from_partition(NVS_PART_NAME, XZ_NS, NVS_READWRITE, &hl), TAG, "can't open partition %s", NVS_PART_NAME);
    if((ret=nvs_get_str(hl, XZ_UUID, uuid_str, &n)) || n!=sizeof(uuid_str)) {
        gen_uuid(uuid_str, sizeof(uuid_str));
        ret = nvs_set_str(hl, XZ_UUID, uuid_str) || nvs_commit(hl);
    }
    nvs_close(hl);
    return ret;
}

const char* xz_board_info_uuid() {
    if(*uuid_str == 0) {
        ESP_LOGE(TAG, "UUID is empty");
    }
    assert(*uuid_str);
    return uuid_str;
}

void xz_board_info_init() {
    xz_board_info_uuid();
}

static uint32_t xz_board_info_flash_size() {
    uint32_t flash_size;
    if (esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get flash size");
        return 0;
    }
    return flash_size;
}

size_t xz_board_info_printf(char* buf, size_t len, const char* lang) {
    char* ptr = buf;
    ptr += sprintf(ptr, "{\"version\":2,\"language\":\"%s\",\"flash_size\":%ld,\"minimum_free_heap_size\":%ld,\"mac_address\":\"%s\",\"uuid\":\"%s\",\"chip_model_name\":\""CONFIG_IDF_TARGET"\",",
            lang, xz_board_info_flash_size(), esp_get_minimum_free_heap_size(), xz_board_info_mac(), xz_board_info_uuid());

    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    ptr += sprintf(ptr, "\"chip_info\":{\"model\":%d,\"cores\":%d,\"revision\":%d,\"features\":%ld},",
                    chip_info.model, chip_info.cores, chip_info.revision, chip_info.features);

    const esp_app_desc_t* app_desc = esp_app_get_description();
    char sha256_str[65];
    for (int i = 0; i < 32; i++)
        snprintf(sha256_str + i * 2, sizeof(sha256_str) - i * 2, "%02x", app_desc->app_elf_sha256[i]);
    ptr += sprintf(ptr, "\"application\":{\"name\":\"%s\",\"version\":\"%s\",\"compile_time\":\"%sT%sZ\",\"idf_version\":\"%s\",\"elf_sha256\":\"%s\"},\"partition_table\":[",
        app_desc->project_name, app_desc->version, app_desc->date, app_desc->time, app_desc->idf_ver, sha256_str);
    // ptr += sprintf(ptr, "\"application\":{\"name\":\""APP_NAME"\",\"version\":\""XZ_VER"\",\"compile_time\":\""XZ_COMPILE_DATE"T"XZ_COMPILE_TIME"Z\",\"idf_version\":\""XZ_IDF_VER"\",\"elf_sha256\":\""XZ_ELF_SHA256"\"},\"partition_table\":[");

    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    while (it) {
        const esp_partition_t *partition = esp_partition_get(it);
        ptr += sprintf(ptr, "{\"label\":\"%s\",\"type\":%d,\"subtype\":%d,\"address\":%ld,\"size\":%ld},",
            partition->label, partition->type, partition->subtype, partition->address, partition->size);
        it = esp_partition_next(it);
    }
    ptr --;

    const esp_partition_t* ota_partition = esp_ota_get_running_partition();
    ptr += sprintf(ptr, "],\"ota\":{\"label\":\"%s\"},", ota_partition->label);

    wifi_ap_record_t ap_info;
    esp_wifi_sta_get_ap_info(&ap_info);
    
    esp_netif_ip_info_t ip_info;
    esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_get_ip_info(sta_netif, &ip_info);
    
    ptr += sprintf(ptr, "\"board\":{\"type\":\""XZ_BOARD_TYPE"\",\"name\":\""XZ_BOARD_NAME"\",\"ssid\":\"%s\",\"rssi\":%d,\"channel\":%d,\"ip\":\""IPSTR"\",\"mac\":\"%s\"}}",
                    ap_info.ssid, ap_info.rssi, ap_info.primary, IP2STR(&ip_info.ip), xz_board_info_mac());
    
    return ptr- buf;
}