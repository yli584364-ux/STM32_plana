#pragma once

#include <Arduino.h>
#include <Adafruit_ST7789.h>

enum FlashStateGroup
{
	FLASH_STATE_IDLE = 0,
	FLASH_STATE_EAT,
	FLASH_STATE_SLEEP,
	FLASH_STATE_WORK,
	FLASH_STATE_PLAY,
	FLASH_STATE_ALL,
	FLASH_STATE_COUNT,
};

extern Adafruit_ST7789 tft;

extern bool sdAvailable;
extern bool useSdImages;
extern bool useSdGifMode;
extern bool flashStateAutoRandomEnabled;

extern size_t imageCount;
extern size_t currentImageIndex;
extern unsigned long lastSwitchTime;
extern unsigned long lastFlashStateRandomSwitchTime;
extern const unsigned long FLASH_STATE_RANDOM_INTERVAL_MS;

extern FlashStateGroup activeFlashStateGroup;

extern const uint8_t MAX_SD_IMAGES;
extern String sdImageNames[];
extern size_t sdImageCount;

extern const uint8_t MAX_GIF_FRAMES;
extern String gifFrameSdNames[];
extern size_t gifFrameSdCount;

extern const uint8_t MAX_GIF_SETS;
extern String gifSetFolders[];
extern size_t gifSetCount;

FlashStateGroup parseStateName(const String &name);
const char *stateNameOf(FlashStateGroup group);
void buildFlashStateGroups();

void scanSdImages();
void scanGifSetsOnSd();
void showCurrentImage();

size_t loadGifFramesFromSdFolder(const char *folder);
void showGifFrameFromSd(const String &path);
void playGifFromSdFolder(const char *folder);

bool pickRandomFlashImageFromActiveState();
