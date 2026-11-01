# ESP32-S3-Nano RLCD（ST7305 + ES7210/ES8311 AEC）

自制桌面面板版：ESP32-S3-Nano（N16R8）+ 4.2 寸 ST7305 反射式 1bpp 屏（400×300）
+ CST836U 单点触摸 + SurferAI ES7210/ES8311/NS4150B AEC 音频板。

## 硬件接线

> **引脚布局（2026-11-01 重排，已实机验证）**：屏与触摸全部走 Nano **上排**（从 GND/3V3 端起数），
> 音频 10 根全部走**下排**（从 GND/5V 端起数），两者同侧出线、互不交叉。天线区
> （GPIO 01/02/03、41/42/45/46）保持悬空。本板丝印数字即 GPIO 编号。

### 显示屏（SPI3，ST7305，**20MHz**）

| 信号 | 丝印（=GPIO） | 上排第几位 |
| :--- | :---: | :---: |
| RST | 14 | 4 |
| SCK | 13 | 5 |
| MOSI | 12 | 6 |
| DC | 11 | 7 |
| CS | 10 | 8 |

> **SPI 时钟固定 20MHz**。SPI3 没有 IOMUX，全部引脚经 GPIO 交换矩阵路由，40MHz 下
> 整帧 15000 字节会出现错位/花屏，本板实测 20MHz 长期稳定。

### 触摸（I2C_NUM_0，0x15，CST836U）

| 信号 | 丝印（=GPIO） | 上排第几位 |
| :--- | :---: | :---: |
| TP_RST | 9 | 9 |
| TP_INT | 8 | 10 |
| TP_SDA | 7 | 11 |
| TP_SCL | 6 | 12 |

> 触摸独占总线，不再与 codec 共用。屏模组自带 I2C 上拉，主控侧无需外挂。
> **速率必须 400kHz**——100kHz 与 10kHz 实测读数据恒返回 `ESP_ERR_INVALID_RESPONSE`，
> 详见「触摸排查记录」。

### 音频板（I2C_NUM_1，10Pin 排针，丝印 `GND VCC EN SDA SCL MCK BCK DO WS DI`）

引脚顺序以板上丝印为准，**最左侧 GND 为 1 脚**。原理图（`7210-SC..V1.pdf`）的网络名与丝印对应关系：
`PA_EN → EN`、`ES_I2C_SDA → SDA`、`ES_I2C_SCL → SCL`、`I2S_MCK → MCK`、`I2S_BCK → BCK`、`I2S_DO → DO`、`I2S_WS → WS`、`I2S_DI → DI`。

音频 10 根全部落在 Nano **下排第 1~12 位**，跨度约 28mm，3~4cm 直线即可，不再跨板绕行。

| 序号 | 丝印 | 下排第几位 | GPIO | 说明 | 线径 |
| :---: | :--- | :---: | :---: | :--- | :---: |
| 1 | GND | 1 | GND | 主接地点（**只引这一根**） | **26AWG** |
| 2 | VCC | 2 | **5V** | 板载 XC6206 需压差，**不可接 3V3** | **26AWG** |
| 3 | EN | 4 | 48 | 功放使能（PA_EN），经 R10 10k 到 NS4150B CTRL | 32AWG |
| 4 | SDA | 5 | 47 | 独立 I2C 总线，**主控侧需 2.2k 上拉** | 32AWG |
| 5 | SCL | 6 | 38 | 独立 I2C 总线，**主控侧需 2.2k 上拉** | 32AWG |
| 6 | MCK | 9 | 21 | ESP32 主出，两片 codec 共用 | 32AWG |
| 7 | BCK | 11 | 18 | | 32AWG |
| 8 | DO | 12 | 17 | ESP32 → ES8311 DSDIN | 32AWG |
| 9 | WS | 13 | 16 | | 32AWG |
| 10 | DI | 14 | 15 | ES7210 SDOUT1/TDMOUT → ESP32 | 32AWG |

下排空置：第 3 位（BOOT/GPIO0）、第 7~8 位（GPIO 39/40）、第 10 位（GND，可选时钟回流地）、
第 15~18 位（GPIO 41/42/45/46，其中 **45/46 禁用**：45 是 VDD_SPI 电压 strapping）。

