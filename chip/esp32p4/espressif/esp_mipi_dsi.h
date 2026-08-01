/****************************************************************************
 * arch/risc-v/src/esp32p4/esp_mipi_dsi.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __ARCH_RISCV_SRC_ESP32P4_ESP_MIPI_DSI_H
#define __ARCH_RISCV_SRC_ESP32P4_ESP_MIPI_DSI_H

#include <nuttx/config.h>

#ifdef CONFIG_ESPRESSIF_MIPI_DSI
int esp_mipi_dsi_panel_initialize(void);
#endif

#endif
