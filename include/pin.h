#include <stdlib.h>

// 根据你的实际接线修改以下引脚定义
#define TFT_CS 5    // 屏幕 CS（片选）
#define TFT_DC 2    // 屏幕 DC / RS
#define TFT_RST 4   // 屏幕 RST（如接到 EN 或固定复位，可设为 -1）
#define TFT_BL 15   // 背光控制引脚（如果直接接 3.3V，可删掉相关代码）

// SD 卡 SPI 片选引脚（SCK/MOSI/MISO 复用 VSPI: 18/23/19）
// 如果与其他外设冲突，请按实际接线修改
#define SD_CS 26
static const uint32_t SD_SPI_FREQ = 4000000; // 4MHz，优先稳定性

// 外接 SPI NOR Flash（128Mbit = 16MB）引脚
// 默认使用 HSPI 总线；请按你的实际接线修改
#define EXT_FLASH_CS 27
#define EXT_FLASH_SCK 14
#define EXT_FLASH_MISO 12
#define EXT_FLASH_MOSI 13
static const uint32_t EXT_FLASH_SPI_FREQ = 20000000; // 20MHz