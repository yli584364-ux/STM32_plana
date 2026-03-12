#include "display.h"

#include <SPI.h>
#include <FS.h>
#include <SD.h>
#include "SPIFFS.h"
#include <stdlib.h>
#include <esp_task_wdt.h>

#include "pin.h"
#include "plana.h"
#include "gif_frames.h"
#include "external_flash.h"

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

bool flashStateAutoRandomEnabled = false;
bool sdAvailable = false;
bool useSdImages = false;
bool extFlashAvailable = false;
bool extFlashGifReady = false;
bool useExternalFlashForGif = false;

size_t imageCount = 0;
size_t currentImageIndex = 0;
unsigned long lastSwitchTime = 0;
unsigned long lastFlashStateRandomSwitchTime = 0;
const unsigned long FLASH_STATE_RANDOM_INTERVAL_MS = 5000;

FlashStateGroup activeFlashStateGroup = FLASH_STATE_ALL;
const uint8_t MAX_SD_IMAGES = 64;
String sdImageNames[MAX_SD_IMAGES];
size_t sdImageCount = 0;

const size_t MAX_FLASH_GROUP_IMAGES = 64;
static size_t flashStateGroupCounts[FLASH_STATE_COUNT] = {0};
static size_t flashStateGroupIndices[FLASH_STATE_COUNT][MAX_FLASH_GROUP_IMAGES];

static const uint32_t EXT_GIF_MAGIC = 0x31464745UL; // "EGF1"
static const uint16_t EXT_GIF_VERSION = 1;
static const uint32_t EXT_GIF_DATA_START = 4096;
static ExternalGifHeader g_extGifHeader = {};
static const uint8_t MAX_SYNC_GIF_FRAMES = 120;
static String g_syncGifFramePaths[MAX_SYNC_GIF_FRAMES];
static size_t g_syncGifFrameCount = 0;

static void syncHeartbeatTick(size_t finishedFrames, size_t totalFrames)
{
  static unsigned long lastLogMs = 0;

  // 喂狗并让出 CPU，避免长时间同步期间被判定为卡死。
  esp_task_wdt_reset();
  yield();
  delay(1);

  unsigned long now = millis();
  if ((now - lastLogMs) >= 300)
  {
    Serial.print("GIF sync heartbeat: ");
    Serial.print(finishedFrames);
    Serial.print("/");
    Serial.println(totalFrames);
    lastLogMs = now;
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

static bool validateExternalGifHeader(const ExternalGifHeader &h)
{
  if (h.magic != EXT_GIF_MAGIC || h.version != EXT_GIF_VERSION)
  {
    return false;
  }
  if (h.width != planaWidth || h.height != planaHeight)
  {
    return false;
  }
  if (h.frameCount == 0 || h.frameSizeBytes == 0)
  {
    return false;
  }
  if (h.dataStartAddr < EXT_GIF_DATA_START)
  {
    return false;
  }

  uint64_t endAddr = (uint64_t)h.dataStartAddr + (uint64_t)h.totalDataBytes;
  if (endAddr > (uint64_t)EXT_FLASH_TOTAL_BYTES)
  {
    return false;
  }
  return true;
}

bool refreshExternalGifHeader()
{
  if (!extFlashAvailable)
  {
    extFlashGifReady = false;
    return false;
  }

  ExternalGifHeader h = {};
  if (!extFlashLoadGifHeader(h))
  {
    extFlashGifReady = false;
    return false;
  }

  if (!validateExternalGifHeader(h))
  {
    extFlashGifReady = false;
    return false;
  }

  g_extGifHeader = h;
  extFlashGifReady = true;
  return true;
}

void scanSdImages()
{
  sdImageCount = 0;

  if (!sdAvailable)
  {
    return;
  }

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
    return;
  }

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

size_t getOnboardGifFrameCount()
{
  return gifFrameCount;
}

static bool openGifFrameFromSpiffs(const char *path, File &outFile, String &openedPath)
{
  outFile = SPIFFS.open(path, "rb");
  if (outFile)
  {
    openedPath = String(path);
    return true;
  }

  String altPath = String("/gif_data") + String(path);
  outFile = SPIFFS.open(altPath, "rb");
  if (outFile)
  {
    openedPath = altPath;
    return true;
  }

  return false;
}

static size_t scanGifFramesFromSdForSync()
{
  g_syncGifFrameCount = 0;

  if (!sdAvailable)
  {
    return 0;
  }

  File dir = SD.open("/gif_data");
  if (!dir || !dir.isDirectory())
  {
    return 0;
  }

  File file = dir.openNextFile();
  while (file && g_syncGifFrameCount < MAX_SYNC_GIF_FRAMES)
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
      if (lower.endsWith(".bin"))
      {
        g_syncGifFramePaths[g_syncGifFrameCount++] = name;
      }
    }
    file = dir.openNextFile();
  }

  for (size_t i = 0; i + 1 < g_syncGifFrameCount; ++i)
  {
    for (size_t j = i + 1; j < g_syncGifFrameCount; ++j)
    {
      if (g_syncGifFramePaths[j] < g_syncGifFramePaths[i])
      {
        String t = g_syncGifFramePaths[i];
        g_syncGifFramePaths[i] = g_syncGifFramePaths[j];
        g_syncGifFramePaths[j] = t;
      }
    }
  }

  return g_syncGifFrameCount;
}