### 喇叭与麦克风

- **喇叭接口是 1.25mm 2P 座**（PH1.25 带线端子），不是 2.54mm。原理图按 4 脚画
  （OUTP/OUTN/GND/GND），实物只装 2P 座、引出差分两线，第 3/4 脚为 GND 焊盘。
  单声道，4Ω 3W 或 8Ω 均可；习惯上红线接 OUTP、黑线接 OUTN。
- 麦克风为**板载** MIC1 / MIC2 小板（由 ES7210 的 MICBIAS 供电），无需外接。

### 接线验证顺序

1. 只接 `GND / VCC / SDA / SCL` 四根，上电后 I2C 扫描应同时出现
   `0x15`（CST836U）、`0x18`（ES8311）、`0x41`（ES7210）。
2. 三个地址齐全后再补 `EN / MCK / BCK / DO / WS / DI` 六根，串口日志应出现
   `BoxAudioCodec: Duplex channels created`。
3. 最后插喇叭。NS4150B 满功率峰值近 1A，USB 供电不足时会掉压复位，
   此时给音频板单独供 5V 并与 Nano 共地。

## 适配要点

1. **接官方服务端，24k**：本板卡 `config.json` 不覆盖 `CONFIG_OTA_URL`，走官方默认
   `https://api.tenclass.net/xiaozhi/ota/`；采样率 24000/24000。若改用自部署
   xiaozhi-server（仅 16k），需把 `config.h` 两个采样率改成 16000 并补 OTA 地址。
2. **ES7210 地址 0x41**：板上 AD0 经 R3(10k) 上拉到 AU_3V3、AD1 经 R4(10k) 下拉，
   不是驱动默认的 `0x40`。
3. **AEC 参考通道**：ES8311 的 `OUTP/OUTN` 经 R5/R7(0R) 硬件回接到 ES7210 的
   `ADC_MIC3P/MIC3N`，因此开启 `AUDIO_INPUT_REFERENCE`。音频链路复用
   `BoxAudioCodec`（ES7210 四通道 TDM + ES8311 播放），与 ESP-BOX-3 同构。
4. **I2C 拆成两条独立总线**：触摸走 `I2C_NUM_0`（SCL 6 / SDA 7），两片 codec 走
   `I2C_NUM_1`（SCL 38 / SDA 47）。这样触摸约 30Hz 的持续轮询流量不再顺着音频排线
   进入模拟前端。`esp32-s3-nano-rlcd.cc` 的 `CreateI2cBus()` 建两条，
   `BoxAudioCodec` 构造首参直接传 codec 总线句柄，公共层无需改动。
   音频板板载 R1/R2(2.2k) 位于 BSS138 **漏极侧**，不能替代主控侧上拉，主控侧必须外挂 2.2k。
5. **显示沿用微雪方案**：`custom_lcd_display.cc` 直接取自
   `boards/waveshare/esp32-s3-rlcd-4.2`（同为 ST7305 400×300 RLCD，已实机验证）。
   ST7305 显存是「每列 3 字节 = 2×4 像素块」的 1bpp 排布，整帧固定 15000 字节；
   构造时 `panel_io`/`panel` 传 NULL，自行 `lv_display_create` + flush 回调，
   用 400×300 的 index/bit 查找表把 RGB565 像素直写帧缓存，再整帧 SPI 刷出。
   不实现 `esp_lcd_panel_t`，避免与 IDF 版本相关的私有结构耦合。
6. **无背光**：不实现 `GetBacklight()`（基类默认返回 nullptr）。

## 构建

```bash
python scripts/build.py luobo/esp32-s3-nano-rlcd --name esp32-s3-nano-rlcd
```

或在仓库 Actions 里手动触发 `Build Boards`，`board` 选 `luobo/esp32-s3-nano-rlcd`。

## 已验证结论（2026-11-01）

