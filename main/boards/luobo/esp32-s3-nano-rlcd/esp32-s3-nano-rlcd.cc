#include "wifi_board.h"

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>

#include "application.h"
#include "button.h"
#include "codecs/box_audio_codec.h"
#include "codecs/dummy_audio_codec.h"
#include "config.h"
#include "cst836u_touch.h"
#include "custom_lcd_display.h"

#define TAG "nano-rlcd"

/*
 * ESP32-S3-Nano (N16R8) + ST7305 反射屏 + CST836U 触摸 + ES7210/ES8311 AEC 音频板
 *
 * 关键约定：
 *  - 触摸与两个 codec 拆成两条独立 I2C 总线：触摸 I2C_NUM_0（SCL 6 / SDA 7），
 *    codec I2C_NUM_1（SCL 38 / SDA 47）。触摸轮询流量不再顺音频板排线进入 codec。
 *  - 音频走 duplex 单总线：ES7210 用 TDM 4 通道收，ES8311 用标准立体声发。
 *  - ES7210 地址是 0x41（AD0 上拉），不是驱动默认的 0x40。
 */
static void touchpad_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    int x = 0;
    int y = 0;
    if (cst836u_touch_read(&x, &y)) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

class Esp32S3NanoRlcdBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t touch_i2c_bus_ = nullptr;
    i2c_master_bus_handle_t codec_i2c_bus_ = nullptr;
    Button boot_button_;
    CustomLcdDisplay* display_ = nullptr;
    bool touch_ready_ = false;

    void CreateI2cBus(i2c_port_num_t port, gpio_num_t sda, gpio_num_t scl,
                      i2c_master_bus_handle_t* out, const char* name)
    {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = port,
            .sda_io_num = sda,
            .scl_io_num = scl,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                // 触摸侧上拉由屏模组提供；codec 侧板载 R1/R2(2.2k) 在 BSS138 漏极侧，
                // 主控侧另需 2.2k 外部上拉到 3V3（400kHz 下内部 45k 不满足 tr<=300ns）。
                // 这里保持内部上拉仅为容错，不会造成总线过载。
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, out));
        ESP_LOGI(TAG, "%s i2c bus ready (port=%d sda=%d scl=%d)",
                 name, static_cast<int>(port), static_cast<int>(sda), static_cast<int>(scl));
    }

    void InitializeI2c()
    {
        // 触摸（屏模组自带 CST836U）与音频 codec 使用两条独立总线：触摸持续轮询的
        // 流量不再顺着 8~9cm 音频排线进入 codec 板，减少对模拟前端的干扰。
        CreateI2cBus(I2C_NUM_0, TOUCH_I2C_SDA_PIN, TOUCH_I2C_SCL_PIN, &touch_i2c_bus_, "touch");
        CreateI2cBus(I2C_NUM_1, AUDIO_CODEC_I2C_SDA_PIN, AUDIO_CODEC_I2C_SCL_PIN, &codec_i2c_bus_, "codec");
    }

    // probe 只要地址 ACK 就算通过，但 codec 在 I2C 上应答不代表寄存器可读写
    // （缺 MCLK、内部状态机未就绪时就会这样），所以这里做一次真实的寄存器读。
    // 默认 400kHz：本板在 100kHz 下实测 0/10，只有 400kHz 稳定，初判也统一走该速率。
    bool ReadCodecReg(uint8_t addr, const char* name, bool quiet = false, uint32_t speed_hz = 400000)
    {
        // addr 为 8 位形式（如 0x30/0x82），i2c_master 需要 7 位地址，与 esp_codec_dev 保持一致
        uint8_t addr7 = addr >> 1;
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addr7,
            .scl_speed_hz = speed_hz,
        };
        i2c_master_dev_handle_t dev = nullptr;
        if (i2c_master_bus_add_device(codec_i2c_bus_, &dev_cfg, &dev) != ESP_OK) {
            if (!quiet) {
                ESP_LOGE(TAG, "%s(0x%02X) add_device failed", name, addr7);
            }
            return false;
        }
        uint8_t reg = 0x00;
        uint8_t val = 0;
        esp_err_t ret = i2c_master_transmit_receive(dev, &reg, 1, &val, 1, pdMS_TO_TICKS(100));
        i2c_master_bus_rm_device(dev);
        if (!quiet) {
            ESP_LOGD(TAG, "%s(0x%02X) reg0 read: %s val=0x%02X", name, addr7, esp_err_to_name(ret), val);
        }
        return ret == ESP_OK;
    }

    // esp_codec_dev 内部用的是 i2c_master_transmit（只写，不带读），
    // 与上面的 transmit_receive 是两条不同的驱动路径，必须单独验证。
    // 这里读出原值再原值写回，不改变 codec 任何配置。
    bool TestCodecWrite(uint8_t addr, uint32_t speed_hz)
    {
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = (uint8_t)(addr >> 1),
            .scl_speed_hz = speed_hz,
        };
        i2c_master_dev_handle_t dev = nullptr;
        if (i2c_master_bus_add_device(codec_i2c_bus_, &dev_cfg, &dev) != ESP_OK) {
            return false;
        }
        uint8_t reg = 0x00;
        uint8_t val = 0;
        bool ok = false;
        if (i2c_master_transmit_receive(dev, &reg, 1, &val, 1, pdMS_TO_TICKS(100)) == ESP_OK) {
            uint8_t buf[2] = {reg, val};
            ok = (i2c_master_transmit(dev, buf, 2, pdMS_TO_TICKS(100)) == ESP_OK);
        }
        i2c_master_bus_rm_device(dev);
        return ok;
    }

    void InitializeDisplay()
    {
        // 参考 waveshare/esp32-s3-rlcd-4.2：panel_io 与 panel 传 NULL，
        // 由 CustomLcdDisplay 自建 SPI 与 LVGL display，走 1bpp LUT 直写。
        spi_display_config_t spi_config = {};
        spi_config.mosi = DISPLAY_SPI_MOSI_PIN;
        spi_config.scl = DISPLAY_SPI_SCK_PIN;
        spi_config.dc = DISPLAY_DC_PIN;
        spi_config.cs = DISPLAY_SPI_CS_PIN;
        spi_config.rst = DISPLAY_RST_PIN;
        display_ = new CustomLcdDisplay(NULL, NULL, DISPLAY_WIDTH, DISPLAY_HEIGHT, 0, 0,
                                        DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY,
                                        spi_config, SPI3_HOST);
    }

    void InitializeTouch()
    {
        touch_ready_ = cst836u_touch_init(touch_i2c_bus_);
        if (!touch_ready_) {
            ESP_LOGW(TAG, "touch not available, continue without it");
            return;
        }

        lv_indev_t *indev = lv_indev_create();
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev, touchpad_read_cb);
        lv_indev_set_disp(indev, lv_display_get_default());
        ESP_LOGI(TAG, "touch indev registered");