static bool drawGifFrameFromSpiffs(const char *path)
{
  File f;
  String openedPath;
  if (!openGifFrameFromSpiffs(path, f, openedPath))
  {
    Serial.print("GIF onboard: frame file not found: ");
    Serial.println(path);
    return false;
  }

  const size_t expectedSize = (size_t)planaWidth * (size_t)planaHeight * 2;
  if (f.size() < expectedSize)
  {
    Serial.print("GIF onboard: frame file too small: ");
    Serial.print(openedPath);
    Serial.print(", size=");
    Serial.print((size_t)f.size());
    Serial.print(", expected=");
    Serial.println(expectedSize);
    f.close();
    return false;
  }

  static uint16_t lineBuf[240];
  for (uint16_t y = 0; y < planaHeight; ++y)
  {
    size_t bytesPerLine = (size_t)planaWidth * 2;
    size_t readBytes = f.read((uint8_t *)lineBuf, bytesPerLine);
    if (readBytes != bytesPerLine)
    {
      f.close();
      return false;
    }
    tft.drawRGBBitmap(0, y, lineBuf, planaWidth, 1);
  }

  f.close();
  return true;
}

static bool drawGifFrameFromExternal(size_t frameIndex)
{
  if (!extFlashGifReady || g_extGifHeader.frameCount == 0)
  {
    return false;
  }

  size_t idx = frameIndex % g_extGifHeader.frameCount;
  uint32_t frameAddr = g_extGifHeader.dataStartAddr + (uint32_t)(idx * g_extGifHeader.frameSizeBytes);
  static uint16_t lineBuf[240];

  for (uint16_t y = 0; y < planaHeight; ++y)
  {
    uint32_t lineAddr = frameAddr + (uint32_t)y * (uint32_t)planaWidth * 2UL;
    if (!extFlashReadBytes(lineAddr, (uint8_t *)lineBuf, (size_t)planaWidth * 2))
    {
      return false;
    }
    tft.drawRGBBitmap(0, y, lineBuf, planaWidth, 1);
  }

  return true;
}

void playGifFromOnboardFlash()
{
  if (gifFrameCount == 0)
  {
    Serial.println("GIF onboard: no frames");
    return;
  }

  const uint16_t frameDelayMs = 80;
  for (size_t i = 0; i < gifFrameCount; ++i)
  {
    if (!drawGifFrameFromSpiffs(gifFrames[i]))
    {
      Serial.print("GIF onboard: failed frame ");
      Serial.println(i);
      return;
    }
    delay(frameDelayMs);
  }
}

void playGifFromExternalFlash()
{
  if (!extFlashGifReady)
  {
    Serial.println("GIF external: no valid data");
    return;
  }

  const uint16_t frameDelayMs = 80;
  for (size_t i = 0; i < g_extGifHeader.frameCount; ++i)
  {
    if (!drawGifFrameFromExternal(i))
    {
      Serial.print("GIF external: failed frame ");
      Serial.println(i);
      return;
    }
    delay(frameDelayMs);
  }
}

bool syncGifDataToExternalFlash()
{
  if (!extFlashAvailable)
  {
    return false;
  }

  const size_t frameCountFromSd = scanGifFramesFromSdForSync();
  const bool useSdSource = frameCountFromSd > 0;
  const size_t sourceFrameCount = useSdSource ? frameCountFromSd : gifFrameCount;
  if (sourceFrameCount == 0)
  {
    Serial.println("GIF sync: no frame source from SD or onboard");
    return false;
  }

  const uint32_t frameSize = (uint32_t)planaWidth * (uint32_t)planaHeight * 2UL;
  const uint32_t totalBytes = frameSize * (uint32_t)sourceFrameCount;
  const uint32_t endAddr = EXT_GIF_DATA_START + totalBytes;
  if (endAddr > EXT_FLASH_TOTAL_BYTES)
  {
    return false;
  }

  if (!extFlashEraseRange(0, EXT_GIF_DATA_START + totalBytes))
  {
    return false;
  }

  uint8_t *buf = (uint8_t *)malloc(1024);
  if (!buf)
  {
    return false;
  }

  uint32_t writeAddr = EXT_GIF_DATA_START;
  for (size_t i = 0; i < sourceFrameCount; ++i)
  {
    File f;
    if (useSdSource)
    {
      f = SD.open(g_syncGifFramePaths[i], "rb");
      if (!f)
      {
        Serial.print("GIF sync: failed to open SD frame: ");
        Serial.println(g_syncGifFramePaths[i]);
        free(buf);
        return false;
      }
    }
    else
    {
      String openedPath;
      if (!openGifFrameFromSpiffs(gifFrames[i], f, openedPath))
      {
        Serial.print("GIF sync: frame file not found: ");
        Serial.println(gifFrames[i]);
        free(buf);
        return false;
      }
    }

    uint32_t frameWritten = 0;
    while (frameWritten < frameSize)
    {
      size_t need = frameSize - frameWritten;
      if (need > 1024)
      {
        need = 1024;
      }
      size_t got = f.read(buf, need);
      if (got != need)
      {
        f.close();
        free(buf);
        return false;
      }

      if (!extFlashWriteBytes(writeAddr, buf, got))
      {
        f.close();
        free(buf);
        return false;
      }

      writeAddr += (uint32_t)got;
      frameWritten += (uint32_t)got;
    }

    f.close();
    syncHeartbeatTick(i + 1, sourceFrameCount);
  }

  free(buf);

  ExternalGifHeader hdr = {};
  hdr.magic = EXT_GIF_MAGIC;
  hdr.version = EXT_GIF_VERSION;
  hdr.width = planaWidth;
  hdr.height = planaHeight;
  hdr.frameCount = (uint32_t)sourceFrameCount;
  hdr.frameSizeBytes = frameSize;
  hdr.dataStartAddr = EXT_GIF_DATA_START;
  hdr.totalDataBytes = totalBytes;

  if (!extFlashStoreGifHeader(hdr))
  {
    return false;
  }

  return refreshExternalGifHeader();
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
