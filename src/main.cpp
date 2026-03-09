#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <WiFi.h>              // WiFi AP + WebServer
#include <WebServer.h>
#include <FS.h>
#include <SD.h>
#include <TJpg_Decoder.h>
#include "BluetoothSerial.h"  // ESP32 经典蓝牙串口
#include "SPIFFS.h"           // SPIFFS 文件系统

#include "plana.h"  // 由 tools/convert_plana.py 生成

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

// TJpg_Decoder 回调：把解码出的块绘制到 TFT
// 注意：TJpg_Decoder 的回调签名是
// bool (*)(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap)
static bool tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap)
{
  if (x >= tft.width() || y >= tft.height())
  {
    return false;
  }
  tft.drawRGBBitmap(x, y, bitmap, w, h);
  return true;
}

// 扫描 SD 根目录中的 JPG 文件
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
      // 确保文件名以 "/" 开头，TJpg_Decoder 示例也都是用 "/xxx.jpg"
      if (!name.startsWith("/"))
      {
        name = String("/") + name;
      }
      String lower = name;
      lower.toLowerCase();
      if (lower.endsWith(".jpg") || lower.endsWith(".jpeg"))
      {
        sdImageNames[sdImageCount++] = name;
        Serial.print("Found SD image: ");
        Serial.println(name);
      }
    }
    file = root.openNextFile();
  }
}

// 显示当前索引图片：优先使用 SD 上的 JPG，否则回退到 SPIFFS .bin
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
    // 使用 SD JPG，假定为 240x240
    String path = sdImageNames[currentImageIndex % sdImageCount];
    Serial.print("Draw JPG from SD: ");
    Serial.println(path);
    tft.fillScreen(ST77XX_BLACK);
    TJpgDec.drawSdJpg(0, 0, path.c_str());
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
  if (SD.begin(SD_CS))
  {
    sdAvailable = true;
    Serial.println("SD card initialized");
    scanSdImages();
  }
  else
  {
    Serial.println("SD card initialization failed");
  }

  // 配置 JPEG 解码回调
  TJpgDec.setJpgScale(1); // 不缩放，要求 JPG 本身为 240x240
  TJpgDec.setCallback(tft_output);

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