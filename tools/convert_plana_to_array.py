import os
from pathlib import Path

from PIL import Image
import struct


# 目标分辨率，根据你的屏幕分辨率修改
TARGET_WIDTH = 240
TARGET_HEIGHT = 240

# 最多转换多少张图片（比如 6 = 只取前 6 张 jpg）
MAX_IMAGES = 100


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


def save_image_to_arr(src_path: Path, arr_path: Path) -> None:
    """把一张图片转换成 RGB565 数组文本，并写入 .arr 文件。"""
    img = Image.open(src_path).convert("RGB")
    resample = Image.Resampling.BILINEAR  # Pillow >= 9
    img = img.resize((TARGET_WIDTH, TARGET_HEIGHT), resample)
    pixels = [(int(r), int(g), int(b)) for (r, g, b) in img.getdata()]

    values = [rgb888_to_rgb565(r, g, b) for (r, g, b) in pixels]

    with open(arr_path, "w", encoding="utf-8") as f:
        # 每行固定数量便于查看，解析器按分隔符读取，不依赖具体换行
        per_line = 16
        for i in range(0, len(values), per_line):
            chunk = values[i : i + per_line]
            line = ", ".join(f"0x{v:04X}" for v in chunk)
            if i + per_line < len(values):
                f.write(line + ",\n")
            else:
                f.write(line + "\n")


def main():
    root = Path(__file__).resolve().parents[1]
    photo_dir = root / "photo_for_sd"
    out_path = root / "include" / "plana.h"
    data_dir = root / "data"
    sd_arr_dir = root / "sd_arrays"

    jpg_list = sorted(photo_dir.glob("*.jpg"))
    if not jpg_list:
        raise FileNotFoundError(f"在 {photo_dir} 下面没有找到任何 .jpg 文件")

    # 只保留前 MAX_IMAGES 张，避免图片太多导致程序体积超出 Flash
    if len(jpg_list) > MAX_IMAGES:
        jpg_list = jpg_list[:MAX_IMAGES]

    # 确保 SPIFFS 数据目录存在（PlatformIO 默认使用 data/ 上传到 SPIFFS）
    data_dir.mkdir(parents=True, exist_ok=True)
    # 生成给 SD 卡使用的数组文本目录
    sd_arr_dir.mkdir(parents=True, exist_ok=True)

    bin_files: list[str] = []

    for idx, path in enumerate(jpg_list):
        arr_name = f"img_{idx}.arr"
        arr_path = sd_arr_dir / arr_name
        print(f"转换图片 {path.name} -> {arr_path}")
        save_image_to_arr(path, arr_path)

    print(f"生成完成: {out_path}，共 {len(bin_files)} 张图片")
    print(f"SD 数组文件目录: {sd_arr_dir}（将其中 .arr 复制到 SD 卡根目录）")


if __name__ == "__main__":
    main()
