#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <WiFi.h>              // WiFi AP + WebServer
#include <WebServer.h>
#include <FS.h>
#include <SD.h>
#include "SPIFFS.h"           // SPIFFS 文件系统
#include <stdlib.h>

// 为排查内存与稳定性问题，默认关闭蓝牙栈。
// 需要恢复蓝牙时改为 1。
#define ENABLE_BLUETOOTH 0

#if ENABLE_BLUETOOTH
#include "BluetoothSerial.h"      // ESP32 经典蓝牙串口
#include "BluetoothA2DPSource.h"  // ESP32 A2DP 音频源
#endif

#include "plana.h"  // 由 tools/convert_plana.py 生成

// 根据你的实际接线修改以下引脚定义
#define TFT_CS 5    // 屏幕 CS（片选）
#define TFT_DC 2    // 屏幕 DC / RS
#define TFT_RST 4   // 屏幕 RST（如接到 EN 或固定复位，可设为 -1）
#define TFT_BL 15   // 背光控制引脚（如果直接接 3.3V，可删掉相关代码）

// SD 卡 SPI 片选引脚（SCK/MOSI/MISO 复用 VSPI: 18/23/19）
#define SD_CS 13
static const uint32_t SD_SPI_FREQ = 4000000; // 4MHz，优先稳定性

// 使用硬件 SPI (ESP32 VSPI: SCK=18, MOSI=23, MISO=19)
// 如需改成 HSPI 或自定义 SPI 引脚，可以改用另一个构造函数
Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

// 蓝牙串口对象
#if ENABLE_BLUETOOTH
BluetoothSerial SerialBT;
#endif

// 简单 Web 服务器
WebServer server(80);

// SD 卡相关
static bool sdAvailable = false;
static const uint8_t MAX_SD_IMAGES = 64;
static String sdImageNames[MAX_SD_IMAGES];
static size_t sdImageCount = 0;

// GIF 播放用：从 SD 卡目录加载帧列表（单个 GIF 的帧）
static const uint8_t MAX_GIF_FRAMES = 80;
static String gifFrameSdNames[MAX_GIF_FRAMES];
static size_t gifFrameSdCount = 0;

// 多个 GIF 集合：gif_1、gif_2 等目录列表
static const uint8_t MAX_GIF_SETS = 16;
static String gifSetFolders[MAX_GIF_SETS];
static size_t gifSetCount = 0;

// 实际可用的图片数量（优先 SD，其次 SPIFFS）。
// 在 "普通图片模式" 下：表示当前模式下图片数量；
// 在 "SD GIF 模式" 下且使用 SD 时：表示可选 GIF 集合数量。
static size_t imageCount = 0;

// 当前是否使用 SD 卡图片（false = SPIFFS/Flash，true = SD 卡）
static bool useSdImages = false;

// 当前 SD 模式：false = 播放 SD 上单张图片（sdImageNames）
//              true  = 播放 SD 上 GIF（gif_1、gif_2 ...）
static bool useSdGifMode = false;

// 用于轮播图片的索引与定时
static size_t currentImageIndex = 0;
static unsigned long lastSwitchTime = 0;

// SPIFFS 图片状态分组：idle/eat/sleep/work/play
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

static const size_t MAX_FLASH_GROUP_IMAGES = 64;
static size_t flashStateGroupCounts[FLASH_STATE_COUNT] = {0};
static size_t flashStateGroupIndices[FLASH_STATE_COUNT][MAX_FLASH_GROUP_IMAGES];

static FlashStateGroup activeFlashStateGroup = FLASH_STATE_ALL;
static bool flashStateAutoRandomEnabled = false;
static unsigned long lastFlashStateRandomSwitchTime = 0;
static const unsigned long FLASH_STATE_RANDOM_INTERVAL_MS = 5000;

static FlashStateGroup parseStateName(const String &name)
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

static const char *stateNameOf(FlashStateGroup group)
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

  // 未标注时默认归入 idle
  return FLASH_STATE_IDLE;
}

