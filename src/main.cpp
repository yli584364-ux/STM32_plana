#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SD.h>
#include "SPIFFS.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#include "pin.h"
#include "display.h"
#include "display_commands.h"
#include "web.h"
#include "plana.h"
#include "external_flash.h"

#define ENABLE_BLUETOOTH 0

#if ENABLE_BLUETOOTH
#include "bluetooth.h"
BluetoothSerial SerialBT;
#endif

WebServer server(80);
static QueueHandle_t g_displayCommandQueue = nullptr;

static void applyDisplayCommand(const DisplayCommand &cmd)
{
  flashStateAutoRandomEnabled = false;

  switch (cmd.flag)
  {
  case DISPLAY_CMD_NEXT:
    if (imageCount == 0)
    {
      return;
    }
    currentImageIndex++;
    if (currentImageIndex >= imageCount)
    {
      currentImageIndex = 0;
    }
    showCurrentImage();
    lastSwitchTime = millis();
    return;

  case DISPLAY_CMD_PREV:
    if (imageCount == 0)
    {
      return;
    }
    if (currentImageIndex == 0)
    {
      currentImageIndex = imageCount - 1;
    }
    else
    {
      currentImageIndex--;
    }
    showCurrentImage();
    lastSwitchTime = millis();
    return;

  case DISPLAY_CMD_SET_INDEX:
  {
    if (imageCount == 0)
    {
      return;
    }
    int idx = (int)cmd.value;
    if (idx < 0 || (size_t)idx >= imageCount)
    {
      return;
    }
    currentImageIndex = (size_t)idx;
    showCurrentImage();
    lastSwitchTime = millis();
    return;
  }

  case DISPLAY_CMD_STATE_OFF:
    activeFlashStateGroup = FLASH_STATE_ALL;
    return;

  case DISPLAY_CMD_STATE_SET:
  {
    FlashStateGroup g = (FlashStateGroup)cmd.value;
    if (g >= FLASH_STATE_COUNT)
    {
      return;
    }
    activeFlashStateGroup = g;
    flashStateAutoRandomEnabled = true;
    if (!pickRandomFlashImageFromActiveState())
    {
      flashStateAutoRandomEnabled = false;
    }
    return;
  }

  case DISPLAY_CMD_PHOTO_TOGGLE:
    if (!sdAvailable || sdImageCount == 0)
    {
      return;
    }

    if (planaImageCount == 0)
    {
      useSdImages = true;
      imageCount = sdImageCount;
      currentImageIndex %= imageCount;
      showCurrentImage();
      lastSwitchTime = millis();
      return;
    }

    useSdImages = !useSdImages;
    imageCount = useSdImages ? sdImageCount : planaImageCount;
    if (imageCount == 0)
    {
      return;
    }
    if (currentImageIndex >= imageCount)
    {
      currentImageIndex = 0;
    }
    showCurrentImage();
    lastSwitchTime = millis();
    return;

  case DISPLAY_CMD_GIFSOURCE_EXT:
    if (refreshExternalGifHeader())
    {
      useExternalFlashForGif = true;
    }
    else
    {
      useExternalFlashForGif = false;
    }
    return;

  case DISPLAY_CMD_GIFSOURCE_ONBOARD:
    useExternalFlashForGif = false;
    return;

  case DISPLAY_CMD_GIFSOURCE_TOGGLE:
    useExternalFlashForGif = !useExternalFlashForGif;
    if (useExternalFlashForGif && !refreshExternalGifHeader())
    {
      useExternalFlashForGif = false;
    }
    return;

  case DISPLAY_CMD_PLAY_GIF:
    refreshExternalGifHeader();

    if (useExternalFlashForGif)
    {
      if (!extFlashGifReady)
      {
        Serial.println("GIF play aborted: external GIF not ready");
        return;
      }
      playGifFromExternalFlash();
      return;
    }

    if (!isOnboardGifReady())
    {
      if (extFlashGifReady)
      {
        useExternalFlashForGif = true;
        Serial.println("GIF play fallback: switching to external flash");
        playGifFromExternalFlash();
      }
      else
      {
        Serial.println("GIF play aborted: onboard and external GIF both unavailable");
      }
      return;
    }

    playGifFromOnboardFlash();
    return;

  case DISPLAY_CMD_SYNC_GIF:
    if (!extFlashAvailable)
    {
      Serial.println("GIF sync aborted: external flash unavailable");
      return;
    }

    Serial.println("GIF sync request received");
    if (!syncGifDataToExternalFlash())
    {
      Serial.println("GIF sync failed");
      return;
    }

    refreshExternalGifHeader();
    useExternalFlashForGif = true;
    Serial.println("GIF sync finished");
    return;

  case DISPLAY_CMD_PLAY_SD_GIF:
  {
    if (!sdAvailable)
    {
      Serial.println("GIF SD play aborted: SD unavailable");
      return;
    }

    int32_t idxValue = cmd.value;
    size_t gifCount = getSdGifCount();
    if (gifCount == 0)
    {
      scanSdGifFolders();
      gifCount = getSdGifCount();
    }
    if (gifCount == 0)
    {
      Serial.println("GIF SD play aborted: no gif folders");
      return;
    }

    size_t target = 0;
    if (idxValue < 0)
    {
      target = getSdGifActiveIndex();
    }
    else
    {
      target = (size_t)idxValue;
    }

    if (!playGifFromSd(target))
    {
      Serial.println("GIF SD play failed");
    }
    return;
  }

  default:
    return;
  }
}

