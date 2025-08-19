# esp32-ssd1306-videoplayer
Welp it used to playing video on SSD1306 for fun (look so bad lmao)
HOW TO USE IT:
 video_to_ssd1306.py

Convert any video into a binary movie file playable on an SSD1306 OLED with an ESP32-S3.
Uses Floyd–Steinberg dithering so it looks crispy even on 128×64 pixels.

--- Requirements ---

Install dependencies first:

pip install opencv-python numpy Pillow

📦 Usage

Basic format:

python video_to_ssd1306.py <input_video> <output_file> [options]

Example: Standard movie with header (recommended)
python video_to_ssd1306.py komi.mp4 movie.bin --width 128 --height 64 --fps 15 --preview


Takes komi.mp4

Resizes & dithers to 128×64

Outputs movie.bin with a header (for your ESP32-S3 code to sync FPS)

Also writes PNG preview frames (preview_00000.png, …)

Example: Raw frames (legacy mode, no header)
python video_to_ssd1306.py anime.mp4 movie.bin --raw


👉 ESP will just loop raw frames at ~15fps.

⚙️ Options
Flag	Description
--width N	Output width (default 128)
--height N	Output height (must be multiple of 8, default 64)
--fps N	Target FPS (default = source fps)
--invert	Invert pixels (white ↔ black)
--no-serpentine	Disable serpentine scan (normal Floyd–Steinberg is zig-zag)
--preview	Save PNGs for each frame (helpful to check dithering)
--raw	Don’t write header (just frames back-to-back)
📂 Output Format

Default (movie.bin) has:

Offset	Size	Field
0	8	Magic = "SSD1306V1"
8	2	Width
10	2	Height (multiple of 8)
12	4	FPS ×1000 (e.g. 15000 = 15fps)
16	4	Frame count
20	1	Flags (bit0 = inverted)
21	11	Reserved (zeros)
32	…	Frame0, Frame1, …

Each frame is packed in SSD1306 page format (8px vertical per byte).

--- Notes

Use header mode unless you really need raw.

SSD1306 height must be divisible by 8 (64 works).

ESP32-S3 player code already understands this format → just drop movie.bin into LittleFS.

Previews help check dithering quality before uploading.
