#include "web.h"

#include <Arduino.h>
#include <FS.h>
#include <WiFi.h>
#include "SPIFFS.h"

#include "display.h"
#include "plana.h"

static WebServer *g_server = nullptr;

static bool updateExternalGifReady()
{
  if (!extFlashAvailable)
  {
    extFlashGifReady = false;
    return false;
  }

  bool ready = refreshExternalGifHeader();
  Serial.print("GIF status refresh: extFlashAvailable=");
  Serial.print(extFlashAvailable ? "true" : "false");
  Serial.print(", extFlashGifReady=");
  Serial.println(ready ? "true" : "false");
  return ready;
}

static void handleGifStatus()
{
  bool extReady = updateExternalGifReady();
  bool onboardReady = isOnboardGifReady();

  String body = "{";
  body += "\"extFlashAvailable\":";
  body += (extFlashAvailable ? "true" : "false");
  body += ",\"extGifReady\":";
  body += (extReady ? "true" : "false");
  body += ",\"useExternal\":";
  body += (useExternalFlashForGif ? "true" : "false");
  body += ",\"onboardReady\":";
  body += (onboardReady ? "true" : "false");
  body += "}";

  g_server->send(200, "application/json", body);
}

static void handleRoot()
{
  File f = SPIFFS.open("/index.html", "r");
  if (!f)
  {
    g_server->send(200, "text/html; charset=utf-8",
                   "<html><body><h1>ESP LCD</h1><p>No index.html on SPIFFS</p></body></html>");
    return;
  }

  g_server->streamFile(f, "text/html; charset=utf-8");
  f.close();
}

static void handleNext()
{
  flashStateAutoRandomEnabled = false;

  currentImageIndex++;
  if (currentImageIndex >= imageCount)
  {
    currentImageIndex = 0;
  }

  showCurrentImage();

  lastSwitchTime = millis();
  g_server->send(200, "text/plain", "OK");
}

static void handlePrev()
{
  flashStateAutoRandomEnabled = false;

  if (currentImageIndex == 0)
  {
    currentImageIndex = (imageCount == 0 ? 0 : imageCount - 1);
  }
  else
  {
    currentImageIndex--;
  }

  showCurrentImage();

  lastSwitchTime = millis();
  g_server->send(200, "text/plain", "OK");
}

static void handleSet()
{
  flashStateAutoRandomEnabled = false;

  if (!g_server->hasArg("i"))
  {
    g_server->send(400, "text/plain", "missing i");
    return;
  }

  int idx = g_server->arg("i").toInt();
  if (idx < 0 || (size_t)idx >= imageCount)
  {
    g_server->send(400, "text/plain", "out of range");
    return;
  }

  currentImageIndex = (size_t)idx;
  showCurrentImage();

  lastSwitchTime = millis();
  g_server->send(200, "text/plain", "OK");
}

static void handleState()
{
  if (!g_server->hasArg("name"))
  {
    g_server->send(400, "text/plain", "missing name");
    return;
  }

  String name = g_server->arg("name");
  name.trim();
  name.toLowerCase();

  if (name == "off" || name == "manual")
  {
    flashStateAutoRandomEnabled = false;
    activeFlashStateGroup = FLASH_STATE_ALL;
    g_server->send(200, "text/plain", "OFF");
    return;
  }

  FlashStateGroup g = parseStateName(name);
  if (g == FLASH_STATE_COUNT)
  {
    g_server->send(400, "text/plain", "invalid state");
    return;
  }

  activeFlashStateGroup = g;
  flashStateAutoRandomEnabled = true;

  if (!pickRandomFlashImageFromActiveState())
  {
    flashStateAutoRandomEnabled = false;
    g_server->send(404, "text/plain", "NO_STATE_IMAGE");
    return;
  }

  g_server->send(200, "text/plain", stateNameOf(activeFlashStateGroup));
}

static void handlePhotoToggle()
{
  flashStateAutoRandomEnabled = false;

  if (!sdAvailable || sdImageCount == 0)
  {
    g_server->send(200, "text/plain", "NO_SD");
    return;
  }

  if (planaImageCount == 0)
  {
    useSdImages = true;
    imageCount = sdImageCount;
    currentImageIndex %= imageCount;
    showCurrentImage();
    lastSwitchTime = millis();
    g_server->send(200, "text/plain", "SD_ONLY");
    return;
  }

  useSdImages = !useSdImages;
  imageCount = useSdImages ? sdImageCount : planaImageCount;

  if (imageCount == 0)
  {
    g_server->send(200, "text/plain", "NO_IMG");
    return;
  }

  if (currentImageIndex >= imageCount)
  {
    currentImageIndex = 0;
  }

  showCurrentImage();
  lastSwitchTime = millis();
  g_server->send(200, "text/plain", useSdImages ? "SD" : "FLASH");
}

