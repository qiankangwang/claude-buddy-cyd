#include "nus.h"
#include <NimBLEDevice.h>
#include <esp_mac.h>
#include <vector>

namespace ble {

Nus nus;

static const char *SVC_UUID = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
static const char *RX_UUID = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"; // desktop->device
static const char *TX_UUID = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"; // device->desktop

static NimBLEServer *server = nullptr;
static NimBLECharacteristic *txChar = nullptr;
static char devName[24] = "Claude-CYD";

static volatile bool g_connected = false;
static volatile bool g_secure = false;
static volatile uint16_t g_conn = 0;
static volatile uint32_t g_passkey = 0;
static volatile bool g_passkeyPending = false;

static SemaphoreHandle_t g_mtx = nullptr;
static std::vector<String> g_inQ; // complete RX lines awaiting the app task
static String g_rxBuf;            // partial line accumulator (BLE task only)

static void startAdvertising() {
  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  adv->stop();
  adv->addServiceUUID(SVC_UUID);
  adv->setName(devName);
  adv->setScanResponse(true);
  adv->start();
}

class ServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *s, ble_gap_conn_desc *desc) override {
    g_connected = true;
    g_conn = desc->conn_handle;
  }
  void onDisconnect(NimBLEServer *s) override {
    g_connected = false;
    g_secure = false;
    g_passkeyPending = false;
    startAdvertising();
  }
  uint32_t onPassKeyRequest() override {
    uint32_t k = esp_random() % 1000000UL; // 6-digit display passkey
    g_passkey = k;
    g_passkeyPending = true;
    return k;
  }
  void onAuthenticationComplete(ble_gap_conn_desc *desc) override {
    g_secure = desc->sec_state.encrypted;
    g_passkeyPending = false;
  }
};

class RxCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *c) override {
    std::string v = c->getValue();
    for (char ch : v) {
      if (ch == '\n') {
        if (xSemaphoreTake(g_mtx, portMAX_DELAY) == pdTRUE) {
          g_inQ.push_back(g_rxBuf);
          xSemaphoreGive(g_mtx);
        }
        g_rxBuf = "";
      } else if (ch != '\r') {
        g_rxBuf += ch;
      }
    }
  }
};

void Nus::begin() {
  g_mtx = xSemaphoreCreateMutex();

  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_BT);
  snprintf(devName, sizeof(devName), "Claude-CYD-%02X%02X", mac[4], mac[5]);

  NimBLEDevice::init(devName);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  // LE Secure Connections + bonding + MITM, device displays a passkey.
  NimBLEDevice::setSecurityAuth(true, true, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);

  server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCB());

  NimBLEService *svc = server->createService(SVC_UUID);
  NimBLECharacteristic *rx = svc->createCharacteristic(
      RX_UUID,
      NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC |
          NIMBLE_PROPERTY::WRITE_AUTHEN);
  rx->setCallbacks(new RxCB());
  txChar = svc->createCharacteristic(
      TX_UUID,
      NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ_ENC |
          NIMBLE_PROPERTY::READ_AUTHEN);
  svc->start();

  startAdvertising();
}

void Nus::pollLines(LineHandler handler) {
  std::vector<String> batch;
  if (g_mtx && xSemaphoreTake(g_mtx, 0) == pdTRUE) {
    batch.swap(g_inQ);
    xSemaphoreGive(g_mtx);
  }
  for (auto &line : batch)
    handler(line);
}

void Nus::send(const String &line) {
  if (!g_connected || !txChar)
    return;
  String payload = line;
  payload += '\n';
  uint16_t mtu = server ? server->getPeerMTU(g_conn) : 23;
  int chunk = (mtu > 3) ? (int)mtu - 3 : 20;
  int n = payload.length();
  for (int off = 0; off < n; off += chunk) {
    int len = min(chunk, n - off);
    txChar->setValue((uint8_t *)payload.c_str() + off, len);
    txChar->notify();
    delay(2);
  }
}

bool Nus::connected() const { return g_connected; }
bool Nus::secure() const { return g_secure; }

bool Nus::consumePasskey(uint32_t &out) {
  if (!g_passkeyPending)
    return false;
  out = g_passkey;
  g_passkeyPending = false;
  return true;
}

void Nus::unpair() { NimBLEDevice::deleteAllBonds(); }

const char *Nus::deviceName() const { return devName; }

} // namespace ble
