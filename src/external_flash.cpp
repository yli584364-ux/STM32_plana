#include "external_flash.h"

#include <SPI.h>

#include "pin.h"

namespace
{
  SPIClass g_extFlashSpi(HSPI);

  static const uint8_t CMD_READ_STATUS = 0x05;
  static const uint8_t CMD_WRITE_ENABLE = 0x06;
  static const uint8_t CMD_READ_DATA = 0x03;
  static const uint8_t CMD_PAGE_PROGRAM = 0x02;
  static const uint8_t CMD_SECTOR_ERASE_4K = 0x20;

  static const uint32_t SECTOR_SIZE = 4096;
  static const uint32_t PAGE_SIZE = 256;

  void extFlashSelect()
  {
    digitalWrite(EXT_FLASH_CS, LOW);
  }

  void extFlashDeselect()
  {
    digitalWrite(EXT_FLASH_CS, HIGH);
  }

  uint8_t extFlashReadStatusRaw()
  {
    extFlashSelect();
    g_extFlashSpi.transfer(CMD_READ_STATUS);
    uint8_t st = g_extFlashSpi.transfer(0x00);
    extFlashDeselect();
    return st;
  }

  bool extFlashWaitReady(uint32_t timeoutMs)
  {
    uint32_t start = millis();
    while (true)
    {
      uint8_t st = extFlashReadStatusRaw();
      if ((st & 0x01) == 0)
      {
        return true;
      }
      if ((millis() - start) > timeoutMs)
      {
        return false;
      }
      delay(1);
    }
  }

  void extFlashWriteEnable()
  {
    extFlashSelect();
    g_extFlashSpi.transfer(CMD_WRITE_ENABLE);
    extFlashDeselect();
  }

  void extFlashSendAddr24(uint32_t addr)
  {
    g_extFlashSpi.transfer((uint8_t)((addr >> 16) & 0xFF));
    g_extFlashSpi.transfer((uint8_t)((addr >> 8) & 0xFF));
    g_extFlashSpi.transfer((uint8_t)(addr & 0xFF));
  }
}

bool extFlashBegin()
{
  pinMode(EXT_FLASH_CS, OUTPUT);
  extFlashDeselect();
  g_extFlashSpi.begin(EXT_FLASH_SCK, EXT_FLASH_MISO, EXT_FLASH_MOSI, EXT_FLASH_CS);
  return extFlashWaitReady(200);
}

bool extFlashIsChipReady()
{
  return extFlashWaitReady(100);
}

bool extFlashReadBytes(uint32_t addr, uint8_t *buf, size_t len)
{
  if (!buf)
  {
    return false;
  }
  if ((uint64_t)addr + (uint64_t)len > (uint64_t)EXT_FLASH_TOTAL_BYTES)
  {
    return false;
  }

  extFlashSelect();
  g_extFlashSpi.transfer(CMD_READ_DATA);
  extFlashSendAddr24(addr);
  for (size_t i = 0; i < len; ++i)
  {
    buf[i] = g_extFlashSpi.transfer(0x00);
  }
  extFlashDeselect();
  return true;
}

bool extFlashWriteBytes(uint32_t addr, const uint8_t *buf, size_t len)
{
  if (!buf)
  {
    return false;
  }
  if ((uint64_t)addr + (uint64_t)len > (uint64_t)EXT_FLASH_TOTAL_BYTES)
  {
    return false;
  }

  size_t written = 0;
  while (written < len)
  {
    uint32_t curAddr = addr + (uint32_t)written;
    uint32_t pageOffset = curAddr % PAGE_SIZE;
    size_t pageRemain = PAGE_SIZE - pageOffset;
    size_t chunk = len - written;
    if (chunk > pageRemain)
    {
      chunk = pageRemain;
    }

    extFlashWriteEnable();
    extFlashSelect();
    g_extFlashSpi.transfer(CMD_PAGE_PROGRAM);
    extFlashSendAddr24(curAddr);
    for (size_t i = 0; i < chunk; ++i)
    {
      g_extFlashSpi.transfer(buf[written + i]);
    }
    extFlashDeselect();

    if (!extFlashWaitReady(100))
    {
      return false;
    }

    written += chunk;
  }

  return true;
}

bool extFlashEraseRange(uint32_t addr, size_t len)
{
  if (len == 0)
  {
    return true;
  }

  uint32_t startAddr = addr - (addr % SECTOR_SIZE);
  uint64_t endAddr64 = (uint64_t)addr + (uint64_t)len;
  if (endAddr64 > (uint64_t)EXT_FLASH_TOTAL_BYTES)
  {
    return false;
  }
  uint32_t endAddr = (uint32_t)endAddr64;
  if (endAddr % SECTOR_SIZE != 0)
  {
    endAddr += (SECTOR_SIZE - (endAddr % SECTOR_SIZE));
  }

  for (uint32_t cur = startAddr; cur < endAddr; cur += SECTOR_SIZE)
  {
    extFlashWriteEnable();
    extFlashSelect();
    g_extFlashSpi.transfer(CMD_SECTOR_ERASE_4K);
    extFlashSendAddr24(cur);
    extFlashDeselect();

    if (!extFlashWaitReady(3000))
    {
      return false;
    }
  }

  return true;
}

bool extFlashLoadGifHeader(ExternalGifHeader &header)
{
  return extFlashReadBytes(0, (uint8_t *)&header, sizeof(ExternalGifHeader));
}

bool extFlashStoreGifHeader(const ExternalGifHeader &header)
{
  return extFlashWriteBytes(0, (const uint8_t *)&header, sizeof(ExternalGifHeader));
}