static void handleGifSourceToggle()
{
  flashStateAutoRandomEnabled = false;

  updateExternalGifReady();

  String mode = g_server->hasArg("mode") ? g_server->arg("mode") : "toggle";
  mode.trim();
  mode.toLowerCase();

  if (mode == "ext")
  {
    if (!extFlashGifReady)
    {
      useExternalFlashForGif = false;
      g_server->send(200, "text/plain", "NO_EXT_GIF");
      return;
    }
    useExternalFlashForGif = true;
    g_server->send(200, "text/plain", "EXT_FLASH");
    return;
  }

  if (mode == "onboard")
  {
    useExternalFlashForGif = false;
    g_server->send(200, "text/plain", "ONBOARD_FLASH");
    return;
  }

  useExternalFlashForGif = !useExternalFlashForGif;
  if (useExternalFlashForGif && !extFlashGifReady)
  {
    useExternalFlashForGif = false;
    g_server->send(200, "text/plain", "NO_EXT_GIF");
    return;
  }

  g_server->send(200, "text/plain", useExternalFlashForGif ? "EXT_FLASH" : "ONBOARD_FLASH");
}

static void handleGif()
{
  flashStateAutoRandomEnabled = false;
  updateExternalGifReady();

  Serial.print("GIF play request: useExternal=");
  Serial.print(useExternalFlashForGif ? "true" : "false");
  Serial.print(", extReady=");
  Serial.print(extFlashGifReady ? "true" : "false");
  Serial.print(", onboardReady=");
  Serial.println(isOnboardGifReady() ? "true" : "false");

  if (useExternalFlashForGif)
  {
    if (!extFlashGifReady)
    {
      Serial.println("GIF play aborted: external GIF not ready");
      g_server->send(200, "text/plain", "NO_EXT_GIF");
      return;
    }
    playGifFromExternalFlash();
  }
  else
  {
    if (!isOnboardGifReady())
    {
      if (extFlashGifReady)
      {
        useExternalFlashForGif = true;
        Serial.println("GIF play fallback: switching to external flash");
        playGifFromExternalFlash();
        g_server->send(200, "text/plain", "FALLBACK_EXT");
        return;
      }

      Serial.println("GIF play aborted: onboard and external GIF both unavailable");
      g_server->send(200, "text/plain", "NO_ONBOARD_GIF");
      return;
    }
    playGifFromOnboardFlash();
  }

  g_server->send(200, "text/plain", "GIF_OK");
}

static void handleGifSync()
{
  flashStateAutoRandomEnabled = false;
  Serial.println("GIF sync request received");

  if (!extFlashAvailable)
  {
    Serial.println("GIF sync aborted: external flash unavailable");
    g_server->send(200, "text/plain", "NO_EXT_FLASH");
    return;
  }

  if (!syncGifDataToExternalFlash())
  {
    Serial.println("GIF sync failed");
    g_server->send(500, "text/plain", "SYNC_FAIL");
    return;
  }

  updateExternalGifReady();
  useExternalFlashForGif = true;
  Serial.println("GIF sync finished");
  g_server->send(200, "text/plain", extFlashGifReady ? "SYNC_OK_EXT" : "SYNC_OK_BUT_NOT_READY");
}

void registerWebHandlers(WebServer &server)
{
  g_server = &server;

  server.on("/", HTTP_GET, handleRoot);
  server.on("/next", HTTP_GET, handleNext);
  server.on("/prev", HTTP_GET, handlePrev);
  server.on("/set", HTTP_GET, handleSet);
  server.on("/state", HTTP_GET, handleState);
  server.on("/photo", HTTP_GET, handlePhotoToggle);
  server.on("/gifsource", HTTP_GET, handleGifSourceToggle);
  server.on("/gifstatus", HTTP_GET, handleGifStatus);
  server.on("/gif", HTTP_GET, handleGif);
  server.on("/gifsync", HTTP_GET, handleGifSync);
}
