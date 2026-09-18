# ESP32-P4 MIPI CSI / SC2336 驱动需求

## 1. Datasheet 与参考实现

### 1.1 工作原理

SC2336 通过 SCCB/I2C 配置，输出双通道 MIPI CSI-2、BGGR Bayer RAW10 数据。ESP32-P4 MIPI CSI PHY/Host 接收数据，CSI bridge 将数据送入 DW-GDMA，再写入调用者提供的帧缓冲区。本实现只负责 RAW10 transport，不实现 ISP、去马赛克或颜色转换。

### 1.2 寄存器介绍

| 名称 | 地址 | 值/用途 |
|---|---:|---|
| Software reset | `0x0103` | 写 `0x01` 复位 |
| Sleep/stream | `0x0100` | `0x00` standby，`0x01` stream |
| Product ID high | `0x3107` | PID 高字节，期望 `0xcb` |
| Product ID low | `0x3108` | PID 低字节，期望 `0x3a` |
| Output width | `0x3208`/`0x3209` | `0x0500`，1280 pixels |
| Output height | `0x320a`/`0x320b` | `0x02d0`，720 lines |

完整模式表必须逐项采用 Espressif `esp-video-components` 中 Apache-2.0 授权的 `sc2336_mipi_2lane_24Minput_1280x720_raw10_30fps`，不得推测或缩减。

### 1.3 工作模式

- 初始化：software reset → standby → 写入官方 720p30 RAW10 模式表 → 保持 standby。
- 采集：CSI/DMA 就绪后写 `0x0100 = 0x01`。
- 停止：先写 `0x0100 = 0x00`，再停止 CSI bridge 和 DMA。
- 当前仅支持固定模式 `1280x720 @ 30 fps, RAW10, BGGR, 2 lanes, 405 Mbps/lane`。

### 1.4 低功耗

停止采集时将 SC2336 置于 standby，并停用 CSI bridge/DMA。MIPI PHY LDO3 与 DSI 共用，驱动不得在无法确认其他使用者状态时关闭共享电源轨。

### 1.5 代码流程

1. 初始化 I2C0（SDA GPIO7，SCL GPIO8，400 kHz）。
2. 确认 `0x3107:0x3108 == 0xcb3a`。
3. 使用权威模式表配置 SC2336。
4. 初始化 2-lane CSI PHY/Host/bridge，lane rate 405 Mbps。
5. 检查帧缓冲区地址、容量、DMA 对齐和 PSRAM/cache 条件。
6. 配置 DW-GDMA channel 1，从固定的 CSI bridge memory address 向递增内存地址传输。
7. DMA ISR 仅清状态并投递 LPWORK；worker 完成 cache 同步并通知 NuttX capture upper-half。
8. 停止/反初始化时按相反顺序清理，并同步取消 worker。

## 2. 架构与接口

### 2.1 架构选择

- CSI transport：NuttX `struct imgdata_s` lower-half。
- SC2336 sensor：NuttX `struct imgsensor_s` lower-half。
- Board registration：依次调用 `imgdata_register()`、`imgsensor_register()`、`capture_register("/dev/video0", ...)`。
- `/dev/video0` 由独立 experimental Kconfig 门控。

### 2.2 接口规格

- Sensor control：I2C0，7-bit address `0x30`，SDA GPIO7，SCL GPIO8，400 kHz。
- Sensor input clock：24 MHz。官方 Function EV Board BSP 将 camera XCLK 标记为 `GPIO_NUM_NC`；因此该板型假定摄像头模块提供板外/模块内 24 MHz 时钟，驱动不得虚构 SoC XCLK GPIO。初始化必须明确校验该固定板级假设。
- CSI：2 data lanes，405 Mbps/lane，RAW10，BGGR，1280×720，30 fps。
- RAW10 packed payload：`1280 * 720 * 10 / 8 = 1,152,000 bytes/frame`。
- DMA：DW-GDMA channel 1；source fixed，destination increment，64-bit transfer width，hardware handshake，source flow control。

### 2.3 RAW upper-half 限制

当前 NuttX `imgdata`/`imgsensor` 枚举及 V4L2 capture fourcc 映射没有 Bayer RAW10。overlay-only 实现不得修改 NuttX 公共头文件，也不得把 RAW10 谎报为 RGB565。驱动须：

- 将 `/dev/video0` 标记为 experimental；
- 使用明确命名的内部 experimental format token，仅用于打通现有 upper/lower-half 调用链；
- 在 Kconfig、README 和日志中声明输出仍是 packed BGGR RAW10；
- 不承诺标准 V4L2 RAW fourcc ABI；标准应用兼容需后续在上游 NuttX 增加 RAW10/Bayer format mapping；
- 不实现或声称存在 ISP。

