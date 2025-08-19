/* ESP32-S3 — SSD1306 movie player + web uploader (LittleFS)
   - Connects to your WiFi (edit WIFI_SSID / WIFI_PASS)
   - Hosts a web page at http://<ESP_IP>/ where you can upload a .bin
   - Saves upload to /movie.bin and immediately switches playback to it
   - Playback uses page-packed SSD1306 frames (matches python converter header)
   - I2C pins: SDA_PIN=8, SCL_PIN=9 (edit if needed)
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <U8g2lib.h>
#include <FS.h>
#include <LittleFS.h>

//
// === CONFIG ===
//
const char* WIFI_SSID = "WIFI_NAME";      // <-- replace
const char* WIFI_PASS = "PASSWORD";      // <-- replace
const char* UPLOAD_FILENAME = "/movie.bin";

#define SDA_PIN 8   // edit if needed
#define SCL_PIN 9   // edit if needed

// U8x8 (tile API) constructor matching your previous usage
U8X8_SSD1306_128X64_NONAME_HW_I2C u8x8(/* reset=*/ U8X8_PIN_NONE, /* clock=*/ SCL_PIN, /* data=*/ SDA_PIN);

WebServer server(80);

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
uint32_t frameDelayMs = 66;      // computed from header (ms per frame)
bool hasHeader = false;

// Forward
bool openMovieFileAndReadHeader();
void handleRoot();
void handleUpload();
void handleNotFound();
void sendStatusPage();

//
// ===== helpers =====
//
bool safeCloseFile(File &file) {
  if (file) {
    file.close();
    return true;
  }
  return false;
}

bool readHeaderFromFile(File &file, MovieHeader &outHdr) {
  if (!file) return false;
  file.seek(0);
  if (file.read((uint8_t*)&outHdr, sizeof(outHdr)) != sizeof(outHdr)) return false;
  if (strncmp(outHdr.magic, "SSD1306V1", 8) != 0) return false;
  if ((outHdr.height % 8) != 0) return false;
  return true;
}

// Convert hdr.fps_milli (fps*1000) to ms per frame (rounded)
uint32_t fpsMilliToMs(uint32_t fps_milli) {
  if (fps_milli == 0) return 66; // fallback ~15fps
  // ms_per_frame = 1000 / fps  => fps = fps_milli/1000 => ms = 1000 / (fps_milli/1000) = 1e6 / fps_milli
  return (uint32_t)((1000000ULL + fps_milli/2) / fps_milli); // rounded
}

//
// ===== open movie and set playback params =====
//
bool openMovieFileAndReadHeader() {
  if (f) { safeCloseFile(f); }
  f = LittleFS.open(UPLOAD_FILENAME, "r");
  if (!f) {
    Serial.println("openMovieFileAndReadHeader: file not found");
    hasHeader = false;
    return false;
  }

  MovieHeader tmp;
  bool ok = readHeaderFromFile(f, tmp);
  if (!ok) {
    // Legacy fallback: assume raw frames 128x64 @ 15fps, frame data from start
    Serial.println("openMovieFileAndReadHeader: header invalid -> legacy raw mode");
    hasHeader = false;
    hdr.width = 128;
    hdr.height = 64;
    hdr.fps_milli = 15000;
    hdr.frame_count = 0;
    hdr.flags = 0;
    frameSize = hdr.width * (hdr.height / 8);
    frameDelayMs = 1000 / 15;
    f.seek(0);
    return true;
  }

  // Good header
  memcpy(&hdr, &tmp, sizeof(MovieHeader));
  hasHeader = true;
  frameSize = (uint32_t)hdr.width * (hdr.height / 8);
  frameDelayMs = fpsMilliToMs(hdr.fps_milli);
  // Seek to first frame (header size = 32)
  f.seek(sizeof(MovieHeader));
  Serial.printf("Movie opened: %u x %u, fps_milli=%u, frames=%u, frameSize=%u, delay=%ums\n",
                hdr.width, hdr.height, hdr.fps_milli, hdr.frame_count, frameSize, frameDelayMs);
  return true;
}

//
// ===== Web server handlers =====
//
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!doctype html>
<html>
  <head>
    <meta charset="utf-8">
    <title>SSD1306 Movie Upload</title>
  </head>
  <body>
    <h2>Upload movie.bin</h2>
    <p>Upload a valid <b>movie.bin</b> (SSD1306V1 header or raw frames).</p>
    <form method="POST" action="/upload" enctype="multipart/form-data">
      <input type="file" name="moviefile" accept=".bin"><br><br>
      <input type="submit" value="Upload">
    </form>
    <hr>
    <a href="/status">Status</a>
  </body>
</html>
)rawliteral";

void handleRoot() {
  server.sendHeader("Connection", "close");
  server.send(200, "text/html", INDEX_HTML);
}

