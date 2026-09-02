/****************************************************************************
 * arch/xtensa/src/esp32p4/esp32p4_sdmmc.c
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <debug.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <nuttx/arch.h>
#include <nuttx/clock.h>
#include <nuttx/mmcsd.h>
#include <nuttx/sdio.h>
#include <nuttx/semaphore.h>
#include <nuttx/spinlock.h>
#include <nuttx/wdog.h>
#include <nuttx/wqueue.h>

#include <arch/chip/gpio_sig_map.h>
#include "espressif/esp_gpio.h"
#include "hal/sdmmc_ll.h"
#include "hal/sdmmc_periph.h"

#ifdef CONFIG_ESP32P4_SDMMC

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Configuration ************************************************************/

/* Required system configuration options:
 *
 *   CONFIG_ARCH_DMA - Enable architecture-specific DMA subsystem
 *     initialization.  Required if CONFIG_ESP32P4_SDMMC_DMA is enabled.
 *   CONFIG_ESP32P4_DMA2 - Enable ESP32P4 DMA2 support.  Required if
 *     CONFIG_ESP32P4_SDMMC_DMA is enabled
 *   CONFIG_SCHED_WORKQUEUE -- Callback support requires work queue support.
 *
 * Driver-specific configuration options:
 *
 *   CONFIG_SDIO_MUXBUS - Setting this configuration enables some locking
 *     APIs to manage concurrent accesses on the SDIO bus.  This is not
 *     needed for the simple case of a single SD card, for example.
 *   CONFIG_ESP32P4_SDMMC_DMA - Enable SDIO.  This is a marginally optional.
 *     For most usages, SDIO will cause data overruns if used without DMA.
 *     NOTE the above system DMA configuration options.
 *   CONFIG_SDIO_WIDTH_D1_ONLY - This may be selected to force the
 *     driver operate with only a single data line (the default is to use
 *     all 4 SD data lines).
 *   CONFIG_SDIO_XFRDEBUG - Enables some very low-level debug output
 *     This also requires CONFIG_DEBUG_FS and CONFIG_DEBUG_INFO
 */

/* Timing : 100mS short timeout, 2 seconds for long one */

#define SDCARD_CMDTIMEOUT MSEC2TICK(100)
#define SDCARD_LONGTIMEOUT MSEC2TICK(2000)

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* This structure defines the state of the ESP32P4 SDIO interface */

struct esp32p4_dev_s {
  struct sdio_dev_s dev; /* Standard, base SDIO interface */

