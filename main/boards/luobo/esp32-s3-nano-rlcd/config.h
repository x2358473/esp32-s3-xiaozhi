#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>
#include <es8311_codec.h>  // ES8311_CODEC_DEFAULT_ADDR

/*
 * ESP32-S3-Nano (N16R8) + ST7305 400x300 1bpp 反射屏 + CST836U 单点触摸
 *                                 + ES7210(ADC) / ES8311(DAC) / NS4150B(功放) AEC 音频板
 *
 * 引脚来源：本机实测（屏/触摸）与 7210-SC..V1.pdf 原理图 H1 排针（音频）。
 * 布局原则：上排集中屏与触摸、下排集中音频，天线区（01/02/03、41/42/45/46）留空。
 * 上排占用：14/13/12/11/10（屏）、9/8/7/6（触摸）。
 * 下排占用：48(PA_EN)、47/38（codec I2C）、21/18/17/16/15（I2S）。
 * 不可用：0（BOOT 按键）、19/20（USB D±）、33~37（Octal PSRAM）、43/44（USB CDC/JTAG）、
 *         45/46（strapping：45 会改 VDD_SPI 电压，上电被拉高将无法启动）。
 *
 * 本板卡接官方服务端（24k）。若改用自部署 xiaozhi-server（仅 16k），
 * 需把下面两个采样率改成 16000，并在 config.json 里补 CONFIG_OTA_URL。
 */

// ---------------- 音频（H1 排针，双工单总线） ----------------
#define AUDIO_INPUT_SAMPLE_RATE  24000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

#define AUDIO_I2S_GPIO_MCLK GPIO_NUM_21  // H1-6  I2S_MCK，需由 ESP32 主出（ES7210/ES8311 共用）
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_18  // H1-7  I2S_BCK
#define AUDIO_I2S_GPIO_WS   GPIO_NUM_16  // H1-9  I2S_WS
#define AUDIO_I2S_GPIO_DIN  GPIO_NUM_15  // H1-10 I2S_DI，ES7210 SDOUT1/TDMOUT -> ESP32
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_17  // H1-8  I2S_DO，ESP32 -> ES8311 DSDIN

#define AUDIO_CODEC_PA_PIN       GPIO_NUM_48  // H1-3 PA_EN -> NS4150B CTRL
#define AUDIO_CODEC_I2C_SDA_PIN  GPIO_NUM_47  // H1-4 ES_I2C_SDA，独立总线（不再与触摸共用）
#define AUDIO_CODEC_I2C_SCL_PIN  GPIO_NUM_38  // H1-5 ES_I2C_SCL，独立总线（不再与触摸共用）

// codec I2C 速率：必须保持 400000。
// 本板 I2C 线上有 BSS138 电平转换，低速（10k/50k/100k）时 MOS 管长时间处于
// 中间导通态，写操作（主机驱动 SDA 高电平）实测只有 1~2/3 成功，导致 ES8311
// 初始化中途写寄存器失败、codec 无法就绪并回退到 dummy（无声音、无 AEC 参考）。
// 400kHz 下上升沿更陡，实测 read/write 均 3/3。
#define AUDIO_CODEC_I2C_SPEED_HZ 400000

#define AUDIO_CODEC_ES8311_ADDR  ES8311_CODEC_DEFAULT_ADDR  // 0x30，8 位形式
#define AUDIO_CODEC_ES7210_ADDR  0x82  // 7 位 0x41 << 1：AD0 经 10k 上拉到 AU_3V3，非默认 0x40

// ES8311 的 OUTP/OUTN 经 R5/R7(0R) 硬件回接到 ES7210 的 MIC3 输入，构成 AEC 参考通道。
// 必须为 true，否则设备端 AEC 不可用、实时打断失效。
#define AUDIO_INPUT_REFERENCE    true

// ---------------- 显示（ST7305 400x300 1bpp，无背光无灰阶） ----------------
#define DISPLAY_WIDTH   400
#define DISPLAY_HEIGHT  300

#define DISPLAY_RST_PIN      GPIO_NUM_14
#define DISPLAY_SPI_SCK_PIN  GPIO_NUM_13
#define DISPLAY_SPI_MOSI_PIN GPIO_NUM_12
#define DISPLAY_DC_PIN       GPIO_NUM_11
#define DISPLAY_SPI_CS_PIN   GPIO_NUM_10

// MADCTL 0x48 已在驱动内部定型，这里全部交给驱动，勿再镜像/交换
#define DISPLAY_MIRROR_X false
#define DISPLAY_MIRROR_Y false
#define DISPLAY_SWAP_XY  false

#define DISPLAY_BACKLIGHT_PIN         GPIO_NUM_NC
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false

// ---------------- 触摸（CST836U，I2C 0x15，独立 I2C_NUM_0） ----------------
#define TOUCH_I2C_ADDR    0x15
#define TOUCH_RST_PIN     GPIO_NUM_9
#define TOUCH_INT_PIN     GPIO_NUM_8
#define TOUCH_I2C_SDA_PIN GPIO_NUM_7
#define TOUCH_I2C_SCL_PIN GPIO_NUM_6

// 触摸诊断日志：每 100ms 轮询一次，按下时打印坐标，用于验证触摸接线与坐标映射。
// 界面无交互控件，必须靠日志才能确认触摸是否生效。验证完成后置 0。
#define TOUCH_DEBUG_LOG 0

// ---------------- 按键 ----------------
#define BOOT_BUTTON_GPIO GPIO_NUM_0

#endif  // _BOARD_CONFIG_H_
