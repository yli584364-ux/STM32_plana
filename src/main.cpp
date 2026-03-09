#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <WiFi.h>              // WiFi AP + WebServer
#include <WebServer.h>
#include <FS.h>
#include <SD.h>
#include "BluetoothSerial.h"  // ESP32 经典蓝牙串口
#include "SPIFFS.h"           // SPIFFS 文件系统

#include "plana.h"  // 由 tools/convert_plana.py 生成

// 置为 1 时启用蓝牙 A2DP 音频测试（正弦波），置为 0 时使用原来的屏幕+Web+串口控制程序
#define USE_A2DP_TEST 1

// 根据你的实际接线修改以下引脚定义
#define TFT_CS 5    // 屏幕 CS（片选）
#define TFT_DC 2    // 屏幕 DC / RS
#define TFT_RST 4   // 屏幕 RST（如接到 EN 或固定复位，可设为 -1）
#define TFT_BL 15   // 背光控制引脚（如果直接接 3.3V，可删掉相关代码）

// SD 卡 SPI 片选引脚（SCK/MOSI/MISO 复用 VSPI: 18/23/19）
#define SD_CS 13

// 使用硬件 SPI (ESP32 VSPI: SCK=18, MOSI=23, MISO=19)
// 如需改成 HSPI 或自定义 SPI 引脚，可以改用另一个构造函数
Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

// 蓝牙串口对象
BluetoothSerial SerialBT;

// 简单 Web 服务器
WebServer server(80);

// SD 卡相关
static bool sdAvailable = false;
static const uint8_t MAX_SD_IMAGES = 64;
static String sdImageNames[MAX_SD_IMAGES];
static size_t sdImageCount = 0;

// 实际可用的图片数量（优先 SD，其次 SPIFFS）
static size_t imageCount = 0;

// 用于轮播图片的索引与定时
static size_t currentImageIndex = 0;
static unsigned long lastSwitchTime = 0;

#if USE_A2DP_TEST

//================== A2DP 音频测试代码（ESP32 作为蓝牙音频发送端） ==================

#include "BluetoothA2DPSource.h"

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
    int16_t v = (int16_t)(sin(a2dpPhase) * 3000.0f);

    // 立体声左右声道相同
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

void setup()
{
  Serial.begin(115200);
  Serial.println("Starting ESP32 A2DP sine test...");

  // 配置数据回调，然后启动 A2DP 源
  a2dpSource.set_data_callback(get_sound_data);

  // 这里的名字是 ESP32 这个“蓝牙音频发送端”的名字
  // 你的 M38 模块 / 蓝牙音箱需要搜索并连接这个名字
  a2dpSource.start("ESP_Audio");

  Serial.println("A2DP source started, look for 'ESP_Audio' on your headset/speaker.");
}

void loop()
{
  // A2DP 库在后台通过回调不断取数据，这里不需要做事
  delay(100);
}

#else

// 扫描 SD 根目录中的预处理 .bin 图片文件
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
      // 现在我们期望 SD 卡上存放的是 Python 已经转换好的 RGB565 .bin 文件
      if (lower.endsWith(".bin"))
      {
        sdImageNames[sdImageCount++] = name;
        Serial.print("Found SD image: ");
        Serial.println(name);
      }
    }
    file = root.openNextFile();
  }
}

// 显示当前索引图片：优先使用 SD 上的 .bin，否则回退到 SPIFFS .bin
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

  if (sdAvailable && sdImageCount > 0)
  {
    // 使用 SD 上的 RGB565 .bin 文件，格式与 SPIFFS 中生成的一致
    String path = sdImageNames[currentImageIndex % sdImageCount];
    Serial.print("Draw BIN from SD: ");
    Serial.println(path);

    File f = SD.open(path, "rb");
    if (!f)
    {
      Serial.print("Failed to open SD image file: ");
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
  else
  {
    // 使用 SPIFFS 里的 RGB565 .bin
    if (currentImageIndex >= planaImageCount)
    {
      currentImageIndex = 0;
    }

    const char *path = planaImages[currentImageIndex];
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

  String html;
  html.reserve(f.size() + 64);
  while (f.available())
  {
    html += (char)f.read();
  }
  f.close();

  server.send(200, "text/html; charset=utf-8", html);
}

static void handleNext()
{
  currentImageIndex++;
  if (currentImageIndex >= imageCount)
  {
    currentImageIndex = 0;
  }
  showCurrentImage();
  lastSwitchTime = millis();
  server.send(200, "text/plain", "OK");
}

static void handlePrev()
{
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
  server.send(200, "text/plain", "OK");
}

static void handleSet()
{
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
  showCurrentImage();
  lastSwitchTime = millis();
  server.send(200, "text/plain", "OK");
}

void setup()
{
  Serial.begin(115200); // 调试串口

  // 初始化 SPIFFS
  if (!SPIFFS.begin(true))
  {
    Serial.println("SPIFFS mount failed");
  }

  if (TFT_BL >= 0)
  {
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH); // 打开背光
  }

  // 初始化 240x240 分辨率
  tft.init(240, 240);
  tft.setRotation(1); // 0~3，按需要选择显示方向

  // 初始化 SD 卡并扫描 JPG 图片
  // 显式初始化 VSPI 总线，并把 SD 频率降到 10MHz，减小总线和供电压力
  SPI.begin(18, 19, 23, SD_CS); // SCK=18, MISO=19, MOSI=23, SS=SD_CS

  if (SD.begin(SD_CS, SPI, 10000000))
  {
    sdAvailable = true;
    Serial.println("SD card initialized");
    scanSdImages();
  }
  else
  {
    Serial.println("SD card initialization failed");
  }

  // 先显示第一张图片（索引 0）
  // 决定使用 SD 还是 SPIFFS 图片
  if (sdAvailable && sdImageCount > 0)
  {
    imageCount = sdImageCount;
    Serial.print("Use images from SD, count: ");
    Serial.println(imageCount);
  }
  else
  {
    imageCount = planaImageCount;
    Serial.print("Use images from SPIFFS, count: ");
    Serial.println(imageCount);
  }

  showCurrentImage();
  lastSwitchTime = millis();

  // 启动 WiFi AP + WebServer
  WiFi.mode(WIFI_AP);
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
  server.begin();
  Serial.println("HTTP server started");

  // 启动蓝牙串口，设备名可在此修改
  SerialBT.begin("ESP_LCD");
  Serial.println("Bluetooth started, device name: ESP_LCD");
}

void loop()
{
  unsigned long now = millis();

  // 处理 HTTP 请求
  server.handleClient();

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

  delay(100);
}

#endif // USE_A2DP_TEST