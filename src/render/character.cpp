#include "character.h"
#include <AnimatedGIF.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <LittleFS.h>
#include <map>
#include <vector>

namespace render {

Character character;

// Character region (between the top status line and the bottom UI).
static const int REG_X = 0, REG_Y = 34, REG_W = 240, REG_H = 176;

static TFT_eSPI *g_tft = nullptr;
static AnimatedGIF gif;
static fs::File g_file; // single open GIF file for the read/seek callbacks

static std::map<String, std::vector<String>> g_states;
static String g_packDir;
static String g_cur;
static int g_idx = 0;
static bool g_open = false;

// per-open draw geometry (set after the canvas size is known)
static int g_scale = 1, g_offX = 0, g_offY = 0;
static int g_frameDelay = 0;
static uint32_t g_nextFrame = 0;

// ---- AnimatedGIF file callbacks (LittleFS) ----
static void *gifOpen(const char *fname, int32_t *pSize) {
  g_file = LittleFS.open(fname, "r");
  if (!g_file)
    return nullptr;
  *pSize = g_file.size();
  return (void *)&g_file;
}
static void gifClose(void *h) {
  if (g_file)
    g_file.close();
}
static int32_t gifRead(GIFFILE *pf, uint8_t *buf, int32_t len) {
  fs::File *f = (fs::File *)pf->fHandle;
  int32_t n = f->read(buf, len);
  pf->iPos = f->position();
  return n;
}
static int32_t gifSeek(GIFFILE *pf, int32_t pos) {
  fs::File *f = (fs::File *)pf->fHandle;
  f->seek(pos);
  pf->iPos = pos;
  return pos;
}

// ---- AnimatedGIF draw callback: one scanline, integer-scaled, centered ----
static void gifDraw(GIFDRAW *pDraw) {
  static uint16_t line[REG_W];
  int w = pDraw->iWidth;
  if (w > REG_W)
    w = REG_W;
  uint16_t *pal = pDraw->pPalette;
  uint8_t *s = pDraw->pPixels;
  uint8_t tr = pDraw->ucTransparent;
  bool hasT = pDraw->ucHasTransparency;
  int scale = g_scale;

  int n = 0;
  for (int x = 0; x < w; x++) {
    uint8_t c = s[x];
    uint16_t col = (hasT && c == tr) ? 0x0000 : pal[c];
    for (int k = 0; k < scale && n < REG_W; k++)
      line[n++] = col;
  }
  int dx = g_offX + pDraw->iX * scale;
  int dy = g_offY + (pDraw->iY + pDraw->y) * scale;
  for (int sy = 0; sy < scale; sy++)
    g_tft->pushImage(dx, dy + sy, n, 1, line);
}

void Character::clearArea() {
  if (g_tft)
    g_tft->fillRect(REG_X, REG_Y, REG_W, REG_H, TFT_BLACK);
}

bool Character::openCurrent() {
  auto it = g_states.find(g_cur);
  if (it == g_states.end() || it->second.empty())
    return false;
  if (g_idx >= (int)it->second.size())
    g_idx = 0;
  const String &path = it->second[g_idx];
  g_open = gif.open(path.c_str(), gifOpen, gifClose, gifRead, gifSeek, gifDraw);
  if (g_open) {
    int cw = gif.getCanvasWidth(), ch = gif.getCanvasHeight();
    if (cw <= 0) cw = 96;
    if (ch <= 0) ch = 96;
    int sx = REG_W / cw, sy = REG_H / ch;
    g_scale = sx < sy ? sx : sy;
    if (g_scale < 1) g_scale = 1;
    g_offX = REG_X + (REG_W - cw * g_scale) / 2;
    g_offY = REG_Y + (REG_H - ch * g_scale) / 2;
    g_nextFrame = 0;
  }
  return g_open;
}

bool Character::begin(hal::Display &disp, const char *packDir) {
  g_tft = &disp.tft();
  g_tft->setSwapBytes(true); // AnimatedGIF LE palette -> TFT_eSPI
  // Our data partition is labelled "littlefs" (subtype spiffs); the default
  // LittleFS label is "spiffs", so pass the label explicitly.
  if (!LittleFS.begin(false, "/littlefs", 10, "littlefs")) {
    Serial.println("[character] LittleFS mount failed");
    return false;
  }
  g_packDir = packDir;
  String manifestPath = g_packDir + "/manifest.json";
  fs::File mf = LittleFS.open(manifestPath, "r");
  if (!mf) {
    Serial.printf("[character] no manifest at %s\n", manifestPath.c_str());
    return false;
  }
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, mf);
  mf.close();
  if (e) {
    Serial.println("[character] manifest parse error");
    return false;
  }
  JsonObject st = doc["states"].as<JsonObject>();
  for (JsonPair kv : st) {
    std::vector<String> files;
    JsonVariant v = kv.value();
    if (v.is<JsonArray>()) {
      for (JsonVariant f : v.as<JsonArray>())
        files.push_back(g_packDir + "/" + (const char *)f);
    } else {
      files.push_back(g_packDir + "/" + (const char *)v);
    }
    g_states[String(kv.key().c_str())] = files;
  }
  gif.begin(GIF_PALETTE_RGB565_LE);
  loaded_ = !g_states.empty();
  Serial.printf("[character] loaded %d states from %s\n", (int)g_states.size(),
                packDir);
  return loaded_;
}

void Character::setState(const char *state) {
  if (!loaded_ || g_cur == state)
    return;
  g_cur = state;
  g_idx = 0;
  if (g_open) {
    gif.close();
    g_open = false;
  }
  clearArea();
  openCurrent();
}

void Character::update() {
  if (!loaded_ || !g_open)
    return;
  if (millis() < g_nextFrame)
    return;
  int rc = gif.playFrame(false, &g_frameDelay);
  g_nextFrame = millis() + (g_frameDelay > 0 ? g_frameDelay : 80);
  if (rc == 0) { // reached end of this GIF -> loop / advance idle carousel
    auto it = g_states.find(g_cur);
    if (it != g_states.end() && it->second.size() > 1)
      g_idx = (g_idx + 1) % it->second.size();
    gif.close();
    g_open = false;
    openCurrent();
    g_nextFrame = millis();
  }
}

} // namespace render
