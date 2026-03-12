#include "display.h"

#include <SPI.h>
#include <FS.h>
#include <SD.h>
#include "SPIFFS.h"
#include <stdlib.h>

#include "pin.h"
#include "plana.h"

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

bool sdAvailable = false;
bool useSdImages = false;
bool useSdGifMode = false;
bool flashStateAutoRandomEnabled = false;

size_t imageCount = 0;
size_t currentImageIndex = 0;
unsigned long lastSwitchTime = 0;
unsigned long lastFlashStateRandomSwitchTime = 0;
const unsigned long FLASH_STATE_RANDOM_INTERVAL_MS = 5000;

FlashStateGroup activeFlashStateGroup = FLASH_STATE_ALL;

const uint8_t MAX_SD_IMAGES = 64;
String sdImageNames[MAX_SD_IMAGES];
size_t sdImageCount = 0;

const uint8_t MAX_GIF_FRAMES = 80;
String gifFrameSdNames[MAX_GIF_FRAMES];
size_t gifFrameSdCount = 0;

const uint8_t MAX_GIF_SETS = 16;
String gifSetFolders[MAX_GIF_SETS];
size_t gifSetCount = 0;

const size_t MAX_FLASH_GROUP_IMAGES = 64;
static size_t flashStateGroupCounts[FLASH_STATE_COUNT] = {0};
static size_t flashStateGroupIndices[FLASH_STATE_COUNT][MAX_FLASH_GROUP_IMAGES];

static FlashStateGroup inferStateGroupFromLabel(const String &label)
{
  String lower = label;
  lower.toLowerCase();

  if (lower.indexOf("idle") >= 0 || lower.indexOf("rest") >= 0)
    return FLASH_STATE_IDLE;
  if (lower.indexOf("eat") >= 0 || lower.indexOf("food") >= 0 || lower.indexOf("meal") >= 0)
    return FLASH_STATE_EAT;
  if (lower.indexOf("sleep") >= 0 || lower.indexOf("nap") >= 0)
    return FLASH_STATE_SLEEP;
  if (lower.indexOf("work") >= 0 || lower.indexOf("job") >= 0 || lower.indexOf("office") >= 0)
    return FLASH_STATE_WORK;
  if (lower.indexOf("play") >= 0 || lower.indexOf("game") >= 0 || lower.indexOf("fun") >= 0)
    return FLASH_STATE_PLAY;

  return FLASH_STATE_IDLE;
}

FlashStateGroup parseStateName(const String &name)
{
  String lower = name;
  lower.toLowerCase();

  if (lower == "idle")
    return FLASH_STATE_IDLE;
  if (lower == "eat" || lower == "food")
    return FLASH_STATE_EAT;
  if (lower == "sleep")
    return FLASH_STATE_SLEEP;
  if (lower == "work")
    return FLASH_STATE_WORK;
  if (lower == "play")
    return FLASH_STATE_PLAY;
  if (lower == "all")
    return FLASH_STATE_ALL;

  return FLASH_STATE_COUNT;
}

const char *stateNameOf(FlashStateGroup group)
{
  switch (group)
  {
  case FLASH_STATE_IDLE:
    return "idle";
  case FLASH_STATE_EAT:
    return "eat";
  case FLASH_STATE_SLEEP:
    return "sleep";
  case FLASH_STATE_WORK:
    return "work";
  case FLASH_STATE_PLAY:
    return "play";
  case FLASH_STATE_ALL:
    return "all";
  default:
    return "unknown";
  }
}

void buildFlashStateGroups()
{
  for (size_t g = 0; g < FLASH_STATE_COUNT; ++g)
  {
    flashStateGroupCounts[g] = 0;
  }

  for (size_t i = 0; i < planaImageCount && i < MAX_FLASH_GROUP_IMAGES; ++i)
  {
    String stateLabel = String(planaImageStates[i]);
    FlashStateGroup g = inferStateGroupFromLabel(stateLabel);

    if (flashStateGroupCounts[g] < MAX_FLASH_GROUP_IMAGES)
    {
      flashStateGroupIndices[g][flashStateGroupCounts[g]++] = i;
    }
    if (flashStateGroupCounts[FLASH_STATE_ALL] < MAX_FLASH_GROUP_IMAGES)
    {
      flashStateGroupIndices[FLASH_STATE_ALL][flashStateGroupCounts[FLASH_STATE_ALL]++] = i;
    }
  }
}

