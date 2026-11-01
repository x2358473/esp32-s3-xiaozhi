#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "config.h"
#include "cst836u_touch.h"

#define TAG "CST836U"

#define TP_RAW_X_MAX 400
#define TP_RAW_Y_MAX 300

static i2c_master_dev_handle_t s_tp_dev = NULL;
// LVGL 的 read_cb 与诊断任务会同时调用 cst836u_touch_read，而 i2c_master_transmit_receive
// 对同一个 device handle 不是线程安全的（并发会返回 ESP_ERR_INVALID_RESPONSE），必须串行化。
static SemaphoreHandle_t s_tp_mutex = NULL;

static esp_err_t tp_read_regs(uint8_t reg, uint8_t *buf, size_t len)
{
    // 用"写地址 -> STOP -> 重新读"两步，而不是 transmit_receive 的 repeated START。
    // 实测本板 init 阶段两种都能通（见 [diag] B/C 均 ESP_OK），但进入运行期后
    // repeated START 恒返回 ESP_ERR_INVALID_RESPONSE（从机 NACK），而 C 的分离
    // 方式正常。由于我们固定读 0x00 起始的连续寄存器，STOP 后地址指针归零正好
    // 是需要的起点，两步法语义等价且更稳。
    esp_err_t ret = i2c_master_transmit(s_tp_dev, &reg, 1, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        return ret;
    }
    return i2c_master_receive(s_tp_dev, buf, len, pdMS_TO_TICKS(100));
}

#if TOUCH_DEBUG_LOG
// 原始寄存器快照，1 秒最多一条：用于区分"I2C 读失败""触点数解析不对""坐标越界"
static void tp_dump_raw(const uint8_t *d, esp_err_t ret)
{
    static int64_t s_last_us = 0;
    int64_t now = esp_timer_get_time();
    if (s_last_us != 0 && (now - s_last_us) < 5000000) {
        return;
    }
    s_last_us = now;

    unsigned rx = ((d[3] & 0x0F) << 8) | d[4];
    unsigned ry = ((d[5] & 0x0F) << 8) | d[6];
    ESP_LOGI(TAG, "raw ret=%s pts=%d rx=%u ry=%u | %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
             esp_err_to_name(ret), d[2] & 0x0F, rx, ry,
             d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[9], d[10]);
}
#endif

bool cst836u_touch_init(i2c_master_bus_handle_t bus)
{
    esp_err_t ret;

    gpio_config_t rst_cfg = {
        .pin_bit_mask = (1ULL << TOUCH_RST_PIN),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&rst_cfg), TAG, "rst gpio config failed");
    gpio_set_level(TOUCH_RST_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(30));
    gpio_set_level(TOUCH_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(200));

    gpio_config_t int_cfg = {
        .pin_bit_mask = (1ULL << TOUCH_INT_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&int_cfg);

    ret = i2c_master_probe(bus, TOUCH_I2C_ADDR, pdMS_TO_TICKS(200));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "probe 0x%02X failed: %s", TOUCH_I2C_ADDR, esp_err_to_name(ret));
        return false;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TOUCH_I2C_ADDR,
        // 400kHz：参考项目 ESP32-S3-NANO-RLCD（src/touch_cst8.cpp）同为 CST836U，
        // 在 400kHz 下长期稳定。本板 I2C 线上有电平转换器件，低速时上升沿过长、
        // MOS 长时间处于中间导通态，100kHz/10kHz 实测均返回 ESP_ERR_INVALID_RESPONSE。
        .scl_speed_hz = 400000,
    };
    ret = i2c_master_bus_add_device(bus, &dev_cfg, &s_tp_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "add device failed: %s", esp_err_to_name(ret));
        s_tp_dev = NULL;
        return false;
    }

    if (s_tp_mutex == NULL) {
        s_tp_mutex = xSemaphoreCreateMutex();
        if (s_tp_mutex == NULL) {
            ESP_LOGE(TAG, "create mutex failed");
            return false;
        }
    }

    ESP_LOGI(TAG, "touch found at 0x%02X", TOUCH_I2C_ADDR);