void handleUpload() {
  HTTPUpload& upload = server.upload();
  static File uploadFile = File();

  if (upload.status == UPLOAD_FILE_START) {
    Serial.printf("Upload start: name=%s size=%u\n", upload.filename.c_str(), (unsigned)upload.totalSize);
    // Close playing file to avoid conflicts
    if (f) { safeCloseFile(f); }
    // Remove existing file and open new
    if (LittleFS.exists(UPLOAD_FILENAME)) LittleFS.remove(UPLOAD_FILENAME);
    uploadFile = LittleFS.open(UPLOAD_FILENAME, "w");
    if (!uploadFile) {
      Serial.println("Failed to open upload file for writing");
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadFile) {
      uploadFile.write(upload.buf, upload.currentSize);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (uploadFile) {
      uploadFile.close();
      Serial.printf("Upload finished, saved as %s (%u bytes)\n", UPLOAD_FILENAME, (unsigned)upload.totalSize);
    }
    // Re-open and reload header & playback params
    delay(50);
    if (!openMovieFileAndReadHeader()) {
      // try open even if no header (legacy)
      f = LittleFS.open(UPLOAD_FILENAME, "r");
      if (f) {
        hdr.width = 128; hdr.height = 64; hdr.fps_milli = 15000; hdr.flags = 0;
        frameSize = hdr.width * (hdr.height/8);
        frameDelayMs = 1000 / 15;
        hasHeader = false;
        Serial.println("Opened uploaded file in legacy mode");
      }
    }
  }
}

void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}

void sendStatusPage() {
  String s;
  s += "<html><body><h3>SSD1306 Player Status</h3>";
  s += "<p>IP: " + WiFi.localIP().toString() + "</p>";
  if (LittleFS.exists(UPLOAD_FILENAME)) {
    File info = LittleFS.open(UPLOAD_FILENAME, "r");
    size_t sz = info.size();
    info.close();
    s += "<p>/movie.bin exists — size: " + String(sz) + " bytes</p>";
    if (hasHeader) {
      s += "<p>Header: " + String(hdr.width) + "x" + String(hdr.height) + " fps_milli=" + String(hdr.fps_milli) + " frames=" + String(hdr.frame_count) + "</p>";
      s += "<p>frameSize: " + String(frameSize) + " bytes</p>";
      s += "<p>frameDelayMs: " + String(frameDelayMs) + " ms</p>";
    } else {
      s += "<p>No header — legacy raw mode (128x64 @ 15fps assumed)</p>";
    }
  } else {
    s += "<p>/movie.bin not found</p>";
  }
  s += "<p><a href=\"/\">Upload page</a></p></body></html>";
  server.send(200, "text/html", s);
}

//
// ===== setup & loop =====
//
void setup() {
  Serial.begin(115200);
  delay(50);

  // init display
  u8x8.begin();
  u8x8.setI2CAddress(0x3C << 1);
  // note: insanely high bus clocks can break some modules; 400k-1MHz typical safe.
  u8x8.setBusClock(3400000);
  u8x8.clear();

  // start LittleFS
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed");
    while (1) { delay(200); }
  }

  // Connect to WiFi (station mode)
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting WiFi");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(250);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("Connected. IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println();
    Serial.println("WiFi connect failed — starting AP fallback");
    WiFi.softAP("ESP32_Player", "12345678");
    Serial.print("AP IP: ");
    Serial.println(WiFi.softAPIP());
  }

  // Setup web routes
  server.on("/", HTTP_GET, handleRoot);
  server.on("/status", HTTP_GET, sendStatusPage);
  server.on(
    "/upload", HTTP_POST,
    [](){ server.send(200, "text/plain", "Upload complete"); },
    handleUpload
  );
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("HTTP server started");

  // try open movie on boot
  openMovieFileAndReadHeader();
  if (!f) {
    // if no movie.bin, try create an empty file placeholder (optional)
    Serial.println("No movie.bin found at boot");
  }
}

void loop() {
  // handle web server
  server.handleClient();

  // original playback loop logic
  static uint32_t lastMs = 0;
  uint32_t now = millis();
  if (!f) {
    // no file to play — blank screen
    yield();
    delay(100);
    return;
  }
  if (now - lastMs < frameDelayMs) { yield(); return; }
  lastMs = now;

  // Allocate buffer (PSRAM-friendly on S3 if configured)
  std::unique_ptr<uint8_t[]> buf(new uint8_t[frameSize]);
  int r = f.read(buf.get(), frameSize);
  if (r < (int)frameSize) {
    // EOF -> loop (skip header if present)
    f.seek(hasHeader ? sizeof(MovieHeader) : 0, SeekSet);
    r = f.read(buf.get(), frameSize);
    if (r < (int)frameSize) {
      // corrupt/short file: bail until next loop iteration
      Serial.println("Short frame or corrupt file");
      yield(); delay(50);
      return;
    }
  }

  // Draw tiles (8x8). Data is in page order already.
  const uint8_t pages = hdr.height / 8;         // e.g. 8 pages for 64px high
  const uint8_t tilesPerRow = hdr.width / 8;    // e.g. 16 tiles for 128px wide

  for (uint8_t page = 0; page < pages; ++page) {
    uint32_t rowOff = (uint32_t)page * hdr.width;
    u8x8.drawTile(0, page, tilesPerRow, buf.get() + rowOff);
  }

  // keep WDT / RTOS tasks happy
  yield();
  delay(1);
}