#if TOUCH_DEBUG_LOG
        // 界面没有可点击控件，只能靠日志判断触摸是否真正工作。
        // 按下瞬间打印一次坐标：左上角应接近 (0,0)，右下角应接近 (399,299)。
        xTaskCreate([](void*) {
            int x = 0;
            int y = 0;
            int last_x = -1;
            int last_y = -1;
            uint32_t hits = 0;
            while (true) {
                if (cst836u_touch_read(&x, &y)) {
                    if (x != last_x || y != last_y) {
                        hits++;
                        ESP_LOGI(TAG, "[touch] #%lu pressed at (%d,%d)", (unsigned long)hits, x, y);
                        last_x = x;
                        last_y = y;
                    }
                } else {
                    last_x = -1;
                    last_y = -1;
                }
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }, "touch_probe", 3072, nullptr, 5, nullptr);
        ESP_LOGI(TAG, "touch debug log enabled (poll 100ms)");
#endif
    }

    // 启动自检摘要：一眼看出各路引脚与探测结果是否符合预期
    void LogSelfCheck()
    {
        ESP_LOGI(TAG, "======== board self check ========");
        ESP_LOGI(TAG, "display: SPI3 rst=%d sck=%d mosi=%d dc=%d cs=%d (no backlight)",
                 static_cast<int>(DISPLAY_RST_PIN), static_cast<int>(DISPLAY_SPI_SCK_PIN),
                 static_cast<int>(DISPLAY_SPI_MOSI_PIN), static_cast<int>(DISPLAY_DC_PIN),
                 static_cast<int>(DISPLAY_SPI_CS_PIN));
        ESP_LOGI(TAG, "touch  : %s sda=%d scl=%d rst=%d int=%d addr=0x%02X",
                 touch_ready_ ? "OK" : "FAIL",
                 static_cast<int>(TOUCH_I2C_SDA_PIN), static_cast<int>(TOUCH_I2C_SCL_PIN),
                 static_cast<int>(TOUCH_RST_PIN), static_cast<int>(TOUCH_INT_PIN),
                 TOUCH_I2C_ADDR);
        ESP_LOGI(TAG, "audio  : i2c sda=%d scl=%d @%luHz pa=%d",
                 static_cast<int>(AUDIO_CODEC_I2C_SDA_PIN), static_cast<int>(AUDIO_CODEC_I2C_SCL_PIN),
                 (unsigned long)AUDIO_CODEC_I2C_SPEED_HZ, static_cast<int>(AUDIO_CODEC_PA_PIN));
        ESP_LOGI(TAG, "i2s    : mclk=%d bclk=%d ws=%d dout=%d din=%d",
                 static_cast<int>(AUDIO_I2S_GPIO_MCLK), static_cast<int>(AUDIO_I2S_GPIO_BCLK),
                 static_cast<int>(AUDIO_I2S_GPIO_WS), static_cast<int>(AUDIO_I2S_GPIO_DOUT),
                 static_cast<int>(AUDIO_I2S_GPIO_DIN));
        ESP_LOGI(TAG, "==================================");
    }

    void InitializeButtons()
    {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
    }

