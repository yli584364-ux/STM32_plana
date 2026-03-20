#include "web.h"

#include <Arduino.h>
#include <FS.h>
#include <WiFi.h>
#include "SPIFFS.h"

#include "display.h"
#include "display_commands.h"
#include "plana.h"
#include "audio_player.h"

static WebServer *g_server = nullptr;
static QueueHandle_t g_displayCmdQueue = nullptr;
static QueueHandle_t g_audioCmdQueue = nullptr;

static bool pushDisplayCommand(DisplayCommandFlag flag, int32_t value = 0)
{
  if (!g_displayCmdQueue)
  {
    return false;
  }

  DisplayCommand cmd = {flag, value};
  return xQueueSend(g_displayCmdQueue, &cmd, 0) == pdTRUE;
}

static bool pushAudioCommand(AudioCommandType type)
{
  return enqueueAudioCommand(g_audioCmdQueue, type);
}

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
  if (!pushDisplayCommand(DISPLAY_CMD_NEXT))
  {
    g_server->send(503, "text/plain", "QUEUE_FULL");
    return;
  }
  g_server->send(200, "text/plain", "QUEUED");
}

static void handlePrev()
{
  if (!pushDisplayCommand(DISPLAY_CMD_PREV))
  {
    g_server->send(503, "text/plain", "QUEUE_FULL");
    return;
  }
  g_server->send(200, "text/plain", "QUEUED");
}

static void handleSet()
{
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

  if (!pushDisplayCommand(DISPLAY_CMD_SET_INDEX, idx))
  {
    g_server->send(503, "text/plain", "QUEUE_FULL");
    return;
  }

  g_server->send(200, "text/plain", "QUEUED");
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
    if (!pushDisplayCommand(DISPLAY_CMD_STATE_OFF))
    {
      g_server->send(503, "text/plain", "QUEUE_FULL");
      return;
    }
    g_server->send(200, "text/plain", "QUEUED");
    return;
  }

  FlashStateGroup g = parseStateName(name);
  if (g == FLASH_STATE_COUNT)
  {
    g_server->send(400, "text/plain", "invalid state");
    return;
  }

  if (!pushDisplayCommand(DISPLAY_CMD_STATE_SET, (int32_t)g))
  {
    g_server->send(503, "text/plain", "QUEUE_FULL");
    return;
  }

  g_server->send(200, "text/plain", "QUEUED");
}

static void handlePhotoToggle()
{
  if (!pushDisplayCommand(DISPLAY_CMD_PHOTO_TOGGLE))
  {
    g_server->send(503, "text/plain", "QUEUE_FULL");
    return;
  }
  g_server->send(200, "text/plain", "QUEUED");
}

static void handlePhotoStatus()
{
  String body = "{";
  body += "\"sdAvailable\":";
  body += (sdAvailable ? "true" : "false");
  body += ",\"sdImageCount\":";
  body += String((unsigned long)sdImageCount);
  body += ",\"useSdImages\":";
  body += (useSdImages ? "true" : "false");
  body += ",\"imageCount\":";
  body += String((unsigned long)imageCount);
  body += "}";

  g_server->send(200, "application/json", body);
}

static void handleGifSourceToggle()
{
  String mode = g_server->hasArg("mode") ? g_server->arg("mode") : "toggle";
  mode.trim();
  mode.toLowerCase();

  if (mode == "ext")
  {
    if (!pushDisplayCommand(DISPLAY_CMD_GIFSOURCE_EXT))
    {
      g_server->send(503, "text/plain", "QUEUE_FULL");
      return;
    }
    g_server->send(200, "text/plain", "QUEUED");
    return;
  }

  if (mode == "onboard")
  {
    if (!pushDisplayCommand(DISPLAY_CMD_GIFSOURCE_ONBOARD))
    {
      g_server->send(503, "text/plain", "QUEUE_FULL");
      return;
    }
    g_server->send(200, "text/plain", "QUEUED");
    return;
  }

  if (mode != "toggle")
  {
    g_server->send(400, "text/plain", "invalid mode");
    return;
  }

  if (!pushDisplayCommand(DISPLAY_CMD_GIFSOURCE_TOGGLE))
  {
    g_server->send(503, "text/plain", "QUEUE_FULL");
    return;
  }

  g_server->send(200, "text/plain", "QUEUED");
}

static void handleGif()
{
  if (!pushDisplayCommand(DISPLAY_CMD_PLAY_GIF))
  {
    g_server->send(503, "text/plain", "QUEUE_FULL");
    return;
  }

  g_server->send(200, "text/plain", "QUEUED");
}

static void handleGifSync()
{
  if (!pushDisplayCommand(DISPLAY_CMD_SYNC_GIF))
  {
    g_server->send(503, "text/plain", "QUEUE_FULL");
    return;
  }

  g_server->send(200, "text/plain", "QUEUED");
}

static void handleMusicPlay()
{
  if (!pushAudioCommand(AUDIO_CMD_PLAY))
  {
    g_server->send(503, "text/plain", "QUEUE_FULL");
    return;
  }
  g_server->send(200, "text/plain", "QUEUED");
}

static void handleMusicPause()
{
  if (!pushAudioCommand(AUDIO_CMD_PAUSE))
  {
    g_server->send(503, "text/plain", "QUEUE_FULL");
    return;
  }
  g_server->send(200, "text/plain", "QUEUED");
}

static void handleMusicNext()
{
  if (!pushAudioCommand(AUDIO_CMD_NEXT))
  {
    g_server->send(503, "text/plain", "QUEUE_FULL");
    return;
  }
  g_server->send(200, "text/plain", "QUEUED");
}

static void handleMusicPrev()
{
  if (!pushAudioCommand(AUDIO_CMD_PREV))
  {
    g_server->send(503, "text/plain", "QUEUE_FULL");
    return;
  }
  g_server->send(200, "text/plain", "QUEUED");
}

static void handleMusicStatus()
{
  String body;
  buildAudioStatusJson(body);
  g_server->send(200, "application/json", body);
}

void registerWebHandlers(WebServer &server, QueueHandle_t displayCommandQueue, QueueHandle_t audioCommandQueue)
{
  g_server = &server;
  g_displayCmdQueue = displayCommandQueue;
  g_audioCmdQueue = audioCommandQueue;

  server.on("/", HTTP_GET, handleRoot);
  server.on("/next", HTTP_GET, handleNext);
  server.on("/prev", HTTP_GET, handlePrev);
  server.on("/set", HTTP_GET, handleSet);
  server.on("/state", HTTP_GET, handleState);
  server.on("/photo", HTTP_GET, handlePhotoToggle);
  server.on("/photostatus", HTTP_GET, handlePhotoStatus);
  server.on("/gifsource", HTTP_GET, handleGifSourceToggle);
  server.on("/gifstatus", HTTP_GET, handleGifStatus);
  server.on("/gif", HTTP_GET, handleGif);
  server.on("/gifsync", HTTP_GET, handleGifSync);
  server.on("/music/play", HTTP_GET, handleMusicPlay);
  server.on("/music/pause", HTTP_GET, handleMusicPause);
  server.on("/music/next", HTTP_GET, handleMusicNext);
  server.on("/music/prev", HTTP_GET, handleMusicPrev);
  server.on("/music/status", HTTP_GET, handleMusicStatus);
}