static void buildFlashStateGroups()
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

  Serial.print("Flash state groups idle/eat/sleep/work/play/all = ");
  Serial.print(flashStateGroupCounts[FLASH_STATE_IDLE]);
  Serial.print("/");
  Serial.print(flashStateGroupCounts[FLASH_STATE_EAT]);
  Serial.print("/");
  Serial.print(flashStateGroupCounts[FLASH_STATE_SLEEP]);
  Serial.print("/");
  Serial.print(flashStateGroupCounts[FLASH_STATE_WORK]);
  Serial.print("/");
  Serial.print(flashStateGroupCounts[FLASH_STATE_PLAY]);
  Serial.print("/");
  Serial.println(flashStateGroupCounts[FLASH_STATE_ALL]);
}

// 从数组文本 .arr 中解析下一个像素值（支持 0x1234 或十进制）
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

// 从 .arr 文件按块（10 行）读取并绘制整张图片
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
//================== A2DP 音频（ESP32 作为蓝牙音频发送端） ==================

#if ENABLE_BLUETOOTH
BluetoothA2DPSource a2dpSource;

static const int a2dpSampleRate = 44100;    // 44.1kHz 立体声 16bit
static const float a2dpToneFreq = 440.0f;   // 440Hz 正弦波
static float a2dpPhase = 0.0f;

// 回调：生成要发送给蓝牙设备的 PCM 音频数据
int32_t get_sound_data(uint8_t *data, int32_t byteCount)
{
  int16_t *samples = (int16_t *)data;
  int32_t sampleCount = byteCount / 2; // 16bit = 2 字节

  float phaseInc = 2.0f * PI * a2dpToneFreq / (float)a2dpSampleRate;

  for (int32_t i = 0; i < sampleCount; i += 2)
  {
    // 生成一个比较小的幅度，避免过载
    int16_t v = (int16_t)(sin(a2dpPhase) * 8000.0f);

    // 单声道：左声道有信号，右声道静音
    samples[i] = v;     // Left
    samples[i + 1] = v; // Right

    a2dpPhase += phaseInc;
    if (a2dpPhase > 2.0f * PI)
    {
      a2dpPhase -= 2.0f * PI;
    }
  }

  return byteCount; // 告诉库我们填充了多少字节
}
#endif


// ================== SD 卡图片播放 ==================

// 扫描 SD 根目录中的预处理 .arr 图片文件
static void scanSdImages()
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
      // 确保文件名以 "/" 开头，统一路径格式
      if (!name.startsWith("/"))
      {
        name = String("/") + name;
      }
      String lower = name;
      lower.toLowerCase();
      // 现在我们期望 SD 卡上存放的是 Python 已经转换好的 RGB565 数组文本 .arr 文件
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

// 扫描 SD 根目录中的 gif_X 目录，作为可选 GIF 集合
static void scanGifSetsOnSd()
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

  // 按目录名字排序，确保 gif_1, gif_2 ... 顺序
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

  Serial.print("GIF sets on SD: ");
  Serial.println(gifSetCount);
}

// 显示当前索引图片：根据 useSdImages 在 SD 与 SPIFFS 之间切换
static void showCurrentImage()
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
    // 使用 SD 上的 RGB565 数组文本 .arr 文件
    String path = sdImageNames[currentImageIndex % sdImageCount];
    Serial.print("Draw ARR from SD: ");
    Serial.println(path);

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
    // 使用 SPIFFS 里的 RGB565 .bin
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

    static uint16_t lineBuf[240]; // 假设宽度不超过 240

    tft.fillScreen(ST77XX_BLACK);

    for (uint16_t y = 0; y < planaHeight; ++y)
    {
      size_t toRead = (size_t)planaWidth * 2;
      size_t readBytes = f.read((uint8_t *)lineBuf, toRead);
      if (readBytes != toRead)
      {
        break; // 数据不足，提前结束
      }
      tft.drawRGBBitmap(0, y, lineBuf, planaWidth, 1);
    }

    f.close();
  }
}

// ======== GIF 播放（从 SD 卡目录读取 .arr 帧） ========

