# ESP32-P4X Function EV Board MIPI-DSI 移植记录

## 1. 当前状态

本文记录 openvela/NuttX 在 ESP32-P4X Function EV Board 上适配 MIPI-DSI
显示的实现和真机调试结论。截至当前：

- NuttX 可正常启动，UART0/NSH 可持续输入；
- MIPI D-PHY PLL 可锁定，两条 data lane 可进入 stop state 和 HS 传输；
- EK79007 初始化命令已改为 LPDT 发送，面板开始扫描；
- `/dev/fb0` 可注册，framebuffer 示例可写 PSRAM；
- DSI Host requested/active timing、packet size 和 RGB888 编码一致；
- 当前为 Host 内建竖向彩条诊断模式，屏幕左缘可见一列短彩线，但尚未铺满屏幕；
- 已增加 `COLMOD (0x3A)=0x77`，等待真机验证其能否修复有效像素区；
- framebuffer 连续 DMA 暂未恢复，当前 `/dev/fb0` 内容不会送往屏幕。

这意味着供电、时钟、DCS LP 链路和基本视频扫描已打通；剩余问题集中在面板
像素解释/视频有效数据，以及后续 DW-GDMA 连续刷新。

## 2. 实际目标硬件

| 项目 | 实际配置 |
| --- | --- |
| 开发板 | ESP32-P4X-Function-EV-Board |
| Chip on board | ESP32-P4NRW32X |
| Sample chip revision | v3.2 |
| ROM | `esp32p4-eco7-20260109` |
| 显示模组 | 乐鑫官方 7 英寸 1024×600 LCD adapter 套件 |
| 面板控制器 | EK79007 |
| DSI data lanes | 2 |
| Lane bit rate | 1000 Mbps/lane |
| D-PHY 电源 | 内部 LDO channel 3，2.5 V |
| LCD reset | GPIO27 → `RST_LCD` |
| LCD backlight | GPIO26 → `PWM` |
| 控制台 | UART0，GPIO37 TX / GPIO38 RX，115200 8N1 |

硬件连接已经复查：RST_LCD 跳变正常。P4X 官方要求 DSI FPC 反向插接，LCD
adapter 通过自身 USB 或可靠的 5V/GND 供电。

## 3. 当前显示参数

参数已按 Espressif 官方 EK79007 示例修正：

| 项目 | 数值 |
| --- | ---: |
| Pixel format | RGB888 / 24 bpp |
| DPI clock | 48 MHz（REF_240M / 5） |
| HACTIVE | 1024 |
| HSYNC / HBP / HFP | 10 / 120 / 120 |
| VACTIVE | 600 |
| VSYNC / VBP / VFP | 1 / 20 / 10 |
| Frame rate | 约 60 Hz |
| Framebuffer stride | 3072 bytes |
| Framebuffer size | 1,843,200 bytes |
| Framebuffer memory | 外部 PSRAM BSS，64-byte aligned |

早期 RGB565、52 MHz、H porch 160/160、V porch 23/12 的配置已经废弃。

## 4. 代码结构

核心文件：

```text
chip/esp32p4/espressif/esp_mipi_dsi.c
chip/esp32p4/espressif/esp_mipi_dsi.h
chip/esp32p4/espressif/CMakeLists.txt
chip/esp32p4/espressif/Make.defs
chip/esp32p4/Kconfig
chip/esp32p4/hal_esp32p4.cmake
```

板级配置和启动代码：

```text
board/esp32p4/esp32p4-function-ev-board/configs/mipi/defconfig
board/esp32p4/esp32p4-function-ev-board/src/esp32p4_bringup.c
board/esp32p4/esp32p4-function-ev-board/include/board.h
```

`esp32p4_bringup.c` 使用独立 `mipi_fb` kthread 调用 `fb_register(0, 0)`，避免
显示初始化阻塞 NSH 启动链。后台日志统一使用 `syslog()`，不与 NSH 竞争
stdio console lock。

## 5. 构建与烧写

`esp-hal-3rdparty` 必须由环境变量指定，不在仓库中硬编码内部地址：

