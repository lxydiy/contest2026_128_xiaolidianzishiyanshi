# ESP32-P4 MIPI CSI / SC2336 详细设计

## 1. 范围与边界

本设计在 contestant overlay 内增加 ESP32-P4 CSI transport、SC2336 sensor lower-half 和 Function EV Board 注册代码。输出为 1280×720@30 packed BGGR RAW10；不提供 ISP、demosaic、RGB 转换或标准 V4L2 RAW ABI。

## 2. 架构

```text
Application
  -> /dev/video0 (NuttX v4l2 capture, experimental format token)
    -> imgdata_s: ESP32-P4 MIPI CSI + DW-GDMA channel 1
    -> imgsensor_s: SC2336 over I2C0
      -> 2-lane MIPI CSI-2 RAW10
```

### 2.1 文件职责

- `esp_mipi_csi.h`: 固定模式配置、frame size、experimental token 和初始化入口。
- `esp_mipi_csi.c`: `imgdata_s`、CSI HAL、DMA、IRQ、cache、worker 和生命周期。
- `esp32p4_sc2336.c`: I2C helpers、PID probe、模式表、`imgsensor_s` 和 `/dev/video0` 注册。
- `sc2336_720p30_raw10.h`: Espressif 官方 Apache-2.0 模式寄存器表。
- Board/chip Kconfig and build files: feature gating and source integration。

## 3. 固定数据格式

- Width: 1280
- Height: 720
- Bits per pixel: 10 packed
- Payload: 1,152,000 bytes
- Bayer order: BGGR
- Frame interval: 1/30 second
- CSI lanes: 2
- Lane bit rate: 405 Mbps
- Sensor input clock: 24 MHz external/module-provided

`IMGDATA_PIX_FMT_ENTROPY` / `IMGSENSOR_PIX_FMT_ENTROPY` 仅作为 overlay 内部 experimental opaque payload token，以避免谎报 RGB565。公开描述、日志、Kconfig 和 README 必须始终称数据为 packed BGGR RAW10；该 token 不是标准 Bayer RAW10 ABI。

## 4. CSI transport 设计

### 4.1 状态

```text
UNINITIALIZED -> INITIALIZED -> BUFFER_SET -> CAPTURING
      ^              |              |             |
      +--------------+--------------+-------------+
                         stop/uninit
```

状态由 mutex 保护；ISR 只访问 `capturing`、completion status 和 worker scheduling 所需字段，并在 critical section 内更新。

### 4.2 初始化

1. acquire MIPI PHY LDO3 at 2500 mV；因 Kconfig 与 DSI 互斥，CSI 可独占其生命周期。
2. enable CSI host/bridge clocks。
3. `mipi_csi_hal_init()` 配置 2 lanes、405 Mbps、1280×720、RAW10 source/destination、no byte swap。
4. initialize DW-GDMA HAL and channel 1。
5. setup peripheral IRQ mapping, attach `ESP_IRQ_DW_GDMA`, clear pending, then enable。
6. 初始化失败时按逆序释放。

### 4.3 Buffer validation

`set_buf()` 必须检查：

- format count exactly 1；固定 width/height/token。
- address non-null。
- size at least 1,152,000 bytes。
- address and payload size aligned to 8 bytes。
- buffer位于 PSRAM 时必须可由 DMA 访问。
- capture active时拒绝更换 buffer。

### 4.4 DMA

- Channel: 1。
- Source role: `DW_GDMA_ROLE_PERIPH_CSI`。
- Destination role: `DW_GDMA_ROLE_MEM`。
- Flow controller: source。
- Source address: `MIPI_CSI_BRG_MEM_BASE`, fixed。
- Destination: frame buffer, incrementing。
- Width: 64 bits both sides。
- Block transfer count: payload bits / 64。
- Start order: clean/invalidate destination cache → arm DMA → enable CSI bridge。
- Stop order: disable CSI bridge → disable DMA → clear interrupts。

### 4.5 IRQ and worker

DW-GDMA ISR：

1. read channel interrupt status。
2. clear transfer-complete/error status。
3. record success/failure and timestamp。
4. set `capturing=false` for one-shot buffer completion。
5. queue LPWORK。

Worker：

1. invalidate captured buffer cache。
2. atomically snapshot callback/argument/result。
3. invoke `imgdata_capture_t` outside ISR。

`stop_capture()` and `uninit()` disable hardware first, then `work_cancel_sync(LPWORK, ...)`, then clear callback pointers。

## 5. SC2336 sensor design

### 5.1 I2C protocol

- I2C0, 7-bit address `0x30`, 400 kHz。
- Write: 16-bit register address big-endian + one value byte。
- Read: write 16-bit register address followed by repeated-start one-byte read。

### 5.2 Probe and init