  bool inited;
  /* ESP32P4-specific extensions */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

/* #define CONFIG_ESP32P4_SDMMC_REGDEBUG */

#ifdef CONFIG_ESP32P4_SDMMC_REGDEBUG
static uint32_t __esp32p4_getreg(const char* func, uint32_t addr);
static void __esp32p4_putreg(const char* func, uint32_t val, uint32_t addr);

#define esp32p4_getreg(addr) __esp32p4_getreg(__func__, addr)
#define esp32p4_putreg(val, addr) __esp32p4_putreg(__func__, val, addr)
#else
#define esp32p4_getreg(addr) getreg32(addr)
#define esp32p4_putreg(val, addr) putreg32(val, addr)
#endif

#ifdef CONFIG_SDIO_MUXBUS
static int esp32p4_lock(struct sdio_dev_s* dev, bool lock);
#endif

/* Initialization/setup */

static void esp32p4_reset(struct sdio_dev_s* dev);
static sdio_capset_t esp32p4_capabilities(struct sdio_dev_s* dev);
static sdio_statset_t esp32p4_status(struct sdio_dev_s* dev);
static void esp32p4_widebus(struct sdio_dev_s* dev, bool enable);
static void esp32p4_clock(struct sdio_dev_s* dev, enum sdio_clock_e rate);
static int esp32p4_attach(struct sdio_dev_s* dev);

/* Command/Status/Data Transfer */

static int esp32p4_sendcmd(struct sdio_dev_s* dev, uint32_t cmd, uint32_t arg);
#ifdef CONFIG_SDIO_BLOCKSETUP
static void esp32p4_blocksetup(struct sdio_dev_s* dev, unsigned int blocklen,
                               unsigned int nblocks);
#endif
static int esp32p4_recvsetup(struct sdio_dev_s* dev, uint8_t* buffer,
                             size_t nbytes);
static int esp32p4_sendsetup(struct sdio_dev_s* dev, const uint8_t* buffer,
                             size_t nbytes);
static int esp32p4_cancel(struct sdio_dev_s* dev);

static int esp32p4_waitresponse(struct sdio_dev_s* dev, uint32_t cmd);
static int esp32p4_recvshortcrc(struct sdio_dev_s* dev, uint32_t cmd,
                                uint32_t* rshort);
static int esp32p4_recvlong(struct sdio_dev_s* dev, uint32_t cmd,
                            uint32_t rlong[4]);
static int esp32p4_recvshort(struct sdio_dev_s* dev, uint32_t cmd,
                             uint32_t* rshort);

/* EVENT handler */

static void esp32p4_waitenable(struct sdio_dev_s* dev, sdio_eventset_t eventset,
                               uint32_t timeout);
static sdio_eventset_t esp32p4_eventwait(struct sdio_dev_s* dev);
static void esp32p4_callbackenable(struct sdio_dev_s* dev,
                                   sdio_eventset_t eventset);
static int esp32p4_registercallback(struct sdio_dev_s* dev, worker_t callback,
                                    void* arg);

/* Initialization/uninitialization/reset ************************************/

static void esp32p4_callback(void* arg);

/****************************************************************************
 * Private Data
 ****************************************************************************/

struct esp32p4_dev_s g_sdiodev = {
  .dev =
    {
#ifdef CONFIG_SDIO_MUXBUS
      .lock = esp32p4_lock,
#endif
      .reset = esp32p4_reset,
      .capabilities = esp32p4_capabilities,
      .status = esp32p4_status,
      .widebus = esp32p4_widebus,
      .clock = esp32p4_clock,
      .attach = esp32p4_attach,
      .sendcmd = esp32p4_sendcmd,
#ifdef CONFIG_SDIO_BLOCKSETUP
      .blocksetup = esp32p4_blocksetup,
#endif
      .recvsetup = esp32p4_recvsetup,
      .sendsetup = esp32p4_sendsetup,
      .cancel = esp32p4_cancel,
      .waitresponse = esp32p4_waitresponse,
      .recv_r1 = esp32p4_recvshortcrc,
      .recv_r2 = esp32p4_recvlong,
      .recv_r3 = esp32p4_recvshort,
      .recv_r4 = esp32p4_recvshort,
      .recv_r5 = esp32p4_recvshortcrc,
      .recv_r6 = esp32p4_recvshortcrc,
      .recv_r7 = esp32p4_recvshort,
      .waitenable = esp32p4_waitenable,
      .eventwait = esp32p4_eventwait,
      .callbackenable = esp32p4_callbackenable,
      .registercallback = esp32p4_registercallback,
    },
  .inited = false,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: esp32p4_getreg
 *
 * Description:
 *   This function may to used to intercept an monitor all register accesses.
 *   Clearly this is nothing you would want to do unless you are debugging
 *   this driver.
 *
 * Input Parameters:
 *   addr - The register address to read
 *
 * Returned Value:
 *   The value read from the register
 *
 ****************************************************************************/

#ifdef CONFIG_ESP32P4_SDMMC_REGDEBUG
static uint32_t __esp32p4_getreg(const char* func, uint32_t addr) {
  static uint32_t prevaddr = 0;
  static uint32_t preval = 0;
  static uint32_t count = 0;

  /* Read the value from the register */

  uint32_t val = getreg32(addr);

  /* Is this the same value that we read from the same register last time?
   * Are we polling the register?  If so, suppress some of the output.
   */

  if (addr == prevaddr && val == preval) {
    if (count == 0xffffffff || ++count > 3) {
      if (count == 4) {
        mcerr("%s: ...\n", func);
      }

      return val;
    }
  }

  /* No this is a new address or value */

  else {
    /* Did we print "..." for the previous value? */

    if (count > 3) {
      /* Yes.. then show how many times the value repeated */

      mcerr("%s: [repeats %d more times]\n", func, count - 3);
    }

    /* Save the new address, value, and count */

    prevaddr = addr;
    preval = val;
    count = 1;
  }

  /* Show the register value read */

  mcerr("%s: %08x->%08x\n", func, addr, val);
  return val;
}
#endif

/****************************************************************************
 * Name: esp32p4_putreg
 *
 * Description:
 *   This function may to used to intercept an monitor all register accesses.
 *   Clearly this is nothing you would want to do unless you are debugging
 *   this driver.
 *
 * Input Parameters:
 *   val - The value to write to the register
 *   addr - The register address to read
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

#ifdef CONFIG_ESP32P4_SDMMC_REGDEBUG
static void __esp32p4_putreg(const char* func, uint32_t val, uint32_t addr) {
  /* Show the register value being written */