// 从指定 SD 目录加载 GIF 帧文件列表（.arr）
static size_t loadGifFramesFromSdFolder(const char *folder)
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
        // 确保路径以 "/" 开头，兼容不同返回形式
        if (!name.startsWith("/"))
        {
          name = String("/") + name;
        }
        gifFrameSdNames[gifFrameSdCount++] = name;
      }
    }
    file = dir.openNextFile();
  }

  // 简单按文件名排序，要求帧文件按名字顺序排列
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

  Serial.print("GIF: loaded ");
  Serial.print(gifFrameSdCount);
  Serial.print(" frames from ");
  Serial.println(folder);

  return gifFrameSdCount;
}

// 显示一帧 GIF（从 SD 读取 RGB565 数组 .arr）
static void showGifFrameFromSd(const String &path)
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

static bool pickRandomFlashImageFromActiveState()
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


//================= Web 服务器处理函数 ==================

// Web 处理函数
static void handleRoot()
{
  // 尝试从 SPIFFS 读取 /index.html
  File f = SPIFFS.open("/index.html", "r");
  if (!f)
  {
    // 简单降级：如果文件不存在，就返回一个非常小的页面
    server.send(200, "text/html; charset=utf-8",
                "<html><body><h1>ESP LCD</h1><p>No index.html on SPIFFS</p></body></html>");
    return;
  }

  const size_t fileSize = (size_t)f.size();
  Serial.print("Serving /index.html, size=");
  Serial.print(fileSize);
  Serial.print(", freeHeap=");
  Serial.println(ESP.getFreeHeap());

  // 使用流式发送，避免把整个 HTML 拼接到堆内存导致 reserve 失败
  size_t sent = server.streamFile(f, "text/html; charset=utf-8");
  f.close();

  Serial.print("/index.html sent bytes=");
  Serial.println(sent);
}

// 播放一次 GIF（从指定 SD 目录按顺序播放所有帧）
static void playGifFromSdFolder(const char *folder)
{
  size_t count = loadGifFramesFromSdFolder(folder);
  if (count == 0)
  {
    Serial.println("playGifFromSdFolder: no frames");
    return;
  }

  const uint16_t frameDelayMs = 80; // 每帧间隔，按效果可自行调整

  for (size_t i = 0; i < count; ++i)
  {
    showGifFrameFromSd(gifFrameSdNames[i]);
    delay(frameDelayMs);
  }
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
  server.send(200, "text/plain", "OK");
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
  server.send(200, "text/plain", "OK");
}

