## 网络配置与使用说明
<br>
**现在的工作方式**
<br>
- ESP32 启动后会：
  - 开启 AP 热点：SSID 为 `ESP_LCD_AP`，密码 `12345678`
  - 在 AP 上启动一个 WebServer（端口 80）
  - 同时保留原来的蓝牙控制和自动轮播功能
- Web 接口：
  - 访问 `/`：返回一个简单的 HTML 控制页面（已内嵌在 main.cpp 里）
  - `GET /next`：切到下一张图片
  - `GET /prev`：切到上一张图片
  - `GET /set?i=数字`：跳到指定索引的图片
<br>
**手机端如何使用**
<br>
1. 给 ESP32 上电并烧好当前固件。
2. 在手机 WiFi 设置里：
   - 搜索并连接热点：`ESP_LCD_AP`
   - 密码：`12345678`
3. 连接成功后，在手机浏览器地址栏输入：
   - `http://192.168.4.1/`
4. 打开页面后：
   - 点击“上一张”“下一张”按钮即可切换图片
   - 在输入框里输入索引（0 ~ N-1），点“跳转”可以直接跳到指定图片
<br>
如果你想：
- 改热点名称/密码
- 改网页样式或增加更多控制（比如调整切换间隔、暂停自动轮播等）

硬件连接（默认假设共享 VSPI，总线可按需改）：
屏幕：继续用原来的 VSPI 引脚（SCK=18, MOSI=23, MISO=19, CS=5）。
SD 卡：共用 SCK=18、MOSI=23、MISO=19，新加片选 SD_CS = 13（你可以按实际接线改成别的 GPIO）。 

## 图片显示方式
在 PC 上用 Python 预处理图片

保持用现有的 ESP_LCD/tools/convert_plana.py：
把 photo 里的 JPG 转成 240×240 的 RGB565 .bin 文件。
生成的在 ESP_LCD/data 目录下：img_0.bin、img_1.bin...
这些 .bin 就是“处理好的形式”。
把 .bin 拷到 SD 卡

用读卡器打开 SD 卡，把 data 目录里的 img_0.bin ~ img_5.bin 复制到 SD 卡根目录。
不需要拷 JPG 到 SD（可以删掉 JPG，只留 bin）。
ESP32 上的行为（已经改好在 ESP_LCD/src/main.cpp 里）

启动时：
初始化 SD；
扫描 SD 根目录里的所有 .bin 文件，记录成 /img_0.bin 这种路径；
如果找到，则 imageCount = sdImageCount，优先用 SD 上的 bin；
如果 SD 没有 bin，就回退用 SPIFFS 里的 bin。
showCurrentImage()：
对 SD 分支：SD.open("/img_0.bin", "rb")，按行读出 RGB565 数据到 lineBuf，用 tft.drawRGBBitmap 一行一行画（和 SPIFFS 版本完全一样，只是文件系统从 SPIFFS 换成 SD）。
不再使用 TJpg_Decoder，也不会再出现 JPEG 解码卡顿或错误。
现有控制方式都不变

网页控制：WiFi AP + /index.html 页面，发 /next、/prev、/set 控制 currentImageIndex。
蓝牙控制：n/p/数字 控制索引。
这些都会调用同一个 showCurrentImage()，无论图片最终来自 SD 还是 SPIFFS。