static bool readNextArrayValue(File &f, uint16_t &out)
{
  char token[16];
  size_t len = 0;
  bool inToken = false;

  while (f.available())
  {
    char c = (char)f.read();
    bool isTokenChar = ((c >= '0' && c <= '9') ||
                        (c >= 'a' && c <= 'f') ||
                        (c >= 'A' && c <= 'F') ||
                        c == 'x' || c == 'X');

    if (isTokenChar)
    {
      if (len < sizeof(token) - 1)
      {
        token[len++] = c;
      }
      inToken = true;
    }
    else if (inToken)
    {
      break;
    }
  }

  if (!inToken)
  {
    return false;
  }

  token[len] = '\0';
  out = (uint16_t)(strtoul(token, nullptr, 0) & 0xFFFFUL);
  return true;
}

static bool drawArrImageFromFile(File &f)
{
  const uint16_t chunkRows = 10;
  const size_t totalPixels = (size_t)planaWidth * (size_t)planaHeight;
  const size_t chunkPixels = (size_t)planaWidth * (size_t)chunkRows;

  uint16_t *chunkBuf = (uint16_t *)malloc(chunkPixels * sizeof(uint16_t));
  if (!chunkBuf)
  {
    Serial.println("SD image: chunk buffer malloc failed");
    return false;
  }

  bool parseOk = true;
  size_t parsedCount = 0;
  uint16_t y = 0;

  tft.fillScreen(ST77XX_BLACK);

  while (y < planaHeight && parseOk)
  {
    uint16_t rowsThisChunk = (uint16_t)(planaHeight - y);
    if (rowsThisChunk > chunkRows)
    {
      rowsThisChunk = chunkRows;
    }

    size_t pixelsThisChunk = (size_t)planaWidth * (size_t)rowsThisChunk;

    for (size_t i = 0; i < pixelsThisChunk; ++i)
    {
      if (!readNextArrayValue(f, chunkBuf[i]))
      {
        parseOk = false;
        break;
      }
      parsedCount++;
    }

    if (!parseOk)
    {
      break;
    }

    tft.drawRGBBitmap(0, y, chunkBuf, planaWidth, rowsThisChunk);
    y = (uint16_t)(y + rowsThisChunk);
  }

  free(chunkBuf);

  if (!parseOk || parsedCount != totalPixels)
  {
    Serial.print("SD image: array value count mismatch, parsed=");
    Serial.print(parsedCount);
    Serial.print(", expected=");
    Serial.println(totalPixels);
    return false;
  }

  return true;
}

void scanSdImages()
{
  sdImageCount = 0;

  File root = SD.open("/");
  if (!root)
  {
    Serial.println("Failed to open SD root");
    return;
  }

  File file = root.openNextFile();
  while (file && sdImageCount < MAX_SD_IMAGES)
  {
    if (!file.isDirectory())
    {
      String name = file.name();
      if (!name.startsWith("/"))
      {
        name = String("/") + name;
      }
      String lower = name;
      lower.toLowerCase();
      if (lower.endsWith(".arr"))
      {
        sdImageNames[sdImageCount++] = name;
        Serial.print("Found SD image: ");
        Serial.println(name);
      }
    }
    file = root.openNextFile();
  }
}

void scanGifSetsOnSd()
{
  gifSetCount = 0;

  if (!sdAvailable)
  {
    return;
  }

  File root = SD.open("/");
  if (!root)
  {
    Serial.println("Failed to open SD root when scanning GIF sets");
    return;
  }

  File file = root.openNextFile();
  while (file && gifSetCount < MAX_GIF_SETS)
  {
    if (file.isDirectory())
    {
      String name = file.name();
      if (!name.startsWith("/"))
      {
        name = String("/") + name;
      }
      String lower = name;
      lower.toLowerCase();
      if (lower.startsWith("/gif_"))
      {
        gifSetFolders[gifSetCount++] = name;
      }
    }
    file = root.openNextFile();
  }

  for (size_t i = 0; i + 1 < gifSetCount; ++i)
  {
    for (size_t j = i + 1; j < gifSetCount; ++j)
    {
      if (gifSetFolders[j] < gifSetFolders[i])
      {
        String tmp = gifSetFolders[i];
        gifSetFolders[i] = gifSetFolders[j];
        gifSetFolders[j] = tmp;
      }
    }
  }
}

