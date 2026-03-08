#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

#include "plana.h"  // 由 tools/convert_plana.py 生成

// 根据你的实际接线修改以下引脚定义
#define TFT_CS 5    // 屏幕 CS（片选）
#define TFT_DC 2    // 屏幕 DC / RS
#define TFT_RST 4   // 屏幕 RST（如接到 EN 或固定复位，可设为 -1）
#define TFT_BL 15   // 背光控制引脚（如果直接接 3.3V，可删掉相关代码）

// 使用硬件 SPI (ESP32 VSPI: SCK=18, MOSI=23, MISO=19)
// 如需改成 HSPI 或自定义 SPI 引脚，可以改用另一个构造函数
Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

// 用于轮播图片的索引与定时
static size_t currentImageIndex = 0;
static unsigned long lastSwitchTime = 0;

void setup()
{
  if (TFT_BL >= 0)
  {
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH); // 打开背光
  }

  // 初始化 240x240 分辨率
  tft.init(240, 240);
  tft.setRotation(1); // 0~3，按需要选择显示方向

  // 先显示第一张图片（索引 0）
  tft.fillScreen(ST77XX_BLACK);
  tft.drawRGBBitmap(0, 0, (uint16_t *)planaImages[currentImageIndex], planaWidth, planaHeight);
  lastSwitchTime = millis();
}

void loop()
{
  // 每隔 5 秒切换到下一张图片
  unsigned long now = millis();
  if (now - lastSwitchTime >= 5000UL)
  {
    currentImageIndex++;
    if (currentImageIndex >= planaImageCount)
    {
      currentImageIndex = 0;
    }

    tft.fillScreen(ST77XX_BLACK);
    tft.drawRGBBitmap(0, 0, (uint16_t *)planaImages[currentImageIndex], planaWidth, planaHeight);
    lastSwitchTime = now;
  }

  delay(100);
}