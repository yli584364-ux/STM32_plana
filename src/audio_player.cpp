#include "audio_player.h"

#include <SD.h>
#include <vector>
#include <algorithm>

#include <AudioFileSourceSD.h>
#include <AudioGeneratorWAV.h>
#include <AudioOutputI2S.h>

#include "pin.h"

static std::vector<String> g_playlist;
static volatile bool g_isPlaying = false;
static volatile bool g_sdReady = false;
static volatile bool g_initialized = false;
static int g_trackIndex = -1;

static AudioGeneratorWAV *g_wav = nullptr;
static AudioFileSourceSD *g_source = nullptr;
static AudioOutputI2S *g_output = nullptr;

static portMUX_TYPE g_audioStateMux = portMUX_INITIALIZER_UNLOCKED;

static String basenameFromPath(const String &path)
{
  int slashPos = path.lastIndexOf('/');
  if (slashPos < 0 || slashPos >= (int)path.length() - 1)
  {
    return path;
  }
  return path.substring(slashPos + 1);
}

static bool endsWithIgnoreCase(const String &value, const char *suffix)
{
  String lowerValue = value;
  String lowerSuffix = String(suffix);
  lowerValue.toLowerCase();
  lowerSuffix.toLowerCase();
  return lowerValue.endsWith(lowerSuffix);
}

static void stopPlayback()
{
  if (g_wav)
  {
    if (g_wav->isRunning())
    {
      g_wav->stop();
    }
    delete g_wav;
    g_wav = nullptr;
  }

  if (g_source)
  {
    delete g_source;
    g_source = nullptr;
  }

  if (g_output)
  {
    delete g_output;
    g_output = nullptr;
  }
}

static bool scanMusicFolder()
{
  g_playlist.clear();

  if (!g_sdReady)
  {
    return false;
  }

  File musicDir = SD.open("/music");
  if (!musicDir || !musicDir.isDirectory())
  {
    Serial.println("music folder not found on SD");
    return false;
  }

  for (;;)
  {
    File entry = musicDir.openNextFile();
    if (!entry)
    {
      break;
    }

    if (!entry.isDirectory())
    {
      String path = String(entry.name());
      if (!path.startsWith("/"))
      {
        path = String("/") + path;
      }

      if (endsWithIgnoreCase(path, ".wav"))
      {
        g_playlist.push_back(path);
      }
    }

    entry.close();
  }

  std::sort(g_playlist.begin(), g_playlist.end());

  Serial.print("WAV files found in /music: ");
  Serial.println((unsigned long)g_playlist.size());
  return !g_playlist.empty();
}

static void moveTrackIndex(int step)
{
  if (g_playlist.empty())
  {
    g_trackIndex = -1;
    return;
  }

  if (g_trackIndex < 0)
  {
    g_trackIndex = 0;
    return;
  }

  int total = (int)g_playlist.size();
  g_trackIndex = (g_trackIndex + step + total) % total;
}

static bool startCurrentTrack()
{
  if (g_playlist.empty())
  {
    return false;
  }

  if (g_trackIndex < 0 || g_trackIndex >= (int)g_playlist.size())
  {
    g_trackIndex = 0;
  }

  stopPlayback();

  const String &trackPath = g_playlist[(size_t)g_trackIndex];
  g_source = new AudioFileSourceSD(trackPath.c_str());
  g_output = new AudioOutputI2S();
  g_output->SetPinout(AUDIO_I2S_BCLK, AUDIO_I2S_LRCK, AUDIO_I2S_DOUT);
  g_output->SetGain(0.2f);

  g_wav = new AudioGeneratorWAV();
  if (!g_wav->begin(g_source, g_output))
  {
    Serial.print("Failed to play WAV: ");
    Serial.println(trackPath);
    stopPlayback();
    return false;
  }

  Serial.print("Now playing: ");
  Serial.println(trackPath);
  return true;
}

