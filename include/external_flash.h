#pragma once

#include <Arduino.h>

// External flash capacity for 128Mbit NOR
static const uint32_t EXT_FLASH_TOTAL_BYTES = 16UL * 1024UL * 1024UL;

struct ExternalGifHeader
{
  uint32_t magic;
  uint16_t version;
  uint16_t width;
  uint16_t height;
  uint16_t reserved;
  uint32_t frameCount;
  uint32_t frameSizeBytes;
  uint32_t dataStartAddr;
  uint32_t totalDataBytes;
};

bool extFlashBegin();
bool extFlashIsChipReady();
bool extFlashReadBytes(uint32_t addr, uint8_t *buf, size_t len);
bool extFlashWriteBytes(uint32_t addr, const uint8_t *buf, size_t len);
bool extFlashEraseRange(uint32_t addr, size_t len);

bool extFlashLoadGifHeader(ExternalGifHeader &header);
bool extFlashStoreGifHeader(const ExternalGifHeader &header);