```sh
export ESP_HAL_3RDPARTY_URL='ncepu-pi:/git/esp-hal-3rdparty.git'
export ESP_HAL_3RDPARTY_VERSION='e9a46f7d9d'

rm -rf cmake_out/esp32p4-function-ev-board_mipi
./build.sh \
  contest2026_128_xiaolidianzishiyanshi/board/esp32p4/esp32p4-function-ev-board/configs/mipi/ \
  --cmake -j32

ESPTOOL_PORT=/dev/ttyACM0 \
  cmake --build cmake_out/esp32p4-function-ev-board_mipi --target flash
```

不要选择 `Select ESP32-P4 revisions <3.0 (No >=3.x Support)`。该选项面向旧
silicon，烧入 v3.x 后曾在入口触发 Illegal instruction。更改 revision 配置后
必须删除整个构建目录重新配置。

## 6. 已解决的关键问题

### 6.1 v3.x 启动配置

错误启用 `<3.0` 支持时，ROM SHA 提示后在应用入口产生 Illegal instruction。
取消该配置并全量重建后，NuttX 正常启动。

### 6.2 控制台引脚

有效控制台为 GPIO37/38 上的 UART0。仅看到 ROM/NuttX 装载日志而看不到 NSH，
或无法输入时，应先确认串口连接和 TX/RX 交叉。

### 6.3 LDO 临界区卡死

`esp_ldo_acquire_channel()` 在当前 openvela/ESP OS 临界区组合中会卡住。当前
驱动在 NuttX 临界区内直接使用 `hal/ldo_ll.h`，配置 channel 3 为 2.5 V。

### 6.4 v3.x PHY reference clock

v3.x 必须使用 `MIPI_DSI_PHY_PLLREF_CLK_SRC_DEFAULT` 和 40 MHz reference，不能
使用 `<3.0` 的 legacy 枚举。PHY configuration clock 还必须显式打开上游
`REF_20M` gate，否则 PLL status 可读但 lock 位始终为 0。

### 6.5 DPI clock gate

DPI 默认来自 PLL_F240M。必须打开上游 `REF_240M` gate，再设置 divider=5；
只开 DSI DPI gate 会造成 framebuffer 和 DMA 初始化成功但没有 pixel clock。

### 6.6 v3.2 video shadow

P4X v3.2 存在 requested 与 active video register bank。若不显式执行：

```text
vid_shadow_en = 1
写入 timing / packet / color / mode
vid_shadow_req = 1
进入 video mode
```

则 requested register 有值而 `_act` 全为 0。修复后已测得：

```text
packet size: 1024 / 1024
H timing:    26 / 313 / 3318（requested = active）
V timing:    1 / 20 / 10 / 600（requested = active）
color:       5 / 5（RGB888）
```

### 6.7 EK79007 DCS 必须使用 LPDT

直接写 Generic FIFO 时若保留 `cmd_mode_cfg` 复位值，命令会按 HS 发送。Host
FIFO 会清空，看起来“写成功”，但 EK79007 不接受初始化序列。

当前实现按官方 `esp_lcd_panel_io_dbi` 设置：

- command ACK enabled；
- Generic short/long write/read 使用 LP；
- DCS short/long write/read 使用 LP；
- MRPS 使用 LP。

补齐后屏幕从纯黑变为左缘出现短彩线，是目前最关键的链路突破。

### 6.8 PHY 通用时序

PLL lock 后等待 clock/data lanes 全部进入 stop state，并设置：

```text
timeout count = 0
max read time = 6000
stop wait time = 0x3f
```

### 6.9 DCS read 诊断结论

曾使用 `GET_POWER_MODE (0x0A)` 验证双向链路。LP 配置前读超时；LP 配置后能
得到 payload，但返回 `00`，同时出现 `int_st0=0x00100000`，即 D-PHY LP
contention。为避免 BTA 诊断影响后续视频，当前版本已移除启动时 DCS read。

## 7. 当前 EK79007 初始化序列

硬复位 GPIO27：低 10 ms，高后等待 20 ms。随后以 LPDT 发送：

```text
B2 = 10             2 data lanes
80 = 8B
81 = 78
82 = 84
83 = 88
84 = A8
85 = E3
86 = 88
3A = 77             COLMOD: 24-bit RGB888（最新待验证项）
11                  Sleep Out
等待 120 ms
29                  Display On
等待 20 ms
```

