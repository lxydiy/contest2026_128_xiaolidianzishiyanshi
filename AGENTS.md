# ESP32-P4 开发会话说明

本文件只记录工程位置与长期开发规范。具体功能需求以用户在当前会话中的说明为准，不在此文件中固化。

## 工程与构建位置

- Vendor Overlay 工程根目录：`/home/lxy/openvela/contest2026_128_xiaolidianzishiyanshi`
- OpenVela 工作区根目录：`/home/lxy/openvela`
- NuttX 源码：`/home/lxy/openvela/nuttx`
- Overlay 内容会软链接到：`/home/lxy/openvela/vendor/espressif`
- 当前 CMake 构建目录：`/home/lxy/openvela/cmake_out/esp32p4-function-ev-board_nsh`
- 当前 defconfig：`/home/lxy/openvela/contest2026_128_xiaolidianzishiyanshi/board/esp32p4/esp32p4-function-ev-board/configs/nsh/defconfig`
- 当前构建配置：`/home/lxy/openvela/cmake_out/esp32p4-function-ev-board_nsh/.config`

## SDMMC 相关代码与参考位置

- Overlay 驱动实现：`/home/lxy/openvela/contest2026_128_xiaolidianzishiyanshi/chip/esp32p4/esp32p4_sdmmc.c`
- Overlay 驱动头文件：`/home/lxy/openvela/contest2026_128_xiaolidianzishiyanshi/chip/esp32p4/esp32p4_sdmmc.h`
- ESP-IDF SDMMC 参考驱动：`/home/lxy/openvela/esp_driver_sdmmc`
- ESP32-P4 LL 头文件：`/home/lxy/openvela/cmake_out/esp32p4-function-ev-board_nsh/arch/risc-v/src/common/espressif/esp-hal-3rdparty/components/esp_hal_sd/esp32p4/include/hal/sdmmc_ll.h`
- 共享 HAL 实现：`/home/lxy/openvela/cmake_out/esp32p4-function-ev-board_nsh/arch/risc-v/src/common/espressif/esp-hal-3rdparty/components/esp_hal_sd/sdmmc_hal.c`
- 共享 HAL 头文件：`/home/lxy/openvela/cmake_out/esp32p4-function-ev-board_nsh/arch/risc-v/src/common/espressif/esp-hal-3rdparty/components/esp_hal_sd/include/hal/sdmmc_hal.h`
- ESP32-P4 外设数据：`/home/lxy/openvela/cmake_out/esp32p4-function-ev-board_nsh/arch/risc-v/src/common/espressif/esp-hal-3rdparty/components/esp_hal_sd/esp32p4/sdmmc_periph.c`
- ESP32-S3 BSP 对比参考：`/home/lxy/openvela/nuttx/arch/xtensa/src/esp32s3`

## 长期开发规范

- 只能修改 Vendor Overlay 工程根目录内的文件，不得直接修改 `nuttx`、`vendor`、依赖源码或其他公共工程。
- 如果仅修改 Overlay 无法完成工作，或者必须引入 workaround，应立即停止并向用户说明原因，不得自行扩大修改范围。
- 构建系统只考虑 CMake，不维护传统 Makefile 路径。
- 构建依赖由构建系统自动拉取到上述 CMake 构建目录；其头文件目录会自动加入，可以直接包含依赖头文件。
- 每次开始配置、编译或调试前，重新读取当前 defconfig、生成的 `.config` 以及相关 Kconfig；用户可能已通过 menuconfig 调整配置，不得沿用旧会话中的配置假设。
- 后续配置、编译和烧录统一使用 `configs/nsh/defconfig` 及其对应的 `cmake_out/esp32p4-function-ev-board_nsh/.config`，并以生成的 `.config` 为当前配置的最高优先依据。
- 除非用户明确指定，否则不得尝试修改、替换或覆盖 defconfig。
- 编译和烧录使用 `ESPTOOL_PORT=/dev/ttyACM0 cmake --build cmake_out/esp32p4-function-ev-board_nsh --target flash`；需要重新配置时使用 `./build.sh vendor/espressif/boards/esp32p4/esp32p4-function-ev-board/configs/nsh/ --cmake`。
- SDMMC slot 0 连接 TF 卡槽，slot 1 连接 ESP-Hosted；当前阶段只调试 slot 0，不改动或联调 slot 1，除非用户另行指定。
- 寄存器操作优先使用 ESP32-P4 的 `sdmmc_ll_*` LL 接口，不直接写寄存器地址。
- ESP-IDF 中依赖 FreeRTOS 的机制必须改为对应的 NuttX 信号量、工作队列和中断机制。
- ESP32-S3 代码仅供对比，不得直接照搬；实现依据应是 ESP32-P4 的 LL/HAL、外设数据和 RISC-V 平台代码。
- 删除确实未使用的函数，不保留无意义的空桩。
- 遵循 NuttX 编码规范、命名约定和 Apache 2.0 许可证头要求。
- 可以联网查阅资料；技术结论优先采用官方源码、官方文档和芯片厂商资料。
