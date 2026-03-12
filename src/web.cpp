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

  if (useSdImages && useSdGifMode && gifSetCount > 0)
  {
    size_t idx = currentImageIndex % gifSetCount;
    playGifFromSdFolder(gifSetFolders[idx].c_str());
  }
  else
  {
    showCurrentImage();
  }

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

  if (useSdImages && useSdGifMode && gifSetCount > 0)
  {
    size_t idx = currentImageIndex % gifSetCount;
    playGifFromSdFolder(gifSetFolders[idx].c_str());
  }
  else
  {
    showCurrentImage();
  }

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
  if (useSdImages && useSdGifMode && gifSetCount > 0)
  {
    size_t gifIdx = currentImageIndex % gifSetCount;
    playGifFromSdFolder(gifSetFolders[gifIdx].c_str());
  }
  else
  {
    showCurrentImage();
  }

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

  if (useSdImages)
  {
    g_server->send(200, "text/plain", stateNameOf(activeFlashStateGroup));
    return;
  }

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
  if (useSdImages)
  {
    if (useSdGifMode)
    {
      if (gifSetCount == 0)
      {
        scanGifSetsOnSd();
      }
      imageCount = gifSetCount;
    }
    else
    {
      imageCount = sdImageCount;
    }
  }
  else
  {
    imageCount = planaImageCount;
  }

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

static void handleGif()
{
  flashStateAutoRandomEnabled = false;

  if (gifSetCount == 0)
  {
    scanGifSetsOnSd();
  }

  if (gifSetCount == 0)
  {
    g_server->send(200, "text/plain", "NO_GIF_SET");
    return;
  }

  size_t idx = 0;
  if (g_server->hasArg("id"))
  {
    int id = g_server->arg("id").toInt();
    if (id > 0)
    {
      idx = (size_t)(id - 1);
      if (idx >= gifSetCount)
      {
        idx = gifSetCount - 1;
      }
    }
  }

  playGifFromSdFolder(gifSetFolders[idx].c_str());
  g_server->send(200, "text/plain", "GIF_OK");
}

static void handleSdModeToggle()
{
  flashStateAutoRandomEnabled = false;

  if (!sdAvailable)
  {
    g_server->send(200, "text/plain", "NO_SD");
    return;
  }

  useSdGifMode = !useSdGifMode;

  if (useSdImages)
  {
    if (useSdGifMode)
    {
      if (gifSetCount == 0)
      {
        scanGifSetsOnSd();
      }
      imageCount = gifSetCount;
    }
    else
    {
      imageCount = sdImageCount;
    }
  }

  g_server->send(200, "text/plain", useSdGifMode ? "GIF" : "IMAGE");
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
  server.on("/gif", HTTP_GET, handleGif);
  server.on("/sdmode", HTTP_GET, handleSdModeToggle);
}
