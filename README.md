# ESPJaalee
Simple implementation of a Bluetooth app listening for current sensor broadcasts from a Jaalee sensor and displaying the values on the TFT display.

![Jaalee Sensor](./docs/sensor.png)

## Setup

Install the ESP32 board support and the `TFT_eSPI` library in Arduino IDE.

For console-only output, set `#define ENABLE_TFT 0` near the top of
`ESPJaalee.ino`. This disables TFT initialization and rendering; the
`TFT_eSPI` library is only required when `ENABLE_TFT` is `1` (the default).
Open Serial Monitor at 115200 baud to see sensor readings. Screenshot capture
is unavailable in console-only mode.

On ESP32-C3 boards using native USB, select **Tools → USB CDC On Boot → Enabled**
in Arduino IDE, then upload again. Select the board's USB port and open Serial
Monitor at 115200 baud; press RESET if needed. For boards using an external
USB-to-UART bridge such as CH340 or CP210x, leave USB CDC On Boot disabled.
See [Espressif's USB CDC guide](https://docs.espressif.com/projects/arduino-esp32/en/latest/tutorials/cdc_dfu_flash.html).

The sketch waits up to three seconds for a USB console at startup and prints a
status message every five seconds while waiting for sensor broadcasts or when
the last reading is stale.

`TFT_eSPI` must be configured in the library's setup file because the library is
compiled separately from the sketch. 

Set your device bluetooth address

> const char *const SENSOR_ADDRESS = "AA:AA:AA:AA:AA:AA";

Set the units

> const bool USE_FAHRENHEIT = true; 


## Screenshots

![Screenshot Celsius](./docs/screenshot.png)

![Screenshot Fahrenheit](./docs/screenshot2.png)

The UI renders into a 16-bit framebuffer (about 63 KiB on the original
240 × 135 T-Display). Only changed text or colours trigger a screen update;
each completed frame is pushed to the LCD without clearing it first.

Upload the updated sketch, connect the board over USB, and close Arduino Serial
Monitor or any other program using its serial port. With `uv` installed, run:

```bash
rtk proxy uv run capture_screen.py --list-ports
rtk proxy uv run capture_screen.py --port /dev/cu.usbserial-XXXX --output screenshot.png
```

Replace the port with the device listed for your board (`COM3`, for example,
on Windows). If exactly one serial device is present, `--port` can be omitted.
`uv` installs the script's pySerial and Pillow dependencies automatically.
Alternatively, install those packages in your Python environment and run the
script with `python3`.

The script sends `s` at 115200 baud and saves the exact framebuffer sent to the
LCD as a PNG. Capture takes roughly six seconds; the screen holds its current
frame during transfer while BLE reception continues. This captures the rendered
pixels, rather than the physical panel's brightness or appearance. Some USB
drivers may reset the board when the port opens; if that happens, capture again
once sensor readings arrive.
