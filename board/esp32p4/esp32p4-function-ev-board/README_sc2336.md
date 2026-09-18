# ESP32-P4 Function EV Board SC2336 camera

This optional board driver connects the SC2336 sensor to the ESP32-P4 MIPI
CSI host and registers `/dev/video0`.

## Fixed mode

- 1280 x 720 at 30 frames per second
- Two CSI-2 data lanes at 405 Mbps per lane
- Packed BGGR RAW10, 1,152,000 bytes per frame
- I2C0, address 0x30, 400 kHz (board pins GPIO7/GPIO8)
- 24 MHz sensor input clock supplied externally/by the camera module

The official Function EV Board BSP marks camera XCLK as `GPIO_NUM_NC`; this
port deliberately does not invent or drive an XCLK GPIO.

## Experimental V4L2 contract

NuttX currently defines Bayer RAW10 V4L2 fourcc values, but its
`imgdata`/`imgsensor` conversion layer has no matching RAW10 lower-half enum.
To remain overlay-only, this driver advertises `V4L2_PIX_FMT_ENTROPY` as an
opaque transport token. The bytes are always packed BGGR RAW10; they are not
entropy-coded data, RGB565, or YUV. No ISP or demosaic is implemented.

Enable `CONFIG_ESPRESSIF_MIPI_CSI` and
`CONFIG_ESP32P4_FUNCTION_EV_BOARD_SC2336`. CSI is intentionally mutually
exclusive with the current MIPI-DSI lower half because both implementations
own the single DW-GDMA interrupt route.

The sensor mode table is derived from Espressif's Apache-2.0 licensed
`esp-video-components` SC2336 2-lane, 24 MHz input, 720p30 RAW10 mode.