  mcerr("%s: %08x<-%08x\n", func, addr, val);

  /* Write the value */

  putreg32(val, addr);
}
#endif

/****************************************************************************
 * Name: esp32p4_lock
 *
 * Description:
 *   Locks the bus. Function calls low-level multiplexed bus routines to
 *   resolve bus requests and acknowledgment issues.
 *
 * Input Parameters:
 *   dev    - An instance of the SDIO device interface
 *   lock   - TRUE to lock, FALSE to unlock.
 *
 * Returned Value:
 *   OK on success; a negated errno on failure
 *
 ****************************************************************************/

#ifdef CONFIG_SDIO_MUXBUS
static int esp32p4_lock(struct sdio_dev_s* dev, bool lock) {
  /* Single SDIO instance so there is only one possibility.  The multiplex
   * bus is part of board support package.
   */

  /* FIXME: Implement the below function to support bus share:
   *
   * esp32p4_muxbus_sdio_lock(lock);
   */

  return OK;
}
#endif

/****************************************************************************
 * Name: esp32p4_reset
 *
 * Description:
 *   Reset the SDIO controller.  Undo all setup and initialization.
 *
 * Input Parameters:
 *   dev    - An instance of the SDIO device interface
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

static void esp32p4_reset(struct sdio_dev_s* dev) {}

/****************************************************************************
 * Name: esp32p4_capabilities
 *
 * Description:
 *   Get capabilities (and limitations) of the SDIO driver (optional)
 *
 * Input Parameters:
 *   dev   - Device-specific state data
 *
 * Returned Value:
 *   Returns a bitset of status values (see SDIO_CAPS_* defines)
 *
 ****************************************************************************/

static sdio_capset_t esp32p4_capabilities(struct sdio_dev_s* dev) {}

/****************************************************************************
 * Name: esp32p4_status
 *
 * Description:
 *   Get SDIO status.
 *
 * Input Parameters:
 *   dev   - Device-specific state data
 *
 * Returned Value:
 *   Returns a bitset of status values (see esp32p4_status_* defines)
 *
 ****************************************************************************/

static sdio_statset_t esp32p4_status(struct sdio_dev_s* dev) {}

/****************************************************************************
 * Name: esp32p4_widebus
 *
 * Description:
 *   Called after change in Bus width has been selected (via ACMD6).  Most
 *   controllers will need to perform some special operations to work
 *   correctly in the new bus mode.
 *
 * Input Parameters:
 *   dev  - An instance of the SDIO device interface
 *   wide - true: wide bus (4-bit) bus mode enabled
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

static void esp32p4_widebus(struct sdio_dev_s* dev, bool wide) {}

/****************************************************************************
 * Name: esp32p4_clock
 *
 * Description:
 *   Enable/disable SDIO clocking
 *
 * Input Parameters:
 *   dev  - An instance of the SDIO device interface
 *   rate - Specifies the clocking to use (see enum sdio_clock_e)
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

static void esp32p4_clock(struct sdio_dev_s* dev, enum sdio_clock_e rate) {}

/****************************************************************************
 * Name: esp32p4_attach
 *
 * Description:
 *   Attach and prepare interrupts
 *
 * Input Parameters:
 *   dev - An instance of the SDIO device interface
 *
 * Returned Value:
 *   OK on success; A negated errno on failure.
 *
 ****************************************************************************/

static int esp32p4_attach(struct sdio_dev_s* dev) {}

/****************************************************************************
 * Name: esp32p4_sendcmd
 *
 * Description:
 *   Send the SDIO command
 *
 * Input Parameters:
 *   dev  - An instance of the SDIO device interface
 *   cmd  - The command to send (32-bits, encoded)
 *   arg  - 32-bit argument required with some commands
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

static int esp32p4_sendcmd(struct sdio_dev_s* dev, uint32_t cmd, uint32_t arg) {
}

/****************************************************************************
 * Name: esp32p4_blocksetup
 *
 * Description:
 *   Configure block size and the number of blocks for next transfer
 *
 * Input Parameters:
 *   dev       - An instance of the SDIO device interface
 *   blocklen  - The selected block size.
 *   nblocklen - The number of blocks to transfer
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

#ifdef CONFIG_SDIO_BLOCKSETUP
static void esp32p4_blocksetup(struct sdio_dev_s* dev, unsigned int blocklen,
                               unsigned int nblocks) {}
#endif

/****************************************************************************
 * Name: esp32p4_recvsetup
 *
 * Description:
 *   Setup hardware in preparation for data transfer from the card in non-DMA
 *   (interrupt driven mode).  This method will do whatever controller setup
 *   is necessary.  This would be called for SD memory just BEFORE sending
 *   CMD13 (SEND_STATUS), CMD17 (READ_SINGLE_BLOCK), CMD18
 *   (READ_MULTIPLE_BLOCKS), ACMD51 (SEND_SCR), etc.  Normally,
 *   SDIO_WAITEVENT will be called to receive the indication that the
 *   transfer is complete.
 *
 * Input Parameters:
 *   dev    - An instance of the SDIO device interface
 *   buffer - Address of the buffer in which to receive the data
 *   nbytes - The number of bytes in the transfer
 *
 * Returned Value:
 *   Number of bytes sent on success; a negated errno on failure
 *
 ****************************************************************************/

static int esp32p4_recvsetup(struct sdio_dev_s* dev, uint8_t* buffer,
                             size_t nbytes) {
  return OK;
}

/****************************************************************************
 * Name: esp32p4_sendsetup
 *
 * Description:
 *   Setup hardware in preparation for data transfer from the card.  This
 *   method will do whatever controller setup is necessary.  This would be
 *   called for SD memory just AFTER sending CMD24 (WRITE_BLOCK), CMD25
 *   (WRITE_MULTIPLE_BLOCK), ... and before SDIO_SENDDATA is called.
 *
 * Input Parameters:
 *   dev    - An instance of the SDIO device interface
 *   buffer - Address of the buffer containing the data to send
 *   nbytes - The number of bytes in the transfer
 *
 * Returned Value:
 *   Number of bytes sent on success; a negated errno on failure
 *
 ****************************************************************************/

static int esp32p4_sendsetup(struct sdio_dev_s* dev, const uint8_t* buffer,
                             size_t nbytes) {
  return OK;
}

/****************************************************************************
 * Name: esp32p4_cancel
 *
 * Description:
 *   Cancel the data transfer setup of SDIO_RECVSETUP, SDIO_SENDSETUP,
 *   SDIO_DMARECVSETUP or SDIO_DMASENDSETUP.  This must be called to cancel
 *   the data transfer setup if, for some reason, you cannot perform the
 *   transfer.
 *
 * Input Parameters:
 *   dev  - An instance of the SDIO device interface
 *
 * Returned Value:
 *   OK is success; a negated errno on failure
 *
 ****************************************************************************/

static int esp32p4_cancel(struct sdio_dev_s* dev) {
  return OK;
}

/****************************************************************************
 * Name: esp32p4_waitresponse
 *
 * Description:
 *   Poll-wait for the response to the last command to be ready.  This
 *   function should be called even after sending commands that have no
 *   response (such as CMD0) to make sure that the hardware is ready to
 *   receive the next command.
 *
 * Input Parameters:
 *   dev  - An instance of the SDIO device interface
 *   cmd  - The command that was sent. See 32-bit command definitions above.
 *
 * Returned Value:
 *   OK is success; a negated errno on failure
 *
 ****************************************************************************/

static int esp32p4_waitresponse(struct sdio_dev_s* dev, uint32_t cmd) {
  return OK;
}

/****************************************************************************
 * Name: esp32p4_recvshortcrc
 *
 * Description:
 *   Receive response to SDIO command.  Only the critical payload is
 *   returned -- 32 bits for 48 bit status.  The driver implementation
 *   verifies the correctness of the remaining, non-returned bits (CRCs, CMD
 *   index, etc.).
 *
 * Input Parameters:
 *   dev  - An instance of the SDIO device interface
 *   Rx   - Buffer in which to receive the response
 *
 * Returned Value:
 *   Number of bytes sent on success; a negated errno on failure.  Here a
 *   failure means only a faiure to obtain the requested response (due to
 *   transport problem -- timeout, CRC, etc.).  The implementation only
 *   assures that the response is returned intacta and does not check errors
 *   within the response itself.
 *
 ****************************************************************************/

static int esp32p4_recvshortcrc(struct sdio_dev_s* dev, uint32_t cmd,
                                uint32_t* rshort) {
  return OK;
}

/****************************************************************************
 * Name: esp32p4_recvlong
 *
 * Description:
 *   Receive response to SDIO command.  Only the critical payload is
 *   returned -- 128 bits for 136 bit status.  The driver implementation
 *   verifies the correctness of the remaining, non-returned bits (CRCs, CMD
 *   index, etc.).
 *
 * Input Parameters:
 *   dev  - An instance of the SDIO device interface
 *   Rx   - Buffer in which to receive the response
 *
 * Returned Value:
 *   Number of bytes sent on success; a negated errno on failure.  Here a
 *   failure means only a faiure to obtain the requested response (due to
 *   transport problem -- timeout, CRC, etc.).  The implementation only
 *   assures that the response is returned intacta and does not check errors
 *   within the response itself.
 *
 ****************************************************************************/

static int esp32p4_recvlong(struct sdio_dev_s* dev, uint32_t cmd,
                            uint32_t rlong[4]) {
  return OK;
}

/****************************************************************************
 * Name: esp32p4_recvshort
 *
 * Description:
 *   Receive response to SDIO command.  Only the critical payload is
 *   returned -- 32 bits for 48 bit status.  The driver implementation
 *   verifies the correctness of the remaining, non-returned bits (CMD
 *   index, etc., not including CRC).
 *
 * Input Parameters:
 *   dev  - An instance of the SDIO device interface
 *   Rx   - Buffer in which to receive the response
 *
 * Returned Value:
 *   Number of bytes sent on success; a negated errno on failure.  Here a
 *   failure means only a faiure to obtain the requested response (due to
 *   transport problem -- timeout, CRC, etc.).  The implementation only
 *   assures that the response is returned intacta and does not check errors
 *   within the response itself.
 *
 ****************************************************************************/

static int esp32p4_recvshort(struct sdio_dev_s* dev, uint32_t cmd,
                             uint32_t* rshort) {
  return OK;
}

/****************************************************************************
 * Name: esp32p4_waitenable
 *
 * Description:
 *   Enable/disable of a set of SDIO wait events.  This is part of the
 *   the SDIO_WAITEVENT sequence.  The set of to-be-waited-for events is
 *   configured before calling esp32p4_eventwait.  This is done in this way
 *   to help the driver to eliminate race conditions between the command
 *   setup and the subsequent events.
 *
 *   The enabled events persist until either (1) SDIO_WAITENABLE is called
 *   again specifying a different set of wait events, or (2) SDIO_EVENTWAIT
 *   returns.
 *
 * Input Parameters:
 *   dev      - An instance of the SDIO device interface
 *   eventset - A bitset of events to enable or disable (see SDIOWAIT_*
 *              definitions). 0=disable; 1=enable.
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

static void esp32p4_waitenable(struct sdio_dev_s* dev, sdio_eventset_t eventset,
                               uint32_t timeout) {}

/****************************************************************************
 * Name: esp32p4_eventwait
 *
 * Description:
 *   Wait for one of the enabled events to occur (or a timeout).  Note that
 *   all events enabled by SDIO_WAITEVENTS are disabled when
 *   esp32p4_eventwait returns.  SDIO_WAITEVENTS must be called again before
 *   esp32p4_eventwait can be used again.
 *
 * Input Parameters:
 *   dev     - An instance of the SDIO device interface
 *   timeout - Maximum time in milliseconds to wait.  Zero means immediate
 *             timeout with no wait.  The timeout value is ignored if
 *             SDIOWAIT_TIMEOUT is not included in the waited-for eventset.
 *
 * Returned Value:
 *   Event set containing the event(s) that ended the wait.  Should always
 *   be non-zero.  All events are disabled after the wait concludes.
 *
 ****************************************************************************/

static sdio_eventset_t esp32p4_eventwait(struct sdio_dev_s* dev) {
  return 0;
}

/****************************************************************************
 * Name: esp32p4_callbackenable
 *
 * Description:
 *   Enable/disable of a set of SDIO callback events.  This is part of the
 *   the SDIO callback sequence.  The set of events is configured to enabled
 *   callbacks to the function provided in esp32p4_registercallback.
 *
 *   Events are automatically disabled once the callback is performed and no
 *   further callback events will occur until they are again enabled by
 *   calling this method.
 *
 * Input Parameters:
 *   dev      - An instance of the SDIO device interface
 *   eventset - A bitset of events to enable or disable (see SDIOMEDIA_*
 *              definitions). 0=disable; 1=enable.
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

static void esp32p4_callbackenable(struct sdio_dev_s* dev,
                                   sdio_eventset_t eventset) {}

/****************************************************************************
 * Name: esp32p4_registercallback
 *
 * Description:
 *   Register a callback that that will be invoked on any media status
 *   change.  Callbacks should not be made from interrupt handlers, rather
 *   interrupt level events should be handled by calling back on the work
 *   thread.
 *
 *   When this method is called, all callbacks should be disabled until they
 *   are enabled via a call to SDIO_CALLBACKENABLE
 *
 * Input Parameters:
 *   dev -      Device-specific state data
 *   callback - The function to call on the media change
 *   arg -      A caller provided value to return with the callback
 *
 * Returned Value:
 *   0 on success; negated errno on failure.
 *
 ****************************************************************************/

static int esp32p4_registercallback(struct sdio_dev_s* dev, worker_t callback,
                                    void* arg) {
  return OK;
}

/****************************************************************************
 * Name: esp32p4_callback
 *
 * Description:
 *   Perform callback.
 *
 * Assumptions:
 *   This function does not execute in the context of an interrupt handler.
 *   It may be invoked on any user thread or scheduled on the work thread
 *   from an interrupt handler.
 *
 ****************************************************************************/

static void esp32p4_callback(void* arg) {}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: sdio_initialize
 *
 * Description:
 *   Initialize SDIO for operation.
 *
 * Input Parameters:
 *   slotno - Not used.
 *
 * Returned Value:
 *   A reference to an SDIO interface structure.  NULL is returned on
 *   failures.
 *
 ****************************************************************************/

struct sdio_dev_s* sdio_initialize(int slotno) {
  struct esp32p4_dev_s* priv = &g_sdiodev;

  if (slotno == 0){

  }
  esp32p4_reset(&priv->dev);
  return &priv->dev;
}

#endif /* CONFIG_ESP32P4_SDMMC */