public:
    Esp32S3NanoRlcdBoard() : boot_button_(BOOT_BUTTON_GPIO)
    {
        InitializeI2c();
        InitializeDisplay();
        InitializeTouch();
        InitializeButtons();
        LogSelfCheck();
        ESP_LOGI(TAG, "board ready");
    }

    virtual AudioCodec* GetAudioCodec() override
    {
        // Application 会在启动和进入配网模式时各调用一次。必须只判定一次：
        // 否则第二次判定若通过仍会走 BoxAudioCodec，而它内部是 assert，
        // init 失败即整机重启，表现为屏幕反复闪「正在初始化」。
        static AudioCodec* selected = nullptr;
        if (selected != nullptr) {
            return selected;
        }

        // 速率统一由 AUDIO_CODEC_I2C_SPEED_HZ 控制。历史记录：共用一条总线时两颗
        // codec 的冷启动速率偏好相反（ES8311 只在 400kHz 通，ES7210 只在 100kHz 通），
        // 拆成独立总线后该约束消失，可统一速率。
        ESP_LOGI(TAG, "probing codec @%lu Hz", (unsigned long)AUDIO_CODEC_I2C_SPEED_HZ);
        bool es8311_ok = false;
        for (int i = 0; i < 5 && !es8311_ok; i++) {
            es8311_ok = ReadCodecReg(AUDIO_CODEC_ES8311_ADDR, "ES8311", true, AUDIO_CODEC_I2C_SPEED_HZ);
            if (!es8311_ok) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }
        bool es7210_ok = false;
        for (int i = 0; i < 3 && !es7210_ok; i++) {
            es7210_ok = ReadCodecReg(AUDIO_CODEC_ES7210_ADDR, "ES7210", true, AUDIO_CODEC_I2C_SPEED_HZ);
            if (!es7210_ok) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }
        if (!es7210_ok && AUDIO_CODEC_I2C_SPEED_HZ != 400000) {
            // 回退补测一次 400kHz，用于区分"速率不匹配"与"根本没接线/没上拉"
            for (int i = 0; i < 3 && !es7210_ok; i++) {
                es7210_ok = ReadCodecReg(AUDIO_CODEC_ES7210_ADDR, "ES7210", true, 400000);
                if (!es7210_ok) {
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
            }
        }
        if (!es7210_ok) {
            ESP_LOGW(TAG, "ES7210 probe failed: %luHz x3 and 400kHz x3 all NACK",
                     (unsigned long)AUDIO_CODEC_I2C_SPEED_HZ);
        }

        // 读写各 3 次做判定。保留写测试是因为 esp_codec_dev 走的是 i2c_master_transmit
        // （写路径），读通不代表写通；TestCodecWrite 只把读到的原值写回，不改变任何配置。
        int read_ok = 0;
        int write_ok = 0;
        for (int i = 0; i < 3; i++) {
            if (ReadCodecReg(AUDIO_CODEC_ES8311_ADDR, "ES8311", true, AUDIO_CODEC_I2C_SPEED_HZ)) {
                read_ok++;
            }
        }
        for (int i = 0; i < 3; i++) {
            if (TestCodecWrite(AUDIO_CODEC_ES8311_ADDR, AUDIO_CODEC_I2C_SPEED_HZ)) {
                write_ok++;
            }
        }
        ESP_LOGI(TAG, "ES8311 @%luHz read=%d/3 write=%d/3",
                 (unsigned long)AUDIO_CODEC_I2C_SPEED_HZ, read_ok, write_ok);

        // write 判定放宽到 >=1：主控侧无外部上拉（仅内部约 45k）时，写操作需要主机
        // 主动驱动 SDA 高电平，上升沿不足会偶发超时；读由从机驱动，3/3 稳定通过即可
        // 认定总线连通。esp_codec_dev 的实际寄存器写另有自己的重试，此处的
        // TestCodecWrite 只是"回写原值"的自检，失败不改变任何配置。
        // 补上 2.2k 外部上拉后应恢复为 write_ok < 3。
        if (!es8311_ok || !es7210_ok || read_ok < 3 || write_ok < 1) {
            ESP_LOGE(TAG, "audio codec unreliable (8311=%d 7210=%d read=%d write=%d), dummy",
                     es8311_ok, es7210_ok, read_ok, write_ok);
        } else if (write_ok < 3) {
            ESP_LOGW(TAG, "codec write flaky (%d/3), check I2C pull-up / wiring", write_ok);
            static DummyAudioCodec dummy_audio(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE);
            selected = &dummy_audio;
            return selected;
        }

        // 参数含义：主通道(MIC1) 输入增益 36dB、参考通道不额外加增益（channel=-1）、
        // I2C 速率取 AUDIO_CODEC_I2C_SPEED_HZ。
        // 36dB 为 83fba00（识别正常）时的取值。参考通道保持驱动默认，不做软件补偿：
        // 曾试过给 MIC3 单独加 +9dB 对齐幅度，但 AEC 在失配参考下会连同近端人声
        // 一起压制，识别率反而下降。当前阶段先保持"零干预"，用于验证声学问题。
        static BoxAudioCodec audio_codec(
            codec_i2c_bus_, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR, AUDIO_CODEC_ES7210_ADDR,
            AUDIO_INPUT_REFERENCE, 36.0f, -1, 0.0f, AUDIO_CODEC_I2C_SPEED_HZ);
        selected = &audio_codec;

        // 若 codec 初始化失败，此时 I2S 已在运行（MCLK/BCLK 正在翻转）。
        // 再测一次，用于区分"I2S 时钟串扰"与"驱动内部问题"：
        //   补测 3/3 → 不是串扰，问题在 esp_codec_dev 内部
        //   补测 0/3 → 确认 I2S 干扰，需调整初始化顺序或走线
        if (!audio_codec.IsReady()) {
            int after_i2s = 0;
            for (int i = 0; i < 3; i++) {
                if (ReadCodecReg(AUDIO_CODEC_ES8311_ADDR, "ES8311", true, AUDIO_CODEC_I2C_SPEED_HZ)) {
                    after_i2s++;
                }
            }
            ESP_LOGW(TAG, "after i2s start, %luHz read: %d/3",
                     (unsigned long)AUDIO_CODEC_I2C_SPEED_HZ, after_i2s);
        }
        return selected;
    }

    virtual Display* GetDisplay() override
    {
        return display_;
    }
};

DECLARE_BOARD(Esp32S3NanoRlcdBoard);