| 项 | 结论 |
| :--- | :--- |
| ST7305 显示 | ✅ 全链路正常（SPI3 20MHz + 自建 lv_display + 整帧 flush） |
| CST836U 触摸 | ✅ 坐标准确，需用「写地址+STOP+重读」读法 |
| ES8311/ES7210 | ✅ 400kHz 下 read/write 均 3/3，AEC 参考通道在位 |
| NS4150B 功放 | ✅ 5V 供电正常；满功率峰值近 1A，USB 供电不足时会掉压复位 |
| `i2c_cfg.port` 写死 1 | ✅ 无影响，实际走外部 `bus_handle`；拆分后 `I2C_NUM_1` 正好对上 |
| 声学 | ⚠️ 壳体未密封，喇叭泄漏导致幻听（已定位，待结构改造） |

## 设备端 AEC 与 I2C 地址约定

- 实时打断依赖 `CONFIG_USE_DEVICE_AEC=y`，而该选项在 `main/Kconfig.projbuild` 里带板型白名单，
  本板已登记 `BOARD_TYPE_LUOBO_ESP32_S3_NANO_RLCD`。若白名单丢失，构建会告警
  `USE_DEVICE_AEC has user-set value y ... but is not visible`，随后静默退回 `kAecOff`，
  表现就是"能对话但无法实时打断"，排查时先看 `sdkconfig` 里有没有 `CONFIG_USE_DEVICE_AEC=y`。
- `AUDIO_CODEC_ES8311_ADDR` / `AUDIO_CODEC_ES7210_ADDR` 必须写 8 位形式
  （`ES8311_CODEC_DEFAULT_ADDR` = 0x30，ES7210 = `0x82` = `0x41<<1`）：`esp_codec_dev`
  内部会执行 `addr >> 1`，若写成 7 位地址，实际会访问不存在的从机，读写 100% 失败，
  但日志打印的仍是移位前的原值，容易误判成硬件问题。
- **codec I2C 必须跑 400kHz**。本板 I2C 线上有 BSS138 电平转换，低速（10k/50k/100k）
  时 MOS 管长时间处于中间导通态，导致写操作（主机主动驱动 SDA 高电平）只有 1~2/3 成功。
  实测 100kHz 下 `ES8311 read=3/3 write=1/3`，写失败会让 codec 初始化中途中断、
  `init_ok_` 保持 false，最终回退到 `DummyAudioCodec`——表现是**没有声音、AFE 少一个
  参考通道**（日志 `total 1 channels(1 microphone, 0 playback)`）。
  恒定 400kHz 后 `read/write` 均 3/3，`BoxAudioDevice initialized` 正常出现。
  速率由 `config.h` 的 `AUDIO_CODEC_I2C_SPEED_HZ` 统一控制，**不要再降速排查**。

## 底噪与唤醒优化记录（2026-10-31）

本板自查"唤醒经常失败、唤醒后对话正常"的过程与结论，供后续换版或回退对照。

### 现象与根因

- 待机时 MIC1 静默电平长期在 **-41dBFS** 附近，1 秒内可波动 20dB；30cm 说话约 -42dBFS，
  唤醒信噪比仅约 5dB，WakeNet 频繁漏检，但唤醒后走云端 ASR 仍能正常识别。
- 根因不在麦克风：36dB PGA 下折算到 ADC 输入约 -77.5dBFS，比模拟 MIC 自噪高 15~20dB，
  属于模拟前端被电源/地耦合污染。
- 逐项排除后确认主因是 **NS4150B 功放常开**：`AudioService::CheckAndUpdateAudioPowerState()`
  的关功放分支被 `duplex && input_enabled` 条件挡住（要保持 I2S TX 时钟给 RX），
  功放静态噪声持续经电源/地耦合回 MIC。

### 已落地的三项改动

| 改动 | 位置 | 效果 |
| :--- | :--- | :--- |
| 待机 15 秒后单独拉低 PA，不触碰 I2S TX | `audio_service.cc`、`box_audio_codec.cc` | 静默底噪 **-41 → -58dBFS**，降低约 16dB |
| WakeNet 判定模式改为 `DET_MODE_95` | `afe_audio_engine.cc` | 提高召回，实测未出现误唤醒 |
| 待机（只剩唤醒词检测）关闭 AEC | `afe_audio_engine.cc` | 底噪再降 2~3dB 且更平稳；此时参考通道是数字静音，AEC 无实际作用 |

### 实测数据（1 米，安静环境）

