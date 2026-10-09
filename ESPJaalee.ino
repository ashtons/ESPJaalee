/*
 * JAALEE broadcast monitor: no connection, pairing, or sensor writes.
 */
#include <TFT_eSPI.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <math.h>
#include <string.h>

const char *const SENSOR_ADDRESS = "AA:AA:AA:AA:AA:AA"; //TODO: Enter your sensors address, usually printed on the side of the device
const int TFT_BACKLIGHT_PIN = 4; // Set to -1 if your board has no backlight GPIO.
const uint32_t SCAN_SECONDS = 2;
const uint32_t STALE_AFTER_MS = 30000;
const uint32_t SERIAL_INTERVAL_MS = 5000;

const uint8_t FAMILY_ID[] = {
    0xEB, 0xEF, 0xD0, 0x83, 0x70, 0xA2, 0x47, 0xC8,
    0x98, 0x37, 0xE7, 0xB5, 0x63, 0x4D, 0xF5, 0x25
};

struct SensorReading {
    uint16_t rawTemperature;
    uint16_t rawHumidity;
    uint8_t battery;
    int rssi;
    uint32_t receivedAt;
    bool valid;
};

float temperatureC(const SensorReading &reading);
float humidityPercent(const SensorReading &reading);
void drawReading(const SensorReading &reading, uint32_t now);
void logReading(const SensorReading &reading, uint32_t now);
void drawLine(const String &text, int row, uint16_t color, int font = 2);

TFT_eSPI tft = TFT_eSPI();
BLEScan *scanner = nullptr;
SensorReading latestReading = {};
portMUX_TYPE readingMutex = portMUX_INITIALIZER_UNLOCKED;

uint16_t readBigEndian16(const uint8_t *data)
{
    return (uint16_t(data[0]) << 8) | data[1];
}

class JaaleeCallbacks : public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice device) override
    {
        String sender(device.getAddress().toString().c_str());
        const bool senderMatches = sender.equalsIgnoreCase(SENSOR_ADDRESS);
        const uint8_t *payload = device.getPayload();
        const size_t payloadLength = device.getPayloadLength();
        if (payload == nullptr) return;

        SensorReading reading = {};
        bool haveServiceReading = false;
        for (size_t offset = 0; offset < payloadLength;) {
            const size_t fieldLength = payload[offset];
            if (fieldLength == 0) break;
            if (fieldLength > payloadLength - offset - 1) break;
            const uint8_t type = payload[offset + 1];
            const uint8_t *data = payload + offset + 2;
            const size_t dataLength = fieldLength - 1;

            if (type == 0x16 && dataLength == 14 &&
                data[0] == 0x1C && data[1] == 0xF5) {
                const uint8_t *service = data + 2;
                char embeddedAddress[18];
                snprintf(embeddedAddress, sizeof(embeddedAddress),
                         "%02X:%02X:%02X:%02X:%02X:%02X",
                         service[6], service[5], service[4],
                         service[3], service[2], service[1]);
                if (senderMatches ||
                    String(embeddedAddress).equalsIgnoreCase(SENSOR_ADDRESS)) {
                    reading.rawTemperature = readBigEndian16(service + 8);
                    reading.rawHumidity = readBigEndian16(service + 10);
                    reading.battery = service[0];
                    reading.valid = true;
                    haveServiceReading = true;
                }
            } else if (!haveServiceReading && senderMatches &&
                       type == 0xFF && dataLength == 26 &&
                       data[0] == 0x4C && data[1] == 0x00 &&
                       data[2] == 0x02 && data[3] == 0x15 &&
                       memcmp(data + 4, FAMILY_ID, sizeof(FAMILY_ID)) == 0) {
                reading.rawTemperature = readBigEndian16(data + 20);
                reading.rawHumidity = readBigEndian16(data + 22);
                reading.battery = data[25];
                reading.valid = true;
            }
            offset += fieldLength + 1;
        }
        if (!reading.valid) return;
        reading.rssi = device.getRSSI(); 
        reading.receivedAt = millis();

        // BLE callbacks run on another task. Only exchange a small snapshot;
        // all LCD and Serial work stays in the Arduino loop.
        portENTER_CRITICAL(&readingMutex);
        latestReading = reading;
        portEXIT_CRITICAL(&readingMutex);
    }
};

JaaleeCallbacks scanCallbacks;

float temperatureC(const SensorReading &reading)
{
    return reading.rawTemperature * 175.0f / 65535.0f - 45.0f;
}

float humidityPercent(const SensorReading &reading)
{
    return reading.rawHumidity * 100.0f / 65535.0f;
}

float vpdKPa(float temperature, float humidity)
{
    // Calculated air VPD, using the Tetens formula
    return 0.6108f * expf(17.27f * temperature / (temperature + 237.3f)) *
           (1.0f - humidity / 100.0f);
}

