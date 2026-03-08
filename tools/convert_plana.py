import os
from pathlib import Path

from PIL import Image


# 目标分辨率，根据你的屏幕分辨率修改
TARGET_WIDTH = 240
TARGET_HEIGHT = 240

# 最多转换多少张图片（比如 9 = 只取前 9 张 jpg）
MAX_IMAGES = 9


def rgb888_to_rgb565(r, g, b):
    r5 = (r & 0xF8) >> 3
    g6 = (g & 0xFC) >> 2
    b5 = (b & 0xF8) >> 3
    return (r5 << 11) | (g6 << 5) | b5


def image_array_name(stem: str) -> str:
    """把文件名 stem 转成合法的 C 标识符。"""
    result = []
    for ch in stem:
        if ch.isalnum():
            result.append(ch)
        else:
            result.append("_")
    return "plana_" + "".join(result) + "_Img"


def convert_image(path: Path, array_name: str) -> list[str]:
    """把一张图片转换成 RGB565 数组的 C 代码行。"""
    img = Image.open(path).convert("RGB")
    img = img.resize((TARGET_WIDTH, TARGET_HEIGHT), Image.BILINEAR)
    pixels = list(img.getdata())

    lines: list[str] = []
    lines.append(f"const uint16_t {array_name}[] PROGMEM = {{")
    for i, (r, g, b) in enumerate(pixels):
        value = rgb888_to_rgb565(r, g, b)
        if i % 12 == 0:
            lines.append("    ")
        lines[-1] += f"0x{value:04X}, "
    lines.append("};")
    lines.append("")
    return lines


def main():
    root = Path(__file__).resolve().parents[1]
    photo_dir = root / "photo"
    out_path = root / "include" / "plana.h"

    jpg_list = sorted(photo_dir.glob("*.jpg"))
    if not jpg_list:
        raise FileNotFoundError(f"在 {photo_dir} 下面没有找到任何 .jpg 文件")

    # 只保留前 MAX_IMAGES 张，避免图片太多导致程序体积超出 Flash
    if len(jpg_list) > MAX_IMAGES:
        jpg_list = jpg_list[:MAX_IMAGES]

    lines: list[str] = []
    lines.append("#pragma once")
    lines.append("#include <Arduino.h>")
    lines.append("")
    lines.append(f"const uint16_t planaWidth = {TARGET_WIDTH};")
    lines.append(f"const uint16_t planaHeight = {TARGET_HEIGHT};")
    lines.append("")

    array_names: list[str] = []

    for path in jpg_list:
        stem = path.stem
        arr_name = image_array_name(stem)
        array_names.append(arr_name)
        lines.append(f"// 来自图片: {path.name}")
        lines.extend(convert_image(path, arr_name))

    # 指针数组 + 数量，便于在 Arduino 里轮播
    lines.append(f"const uint16_t* const planaImages[{len(array_names)}] PROGMEM = {{")
    for name in array_names:
        lines.append(f"    {name},")
    lines.append("};")
    lines.append(f"const size_t planaImageCount = {len(array_names)};")

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text("\n".join(lines), encoding="utf-8")
    print(f"生成完成: {out_path}，共 {len(array_names)} 张图片")


if __name__ == "__main__":
    main()
