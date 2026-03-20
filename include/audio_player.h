#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

enum AudioCommandType
{
  AUDIO_CMD_PLAY = 0,
  AUDIO_CMD_PAUSE,
  AUDIO_CMD_NEXT,
  AUDIO_CMD_PREV
};

struct AudioCommand
{
  AudioCommandType type;
};

bool initAudioPlayer(bool sdReady);
void audioTask(void *arg);
bool enqueueAudioCommand(QueueHandle_t queue, AudioCommandType type);
void buildAudioStatusJson(String &outJson);