static void handleSet()
{
  flashStateAutoRandomEnabled = false;

  if (!server.hasArg("i"))
  {
    server.send(400, "text/plain", "missing i");
    return;
  }
  int idx = server.arg("i").toInt();
  if (idx < 0 || (size_t)idx >= imageCount)
  {
    server.send(400, "text/plain", "out of range");
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
  server.send(200, "text/plain", "OK");
}

static void handleState()
{
  if (!server.hasArg("name"))
  {
    server.send(400, "text/plain", "missing name");
    return;
  }

  String name = server.arg("name");
  name.trim();
  name.toLowerCase();

  if (name == "off" || name == "manual")
  {
    flashStateAutoRandomEnabled = false;
    activeFlashStateGroup = FLASH_STATE_ALL;
    server.send(200, "text/plain", "OFF");
    return;
  }

  FlashStateGroup g = parseStateName(name);
  if (g == FLASH_STATE_COUNT)
  {
    server.send(400, "text/plain", "invalid state");
    return;
  }

  activeFlashStateGroup = g;
  flashStateAutoRandomEnabled = true;

  if (useSdImages)
  {
    // 状态组随机仅针对 SPIFFS 图片；当前在 SD 模式则只记录状态。
    server.send(200, "text/plain", stateNameOf(activeFlashStateGroup));
    return;
  }

  if (!pickRandomFlashImageFromActiveState())
  {
    flashStateAutoRandomEnabled = false;
    server.send(404, "text/plain", "NO_STATE_IMAGE");
    return;
  }

  server.send(200, "text/plain", stateNameOf(activeFlashStateGroup));
}

// 切换图片来源：在 SPIFFS(Flash) 与 SD 卡图片之间互相切换
static void handlePhotoToggle()
{
  flashStateAutoRandomEnabled = false;

  // 如果两边都没有图片，就直接返回
  if (!sdAvailable || sdImageCount == 0)
  {
    // 只有 SPIFFS 或都没有 SD，则维持当前模式
    server.send(200, "text/plain", "NO_SD");
    Serial.println("SD_ERROR");
    return;
  }

  if (planaImageCount == 0)
  {
    // 没有 SPIFFS 图片，只能使用 SD
    useSdImages = true;
    imageCount = sdImageCount;
    currentImageIndex %= imageCount;
    showCurrentImage();
    lastSwitchTime = millis();
    server.send(200, "text/plain", "SD_ONLY");
    return;
  }

  // 两边都有图片时在 Flash(SPIFFS) / SD 间来回切换
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
      Serial.println("Switch to SD GIF mode");
    }
    else
    {
      imageCount = sdImageCount;
      Serial.println("Switch to SD images");
    }
  }
  else
  {
    imageCount = planaImageCount;
    Serial.println("Switch to SPIFFS images");
  }

  if (imageCount == 0)
  {
    server.send(200, "text/plain", "NO_IMG");
    return;
  }

  if (currentImageIndex >= imageCount)
  {
    currentImageIndex = 0;
  }

  showCurrentImage();
  lastSwitchTime = millis();
  server.send(200, "text/plain", useSdImages ? "SD" : "FLASH");
}

// HTTP 接口：/gif -> 从 SD 卡 gif_X 目录播放一次 GIF
static void handleGif()
{
  flashStateAutoRandomEnabled = false;

  if (gifSetCount == 0)
  {
    scanGifSetsOnSd();
  }

  if (gifSetCount == 0)
  {
    server.send(200, "text/plain", "NO_GIF_SET");
    return;
  }

  size_t idx = 0;

  // 可选参数：/gif?id=1 -> 选择第 1 个 GIF 集合（从 1 开始更加直观）
  if (server.hasArg("id"))
  {
    int id = server.arg("id").toInt();
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
  server.send(200, "text/plain", "GIF_OK");
}

// HTTP 接口：/sdmode -> 切换 SD 普通图片 / SD GIF 模式
static void handleSdModeToggle()
{
  flashStateAutoRandomEnabled = false;

  if (!sdAvailable)
  {
    server.send(200, "text/plain", "NO_SD");
    Serial.println("SD_ERROR");
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
      Serial.println("Now in SD GIF mode");
    }
    else
    {
      imageCount = sdImageCount;
      Serial.println("Now in SD still-image mode");
    }
  }

  server.send(200, "text/plain", useSdGifMode ? "GIF" : "IMAGE");
}

