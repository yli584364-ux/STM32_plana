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

extern bool flashStateAutoRandomEnabled;
extern bool sdAvailable;
extern bool useSdImages;
extern bool extFlashAvailable;
extern bool extFlashGifReady;
extern bool useExternalFlashForGif;

extern size_t imageCount;
extern size_t currentImageIndex;
extern unsigned long lastSwitchTime;
extern unsigned long lastFlashStateRandomSwitchTime;
extern const unsigned long FLASH_STATE_RANDOM_INTERVAL_MS;

extern FlashStateGroup activeFlashStateGroup;
extern const uint8_t MAX_SD_IMAGES;
extern String sdImageNames[];
extern size_t sdImageCount;

FlashStateGroup parseStateName(const String &name);
const char *stateNameOf(FlashStateGroup group);
void buildFlashStateGroups();
void scanSdImages();
void scanSdGifFolders();

void showCurrentImage();

size_t getOnboardGifFrameCount();
bool isOnboardGifReady();
void playGifFromOnboardFlash();
void playGifFromExternalFlash();
bool playGifFromSd(size_t gifIndex);
bool syncGifDataToExternalFlash();
bool refreshExternalGifHeader();

size_t getSdGifCount();
size_t getSdGifActiveIndex();
size_t getSdGifFrameCount(size_t gifIndex);

bool pickRandomFlashImageFromActiveState();