void showCurrentImage()
{
  if (imageCount == 0)
  {
    return;
  }

  if (currentImageIndex >= imageCount)
  {
    currentImageIndex = 0;
  }

  if (useSdImages && sdAvailable && sdImageCount > 0)
  {
    String path = sdImageNames[currentImageIndex % sdImageCount];
    File f = SD.open(path, "r");
    if (!f)
    {
      Serial.print("Failed to open SD image file: ");
      Serial.println(path);
      return;
    }
    if (!drawArrImageFromFile(f))
    {
      Serial.println("Failed to draw SD ARR image");
    }
    f.close();
  }
  else
  {
    if (planaImageCount == 0)
    {
      return;
    }

    const char *path = planaImages[currentImageIndex % planaImageCount];
    File f = SPIFFS.open(path, "rb");
    if (!f)
    {
      Serial.print("Failed to open image file: ");
      Serial.println(path);
      return;
    }

    static uint16_t lineBuf[240];
    tft.fillScreen(ST77XX_BLACK);

    for (uint16_t y = 0; y < planaHeight; ++y)
    {
      size_t toRead = (size_t)planaWidth * 2;
      size_t readBytes = f.read((uint8_t *)lineBuf, toRead);
      if (readBytes != toRead)
      {
        break;
      }
      tft.drawRGBBitmap(0, y, lineBuf, planaWidth, 1);
    }

    f.close();
  }
}

size_t loadGifFramesFromSdFolder(const char *folder)
{
  gifFrameSdCount = 0;

  if (!sdAvailable)
  {
    Serial.println("GIF: SD not available");
    return 0;
  }

  File dir = SD.open(folder);
  if (!dir || !dir.isDirectory())
  {
    Serial.print("GIF: not a directory: ");
    Serial.println(folder);
    return 0;
  }

  File file = dir.openNextFile();
  while (file && gifFrameSdCount < MAX_GIF_FRAMES)
  {
    if (!file.isDirectory())
    {
      String name = file.name();
      String lower = name;
      lower.toLowerCase();
      if (lower.endsWith(".arr"))
      {
        if (!name.startsWith("/"))
        {
          name = String("/") + name;
        }
        gifFrameSdNames[gifFrameSdCount++] = name;
      }
    }
    file = dir.openNextFile();
  }

  for (size_t i = 0; i + 1 < gifFrameSdCount; ++i)
  {
    for (size_t j = i + 1; j < gifFrameSdCount; ++j)
    {
      if (gifFrameSdNames[j] < gifFrameSdNames[i])
      {
        String tmp = gifFrameSdNames[i];
        gifFrameSdNames[i] = gifFrameSdNames[j];
        gifFrameSdNames[j] = tmp;
      }
    }
  }

  return gifFrameSdCount;
}

void showGifFrameFromSd(const String &path)
{
  File f = SD.open(path.c_str(), "r");
  if (!f)
  {
    Serial.print("Failed to open GIF frame from SD: ");
    Serial.println(path);
    return;
  }

  if (!drawArrImageFromFile(f))
  {
    Serial.println("Failed to draw GIF ARR frame from SD");
  }

  f.close();
}

void playGifFromSdFolder(const char *folder)
{
  size_t count = loadGifFramesFromSdFolder(folder);
  if (count == 0)
  {
    Serial.println("playGifFromSdFolder: no frames");
    return;
  }

  const uint16_t frameDelayMs = 80;
  for (size_t i = 0; i < count; ++i)
  {
    showGifFrameFromSd(gifFrameSdNames[i]);
    delay(frameDelayMs);
  }
}

bool pickRandomFlashImageFromActiveState()
{
  if (activeFlashStateGroup >= FLASH_STATE_COUNT)
  {
    return false;
  }

  size_t count = flashStateGroupCounts[activeFlashStateGroup];
  if (count == 0)
  {
    return false;
  }

  size_t slot = (size_t)random((long)count);
  currentImageIndex = flashStateGroupIndices[activeFlashStateGroup][slot];
  showCurrentImage();
  lastSwitchTime = millis();
  lastFlashStateRandomSwitchTime = lastSwitchTime;
  return true;
}
