/*
 * easycon_storage.c
 *
 * EasyCon 的掉电存储（脚本 + 种子 + LED 设置）实现。
 *
 * 官方 API 要求实现「按字节随机读写」，而 ESP32 的 NVS 不适合按字节存
 * （每个键都有开销，脚本几百上千字节会爆掉 NVS）。
 * 这里采用「RAM 影子 + NVS 整块 blob」的方式：
 *   - 开机把整块区域读进 RAM；
 *   - 读操作直接命中 RAM；
 *   - 写操作先改 RAM 并置脏；
 *   - 攒一小段时间（默认 300ms）后一次性写回 NVS，避免烧录脚本时
 *     每 20 字节就擦写一次 Flash（既慢又伤 Flash）。
 */
#include "easycon.h"

#include <string.h>
#include "nvs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define EASYCON_STORE_TAG   "easycon.store"
#define EASYCON_NVS_NS      "easycon"
#define EASYCON_NVS_KEY     "flash"

/* 掉电存储区大小：脚本(0..MEM_SIZE-1) + SEED(2) + LED(1) + 余量 */
#define EASYCON_FLASH_BYTES (MEM_SIZE + 16)

static uint8_t  s_flash[EASYCON_FLASH_BYTES];
static bool     s_dirty       = false;
static bool     s_flush_req   = false;
static bool     s_nvs_ok      = false;
static nvs_handle s_nvs       = 0;
static TickType_t s_last_activity = 0;

/* 攒盘时间：最后一次写之后这么久没有新写入才落盘 */
#define EASYCON_FLUSH_DELAY_MS 300

void easycon_storage_init(void)
{
    /* 默认整块 0xFF —— 与 AVR 上未烧录 EEPROM 的状态一致，
     * 这样 spec 里读到的 script_eof = 0xFFFF 就会被判为「没有脚本」。 */
    memset(s_flash, 0xFF, sizeof(s_flash));

    nvs_handle h;
    if (nvs_open(EASYCON_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        s_nvs = h;
        s_nvs_ok = true;

        size_t len = sizeof(s_flash);
        if (nvs_get_blob(s_nvs, EASYCON_NVS_KEY, s_flash, &len) == ESP_OK) {
            ESP_LOGI(EASYCON_STORE_TAG, "loaded %u bytes from NVS", (unsigned)len);
        } else {
            ESP_LOGI(EASYCON_STORE_TAG, "no saved script, using blank storage");
        }
    } else {
        ESP_LOGW(EASYCON_STORE_TAG, "nvs_open(\"%s\") failed, storage is RAM-only",
                 EASYCON_NVS_NS);
    }
    s_last_activity = xTaskGetTickCount();
}

uint8_t EasyCon_read_byte(uint16_t addr)
{
    return (addr < sizeof(s_flash)) ? s_flash[addr] : 0xFF;
}

void EasyCon_read_start(void)
{
    /* 可选回调，无需实现 */
}

void EasyCon_write_start(uint8_t mode)
{
    (void)mode;
}

void EasyCon_write_data(uint16_t addr, uint8_t *data, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++) {
        uint16_t a = (uint16_t)(addr + i);
        if (a >= sizeof(s_flash)) {
            break;
        }
        if (s_flash[a] != data[i]) {
            s_flash[a] = data[i];
            s_dirty = true;
        }
    }
    s_last_activity = xTaskGetTickCount();
}

void EasyCon_write_end(uint8_t mode)
{
    (void)mode;
    /* 只登记「需要落盘」，真正的写 Flash 交给 easycon_storage_tick()
     * 延迟执行，避免阻塞协议应答（上位机对应答有 200~1000ms 超时）。 */
    if (s_dirty) {
        s_flush_req = true;
        s_last_activity = xTaskGetTickCount();
    }
}

void easycon_storage_request_flush(void)
{
    if (s_dirty) {
        s_flush_req = true;
        s_last_activity = xTaskGetTickCount();
    }
}

/* 由 easycon 的 1ms tick 任务周期调用 */
void easycon_storage_tick(void)
{
    if (!s_flush_req || !s_dirty || !s_nvs_ok) {
        return;
    }
    if ((xTaskGetTickCount() - s_last_activity) < pdMS_TO_TICKS(EASYCON_FLUSH_DELAY_MS)) {
        return;
    }

    s_flush_req = false;
    if (nvs_set_blob(s_nvs, EASYCON_NVS_KEY, s_flash, sizeof(s_flash)) == ESP_OK) {
        nvs_commit(s_nvs);
        s_dirty = false;
        ESP_LOGI(EASYCON_STORE_TAG, "script/config saved to NVS");
    } else {
        ESP_LOGW(EASYCON_STORE_TAG, "nvs_set_blob failed, will retry");
        s_flush_req = true;
    }
}
