import os
from pathlib import Path

from PIL import Image, ImageSequence
import struct

# 目标分辨率，根据你的屏幕分辨率修改
TARGET_WIDTH = 240
TARGET_HEIGHT = 240

# GIF 源文件名（位于工程根目录下的 photo/ 目录）
GIF_NAME = "gif_test.gif"


def rgb888_to_rgb565(r: int, g: int, b: int) -> int:
    r5 = (r & 0xF8) >> 3
    g6 = (g & 0xFC) >> 2
    b5 = (b & 0xF8) >> 3
    return (r5 << 11) | (g6 << 5) | b5


def save_frame_to_bin(frame: Image.Image, bin_path: Path) -> None:
    """把一帧图片转换成 RGB565 原始数据，并写入 .bin 文件（行优先）。"""
    img = frame.convert("RGB")
    img = img.resize((TARGET_WIDTH, TARGET_HEIGHT), Image.Resampling.BILINEAR)

    pixels = [(int(r), int(g), int(b)) for (r, g, b) in img.getdata()]

    with open(bin_path, "wb") as f:
        for r, g, b in pixels:
            value = rgb888_to_rgb565(r, g, b)
            f.write(struct.pack("<H", value))


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    photo_dir = root / "photo"
    data_dir = root / "gif_data"
    include_dir = root / "include"
    src_dir = root / "src"

    gif_path = photo_dir / GIF_NAME
    if not gif_path.exists():
        raise FileNotFoundError(f"未找到 GIF 文件: {gif_path}")

    data_dir.mkdir(parents=True, exist_ok=True)
    include_dir.mkdir(parents=True, exist_ok=True)
    src_dir.mkdir(parents=True, exist_ok=True)

    img = Image.open(gif_path)

    frame_bin_names: list[str] = []
    frame_index = 0

    for frame in ImageSequence.Iterator(img):
        bin_name = f"/gif_f{frame_index}.bin"
        bin_path = data_dir / bin_name.lstrip("/")
        print(f"转换 GIF 帧 {frame_index} -> {bin_path}")
        save_frame_to_bin(frame, bin_path)
        frame_bin_names.append(bin_name)
        frame_index += 1

    # 生成 gif_frames.h
    h_path = include_dir / "gif_frames.h"
    h_lines: list[str] = []
    h_lines.append("#pragma once")
    h_lines.append("#include <Arduino.h>")
    h_lines.append("")
    h_lines.append(f"const size_t gifFrameCount = {len(frame_bin_names)};")
    h_lines.append("extern const char* const gifFrames[];")
    h_path.write_text("\n".join(h_lines), encoding="utf-8")

    # 生成 gif_frames.cpp
    cpp_path = src_dir / "gif_frames.cpp"
    cpp_lines: list[str] = []
    cpp_lines.append("#include <Arduino.h>")
    cpp_lines.append("#include \"gif_frames.h\"")
    cpp_lines.append("")
    cpp_lines.append("const char* const gifFrames[] = {")
    for name in frame_bin_names:
        cpp_lines.append(f"    \"{name}\",")
    cpp_lines.append("};")

    cpp_path.write_text("\n".join(cpp_lines), encoding="utf-8")

    print(f"生成完成: {h_path} 和 {cpp_path}，共 {len(frame_bin_names)} 帧")


if __name__ == "__main__":
    main()