void setup()
{
  Serial.begin(115200); // 调试串口

  // 初始化 SPIFFS
  if (!SPIFFS.begin(true))
  {
    Serial.println("SPIFFS mount failed");
  }

  randomSeed((uint32_t)micros());

  if (TFT_BL >= 0)
  {
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH); // 打开背光
  }

  // 共享 SPI 总线场景：先把 TFT CS 拉高，避免 SD 初始化期间总线竞争
  pinMode(TFT_CS, OUTPUT);
  digitalWrite(TFT_CS, HIGH);

  // 先释放 SD 片选
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);

  // 初始化 240x240 分辨率
  tft.init(240, 240);
  tft.setRotation(1); // 0~3，按需要选择显示方向

  // 初始化 SD 卡并扫描 JPG 图片
  // 显式初始化 VSPI 总线，并把 SD 频率降到 4MHz，减小线材/电源压力
  SPI.begin(18, 19, 23); // SCK=18, MISO=19, MOSI=23

  if (SD.begin(SD_CS, SPI, SD_SPI_FREQ))
  {
    sdAvailable = true;
    Serial.print("SD card initialized, CS=");
    Serial.print(SD_CS);
    Serial.print(", freq=");
    Serial.println(SD_SPI_FREQ);
    scanSdImages();
  }
  else
  {
    Serial.println("SD card initialization failed");
  }

  // 先显示第一张图片（索引 0）
  // 默认从 SPIFFS(Flash) 显示，如果没有 SPIFFS 图片则退回到 SD
  if (planaImageCount > 0)
  {
    useSdImages = false;
    imageCount = planaImageCount;
    buildFlashStateGroups();
    Serial.print("Use images from SPIFFS (Flash), count: ");
    Serial.println(imageCount);
  }
  else if (sdAvailable && sdImageCount > 0)
  {
    useSdImages = true;
    imageCount = sdImageCount;
    Serial.print("Use images from SD, count: ");
    Serial.println(imageCount);
  }
  else
  {
    imageCount = 0;
    Serial.println("No images found on SPIFFS or SD");
  }

  showCurrentImage();
  lastSwitchTime = millis();

  // 启动 WiFi AP + WebServer
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false); // 降低 AP 场景下的响应抖动
  const char *ssid = "ESP_LCD_AP";
  const char *password = "12345678"; // 简单示例，实际可自行修改
  WiFi.softAP(ssid, password);
  IPAddress ip = WiFi.softAPIP();
  Serial.print("WiFi AP SSID: ");
  Serial.println(ssid);
  Serial.print("AP IP address: ");
  Serial.println(ip);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/next", HTTP_GET, handleNext);
  server.on("/prev", HTTP_GET, handlePrev);
  server.on("/set", HTTP_GET, handleSet);
  server.on("/state", HTTP_GET, handleState);
  server.on("/photo", HTTP_GET, handlePhotoToggle);
  server.on("/gif", HTTP_GET, handleGif);
  server.on("/sdmode", HTTP_GET, handleSdModeToggle);
  server.begin();
  Serial.println("HTTP server started");

#if ENABLE_BLUETOOTH
  // 启动蓝牙串口，设备名可在此修改
  SerialBT.begin("ESP_LCD");
  Serial.println("Bluetooth started, device name: ESP_LCD");
#else
  Serial.println("Bluetooth disabled");
#endif
}

void loop()
{
  unsigned long now = millis();

  // 处理 HTTP 请求
  server.handleClient();

  if (flashStateAutoRandomEnabled && !useSdImages && imageCount > 0)
  {
    if ((now - lastFlashStateRandomSwitchTime) >= FLASH_STATE_RANDOM_INTERVAL_MS)
    {
      if (!pickRandomFlashImageFromActiveState())
      {
        flashStateAutoRandomEnabled = false;
      }
    }
  }

#if ENABLE_BLUETOOTH
  // 处理来自手机的蓝牙命令
  // 建议在手机上使用 "Serial Bluetooth Terminal" 等 APP 连接后发送字符：
  // 'n' 或 'N' -> 下一张
  // 'p' 或 'P' -> 上一张
  // 数字 '0'..'9' -> 跳转到对应编号（存在则跳转）
  while (SerialBT.available() > 0)
  {
    char cmd = SerialBT.read();

    bool needUpdate = false;

    if (cmd == 'n' || cmd == 'N')
    {
      currentImageIndex++;
      if (currentImageIndex >= imageCount)
      {
        currentImageIndex = 0;
      }
      needUpdate = true;
    }
    else if (cmd == 'p' || cmd == 'P')
    {
      if (currentImageIndex == 0)
      {
        currentImageIndex = (imageCount == 0 ? 0 : imageCount - 1);
      }
      else
      {
        currentImageIndex--;
      }
      needUpdate = true;
    }
    else if (cmd >= '0' && cmd <= '9')
    {
      size_t idx = (size_t)(cmd - '0');
      if (idx < imageCount)
      {
        currentImageIndex = idx;
        needUpdate = true;
      }
    }

    if (needUpdate)
    {
      showCurrentImage();
      lastSwitchTime = now; // 蓝牙控制后重置定时
    }
  }
#endif

  delay(10);
}