static void webTask(void *arg)
{
  (void)arg;
  for (;;)
  {
    server.handleClient();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

static void displayTask(void *arg)
{
  (void)arg;
  DisplayCommand cmd;

  for (;;)
  {
    if (!g_displayCommandQueue)
    {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    while (xQueueReceive(g_displayCommandQueue, &cmd, 0) == pdTRUE)
    {
      applyDisplayCommand(cmd);
    }

    unsigned long now = millis();

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
      char btCmd = SerialBT.read();
      DisplayCommand btDisplayCmd = {DISPLAY_CMD_NEXT, 0};
      bool hasCmd = false;

      if (btCmd == 'n' || btCmd == 'N')
      {
        btDisplayCmd.flag = DISPLAY_CMD_NEXT;
        hasCmd = true;
      }
      else if (btCmd == 'p' || btCmd == 'P')
      {
        btDisplayCmd.flag = DISPLAY_CMD_PREV;
        hasCmd = true;
      }
      else if (btCmd >= '0' && btCmd <= '9')
      {
        btDisplayCmd.flag = DISPLAY_CMD_SET_INDEX;
        btDisplayCmd.value = btCmd - '0';
        hasCmd = true;
      }

      if (hasCmd)
      {
        applyDisplayCommand(btDisplayCmd);
      }
    }
#endif

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

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
    scanSdGifFolders();
  }
  else
  {
    Serial.println("SD card initialization failed");
  }

  extFlashAvailable = extFlashBegin();
  if (extFlashAvailable)
  {
    extFlashGifReady = refreshExternalGifHeader();
  }
  else
  {
    Serial.println("External flash initialization failed");
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

  g_displayCommandQueue = xQueueCreate(16, sizeof(DisplayCommand));
  if (!g_displayCommandQueue)
  {
    Serial.println("Failed to create display command queue");
  }

  registerWebHandlers(server, g_displayCommandQueue);
  server.begin();

  if (xTaskCreatePinnedToCore(webTask, "web_task", 4096, nullptr, 2, nullptr, 0) != pdPASS)
  {
    Serial.println("Failed to create web_task");
  }
  if (xTaskCreatePinnedToCore(displayTask, "display_task", 6144, nullptr, 2, nullptr, 1) != pdPASS)
  {
    Serial.println("Failed to create display_task");
  }

#if ENABLE_BLUETOOTH
  SerialBT.begin("ESP_LCD");
#endif
}

void loop()
{
  delay(1000);
}
