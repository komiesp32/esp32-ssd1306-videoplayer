#include <Arduino.h>
#include <U8g2lib.h>
#include <FS.h>
#include <LittleFS.h>

// ---- OLED (SSD1306 128x64, I2C on custom pins) ----
#define SDA_PIN 8  //edit this
#define SCL_PIN 9 //edit this
// U8x8 = tile-based API (matches SSD1306 page format)
U8X8_SSD1306_128X64_NONAME_HW_I2C u8x8(/* reset=*/ U8X8_PIN_NONE, /* clock=*/ SCL_PIN, /* data=*/ SDA_PIN);

// ---- Movie header (must match the Python script) ----
struct __attribute__((packed)) MovieHeader {
  char     magic[8];      // "SSD1306V1"
  uint16_t width;         // e.g. 128
  uint16_t height;        // e.g. 64 (must be multiple of 8)
  uint32_t fps_milli;     // fps * 1000 (e.g. 15000 for 15 fps)
  uint32_t frame_count;   // number of frames
  uint8_t  flags;         // bit0 = inverted
  uint8_t  reserved[11];
};

File f;
MovieHeader hdr{};
uint32_t frameSize = 0;          // width * (height/8)
uint32_t frameDelayMs = 66;      // computed from header
bool hasHeader = false;

bool readHeader() {
  if (!f) return false;
  if (f.read((uint8_t*)&hdr, sizeof(hdr)) != sizeof(hdr)) return false;
  if (strncmp(hdr.magic, "SSD1306V1", 8) != 0) return false;
  if ((hdr.height % 8) != 0) return false;

  frameSize = (uint32_t)hdr.width * (hdr.height / 8);

  // ms per frame = 1000000 / fps_milli  (since fps_milli = fps*1000)
  if (hdr.fps_milli == 0) hdr.fps_milli = 15000; // fallback 15 fps
  frameDelayMs = (1000000UL / hdr.fps_milli);
  if (frameDelayMs == 0) frameDelayMs = 1;

  return true;
}

void setup() {
  Serial.begin(115200);

  // Init display
  u8x8.begin();
  u8x8.setI2CAddress(0x3C << 1);   // default 0x3C
  u8x8.setBusClock(3400000L);        // 3.4MHz I2C for turbo speed, fix if it too fast for your SSD1306
  u8x8.clear();

  // FS
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed");
    while (1) { delay(100); }
  }

  f = LittleFS.open("/movie.bin", "r");
  if (!f) {
    Serial.println("Open /movie.bin failed");
    while (1) { delay(100); }
  }

  hasHeader = readHeader();
  if (!hasHeader) {
    // Legacy raw mode fallback: 128x64 @ 15fps, frames back-to-back
    Serial.println("Header invalid → legacy raw mode");
    f.seek(0, SeekSet);
    hdr.width = 128;
    hdr.height = 64;
    hdr.fps_milli = 15000;
    hdr.frame_count = 0;      // unknown
    hdr.flags = 0;
    frameSize = 128 * (64 / 8);
    frameDelayMs = 1000 / 15;
  }

  // Clear screen
  u8x8.clear();
}

void loop() {
  static uint32_t lastMs = 0;
  uint32_t now = millis();
  if (now - lastMs < frameDelayMs) { yield(); return; }
  lastMs = now;

  // Grab one full frame (page-packed SSD1306 format)
  std::unique_ptr<uint8_t[]> buf(new uint8_t[frameSize]);
  int r = f.read(buf.get(), frameSize);
  if (r < (int)frameSize) {
    // EOF → loop (skip header if present)
    f.seek(hasHeader ? sizeof(MovieHeader) : 0, SeekSet);
    f.read(buf.get(), frameSize);
  }

  // Optional runtime invert if you want to flip regardless of header flag
  // bool runtimeInvert = false;
  // if (runtimeInvert ^ (hdr.flags & 0x01)) { for (uint32_t i=0;i<frameSize;i++) buf[i] ^= 0xFF; }

  // Draw using tiles. Each tile is 8x8 pixels; data is already in page order.
  const uint8_t pages = hdr.height / 8;         // e.g. 8 pages for 64px high
  const uint8_t tilesPerRow = hdr.width / 8;    // 16 tiles for 128px wide

  for (uint8_t page = 0; page < pages; ++page) {
    uint32_t rowOff = (uint32_t)page * hdr.width;
    // We can draw the entire row in one call: cnt = tilesPerRow
    u8x8.drawTile(/*x=*/0, /*y=*/page, /*cnt=*/tilesPerRow, buf.get() + rowOff);
  }

  // keep WDT happy
  yield();
  delay(1);
}