bool initAudioPlayer(bool sdReady)
{
  g_sdReady = sdReady;
  g_initialized = true;
  g_isPlaying = false;
  g_trackIndex = -1;
  stopPlayback();

  if (!g_sdReady)
  {
    Serial.println("Audio init skipped: SD unavailable");
    return false;
  }

  return scanMusicFolder();
}

bool enqueueAudioCommand(QueueHandle_t queue, AudioCommandType type)
{
  if (!queue)
  {
    return false;
  }

  AudioCommand cmd = {type};
  return xQueueSend(queue, &cmd, 0) == pdTRUE;
}

void buildAudioStatusJson(String &outJson)
{
  bool isPlayingSnapshot;
  int trackIndexSnapshot;

  portENTER_CRITICAL(&g_audioStateMux);
  isPlayingSnapshot = g_isPlaying;
  trackIndexSnapshot = g_trackIndex;
  portEXIT_CRITICAL(&g_audioStateMux);

  String trackName = "";
  if (trackIndexSnapshot >= 0 && trackIndexSnapshot < (int)g_playlist.size())
  {
    trackName = basenameFromPath(g_playlist[(size_t)trackIndexSnapshot]);
  }

  outJson = "{";
  outJson += "\"initialized\":";
  outJson += (g_initialized ? "true" : "false");
  outJson += ",\"sdReady\":";
  outJson += (g_sdReady ? "true" : "false");
  outJson += ",\"playing\":";
  outJson += (isPlayingSnapshot ? "true" : "false");
  outJson += ",\"trackIndex\":";
  outJson += String(trackIndexSnapshot);
  outJson += ",\"trackCount\":";
  outJson += String((unsigned long)g_playlist.size());
  outJson += ",\"trackName\":\"";
  outJson += trackName;
  outJson += "\"}";
}

void audioTask(void *arg)
{
  QueueHandle_t audioQueue = (QueueHandle_t)arg;
  AudioCommand cmd;

  for (;;)
  {
    while (audioQueue && xQueueReceive(audioQueue, &cmd, 0) == pdTRUE)
    {
      switch (cmd.type)
      {
      case AUDIO_CMD_PLAY:
        portENTER_CRITICAL(&g_audioStateMux);
        g_isPlaying = true;
        if (g_trackIndex < 0 && !g_playlist.empty())
        {
          g_trackIndex = 0;
        }
        portEXIT_CRITICAL(&g_audioStateMux);
        break;

      case AUDIO_CMD_PAUSE:
        portENTER_CRITICAL(&g_audioStateMux);
        g_isPlaying = false;
        portEXIT_CRITICAL(&g_audioStateMux);
        stopPlayback();
        break;

      case AUDIO_CMD_NEXT:
        moveTrackIndex(1);
        if (g_isPlaying)
        {
          if (!startCurrentTrack())
          {
            portENTER_CRITICAL(&g_audioStateMux);
            g_isPlaying = false;
            portEXIT_CRITICAL(&g_audioStateMux);
          }
        }
        break;

      case AUDIO_CMD_PREV:
        moveTrackIndex(-1);
        if (g_isPlaying)
        {
          if (!startCurrentTrack())
          {
            portENTER_CRITICAL(&g_audioStateMux);
            g_isPlaying = false;
            portEXIT_CRITICAL(&g_audioStateMux);
          }
        }
        break;

      default:
        break;
      }
    }

    if (g_isPlaying)
    {
      if (!g_wav)
      {
        if (!startCurrentTrack())
        {
          portENTER_CRITICAL(&g_audioStateMux);
          g_isPlaying = false;
          portEXIT_CRITICAL(&g_audioStateMux);
        }
      }
      else if (g_wav->isRunning())
      {
        if (!g_wav->loop())
        {
          stopPlayback();
          moveTrackIndex(1);
          if (!startCurrentTrack())
          {
            portENTER_CRITICAL(&g_audioStateMux);
            g_isPlaying = false;
            portEXIT_CRITICAL(&g_audioStateMux);
          }
        }
      }
      else
      {
        stopPlayback();
      }
    }

    vTaskDelay(pdMS_TO_TICKS(2));
  }
}