#if TOUCH_DEBUG_LOG
    /* 一次性读诊断：probe 只验证了"地址写被 ACK"，读数据分四个阶段，
     * 需要拆开定位到底卡在"帧长"还是"repeated START 不被支持"。 */
    {
        uint8_t reg = 0x00;
        uint8_t v1 = 0;
        uint8_t b11[11] = {0};
        uint8_t b2[11] = {0};

        esp_err_t e1 = i2c_master_transmit_receive(s_tp_dev, &reg, 1, &v1, 1, pdMS_TO_TICKS(100));
        esp_err_t e2 = i2c_master_transmit_receive(s_tp_dev, &reg, 1, b11, sizeof(b11), pdMS_TO_TICKS(100));
        esp_err_t e3 = i2c_master_transmit(s_tp_dev, &reg, 1, pdMS_TO_TICKS(100));
        esp_err_t e4 = i2c_master_receive(s_tp_dev, b2, sizeof(b2), pdMS_TO_TICKS(100));

        ESP_LOGI(TAG, "[diag] A(1B repeated)=%s val=0x%02X", esp_err_to_name(e1), v1);
        ESP_LOGI(TAG, "[diag] B(11B repeated)=%s first4=%02X %02X %02X %02X",
                 esp_err_to_name(e2), b11[0], b11[1], b11[2], b11[3]);
        ESP_LOGI(TAG, "[diag] C(write-then-read) tx=%s rx=%s first4=%02X %02X %02X %02X",
                 esp_err_to_name(e3), esp_err_to_name(e4), b2[0], b2[1], b2[2], b2[3]);
    }
#endif

    return true;
}

bool cst836u_touch_read(int *x, int *y)
{
    if (s_tp_dev == NULL) {
        return false;
    }

    // LVGL 的 read_cb（约 30Hz）与诊断轮询任务（10Hz）会并发进入本函数。
    // i2c_master_transmit_receive 对同一 device handle 并发调用会返回
    // ESP_ERR_INVALID_RESPONSE 甚至 ESP_ERR_TIMEOUT，必须串行化。
    if (s_tp_mutex != NULL && xSemaphoreTake(s_tp_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return false;
    }

    uint8_t d[11] = {0};
    esp_err_t ret = tp_read_regs(0x00, d, sizeof(d));
#if TOUCH_DEBUG_LOG
    tp_dump_raw(d, ret);
#endif
    if (s_tp_mutex != NULL) {
        xSemaphoreGive(s_tp_mutex);
    }

    if (ret != ESP_OK) {
        return false;
    }

    // 触点数只接受 1~2：空闲时 IC 会读回整片 0xFF，此时 d[2] & 0x0F == 0x0F
    uint8_t pts = d[2] & 0x0F;
    if (pts < 1 || pts > 2) {
        return false;
    }

    uint16_t rx = (uint16_t)(((d[3] & 0x0F) << 8) | d[4]);
    uint16_t ry = (uint16_t)(((d[5] & 0x0F) << 8) | d[6]);
    if (rx > TP_RAW_X_MAX || ry > TP_RAW_Y_MAX) {
        return false;  // 越界读数一律丢弃
    }

    int px = (int)((uint32_t)rx * (DISPLAY_WIDTH - 1) / TP_RAW_X_MAX);
    int py = (int)((uint32_t)ry * (DISPLAY_HEIGHT - 1) / TP_RAW_Y_MAX);
    if (px < 0) px = 0;
    if (py < 0) py = 0;
    if (px > DISPLAY_WIDTH - 1) px = DISPLAY_WIDTH - 1;
    if (py > DISPLAY_HEIGHT - 1) py = DISPLAY_HEIGHT - 1;

    *x = px;
    *y = py;
    return true;
}