- 待机底噪：-56 ~ -62dBFS
- 说话时 ch0：-37 ~ -43dBFS，唤醒信噪比约 20dB
- 唤醒成功率：约 75~80%（12~13 次尝试中成功 10 次）
- 未触发的案例中最高电平达 **-36.4dBFS**，比多数成功案例更响，
  说明剩余漏检与电平无关，属于 WakeNet 模型对真人发音的泛化问题。

### 已知局限与后续选项（未实施）

- 当前模型 `wn9_nihaoxiaozhi_tts` 由 TTS 合成语料训练，对真人口音/语速泛化有限。
  可换 `wn9l_nihaoxiaozhi_tts3`（大模型，资源约 1.3 倍）验证；需在 `config.json` 的
  `sdkconfig_append` 里显式把旧模型置 `n`，否则 build 脚本会把两个模型都打进
  `srmodels.bin`，AFE 只取第一个，等于没换。
- 硬件侧仍有优化空间（板上为 0402/0603，不便手工改）：AEC 参考取自 ES8311 输出而非
  功放后级、参考通道无衰减网络、音频地与主控仅靠排针单点相连。
- 待机 15 秒关功放实现在 `AudioCodec` 抽象层，所有使用 `BoxAudioCodec` 的板卡同样生效。

### 临时诊断日志（稳定后可关闭）

| 日志 | 位置 | 关闭方式 |
| :--- | :--- | :--- |
| `MicLevel: ch0=.. ch1=..` | `audio_service.cc` `ReadAudioData` | 删除该代码块 |
| `afe controls: wakenet=.. aec=..` | `afe_audio_engine.cc` `ApplyAfeControls` | 保留（开销极小） |
| `[touch] #N pressed at (x,y)` | `esp32-s3-nano-rlcd.cc` `InitializeTouch` | `config.h` 里 `TOUCH_DEBUG_LOG 0` |
| `CST836U: raw ret=.. pts=..` | `cst836u_touch.c` `tp_dump_raw` | 同上（与 touch 日志共用宏） |
| `Dropped N audio packets outside speaking state` | `application.cc` | 删除该代码块 |
| `Decode queue full, dropped N` | `audio_service.cc` | 删除该代码块 |

注：`speaking -> listening` 时固定出现一次的 `Dropped 1 audio packets (state: 5)` 是
服务端 stop 之后的尾包，属正常现象。

## 幻听（误触发）排查记录（2026-11-01，已定位）

现象：**用户没说话，服务端却收到语音并回答**，日志表现为 `>> 好。` `>> 师否。` 之类的
单字/残词，随后触发工具调用。

### 结论：壳体声学密封问题，不是电路问题

**关键验证**：把喇叭从壳里拆出来（用线临时引出），播放同一段长故事，**幻听完全消失**；
装回壳内立即重现。

这排除了所有电路/AEC 方向的可能性，根因是**喇叭背面辐射经壳腔泄漏到麦克风开孔**：

- 喇叭只有振膜边缘与壳体之间**没有密封圈**，背面声压直接灌进壳内空腔
- 麦克风开孔在顶边，与壳腔连通，等于"从内部听喇叭"
- 麦克风拾音孔朝屏幕（垂直），而侧孔水平，声波在腔内绕行后各方向声压趋于均匀，
  于是内腔声压被完整拾取

### 已全部回退的软件补偿（4 项）

以下改动曾用于压制幻听，**已确认无效并全部移除**，不要再加回来：

| 改动 | 曾用取值 | 回退原因 |
| :--- | :--- | :--- |
| 播放期间上行 PCM 清零 / 阈值放行 | `SetUplinkMuted()` | 治标：直接导致"无法语音打断"，且掩盖真实问题 |
| 播放结束上行抑制窗口 | `SuppressUplinkForMs(300)` | 同上，只能挡尾音，挡不住播放中的持续泄漏 |
| 输入二阶高通 150Hz | `ReadAudioData()` 内联 biquad | 对壳体泄漏无效（泄漏在 300Hz 以上） |
| 参考通道单独加增益 | `reference_gain_channel=2, +9dB` | 会让 AEC 在参考失配时连带压制近端人声，识别率反而下降 |