### 2.4 DSI 并存边界

现有 DSI 与 CSI 均依赖同一 DW-GDMA interrupt source，且现有 DSI 实现独占 IRQ handler。首版安全实现必须在 Kconfig 中令 CSI capture 与 MIPI DSI framebuffer 互斥；CSI 使用 channel 1、DSI 使用 channel 0，但仅通道分离不足以解决共享 IRQ handler ownership。HAL build 文件须以 DSI-or-CSI 条件仅编译一次 `dw_gdma_hal.c`。

## 3. 功能 Checklist

- [x] SC2336 PID `0xcb3a` 校验
- [x] 官方 1280×720 RAW10 30 fps 初始化表
- [x] Sensor standby / stream on / stream off
- [x] 固定帧间隔 30 fps
- [x] 2-lane CSI PHY/Host/bridge 初始化
- [x] 405 Mbps/lane 配置
- [x] RAW10 frame-size 和 buffer validation
- [x] DW-GDMA channel 1 descriptor/setup/start/stop
- [x] DMA completion ISR → LPWORK callback
- [x] PSRAM address check 与 cache synchronization
- [x] stop/uninit 资源对称清理
- [x] `imgdata_s` 与 `imgsensor_s` 注册
- [x] experimental `/dev/video0` 注册
- [x] Kconfig、Make.defs、CMake、HAL 和 bringup 集成
- [x] DSI/CSI 共享 IRQ 冲突的 Kconfig 互斥保护
- [ ] ISP / demosaic / RGB 转换（不在本次范围）
- [ ] 标准 V4L2 Bayer RAW10 fourcc（需要修改上游 NuttX，不在 overlay-only 范围）

## 4. 实现约束

1. 只修改 `/home/lxy/openvela/contest2026_128_xiaolidianzishiyanshi`。
2. 不修改 NuttX、vendor、ESP-IDF、`cmake_out` 或 manifest。
3. 保留 ES8311、XiaoZhi、DSI、touchscreen 及其他现有未提交改动，只做最小增量。
4. 本轮禁止编译、构建、链接、测试编译和 reconfigure；仅允许静态 diff、grep、API 与格式人工检查。
5. 寄存器和时序以 Espressif 官方 SC2336 实现为准，并保留 Apache-2.0 attribution。
6. DMA buffer 必须满足 64-bit transfer、cache-line 和外部 RAM DMA 可达性要求。
7. IRQ map/attach 成功后才能 enable；ISR 不执行 capture callback。
8. `stop_capture()`/`uninit()` 必须使用同步 worker cancellation，避免释放后回调。
9. 所有失败路径返回负 errno，并保持资源获取/释放对称。
10. Sensor stream start 必须晚于 CSI/DMA arm；stop 必须先停 sensor stream，再停 transport。
11. 不得伪造 XCLK GPIO；Function EV Board 的 `GPIO_NUM_NC` 事实必须在代码和 README 中明确。
12. 不得将 packed RAW10 描述、上报或解释为 RGB565。

## 5. 性能指标与参考驱动

- 分辨率：1280×720。
- 帧率：30 fps，固定 interval `1/30 s`。
- 数据：BGGR RAW10 packed，1,152,000 bytes/frame。
- CSI：2 lanes × 405 Mbps/lane。
- XCLK：sensor 所需 24 MHz，由 Function EV Board 摄像头模块/外部时钟提供。
- 芯片参考：Espressif `esp-video-components/esp_cam_sensor/sensors/sc2336`。
- CSI 寄存器/时序参考：ESP-IDF v6.0.2 CSI controller、`mipi_csi_hal`、ESP32-P4 CSI peripheral table。
- NuttX 骨架：现有 `imgdata`/`imgsensor` lower-half 与 `v4l2_cap` registration；DMA/cache/HAL风格参考 overlay `esp_mipi_dsi.c`。

## 6. 目标产物

- `chip/esp32p4/esp_mipi_csi.c`
- `chip/esp32p4/esp_mipi_csi.h`
- Board-level SC2336 sensor/registration source and authoritative register table
- Chip/board Kconfig、Make.defs、CMakeLists、HAL source integration
- Board header、private header、bringup integration
- `README_sc2336.md`

需求锚点：在不越过 overlay 边界、不伪装 RAW 格式、不虚构板级时钟连线的前提下，完整实现可供后续硬件验证的 CSI transport、SC2336 sensor lower-half 和 experimental capture registration。