`0x3A=0x77` 是针对“左缘短线、有效像素未铺满”的最新修正。官方组件文档说明
16/18/24 bpp 由 `3Ah` 实现，但默认 vendor command 表未实际发送该命令。

## 8. 当前诊断模式

为隔离 DMA 问题，当前版本：

- 关闭 DSI Bridge DPI output；
- 不启动 framebuffer DW-GDMA；
- 启用 DSI Host `MIPI_DSI_PATTERN_BAR_VERTICAL`；
- 使用 AUTO clock lane；
- blanking period 允许 LP；
- 关闭 per-frame ACK，避免视频依赖面板反向响应；
- `/dev/fb0` 仍注册，但运行 `fb /dev/fb0` 不会改变屏幕彩条。

正常诊断日志包括：

```text
MIPI: PHY PLL locked
MIPI: PHY lanes stopped
MIPI: DBI commands configured for LP mode
MIPI: DSI write cmd=3a result=2
MIPI: status shadow=...
MIPI: H req=... act=...
MIPI: V req=... act=... color=...
MIPI: vertical color-bar test pattern enabled
MIPI: /dev/fb0 registered
```

## 9. DMA 调试结论

`/dev/fb0` 曾使用 RGB565 成功映射 PSRAM，`fb /dev/fb0` 可写入嵌套矩形，说明
NuttX framebuffer 和 PSRAM mapping 基本正常。但显示 DMA 尚未形成稳定连续流：

- 官方 full-transfer ISR 中连续重启 DMA 时，UART RX/NSH 无法输入；
- 用 25 ms、100 ms kthread 周期重提 DMA，同样会使 NSH 停止响应；
- `dw_gdma_channel_abort()` 可能造成 AXI protocol violation，已禁止使用；
- 降低 channel priority、缩短 burst 不能解决周期重装卡死；
- 当前只保留 `FBIO_UPDATE` cache sync/单帧提交代码，诊断模式不启动它。

恢复 DMA 时应先保持彩条模式验证通过，再严格复制官方
`mipi_dsi_dma_trans_done_cb()` 的 restart 顺序，并检查 NuttX 的 ESP shared IRQ
映射和 `portYIELD_FROM_ISR()` 兼容性，不应再用轮询线程反复 stop/restart。

## 10. 下一步

1. 烧写含 `COLMOD 0x3A=0x77` 的版本，观察左缘短线是否扩展为完整竖向彩条；
2. 若仍异常，比较发送/不发送 COLMOD，并尝试面板要求的 24-bit COLMOD 值；
3. 保留 requested/active timing 日志，确认修改没有破坏 v3.2 shadow；
4. 彩条完整后关闭 VPG，恢复 DSI Bridge 和 framebuffer DMA；
5. 修复 full-transfer ISR 连续刷新与 UART RX 的中断兼容问题；
6. 最终执行 `fb /dev/fb0`，验证 RGB888 图案、颜色顺序和持续刷新；
7. 完成后再精简阶段日志，增加背光 PWM、双缓冲和 VSYNC。

## 11. 验收标准

- NuttX 和 NSH 稳定，UART 输入不受显示刷新影响；
- `/dev/fb0` 注册成功；
- EK79007 全屏显示正确的 Host 彩条；
- framebuffer RGB888 图案可完整显示；
- 连续刷新无 underrun、花屏、撕裂或 DMA 停止；
- PSRAM cache 同步和 `FBIO_UPDATE` 生效；
- 失败路径不会卡死启动链，并能关闭背光、释放显示资源。

## 12. 参考资料

- Espressif ESP32-P4X-Function-EV-Board User Guide
- ESP-IDF `components/esp_lcd/dsi/esp_lcd_mipi_dsi_bus.c`
- ESP-IDF `components/esp_lcd/dsi/esp_lcd_panel_io_dbi.c`
- ESP-IDF `components/esp_lcd/dsi/esp_lcd_panel_dpi.c`
- Espressif `esp_lcd_ek79007` component
- EK79007 controller datasheet