**当前配置**：`reference_gain_channel = -1`、`gain = 0.0f`、输入 36dB、`AEC_MODE_FD_LOW_COST`，
即与 `83fba00`（识别正常的版本）音频前端等价，无任何上行门控。

### 声学根因的定量分析

壳内 100×80×9mm，音频板厚 1.6mm，腔内净高约 7.4mm。

**Helmholtz 共振**（开孔面积 A=60mm²、腔体积 V=59200mm³、壁厚 L≈3mm）：

```
f = (c/2π)·√(A/(V·L)) ≈ 1000 Hz
```

共振点正好落在"你好小智"的主要能量区，导致 300~500Hz 衰减、1kHz 凸起、2kHz 以上滚降。
**这同时解释了唤醒不灵**（频响畸变使 WakeNet 收到的特征偏离训练集）。

**围成小腔后**（A 不变）：

| 腔体尺寸 | 体积 | 共振频率 |
| :--- | :---: | :---: |
| 整个壳（现状） | 59200mm³ | **1000Hz** |
| 30×30×7.4 | 6660mm³ | 3300Hz |
| **25×20×7.4（推荐）** | 3700mm³ | **4000Hz** |
| 20×15×7.4 | 2220mm³ | 5200Hz |

### 结构改进方案（待实施）

| 优先级 | 措施 | 说明 |
| :---: | :--- | :--- |
| **1** | 喇叭边缘加密封泡棉圈 | 1~2mm EVA 泡棉，压缩 30%，振膜外圈一整圈连续；这是消除幻听的决定性一步 |
| **2** | 围小腔（25×20mm）+ 四面密封 | 把音频板与两个开孔围成独立小腔，挡墙顶到屏幕背面并压实；共振从 1kHz 推到 4kHz |
| **3** | 确认 MIC 与屏幕间隙 ≥2mm | 防止拾音孔被屏幕压死形成死腔 |
| 4 | 小腔内贴 2mm 薄吸音棉 | 抑制腔内驻波，不要堵孔 |
| 5 | 两个开孔都保留 | 孔越大共振越高；单孔会把共振拉回 2.8kHz |

**材料**：EVA 泡棉胶带（1/2/5mm）、704 中性硅酮胶、聚酯纤维吸音棉。
**禁用**：热熔胶（变脆开裂）、薄双面胶（压不实）、502/AB 胶（刚性无弹性）。

**验证技巧**：正式上胶前先用**蓝丁胶**围出小腔做快速验证，确认有效再换永久材料。

### 唤醒不灵的独立结论

与幻听同源（频响畸变），但也有模型因素：

- 失败样本 `ch0 = -30.9dBFS`，成功样本 `ch0 = -32.9dBFS`——**失败时电平更高**，
  证明不是音量/距离问题，是模型漏检
- `srmodels.bin` 只打包了 `wn9_nihaoxiaozhi_tts` 一个模型，没有可替换的第二选择
- 已用尽的手段：`DET_MODE_95`、36dB 增益（ES7210 上限 37.5dB）、待机关 AEC

**剩下来源**：麦克风开孔声学（见上表第 2、3 项）、说话语速放慢、避开退出后 1~2 秒。

## 触摸排查记录（2026-11-01，已解决）

这是本次改造中最容易走弯路的一段，完整记录避免重蹈。

### 症状与排除过程

| 阶段 | 现象 | 结论 |
| :--- | :--- | :--- |
| 初始 | `I2C probe 0x15` 成功，但读 11 字节恒 `ESP_ERR_INVALID_RESPONSE`，数据全 0 | 地址能 ACK，读数据失败 |
| 降速 100k → 10k | **无改善** | ❌ 不是速率问题 |
| 升速 10k → 400k | **无改善** | ❌ 也不是速率问题 |
| 加互斥锁 | 无改善 | ❌ 不是多任务并发 |
| **三段式诊断** | 见下 | ✅ 定位到 repeated START |

### 关键诊断代码（曾临时加在 `cst836u_touch_init` 内）

同时测三种读法，一次烧录就能区分：

```c
// A) 1 字节 repeated START
i2c_master_transmit_receive(dev, &reg, 1, &v1, 1, 100ms);
// B) 11 字节 repeated START
i2c_master_transmit_receive(dev, &reg, 1, b11, 11, 100ms);
// C) 写地址 -> STOP -> 重新读
i2c_master_transmit(dev, &reg, 1, 100ms);
i2c_master_receive(dev, b2, 11, 100ms);
```