String batteryLabel(uint8_t battery)
{
    return battery <= 100 ? String(battery) + "%" : "unknown (" + String(battery) + ")";
}

void drawLine(const String &text, int row, uint16_t color, int font)
{
    const int rowHeight = tft.height() / 7;
    const int y = row * rowHeight;
    // Clear each row rather than blanking the entire screen on every update.
    tft.fillRect(0, y, tft.width(), rowHeight, TFT_BLACK);
    tft.setTextColor(color, TFT_BLACK);
    tft.drawString(text, 4, y, font);
}

void drawReading(const SensorReading &reading, uint32_t now)
{
    drawLine("JAALEE sensor", 0, TFT_CYAN);
    drawLine(SENSOR_ADDRESS, 1, TFT_LIGHTGREY, 1);
    if (!reading.valid) {
        drawLine("Waiting for broadcasts", 2, TFT_YELLOW);
        drawLine("Temperature: --", 3, TFT_WHITE);
        drawLine("Humidity: --", 4, TFT_WHITE);
        drawLine("VPD: --", 5, TFT_WHITE);
        drawLine("Active BLE scan", 6, TFT_LIGHTGREY, 1);
        return;
    }

    const float temperature = temperatureC(reading);
    const float humidity = humidityPercent(reading);
    const uint32_t age = now - reading.receivedAt;
    const bool stale = age >= STALE_AFTER_MS;
    const uint16_t valueColor = stale ? TFT_LIGHTGREY : TFT_WHITE;
    drawLine("Temp: " + String(temperature, 2) + " C", 2, valueColor);
    drawLine("Humidity: " + String(humidity, 2) + "% RH", 3, valueColor);
    drawLine("VPD: " + String(vpdKPa(temperature, humidity), 3) + " kPa", 4, valueColor);
    drawLine("Battery " + batteryLabel(reading.battery) + "  " +
             String(reading.rssi) + " dBm", 5, TFT_LIGHTGREY, 1);
    drawLine(String(stale ? "STALE - last RX " : "Last RX ") +
             String(age / 1000) + "s ago", 6, stale ? TFT_YELLOW : TFT_GREEN, 1);
}

void logReading(const SensorReading &reading, uint32_t now)
{
    static SensorReading lastPrinted = {};
    static uint32_t lastPrintedAt = 0;
    if (!reading.valid) return;
    const bool changed = !lastPrinted.valid ||
                         reading.rawTemperature != lastPrinted.rawTemperature ||
                         reading.rawHumidity != lastPrinted.rawHumidity ||
                         reading.battery != lastPrinted.battery;
    if (!changed && (reading.receivedAt == lastPrinted.receivedAt ||
                     now - lastPrintedAt < SERIAL_INTERVAL_MS)) return;

    const float temperature = temperatureC(reading);
    const float humidity = humidityPercent(reading);
    Serial.printf("%s  %.2f C  %.2f%% RH  VPD %.3f kPa  Battery %s  RSSI %d dBm\n",
                  SENSOR_ADDRESS, temperature, humidity, vpdKPa(temperature, humidity),
                  batteryLabel(reading.battery).c_str(), reading.rssi);
    lastPrinted = reading;
    lastPrintedAt = now;
}

void setup()
{
    Serial.begin(115200);
    delay(200);
    Serial.println("\nESPJaalee starting");
    if (TFT_BACKLIGHT_PIN >= 0) {
        pinMode(TFT_BACKLIGHT_PIN, OUTPUT);
        digitalWrite(TFT_BACKLIGHT_PIN, HIGH);
    }
    tft.init();
    tft.setRotation(1);
    tft.setTextDatum(TL_DATUM);
    tft.fillScreen(TFT_BLACK);
    Serial.printf("LCD initialized: %d x %d\n", tft.width(), tft.height());
    drawReading(latestReading, millis());

    BLEDevice::init("");
    scanner = BLEDevice::getScan();
    // Repeated advertisements refresh reception time even if values are equal.
    scanner->setAdvertisedDeviceCallbacks(&scanCallbacks, true);
    scanner->setActiveScan(true); // Request F51C scan responses; never connect.
    scanner->setInterval(100);
    scanner->setWindow(99);
    Serial.printf("Scanning broadcasts from %s\n", SENSOR_ADDRESS);
}

void loop()
{
    // Short blocking scans work across ESP32 Arduino 2.x and 3.x BLE APIs.
    // Ignore the result return type (it differs between those core versions).
    scanner->start(SCAN_SECONDS, false);
    scanner->clearResults(); // Release discovered devices after every scan.

    SensorReading reading;
    portENTER_CRITICAL(&readingMutex);
    reading = latestReading;
    portEXIT_CRITICAL(&readingMutex);
    const uint32_t now = millis();
    drawReading(reading, now);
    logReading(reading, now);
    delay(10);
}
