#include "character.h"
#include <AnimatedGIF.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <LittleFS.h>
#include <map>
#include <vector>

namespace render {

Character character;

// Character region constants come from character.h (render::REG_*).
static TFT_eSPI *g_tft = nullptr;
static TFT_eSprite *g_spr = nullptr; // off-screen double-buffer for the region
static int g_tint = 1;               // 0 none, 1 warmer/orange, 2 pinker

// Subtle hue nudge for the character art (applied per drawn pixel).
static uint16_t tintColor(uint16_t c) {
  if (g_tint == 0 || c == 0x0000)
    return c;
  int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
  if (g_tint == 1) { // a touch more orange (subtle)
    r = r * 106 / 100;
    b = b * 85 / 100;
  } else { // a touch pinker
    r = r * 108 / 100;
    g = g * 94 / 100;
    b = b * 126 / 100;
  }
  if (r > 31) r = 31;
  if (g > 63) g = 63;
  if (b > 31) b = 31;
  return (uint16_t)((r << 11) | (g << 5) | b);
}
static AnimatedGIF gif;
static fs::File g_file; // single open GIF file for the read/seek callbacks

static std::map<String, std::vector<String>> g_states;
static String g_packDir;
static String g_cur;
static int g_idx = 0;
static bool g_open = false;

// per-open draw geometry (set after the canvas size is known).
// g_div = integer downsample factor (>=1); we never upscale so a small GIF
// stays its native size and a large one is shrunk to fit the box below.
static const int BOX_W = 190, BOX_H = 140; // fill target inside the region
static int g_sx256 = 256, g_offX = 0, g_offY = 0; // scale ×256 (nearest-neighbour)
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
  if (n < 0)
    n = 0; // LittleFS hiccup -> report 0 read, don't feed -1 to the decoder
  pf->iPos = f->position();
  return n;
}
static int32_t gifSeek(GIFFILE *pf, int32_t pos) {
  fs::File *f = (fs::File *)pf->fHandle;
  f->seek(pos);
  pf->iPos = pos;
  return pos;
}

// ---- AnimatedGIF draw callback ----
// One source scanline, nearest-neighbour scaled by g_sx256/256 (up OR down) to
// fill the screen, composited into the off-screen buffer. Assumes full-frame /
// transparent-over-black GIFs (transparent -> black, correct on the black bg).
static void gifDraw(GIFDRAW *pDraw) {
  static uint16_t line[REG_W];
  int S = g_sx256;
  uint16_t *pal = pDraw->pPalette;
  uint8_t *s = pDraw->pPixels;
  uint8_t tr = pDraw->ucTransparent;
  bool hasT = pDraw->ucHasTransparency;
  int iw = pDraw->iWidth;

  int outW = (iw * S) >> 8;
  if (outW > REG_W)
    outW = REG_W;
  if (outW < 1)
    return;
  for (int ox = 0; ox < outW; ox++) {
    int sx = (ox << 8) / S;
    if (sx >= iw)
      sx = iw - 1;
    uint8_t c = s[sx];
    line[ox] = (hasT && c == tr) ? 0x0000 : tintColor(pal[c]);
  }
  int srcY = pDraw->iY + pDraw->y;
  int oy0 = (srcY * S) >> 8;
  int oy1 = ((srcY + 1) * S) >> 8;
  if (oy1 <= oy0)
    oy1 = oy0 + 1; // downscale: at least one row
  int dx = g_offX + ((pDraw->iX * S) >> 8);
  for (int oy = oy0; oy < oy1; oy++) {
    int yy = g_offY + oy;
    if (yy < REG_Y || yy >= REG_Y + REG_H)
      continue;
    if (g_spr)
      g_spr->pushImage(dx - REG_X, yy - REG_Y, outW, 1, line);
    else
      g_tft->pushImage(dx, yy, outW, 1, line);
  }
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
    if (cw <= 0) cw = 120;
    if (ch <= 0) ch = 120;
    int sxx = (BOX_W << 8) / cw, syy = (BOX_H << 8) / ch;
    g_sx256 = sxx < syy ? sxx : syy; // fill the box, keep aspect (up or down)
    if (g_sx256 < 16) g_sx256 = 16;
    int outW = (cw * g_sx256) >> 8, outH = (ch * g_sx256) >> 8;
    g_offX = REG_X + (REG_W - outW) / 2;
    g_offY = REG_Y + (REG_H - outH) / 2;
    if (g_spr)
      g_spr->fillSprite(TFT_BLACK); // new file -> clear buffer (kills residue)
    g_nextFrame = 0;
  }
  return g_open;
}

// Open the current state's GIF; if that fails (missing/corrupt file), fall back
// to a known-good state so the pet never goes permanently black.
bool Character::openCurrentOrFallback() {
  if (openCurrent())
    return true;
  Serial.printf("[character] open failed for '%s', falling back\n",
                g_cur.c_str());
  static const char *fb[] = {"idle", "sleep"};
  for (const char *f : fb) {
    if (g_cur == f)
      continue;
    g_cur = f;
    g_idx = 0;
    if (openCurrent())
      return true;
  }
  return false;
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
  gif.begin(GIF_PALETTE_RGB565_LE);
  g_spr = new TFT_eSprite(g_tft);
  g_spr->setColorDepth(16);
  if (!g_spr->createSprite(REG_W, REG_H)) {
    delete g_spr;
    g_spr = nullptr; // out of heap -> fall back to direct draw
  } else {
    g_spr->setSwapBytes(true); // match palette byte order (fixes R/B swap)
  }
  Serial.printf("[character] sprite=%d freeHeap=%u maxAlloc=%u\n",
                g_spr != nullptr, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  return loadPack(packDir);
}

// (Re)load a character pack dir (e.g. "/clawd"); safe to call at runtime.
bool Character::loadPack(const char *packDir) {
  if (g_open) {
    gif.close();
    g_open = false;
  }
  g_states.clear();
  g_cur = "";
  g_idx = 0;
  g_packDir = packDir;
  fs::File mf = LittleFS.open(g_packDir + "/manifest.json", "r");
  if (!mf) {
    Serial.printf("[character] no manifest at %s\n", g_packDir.c_str());
    loaded_ = false;
    return false;
  }
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, mf);
  mf.close();
  if (e) {
    Serial.println("[character] manifest parse error");
    loaded_ = false;
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
  loaded_ = !g_states.empty();
  Serial.printf("[character] loaded %d states from %s\n",
                (int)g_states.size(), packDir);
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
  openCurrentOrFallback();
}

void Character::update() {
  if (!loaded_ || !g_open)
    return;
  if (millis() < g_nextFrame)
    return;
  int rc = gif.playFrame(false, &g_frameDelay); // composites into the buffer
  if (g_spr)
    g_spr->pushSprite(REG_X, REG_Y); // flicker-free blit of the whole region
  g_nextFrame = millis() + (g_frameDelay > 0 ? g_frameDelay : 80);
  if (rc == 0) { // reached end of this GIF -> loop / advance idle carousel
    auto it = g_states.find(g_cur);
    if (it != g_states.end() && it->second.size() > 1)
      g_idx = (g_idx + 1) % it->second.size();
    gif.close();
    g_open = false;
    openCurrentOrFallback();
    g_nextFrame = millis();
  }
}

} // namespace render
