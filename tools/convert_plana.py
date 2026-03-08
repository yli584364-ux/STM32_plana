import os
from pathlib import Path

from PIL import Image


# 目标分辨率，根据你的屏幕分辨率修改
TARGET_WIDTH = 240
TARGET_HEIGHT = 240


def rgb888_to_rgb565(r, g, b):
    r5 = (r & 0xF8) >> 3
    g6 = (g & 0xFC) >> 2
    b5 = (b & 0xF8) >> 3
    return (r5 << 11) | (g6 << 5) | b5


def main():
    root = Path(__file__).resolve().parents[1]
    img_path = root / "photo" / "plana.jpg"
    out_path = root / "include" / "plana.h"

    if not img_path.exists():
        raise FileNotFoundError(f"找不到图片: {img_path}")

    img = Image.open(img_path).convert("RGB")
    img = img.resize((TARGET_WIDTH, TARGET_HEIGHT), Image.BILINEAR)

    pixels = list(img.getdata())

    lines = []
    lines.append("#pragma once")
    lines.append("#include <Arduino.h>")
    lines.append("")
    lines.append(f"const uint16_t planaWidth = {TARGET_WIDTH};")
    lines.append(f"const uint16_t planaHeight = {TARGET_HEIGHT};")
    lines.append("const uint16_t planaImg[] PROGMEM = {")

    for i, (r, g, b) in enumerate(pixels):
        value = rgb888_to_rgb565(r, g, b)
        if i % 12 == 0:
            lines.append("    ",)
        lines[-1] += f"0x{value:04X}, "

    lines.append("};")

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text("\n".join(lines), encoding="utf-8")
    print(f"生成完成: {out_path}")


if __name__ == "__main__":
    main()
