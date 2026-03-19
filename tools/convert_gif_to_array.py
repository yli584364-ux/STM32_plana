import os
from pathlib import Path

from PIL import Image, ImageSequence

from tools.convert_plana_to_sd import rgb888_to_rgb565, TARGET_WIDTH, TARGET_HEIGHT


def save_gif_to_arr_frames(gif_path: Path, out_dir: Path, max_frames: int | None = None) -> None:
    """把一个 GIF 拆成多帧，每帧转成 RGB565 数组文本 (.arr)。

    生成的目录结构示例（在工程根目录下运行脚本时）：
        photo/gif_test.gif  ->  gif_1/frame_000.arr, frame_001.arr, ...

    之后把 gif_1 整个目录拷到 SD 卡根目录，
    main.cpp 会扫描 /gif_1 目录下所有 .arr 按文件名顺序当作 GIF 帧播放。
    """

    out_dir.mkdir(parents=True, exist_ok=True)

    img = Image.open(gif_path)

    frame_index = 0
    for frame in ImageSequence.Iterator(img):
        if max_frames is not None and frame_index >= max_frames:
            break

        frame_rgb = frame.convert("RGB")
        frame_rgb = frame_rgb.resize((TARGET_WIDTH, TARGET_HEIGHT), Image.Resampling.BILINEAR)
        pixels = [(int(r), int(g), int(b)) for (r, g, b) in frame_rgb.getdata()]

        values = [rgb888_to_rgb565(r, g, b) for (r, g, b) in pixels]

        arr_name = f"frame_{frame_index:03d}.arr"
        arr_path = out_dir / arr_name
        print(f"GIF 帧 {frame_index} -> {arr_path}")

        with open(arr_path, "w", encoding="utf-8") as f:
            per_line = 16
            for i in range(0, len(values), per_line):
                chunk = values[i : i + per_line]
                line = ", ".join(f"0x{v:04X}" for v in chunk)
                if i + per_line < len(values):
                    f.write(line + ",\n")
                else:
                    f.write(line + "\n")

        frame_index += 1

    print(f"共生成 {frame_index} 帧 .arr 文件到 {out_dir}")


def main():
    root = Path(__file__).resolve().parents[1]
    photo_dir = root / "photo"

    # 默认处理 photo 下的 gif_test.gif，如果有多个可以自行修改或扩展
    gif_files = sorted(photo_dir.glob("*.gif"))
    if not gif_files:
        raise FileNotFoundError(f"在 {photo_dir} 下没有找到任何 .gif 文件")

    # 为每个 gif 创建一个 gif_X 目录，X 从 1 开始，便于和 main.cpp 中的扫描逻辑对应
    for idx, gif_path in enumerate(gif_files, start=1):
        out_dir = root / f"gif_{idx}"
        print(f"处理 GIF {gif_path.name} -> 目录 {out_dir}")
        save_gif_to_arr_frames(gif_path, out_dir)

    print("全部 GIF 已转换为 .arr 帧，拷贝 gif_X 目录到 SD 卡根目录即可播放 GIF。")


if __name__ == "__main__":
    main()
