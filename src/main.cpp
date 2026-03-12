#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SD.h>
#include "SPIFFS.h"

#include "pin.h"
#include "display.h"
#include "web.h"
#include "plana.h"

#define ENABLE_BLUETOOTH 0

#if ENABLE_BLUETOOTH
#include "bluetooth.h"
BluetoothSerial SerialBT;
#endif

WebServer server(80);

void setup()
{
  Serial.begin(115200);

  if (!SPIFFS.begin(true))
  {
    Serial.println("SPIFFS mount failed");
  }

  randomSeed((uint32_t)micros());

  if (TFT_BL >= 0)
  {
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);
  }

  pinMode(TFT_CS, OUTPUT);
  digitalWrite(TFT_CS, HIGH);

  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);

  tft.init(240, 240);
  tft.setRotation(1);

  SPI.begin(18, 19, 23);
  if (SD.begin(SD_CS, SPI, SD_SPI_FREQ))
  {
    sdAvailable = true;
    scanSdImages();
  }
  else
  {
    Serial.println("SD card initialization failed");
  }

  if (planaImageCount > 0)
  {
    useSdImages = false;
    imageCount = planaImageCount;
    buildFlashStateGroups();
  }
  else if (sdAvailable && sdImageCount > 0)
  {
    useSdImages = true;
    imageCount = sdImageCount;
  }
  else
  {
    imageCount = 0;
  }

  showCurrentImage();
  lastSwitchTime = millis();

  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  const char *ssid = "ESP_LCD_AP";
  const char *password = "12345678";
  WiFi.softAP(ssid, password);

  registerWebHandlers(server);
  server.begin();

#if ENABLE_BLUETOOTH
  SerialBT.begin("ESP_LCD");
#endif
}

void loop()
{
  unsigned long now = millis();
  server.handleClient();

  if (flashStateAutoRandomEnabled && !useSdImages && imageCount > 0)
  {
    if ((now - lastFlashStateRandomSwitchTime) >= FLASH_STATE_RANDOM_INTERVAL_MS)
    {
      if (!pickRandomFlashImageFromActiveState())
      {
        flashStateAutoRandomEnabled = false;
      }
    }
  }

#if ENABLE_BLUETOOTH
  while (SerialBT.available() > 0)
  {
    char cmd = SerialBT.read();
    bool needUpdate = false;

    if (cmd == 'n' || cmd == 'N')
    {
      currentImageIndex++;
      if (currentImageIndex >= imageCount)
      {
        currentImageIndex = 0;
      }
      needUpdate = true;
    }
    else if (cmd == 'p' || cmd == 'P')
    {
      if (currentImageIndex == 0)
      {
        currentImageIndex = (imageCount == 0 ? 0 : imageCount - 1);
      }
      else
      {
        currentImageIndex--;
      }
      needUpdate = true;
    }
    else if (cmd >= '0' && cmd <= '9')
    {
      size_t idx = (size_t)(cmd - '0');
      if (idx < imageCount)
      {
        currentImageIndex = idx;
        needUpdate = true;
      }
    }

    if (needUpdate)
    {
      showCurrentImage();
      lastSwitchTime = now;
    }
  }
#endif

  delay(10);
}
