#include "web.h"

#include <Arduino.h>
#include <FS.h>
#include <WiFi.h>
#include "SPIFFS.h"

#include "display.h"
#include "plana.h"

static WebServer *g_server = nullptr;

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

  if (useExternalFlashForGif)
  {
    if (!extFlashGifReady)
    {
      g_server->send(200, "text/plain", "NO_EXT_GIF");
      return;
    }
    playGifFromExternalFlash();
  }
  else
  {
    if (getOnboardGifFrameCount() == 0)
    {
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

  if (!extFlashAvailable)
  {
    g_server->send(200, "text/plain", "NO_EXT_FLASH");
    return;
  }

  if (!syncGifDataToExternalFlash())
  {
    g_server->send(500, "text/plain", "SYNC_FAIL");
    return;
  }

  g_server->send(200, "text/plain", "SYNC_OK");
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
  server.on("/gif", HTTP_GET, handleGif);
  server.on("/gifsync", HTTP_GET, handleGifSync);
}