1. Board supplies/validates external 24 MHz XCLK assumption；不配置不存在的 XCLK GPIO。
2. initialize I2C0 bus。
3. read `0x3107` and `0x3108`, compare to `0xcb3a`。
4. software reset and required delay。
5. write full official 720p30 table。
6. force standby。

### 5.3 Sensor operations

- `is_available`: read and compare PID。
- `init`: idempotent mode programming。
- `uninit`: stream off and release owned I2C reference。
- `validate_frame_setting`: accept only video stream, one 1280×720 experimental RAW token, interval 1/30。
- `start_capture`: validate then write stream on。
- `stop_capture`: write stream off。
- `get_frame_interval`: return 1/30。
- unsupported controls return `-ENOTTY` through null ops。

## 6. Registration and startup

Board bringup calls `board_sc2336_initialize()` under experimental config. It obtains the CSI lower-half, then registers `imgdata`, SC2336 `imgsensor`, and `/dev/video0`. Registration is single-shot and reports negative errno without disturbing other bringup devices。

Capture upper-half ordering is retained：set buffer → arm CSI transport → stream sensor。Stopping follows upper-half ordering with sensor standby and transport stop。

## 7. Resource conflict design

`ESPRESSIF_MIPI_CSI` depends on `!ESPRESSIF_MIPI_DSI`. Although DMA channels differ, both current lower-halves attach the single DW-GDMA peripheral IRQ and cannot safely coexist without a shared dispatcher. The build includes `dw_gdma_hal.c` once under an OR condition。

## 8. Error handling

- API validation: `-EINVAL`。
- unsupported mode/format: `-ENOTSUP`。
- no sensor/bus: `-ENODEV`。
- already capturing/resource occupied: `-EBUSY`。
- I2C/HAL/IRQ failures propagate as negative errno。
- Every acquisition has reverse-order cleanup。

## 9. Static verification plan

Because compilation is explicitly prohibited for this task, validation consists only of:

- inspect `git diff --check` and scoped diffs；
- grep all declarations/definitions/build references；
- compare register table with official source；
- inspect API signatures against current NuttX headers and ESP-IDF v6.0.2 HAL；
- verify no files outside overlay changed；
- explicitly report that build/runtime validation was not performed。

## 10. 可追溯性矩阵

| req 编号 | 需求描述 | 目标文件 | 目标函数/结构体 | board 层对接 | 上游适配 | 切换点 |
|---|---|---|---|---|---|---|
| R1 | CSI imgdata lower-half | `chip/esp32p4/esp_mipi_csi.c` | `g_csi_ops`, `esp_mipi_csi_initialize()` | `board_sc2336_initialize()` | `imgdata_register()` | `CONFIG_ESPRESSIF_MIPI_CSI` |
| R2 | SC2336 imgsensor lower-half | `board/.../src/esp32p4_sc2336.c` | `g_sc2336_ops`, `g_sc2336_sensor` | `board_sc2336_initialize()` | `imgsensor_register()` | `CONFIG_ESP32P4_FUNCTION_EV_BOARD_SC2336` |
| R3 | PID 校验 | `board/.../src/esp32p4_sc2336.c` | `sc2336_probe()` | I2C0 GPIO7/8 | `is_available()`/`init()` | SC2336 config guard |
| R4 | 官方模式表 | `board/.../src/sc2336_720p30_raw10.h` | `g_sc2336_720p30_raw10` | sensor init | `start_capture()` prerequisites | SC2336 config guard |
| R5 | RAW10 buffer validation | `chip/esp32p4/esp_mipi_csi.c` | `esp_csi_set_buf()`, `esp_csi_validate()` | fixed board mode | V4L2 capture buffer path | experimental token |
| R6 | DMA/IRQ/LPWORK | `chip/esp32p4/esp_mipi_csi.c` | `esp_csi_dma_start()`, `esp_csi_interrupt()`, `esp_csi_worker()` | channel 1 | capture callback | CSI config guard |
| R7 | 生命周期清理 | `chip/esp32p4/esp_mipi_csi.c` | `esp_csi_stop_capture()`, `esp_csi_uninit()` | bringup registration | imgdata lifecycle | CSI config guard |
| R8 | experimental `/dev/video0` | `board/.../src/esp32p4_sc2336.c` | `board_sc2336_initialize()` | bringup call | `capture_register()` | experimental board config |
| R9 | DSI/CSI 互斥 | `chip/esp32p4/Kconfig` | Kconfig dependency | board config | HAL source OR condition | `!ESPRESSIF_MIPI_DSI` |
| R10 | build integration | chip/board build files | conditional source lists | board target | NuttX build graph | matching CONFIG guards |
| R11 | external XCLK contract | board header and README | board constants/documentation | no GPIO configured | sensor startup contract | SC2336 config guard |
| R12 | RAW/ISP limitation | Kconfig and README | help/documentation | experimental enable | no false V4L2 format claim | experimental board config |
