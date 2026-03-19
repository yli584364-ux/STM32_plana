import shutil
from pathlib import Path

from PIL import Image, ImageSequence
import struct

# 目标分辨率，根据你的屏幕分辨率修改
TARGET_WIDTH = 240
TARGET_HEIGHT = 240

# 是否额外导出到 data/gif_data（用于板载 SPIFFS 播放）。
# 240x240 RGB565 GIF 帧体积很大，默认关闭，避免 uploadfs 因空间不足失败。
EXPORT_TO_SPIFFS = False

# SD 卡导出目录（该目录下会生成 gif_0, gif_1 ... 子目录）。
SD_GIF_ROOT_DIR = "gif_for_sd"


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


def clean_bin_files(folder: Path, pattern: str = "*.bin") -> None:
    if not folder.exists():
        return

    for p in folder.glob(pattern):
        if p.is_file():
            p.unlink()


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    photo_dir = root / "gif"
    data_dir = root / "data" / "gif_data"
    mirror_dir = root / "gif_data"
    sd_root = root / SD_GIF_ROOT_DIR
    include_dir = root / "include"
    src_dir = root / "src"

    gif_files = sorted(photo_dir.glob("*.gif"))
    if not gif_files:
        raise FileNotFoundError(f"未找到 GIF 文件: {photo_dir}")

    mirror_dir.mkdir(parents=True, exist_ok=True)
    if EXPORT_TO_SPIFFS:
        data_dir.mkdir(parents=True, exist_ok=True)
        clean_bin_files(data_dir)

    clean_bin_files(mirror_dir)

    sd_root.mkdir(parents=True, exist_ok=True)
    for old_dir in sd_root.glob("gif_*"):
        if old_dir.is_dir():
            shutil.rmtree(old_dir)

    include_dir.mkdir(parents=True, exist_ok=True)
    src_dir.mkdir(parents=True, exist_ok=True)

    frame_bin_names: list[str] = []

    for gif_idx, gif_path in enumerate(gif_files):
        sd_gif_dir = sd_root / f"gif_{gif_idx}"
        sd_gif_dir.mkdir(parents=True, exist_ok=True)

        print(f"处理 GIF: {gif_path.name} -> {sd_gif_dir}")

        frame_count = 0
        img = Image.open(gif_path)
        for frame in ImageSequence.Iterator(img):
            sd_frame_name = f"frame_{frame_count:03d}.bin"
            save_frame_to_bin(frame, sd_gif_dir / sd_frame_name)

            # 保留原有逻辑：使用第一组 GIF 作为板载/外接 Flash 同步来源。
            if gif_idx == 0:
                bin_name = f"/gif_data/gif_f{frame_count}.bin"
                mirror_path = mirror_dir / f"gif_f{frame_count}.bin"
                save_frame_to_bin(frame, mirror_path)

                if EXPORT_TO_SPIFFS:
                    bin_path = data_dir / bin_name.lstrip("/")
                    save_frame_to_bin(frame, bin_path)

                frame_bin_names.append(bin_name)

            frame_count += 1

        print(f"  已生成 {frame_count} 帧")

    if not frame_bin_names:
        raise RuntimeError("第一组 GIF 未生成任何帧，无法写入 gif_frames 配置")

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
    print(f"SD GIF 输出目录: {sd_root}")
    print("将 gif_for_sd 下的 gif_0, gif_1... 子目录复制到 SD 卡根目录即可播放 SD GIF。")


if __name__ == "__main__":
    main()