诊断输出：

```
[diag] A(1B repeated)=ESP_OK val=0x00
[diag] B(11B repeated)=ESP_OK first4=00 00 00 FF
[diag] C(write-then-read) tx=ESP_OK rx=ESP_OK first4=00 00 00 FF
```

**三种读法在 init 阶段全部成功**，但进入运行期后 `transmit_receive` 恒失败。

### 根因与修复

`i2c_master_transmit_receive`（repeated START）在 CST836U 上运行期不稳定：
写地址后不产生 STOP、直接发 repeated START，芯片会把这次访问当作"上一次传输未结束"而 NACK。

**修复**：`tp_read_regs()` 改成两步——`i2c_master_transmit` 写寄存器地址（带 STOP），
再 `i2c_master_receive` 重新读。因为固定读 0x00 起始的连续寄存器，STOP 后地址指针归零
正好是需要的起点，语义等价。

```c
static esp_err_t tp_read_regs(uint8_t reg, uint8_t *buf, size_t len)
{
    esp_err_t ret = i2c_master_transmit(s_tp_dev, &reg, 1, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) return ret;
    return i2c_master_receive(s_tp_dev, buf, len, pdMS_TO_TICKS(100));
}
```

改完立刻正常，坐标与实际按压位置完全对应：

```
CST836U: raw ret=ESP_OK pts=1 rx=225 ry=44 | 00 00 01 80 E1 00 2C 00 00 FF FF
nano-rlcd: [touch] #4 pressed at (224,43)
```

### 触摸侧不变的正确结论

- **速率 400kHz**（与参考项目 `ESP32-S3-NANO-RLCD` 一致），不是 100kHz
- **屏模组自带 I2C 上拉**，主控侧无需外挂 4.7k
- 读法固定为「写地址 + STOP + 重新读」，不要改回 `transmit_receive`
- `cst836u_touch_read()` 内仍保留互斥锁（`s_tp_mutex`）：LVGL read_cb 与诊断任务
  并发访问同一 device handle 时，驱动本身并不保证安全

### 坐标量程

`TP_RAW_X_MAX=400`、`TP_RAW_Y_MAX=300`，与屏分辨率 1:1 对应，无需缩放补偿。

## 关键日志速查

| 日志 | 含义 |
| :--- | :--- |
| `AFE Pipeline: ... |AEC(FD_LOW_COST, NLP_VERY_AGGRESSIVE)|` | 确认 AEC 档位（当前配置） |
| `Input PCM Config: total 2 channels(1 microphone, 1 playback)` | ✅ AEC 参考通道在位 |
| `Input PCM Config: total 1 channels(1 microphone, 0 playback)` | ❌ codec 回退到 dummy，检查 `ES8311 ... write=3/3` |
| `BoxAudioCodec: BoxAudioDevice initialized` | codec 真正初始化成功 |
| `ES8311 @400000Hz read=3/3 write=1/3` | 写不稳，I2C 电气或速率有问题 |
| `afe controls: wakenet=on aec=off (bits=0x5)` | 待机态（只有唤醒词） |
| `afe controls: wakenet=off aec=on (bits=0x6)` | 对话态（AEC 打开） |
| `MicLevel: ch0=.. ch1=..` | AEC 前电平；ch1 静默时应为 -90dBFS |
| `CST836U: raw ret=ESP_OK pts=1 rx=.. ry=..` | 触摸读通且有触点 |
| `CST836U: raw ret=ESP_ERR_INVALID_RESPONSE` | 读法退化成 repeated START 了 |
| `>> 单字` | 服务端 ASR 结果，若用户未说话即为幻听（检查壳体密封） |
| `Wrong sequence` | UDP 丢包，单次约 60ms，与幻听无关 |

## 开发环境搭建（换机器照此执行）

以下流程在 macOS（Apple Silicon）实测通过，工具链版本 ESP-IDF **v6.1**。

