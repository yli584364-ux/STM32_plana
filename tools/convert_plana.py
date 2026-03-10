import os
from pathlib import Path

from PIL import Image
import struct


# 目标分辨率，根据你的屏幕分辨率修改
TARGET_WIDTH = 240
TARGET_HEIGHT = 240

# 最多转换多少张图片（比如 6 = 只取前 6 张 jpg）
# 如果 SPIFFS 空间不足，可以再调小这个数或者把图片分辨率调低
MAX_IMAGES = 6


def rgb888_to_rgb565(r, g, b):
    r5 = (r & 0xF8) >> 3
    g6 = (g & 0xFC) >> 2
    b5 = (b & 0xF8) >> 3
    return (r5 << 11) | (g6 << 5) | b5


def save_image_to_bin(src_path: Path, bin_path: Path) -> None:
    """把一张图片转换成 RGB565 原始数据，并写入 .bin 文件（行优先）。"""
    img = Image.open(src_path).convert("RGB")
    resample = Image.Resampling.BILINEAR  # Pillow >= 9

    img = img.resize((TARGET_WIDTH, TARGET_HEIGHT), resample)
    # Pylance 对 ImagingCore -> list 的类型推断有点挑剔，这里用列表推导兼容类型检查
    pixels = [(int(r), int(g), int(b)) for (r, g, b) in img.getdata()]

    # 以小端序写入 16 位 RGB565，和 ESP32 内存中的 uint16_t 存一致
    with open(bin_path, "wb") as f:
        for r, g, b in pixels:
            value = rgb888_to_rgb565(r, g, b)
            f.write(struct.pack("<H", value))


def main():
    root = Path(__file__).resolve().parents[1]
    photo_dir = root / "photo"
    out_path = root / "include" / "plana.h"
    data_dir = root / "data"

    jpg_list = sorted(photo_dir.glob("*.jpg"))
    if not jpg_list:
        raise FileNotFoundError(f"在 {photo_dir} 下面没有找到任何 .jpg 文件")

    # 只保留前 MAX_IMAGES 张，避免图片太多导致程序体积超出 Flash
    if len(jpg_list) > MAX_IMAGES:
        jpg_list = jpg_list[:MAX_IMAGES]

    # 确保 SPIFFS 数据目录存在（PlatformIO 默认使用 data/ 上传到 SPIFFS）
    data_dir.mkdir(parents=True, exist_ok=True)

    bin_files: list[str] = []

    for idx, path in enumerate(jpg_list):
        # 统一用简单的顺序文件名，避免中文路径问题
        bin_name = f"/img_{idx}.bin"
        bin_path = data_dir / bin_name.lstrip("/")
        print(f"转换图片 {path.name} -> {bin_path}")
        save_image_to_bin(path, bin_path)
        bin_files.append(bin_name)

    # 只在头文件里保存宽高和 SPIFFS 中文件名列表
    lines: list[str] = []
    lines.append("#pragma once")
    lines.append("#include <Arduino.h>")
    lines.append("")
    lines.append(f"const uint16_t planaWidth = {TARGET_WIDTH};")
    lines.append(f"const uint16_t planaHeight = {TARGET_HEIGHT};")
    lines.append("")
    lines.append(f"const size_t planaImageCount = {len(bin_files)};")
    lines.append("extern const char* const planaImages[];")

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text("\n".join(lines), encoding="utf-8")

    # 为了把字符串常量本体放在一个 .cpp 里，方便编译和链接
    cpp_path = root / "src" / "plana_images.cpp"
    cpp_lines: list[str] = []
    cpp_lines.append("#include <Arduino.h>")
    cpp_lines.append("#include \"plana.h\"")
    cpp_lines.append("")
    cpp_lines.append("const char* const planaImages[] = {")
    for name in bin_files:
        cpp_lines.append(f"    \"{name}\",")
    cpp_lines.append("};")

    cpp_path.parent.mkdir(parents=True, exist_ok=True)
    cpp_path.write_text("\n".join(cpp_lines), encoding="utf-8")

    print(f"生成完成: {out_path} 和 {cpp_path}，共 {len(bin_files)} 张图片")


if __name__ == "__main__":
    main()
