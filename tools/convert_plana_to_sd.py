from pathlib import Path

from PIL import Image
import struct


# 目标分辨率，根据你的屏幕分辨率修改
TARGET_WIDTH = 240
TARGET_HEIGHT = 240

# 最多转换多少张图片
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


def main():
    root = Path(__file__).resolve().parents[1]
    photo_dir = root / "photo_for_sd"
    sd_bin_dir = root / "sd_arrays"

    jpg_list = sorted(photo_dir.glob("*.jpg"))
    if not jpg_list:
        raise FileNotFoundError(f"在 {photo_dir} 下面没有找到任何 .jpg 文件")

    # 只保留前 MAX_IMAGES 张，避免过多文件导致管理困难
    if len(jpg_list) > MAX_IMAGES:
        jpg_list = jpg_list[:MAX_IMAGES]

    # 生成给 SD 卡使用的二进制目录
    sd_bin_dir.mkdir(parents=True, exist_ok=True)

    # 清理旧的 bin 文件，避免残留索引影响显示顺序
    for old_bin in sd_bin_dir.glob("*.bin"):
        old_bin.unlink()

    bin_files: list[str] = []

    for idx, path in enumerate(jpg_list):
        bin_name = f"img_{idx}.bin"
        bin_path = sd_bin_dir / bin_name
        print(f"转换图片 {path.name} -> {bin_path}")
        save_image_to_bin(path, bin_path)
        bin_files.append(bin_name)

    print(f"生成完成，共 {len(bin_files)} 张 BIN 图片")
    print(f"SD 图片目录: {sd_bin_dir}")
    print("请将 .bin 文件复制到 SD 卡根目录。")


if __name__ == "__main__":
    main()