```sh
# 1. 依赖（macOS 下 install.sh 不装 cmake/ninja，必须单独装）
brew install cmake ninja

# 2. 取 ESP-IDF（浅克隆 + 子模块，约 750MB）
mkdir -p ~/esp && cd ~/esp
git clone -b v6.1 --recursive --depth 1 --shallow-submodules https://github.com/espressif/esp-idf.git

# 3. 安装 esp32s3 工具链（约 4.8GB，装到 ~/.espressif）
cd ~/esp/esp-idf && ./install.sh esp32s3

# 4. 每次开新终端都要 source
. ~/esp/esp-idf/export.sh
```

构建与烧录（工程根目录）：

```sh
cd /path/to/esp32-s3-xiaozhi
python3 scripts/build.py luobo/esp32-s3-nano-rlcd --name esp32-s3-nano-rlcd
idf.py -p /dev/cu.usbmodem* flash monitor
```

产物：`build/xiaozhi.bin`（app）、`build/generated_assets.bin`（assets，含唤醒词模型）、
`build/merged-binary.bin`（整包）。`python3 scripts/build.py --list-boards` 可列出全部板卡。

首次构建会下载约 74 个 managed components，之后增量编译约 2 分钟。
`build/`、`sdkconfig`、`managed_components/` 均在 gitignore 内，不纳入版本管理。

## 本次改造的全部代码改动（2026-11-01）

### 保留的改动

| 文件 | 改动 | 原因 |
| :--- | :--- | :--- |
| `config.h` | 屏 14/13/12/11/10、触摸 9/8/7/6、音频 48/47/38/21/18/17/16/15 | 引脚重排，音频集中下排 |
| `config.h` | 新增 `TOUCH_I2C_SDA/SCL_PIN`、`AUDIO_CODEC_I2C_SPEED_HZ=400000`、`TOUCH_DEBUG_LOG` | I2C 拆分与诊断开关 |
| `esp32-s3-nano-rlcd.cc` | 新增 `CreateI2cBus()`，拆 `touch_i2c_bus_` / `codec_i2c_bus_` | 触摸流量不再进音频前端 |
| `esp32-s3-nano-rlcd.cc` | 输入增益 36dB、`reference_gain_channel=-1`、`gain=0.0f` | 回到 `83fba00` 音频前端 |
| `esp32-s3-nano-rlcd.cc` | `LogSelfCheck()` 启动自检摘要 | 一眼核对四组引脚 |
| `custom_lcd_display.cc` | SPI **40MHz → 20MHz** | SPI3 无 IOMUX，40MHz 整帧错位 |
| `cst836u_touch.c` | `tp_read_regs()` 改「写地址+STOP+重读」 | 修复触摸读不出数据 |
| `cst836u_touch.c` | 新增 `s_tp_mutex` 互斥锁 | LVGL 与诊断任务并发访问 |
| `cst836u_touch.c` | 复位等待 100→200ms | 给芯片足够复位时间 |
| `afe_audio_engine.cc` | `AEC_MODE_FD_LOW_COST` | HIGH_PERF 在参考失配时压制人声 |

### 已回退的改动

| 改动 | 回退原因 |
| :--- | :--- |
| `SetUplinkMuted()` 播放期间上行清零/阈值放行 | 治标，导致无法语音打断 |
| `SuppressUplinkForMs(300)` | 只挡尾音，挡不住播放中泄漏 |
| 输入二阶高通 150Hz | 对壳体泄漏无效 |
| 参考通道 +9dB | AEC 过度消除，识别率下降 |
| `AEC_MODE_FD_HIGH_PERF` | 同上 |
| 输入增益 30dB | 人声过弱，识别错误 |

### 当前已知未解问题

| 问题 | 状态 |
| :--- | :--- |
| 壳体声学泄漏导致幻听 | **已定位，待结构改造**（喇叭密封圈） |
| 唤醒漏检（-30dBFS 仍失败） | 已定位为频响畸变 + 模型泛化，待声学改善 |
| 播放中无法语音打断 | 当前无上行门控，属预期行为 |

### 分支状态

- 分支：`board/nano-rlcd`
- 上述保留改动**均在改造后的工作区**，尚未提交
- 建议按逻辑拆成独立提交：引脚重排 / 触摸修复 / 屏幕降频 / 音频参数回归
