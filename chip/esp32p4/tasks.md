# ESP32-P4 MIPI CSI / SC2336 实现任务

## CSI lower-half

- [ ] 新建 `esp_mipi_csi.h`，定义固定模式、RAW10 payload 和初始化 API
- [ ] 新建 `esp_mipi_csi.c`，实现 `struct imgdata_s`
- [ ] 初始化 MIPI LDO3、CSI clocks、PHY/host/bridge
- [ ] 配置 DW-GDMA channel 1
- [ ] 实现 buffer validation、cache/PSRAM checks
- [ ] 实现 start/stop 和 one-shot frame DMA
- [ ] 实现 DMA ISR → LPWORK completion
- [ ] 实现同步取消 worker 和逆序 cleanup

## SC2336 sensor lower-half

- [ ] 导入官方 Apache-2.0 720p30 RAW10寄存器表
- [ ] 实现 16-bit-address I2C read/write
- [ ] 实现 PID `0xcb3a` probe
- [ ] 实现 init/standby/stream on/off
- [ ] 实现固定 format/size/interval validation
- [ ] 明确外部 24 MHz XCLK 契约，不配置虚构 GPIO
- [ ] 注册 `imgdata_s`、`imgsensor_s` 和 experimental `/dev/video0`

## Integration

- [ ] 增加 chip CSI Kconfig 和 DSI互斥约束
- [ ] 更新 chip Make.defs/CMakeLists
- [ ] 更新 HAL Make/CMake，确保 DW-GDMA source只加入一次
- [ ] 增加 board SC2336 experimental Kconfig
- [ ] 更新 board header/private header
- [ ] 更新 board Make.defs/CMakeLists
- [ ] 在 bringup 中追加 camera registration
- [ ] 生成 `README_sc2336.md`

## Static validation only

- [ ] 核对 NuttX imgdata/imgsensor/capture API签名
- [ ] 核对 ESP-IDF v6.0.2 CSI/DW-GDMA HAL API
- [ ] 核对官方 SC2336寄存器表完整性
- [ ] grep声明、定义、CONFIG和build引用
- [ ] 运行 scoped `git diff --check`
- [ ] 确认 overlay 外无修改
- [ ] 明确记录未编译、未构建、未链接、未reconfigure
