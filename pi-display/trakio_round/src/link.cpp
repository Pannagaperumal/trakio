#include "link.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLESecurity.h>
#include <string>
#include "state.h"

#define DEVICE_NAME  "Trakio-Round"
#define FW_VERSION   "0.1.0"
#define NUS_SERVICE  "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX       "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_TX       "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"
#define RX_MAX_BYTES (40 * 1024)          // a dense map_area is ~20 KB

// Advertising: brisk right after boot/disconnect so the phone finds us fast,
// then slow to save power while nobody is listening.
// Phones reconnect in the background with low-duty scans (MIUI especially),
// so a 1 s interval was often missed for minutes; stay brisk for longer.
#define ADV_FAST_MS      (5UL * 60 * 1000)
#define ADV_FAST_UNITS   160              // 100 ms (0.625 ms units)
#define ADV_SLOW_UNITS   800              // 500 ms

static BLEServer *server = nullptr;
static BLECharacteristic *txChar = nullptr;
static std::string rxBuf;
static SemaphoreHandle_t rxMutex = nullptr;
static volatile bool connected = false;
static volatile bool subscribed = false;
static volatile bool helloPending = false;
static volatile bool advRestart = true;
static volatile uint16_t connHandle = 0;
static volatile uint16_t mtu = 23;
static uint32_t advStartedMs = 0;
static bool advFast = true;
static bool riding = false;

static const char *addrStr(const ble_addr_t &a) {
  static char b[18];
  snprintf(b, sizeof b, "%02X:%02X:%02X:%02X:%02X:%02X", a.val[5], a.val[4], a.val[3], a.val[2], a.val[1], a.val[0]);
  return b;
}

static void startAdvertising(bool fast) {
  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->stop();
  adv->setMinInterval(fast ? ADV_FAST_UNITS : ADV_SLOW_UNITS);
  adv->setMaxInterval(fast ? ADV_FAST_UNITS + 80 : ADV_SLOW_UNITS + 400);
  adv->start();
  advFast = fast;
  advStartedMs = millis();
}

class ServerCb : public BLEServerCallbacks {
  void onConnect(BLEServer *, ble_gap_conn_desc *desc) override {
    connHandle = desc->conn_handle;
    connected = true;
    helloPending = true;          // also covers phones that never subscribe
    Serial.printf("[link] connected: %s (bonded=%d encrypted=%d)\n", addrStr(desc->peer_id_addr),
                  desc->sec_state.bonded, desc->sec_state.encrypted);
  }
  void onDisconnect(BLEServer *, ble_gap_conn_desc *desc) override {
    Serial.printf("[link] disconnected: %s\n", addrStr(desc->peer_id_addr));
    connected = false;
    subscribed = false;
    mtu = 23;
    advRestart = true;
  }
  void onMtuChanged(BLEServer *, ble_gap_conn_desc *, uint16_t m) override { mtu = m; }
};

class RxCb : public BLECharacteristicCallbacks {
  // BLE host task: only append bytes; frames are decoded on the loop task.
  void onWrite(BLECharacteristic *c, ble_gap_conn_desc *) override {
    String v = c->getValue();
    if (!v.length()) return;
    xSemaphoreTake(rxMutex, portMAX_DELAY);
    if (rxBuf.size() + v.length() > RX_MAX_BYTES) rxBuf.clear();   // runaway line
    rxBuf.append(v.c_str(), v.length());
    xSemaphoreGive(rxMutex);
  }
};

// Pairing result: a failure here usually means the phone kept old keys the
// device no longer has — remove the device in Android Bluetooth settings and pair again.
class SecCb : public BLESecurityCallbacks {
  void onAuthenticationComplete(ble_gap_conn_desc *desc) override {
    Serial.printf("[link] security: %s encrypted=%d bonded=%d\n", addrStr(desc->peer_id_addr),
                  desc->sec_state.encrypted, desc->sec_state.bonded);
  }
};

class TxCb : public BLECharacteristicCallbacks {
  void onSubscribe(BLECharacteristic *, ble_gap_conn_desc *, uint16_t subValue) override {
    subscribed = subValue != 0;
    if (subscribed) helloPending = true;
    Serial.printf("[link] phone %s notifications\n", subscribed ? "subscribed to" : "unsubscribed from");
  }
};

void link_init() {
  rxMutex = xSemaphoreCreateMutex();
  rxBuf.reserve(1024);

  BLEDevice::init(DEVICE_NAME);
  BLEDevice::setMTU(247);
  // The Trakio app only talks to bonded devices: allow "Just Works" bonding.
  BLESecurity::setAuthenticationMode(true, false, true);
  BLESecurity::setCapability(ESP_IO_CAP_NONE);
  BLEDevice::setSecurityCallbacks(new SecCb());

  server = BLEDevice::createServer();
  server->setCallbacks(new ServerCb());
  BLEService *svc = server->createService(NUS_SERVICE);
  BLECharacteristic *rx = svc->createCharacteristic(NUS_RX, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  rx->setCallbacks(new RxCb());
  txChar = svc->createCharacteristic(NUS_TX, BLECharacteristic::PROPERTY_NOTIFY);
  txChar->setCallbacks(new TxCb());
  svc->start();

  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(NUS_SERVICE);
  adv->setScanResponse(true);
  startAdvertising(true);
  Serial.println("[link] advertising as " DEVICE_NAME);
}

bool link_connected() { return connected; }
bool link_subscribed() { return subscribed; }

void link_send(const String &json) {
  if (!connected || !subscribed || !txChar) return;
  String line = json + "\n";
  size_t chunk = (mtu > 3 ? mtu - 3 : 20);
  for (size_t off = 0; off < line.length(); off += chunk) {
    String part = line.substring(off, min(line.length(), off + chunk));
    txChar->setValue((uint8_t *)part.c_str(), part.length());
    txChar->notify();
  }
}

static void sendHello() {
  link_send("{\"type\":\"hello\",\"device\":\"" DEVICE_NAME "\",\"fw\":\"" FW_VERSION "\","
            "\"caps\":[\"map_area\",\"theme\",\"time\",\"battery\",\"imu\",\"call_action\",\"sos\"]}");
}

void link_set_riding(bool r) {
  riding = r;
  if (!connected || !server) return;
  // Units: interval 1.25 ms, timeout 10 ms. Riding: 30–50 ms. Idle: 200–400 ms with slave latency.
  if (r) server->updateConnParams(connHandle, 24, 40, 0, 400);
  else   server->updateConnParams(connHandle, 160, 320, 4, 600);
}

void link_poll() {
  // Decode complete lines.
  for (;;) {
    std::string line;
    xSemaphoreTake(rxMutex, portMAX_DELAY);
    size_t nl = rxBuf.find('\n');
    if (nl != std::string::npos) {
      line = rxBuf.substr(0, nl);
      rxBuf.erase(0, nl + 1);
    }
    xSemaphoreGive(rxMutex);
    if (nl == std::string::npos) break;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!line.empty()) state_handle_frame(line.c_str(), line.size());
  }

  if (helloPending && connected) {
    helloPending = false;
    sendHello();
    link_set_riding(riding);
  }
  if (advRestart && !connected) {
    advRestart = false;
    startAdvertising(true);
  } else if (!connected && advFast && millis() - advStartedMs > ADV_FAST_MS) {
    startAdvertising(false);
  }
}

void link_stop() {
  BLEDevice::getAdvertising()->stop();
  BLEDevice::deinit(false);
}
