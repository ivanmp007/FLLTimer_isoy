#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <ArduinoJson.h>
#include <FastLED.h>
#include <math.h>

// -----------------------------------------------------------------------
// STARTER_MODE: descomenta la siguiente linea para compilar como arrancador
// -----------------------------------------------------------------------
#define STARTER_MODE
// -----------------------------------------------------------------------

#define SERIAL_BAUD          115200

#define BUTTON_PIN           23
#define STARTER_BUTTON_PIN   22   // Pulsador de inicio de partida (dispositivo arrancador)
#define LED_PIN              16
#define NUM_LEDS             83
#define STARTER_IND_PIN      17   // LED indicador del arrancador (1x WS2812B)
#define STARTER_IND_NUM      1
#define LED_TYPE             WS2812B
#define COLOR_ORDER          GRB
#define BRIGHTNESS           40

#define MAX_NODES            20
#define NODE_TIMEOUT_MS      6000
#define WEB_MASTER_TIMEOUT   7000
#define BUTTON_DEBOUNCE_MS   30
#define STARTER_DEBOUNCE_MS  80    // debounce más largo para el pulsador de inicio
#define LONGPRESS_MS         800
#define STATUS_HEARTBEAT_MS  5000
#define STATUS_REQUEST_MS    3000

#define MUSTGO_BLINK_MS           200
#define MUSTGO_DELAY_MS           150000
#define FINAL_DURATION_MS         20000
#define PARTIDA_DURATION_MS       150000UL
#define PARTIDA_BLINK_SLOW_MS     500UL
#define PARTIDA_BLINK_FAST_MS     120UL

#define PACKET_MAGIC         0x4C454446UL
#define RX_QUEUE_SIZE        24

#define SET_ALL_REPEAT_MS    400
#define SET_ALL_MAX_MS      5000

CRGB leds[NUM_LEDS];
#ifdef STARTER_MODE
CRGB starterLed[STARTER_IND_NUM];
#endif
const uint8_t broadcastMac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

enum NodeState : uint8_t {
  REPOSO = 0,
  PREPARACION = 1,
  MUST_GO = 2,
  READY = 3,
  PARTIDA = 4,
  FINAL = 5
};

enum PacketType : uint8_t {
  PKT_STATUS = 1,
  PKT_SET_ALL_STATE = 2,
  PKT_REQUEST_STATUS = 3,
  PKT_SET_NODE_STATE = 4,
  PKT_CAN_START = 5,          // maestro → todos: bandera de inicio permitido
  PKT_START_REQUEST = 6       // pulsador → maestro: solicitud de inicio de partida
};

struct RadioPacket {
  uint32_t magic;
  uint8_t type;
  uint32_t senderId;
  uint8_t state;
  uint32_t seq;
};

struct NodeInfo {
  uint32_t id;
  uint8_t state;
  bool online;
  uint32_t seq;
  unsigned long lastSeenMs;
};

NodeInfo nodes[MAX_NODES];
String serialLine;

bool setAllPending = false;
NodeState setAllPendingTarget = REPOSO;
unsigned long setAllPendingLastSendMs = 0;
unsigned long setAllPendingStartMs = 0;

uint32_t selfId = 0;
NodeState currentState = REPOSO;
uint32_t localSeq = 0;

bool isMaster = false;
unsigned long lastWebClaimMs = 0;
unsigned long lastStatusHeartbeatMs = 0;
unsigned long lastStatusRequestMs = 0;

// botón
bool rawButton = false;
bool debouncedButton = false;
bool lastDebouncedButton = false;
unsigned long lastDebounceChangeMs = 0;
unsigned long pressStartMs = 0;
bool longPressHandled = false;

// blink MUST_GO
unsigned long lastBlinkMs = 0;
bool blinkOn = false;

// bandera canStart (enviada desde la web al maestro, difundida al resto)
bool canStartFlag = false;
unsigned long lastCanStartBroadcastMs = 0;

// pulsador de inicio (STARTER_BUTTON_PIN)
bool rawStartBtn = false;
bool debouncedStartBtn = false;
bool lastDebouncedStartBtn = false;
unsigned long lastStartDebounceMs = 0;
unsigned long startBtnStableHighMs = 0;  // cuándo el pin se estabilizó en HIGH por última vez



// temporizadores
unsigned long prepTimerStartMs = 0;
unsigned long prepElapsedMs = 0;
unsigned long finalStartMs = 0;
unsigned long partidaStartMs = 0;

// plasma PARTIDA
unsigned long lastPlasmaUpdateMs = 0;
float plasmaTime = 0.0f;
bool partidaBlinkOn = true;
unsigned long lastPartidaBlinkMs = 0;

// respuesta diferida a petición de estado
bool pendingStatusReply = false;
unsigned long statusReplyDueMs = 0;

// cola RX
volatile uint8_t rxHead = 0;
volatile uint8_t rxTail = 0;
volatile uint32_t rxDropped = 0;
RadioPacket rxQueue[RX_QUEUE_SIZE];

void sendEventToWeb(const char* msg);
void sendStatusToWeb();
void broadcastLocalStatus();
void requestAllStatuses();
void setLocalState(NodeState s, bool notify = true);

uint32_t makeSelfId() {
  uint64_t chip = ESP.getEfuseMac();
  // getEfuseMac() almacena el MAC en little-endian: mac[0] en bits 7:0, mac[5] en bits 47:40.
  // Para chips Espressif (OUI 24:6F:28), mac[0..2] son el OUI compartido por todos los chips.
  // Solo mac[3..5] son únicos por dispositivo.
  // chip & 0xFFFFFFFF solo varía en 1 byte → colisiones con chips del mismo lote.
  // Solución: combinar los 6 bytes en 32 bits mediante XOR fold.
  uint32_t lo = (uint32_t)(chip & 0xFFFFFFFF);      // mac[3]<<24 | mac[2]<<16 | mac[1]<<8 | mac[0]
  uint16_t hi = (uint16_t)((chip >> 32) & 0xFFFF);  // mac[5]<<8  | mac[4]
  return lo ^ ((uint32_t)hi << 16) ^ (uint32_t)hi;
}

void printOwnInfo() {
  uint64_t chip = ESP.getEfuseMac();
  // getEfuseMac() almacena mac[0] en los bits más bajos (little-endian)
  uint8_t mac[6];
  mac[0] = (chip      ) & 0xFF;
  mac[1] = (chip >>  8) & 0xFF;
  mac[2] = (chip >> 16) & 0xFF;
  mac[3] = (chip >> 24) & 0xFF;
  mac[4] = (chip >> 32) & 0xFF;
  mac[5] = (chip >> 40) & 0xFF;

  Serial.printf("EFUSE_MAC=%02X:%02X:%02X:%02X:%02X:%02X\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  Serial.printf("SELF_ID=%08lX\n", (unsigned long)selfId);
}

int findNodeIndex(uint32_t id) {
  for (int i = 0; i < MAX_NODES; i++) {
    if (nodes[i].online && nodes[i].id == id) return i;
  }
  return -1;
}

int findFreeSlot() {
  for (int i = 0; i < MAX_NODES; i++) {
    if (!nodes[i].online) return i;
  }
  return -1;
}

void upsertNode(uint32_t id, uint8_t state, uint32_t seq, bool online, unsigned long seenMs) {
  int idx = findNodeIndex(id);
  if (idx < 0) {
    idx = findFreeSlot();
    if (idx < 0) return;
  }

  nodes[idx].id = id;
  nodes[idx].state = state;
  nodes[idx].online = online;
  nodes[idx].seq = seq;
  nodes[idx].lastSeenMs = seenMs;
}

void updateSelfInTable() {
  upsertNode(selfId, currentState, localSeq, true, millis());
}

// --------------------
// Solo LEDs impares
// --------------------
void clearAllLeds() {
  FastLED.clear();
}

void fillAllSolid(const CRGB& color) {
  FastLED.clear();
  for (int i = 0; i < NUM_LEDS; i++) {
    leds[i] = color;
  }
}

// --------------------
// Visuales
// --------------------
void showReposo() {
  clearAllLeds();
  FastLED.show();
}

#ifdef STARTER_MODE
void showStarterIndicator(bool on) {
  starterLed[0] = on ? CRGB(255, 180, 0) : CRGB::Black;  // amarillo o apagado
  FastLED.show();
}
#endif

void showPcion() {
  fillAllSolid(CRGB(255, 180, 0));
  FastLED.show();
}

void showMustGoBlink() {
  if (blinkOn) {
    fillAllSolid(CRGB(255, 180, 0));
  } else {
    clearAllLeds();
  }
  FastLED.show();
}

void showReady() {
  fillAllSolid(CRGB::Green);
  FastLED.show();
}

void showFinal() {
  fillAllSolid(CRGB::Red);
  FastLED.show();
}

static inline uint8_t clamp8int(int v) {
  if (v < 0) return 0;
  if (v > 255) return 255;
  return (uint8_t)v;
}

uint8_t getPartidaPhase() {
  if (partidaStartMs == 0) return 0;
  unsigned long elapsed = millis() - partidaStartMs;
  if (elapsed < 30000UL)  return 0;  // morado           (0-30s)
  if (elapsed < 90000UL)  return 1;  // azul/blanco       (30-90s, quedan >60s)
  if (elapsed < 120000UL) return 2;  // amarillo          (90-120s, quedan >30s)
  if (elapsed < 140000UL) return 3;  // naranja parpadeo lento
  return 4;                           // naranja parpadeo rapido
}

void showPartidaPlasma(uint8_t phase) {
  if (millis() - lastPlasmaUpdateMs < 12) return;
  lastPlasmaUpdateMs = millis();

  plasmaTime += 0.18f;

  for (int i = 0; i < NUM_LEDS; i++) {
    float x = (float)i / (float)NUM_LEDS;

    float v =
      sinf((x * 14.0f) + plasmaTime * 1.8f) +
      sinf((x * 24.0f) - plasmaTime * 2.4f) +
      sinf((x * 31.0f) + plasmaTime * 1.2f);

    v /= 3.0f;
    float n = (v + 1.0f) * 0.5f;

    uint8_t base  = clamp8int((int)(100 + 155 * n));
    uint8_t white = clamp8int((int)(255 * n * n));

    switch (phase) {
      case 0: // morado
        leds[i].r = clamp8int((int)(base * 0.55f) + (int)(white * 0.60f));
        leds[i].g = 0;
        leds[i].b = base;
        break;
      case 1: // azul/blanco plasma
        leds[i].r = white;
        leds[i].g = white;
        leds[i].b = base;
        break;
      case 2: // amarillo
        leds[i].r = clamp8int((int)base + white);
        leds[i].g = clamp8int((int)(base * 0.75f) + (int)(white * 0.85f));
        leds[i].b = 0;
        break;
      case 3: // naranja
        leds[i].r = clamp8int((int)base + white);
        leds[i].g = clamp8int((int)(base * 0.35f) + (int)(white * 0.40f));
        leds[i].b = 0;
        break;
      default: // rojo
        leds[i].r = clamp8int((int)base + white);
        leds[i].g = 0;
        leds[i].b = 0;
        break;
    }
  }

  FastLED.show();
}

// Chase azul: 2 LEDs desde arriba y 2 desde abajo, ciclo de 10 segundos
void showChaseBlue() {
  unsigned long elapsed = millis() - partidaStartMs;
  unsigned long cyclePosMs = elapsed % 10000UL;

  float t;
  if (cyclePosMs < 5000UL) {
    t = (float)cyclePosMs / 5000.0f;
  } else {
    t = 1.0f - (float)(cyclePosMs - 5000UL) / 5000.0f;
  }

  int posA = (int)(t * (NUM_LEDS - 2));  // 0..NUM_LEDS-2, par A va de inicio a fin
  int posB = (NUM_LEDS - 2) - posA;      // par B va en sentido contrario

  FastLED.clear();
  leds[posA]     = CRGB(0, 0, 255);
  leds[posA + 1] = CRGB(0, 0, 255);
  leds[posB]     = CRGB(0, 0, 255);
  leds[posB + 1] = CRGB(0, 0, 255);
  FastLED.show();
}

void applyLocalVisuals(bool force = false) {
  static NodeState lastShown = (NodeState)255;

#ifdef STARTER_MODE
  // La tira principal se comporta igual que en cualquier nodo
  if (force || currentState != lastShown ||
      currentState == MUST_GO || currentState == PARTIDA) {
    switch (currentState) {
      case REPOSO:       showReposo(); break;
      case PREPARACION:  showPcion(); break;
      case MUST_GO:      showMustGoBlink(); break;
      case READY:        showReady(); break;
      case PARTIDA:      showPartidaPlasma(getPartidaPhase()); break;
      case FINAL:        showFinal(); break;
    }
    lastShown = currentState;
  }
  return;
#endif

  if (!force && currentState == lastShown &&
      currentState != MUST_GO &&
      currentState != PARTIDA) {
    return;
  }

  switch (currentState) {
    case REPOSO:       showReposo(); break;
    case PREPARACION:  showPcion(); break;
    case MUST_GO:      showMustGoBlink(); break;
    case READY:        showReady(); break;
    case PARTIDA:      showPartidaPlasma(getPartidaPhase()); break;
    case FINAL:        showFinal(); break;
  }

  lastShown = currentState;
}

bool isPrepTimerRunningState(NodeState s) {
  return (s == PREPARACION || s == MUST_GO || s == READY);
}

void stopPrepTimerIfRunning() {
  if (prepTimerStartMs != 0) {
    prepElapsedMs += millis() - prepTimerStartMs;
    prepTimerStartMs = 0;
  }
}

void startPrepTimerIfNeeded() {
  if (prepTimerStartMs == 0) {
    prepTimerStartMs = millis();
  }
}

unsigned long currentPrepElapsedMs() {
  if (prepTimerStartMs != 0) {
    return prepElapsedMs + (millis() - prepTimerStartMs);
  }
  return prepElapsedMs;
}

void resetPrepTimer() {
  prepTimerStartMs = 0;
  prepElapsedMs = 0;
}

void setLocalState(NodeState s, bool notify) {
  NodeState oldState = currentState;
  currentState = s;

  if (isPrepTimerRunningState(oldState) && !isPrepTimerRunningState(currentState)) {
    stopPrepTimerIfRunning();
  }

  if (!isPrepTimerRunningState(oldState) && isPrepTimerRunningState(currentState)) {
    startPrepTimerIfNeeded();
  }

  if (currentState == REPOSO || currentState == PARTIDA || currentState == FINAL) {
    resetPrepTimer();
  }

  if (currentState == MUST_GO) {
    blinkOn = true;
    lastBlinkMs = millis();
  }

  if (currentState == PARTIDA) {
    plasmaTime = 0.0f;
    lastPlasmaUpdateMs = 0;
    partidaStartMs = millis();
    partidaBlinkOn = true;
    lastPartidaBlinkMs = 0;
  }

  if (currentState == FINAL) {
    finalStartMs = millis();
  } else {
    finalStartMs = 0;
  }

  applyLocalVisuals(true);

  localSeq++;
  updateSelfInTable();

  if (notify) {
    broadcastLocalStatus();
    if (isMaster) sendStatusToWeb();
  }
#ifdef STARTER_MODE
  // Apagar el LED indicador cuando se inicia o termina la partida
  if (currentState == PARTIDA || currentState == FINAL || currentState == REPOSO) {
    showStarterIndicator(false);
  }
#endif
}

void handleShortPress() {
  if (currentState == PARTIDA || currentState == FINAL) return;

  switch (currentState) {
    case REPOSO:
      setLocalState(PREPARACION);
      break;
    case PREPARACION:
      setLocalState(READY);
      break;
    case MUST_GO:
      setLocalState(READY);
      break;
    case READY:
      if (currentPrepElapsedMs() >= MUSTGO_DELAY_MS) setLocalState(MUST_GO);
      else setLocalState(PREPARACION);
      break;
    case PARTIDA:
    case FINAL:
      break;
  }
}

void handleLongPress() {
  if (currentState == PARTIDA || currentState == FINAL) return;
  setLocalState(REPOSO);
}

void updateButton() {
  bool sample = (digitalRead(BUTTON_PIN) == LOW);

  if (sample != rawButton) {
    rawButton = sample;
    lastDebounceChangeMs = millis();
  }

  if ((millis() - lastDebounceChangeMs) >= BUTTON_DEBOUNCE_MS) {
    debouncedButton = rawButton;
  }

  if (debouncedButton && !lastDebouncedButton) {
    pressStartMs = millis();
    longPressHandled = false;
  }

  if (debouncedButton && !longPressHandled) {
    if ((millis() - pressStartMs) >= LONGPRESS_MS) {
      handleLongPress();
      longPressHandled = true;
    }
  }

  if (!debouncedButton && lastDebouncedButton) {
    if (!longPressHandled) {
      handleShortPress();
    }
  }

  lastDebouncedButton = debouncedButton;
}

void updateBlink() {
  if (currentState != MUST_GO) return;

  if (millis() - lastBlinkMs >= MUSTGO_BLINK_MS) {
    lastBlinkMs = millis();
    blinkOn = !blinkOn;
    showMustGoBlink();
  }
}

void updateMustGoTimer() {
  if (currentState != PREPARACION) return;

  if (currentPrepElapsedMs() >= MUSTGO_DELAY_MS) {
    setLocalState(MUST_GO);
  }
}

// overlayRedChase guardado por si se quiere recuperar:
// void overlayRedChase() {
//   float t = (float)(elapsed - 120000UL) / 30000.0f;
//   int pos = (int)((1.0f - t) * (NUM_LEDS - 5));
//   for (int i = 0; i < 5; i++) leds[pos + i] = CRGB::Red;
//   FastLED.show();
// }

void overlayRedFill() {
  if (partidaStartMs == 0) return;
  unsigned long elapsed = millis() - partidaStartMs;
  if (elapsed < 120000UL) return;

  unsigned long elapsed30 = elapsed - 120000UL;
  if (elapsed30 > 30000UL) elapsed30 = 30000UL;

  // Últimos 2 segundos: parpadeo rápido de toda la barra en rojo
  if (elapsed30 >= 28000UL) {
    if ((millis() / 80) % 2 == 0) {
      for (int i = 0; i < NUM_LEDS; i++) leds[i] = CRGB::Red;
    } else {
      FastLED.clear();
    }
    FastLED.show();
    return;
  }

  // Relleno led a led desde arriba (NUM_LEDS-1) hacia abajo (0) en 28 segundos
  int numLit = (int)((float)elapsed30 / 28000.0f * NUM_LEDS);
  if (numLit > NUM_LEDS) numLit = NUM_LEDS;
  for (int i = NUM_LEDS - numLit; i < NUM_LEDS; i++) {
    leds[i] = CRGB::Red;
  }
  FastLED.show();
}

void updatePartidaEffect() {
  if (currentState != PARTIDA) return;

  if (millis() - partidaStartMs >= PARTIDA_DURATION_MS) {
    setLocalState(FINAL, true);
    return;
  }

  uint8_t phase = getPartidaPhase();

  if (phase == 3 || phase == 4) {
    // Plasma naranja restaurado
    unsigned long blinkInterval = (phase == 3) ? PARTIDA_BLINK_SLOW_MS : PARTIDA_BLINK_FAST_MS;
    if (millis() - lastPartidaBlinkMs >= blinkInterval) {
      lastPartidaBlinkMs = millis();
      partidaBlinkOn = !partidaBlinkOn;
    }
    if (partidaBlinkOn) {
      showPartidaPlasma(3); // naranja
    } else {
      FastLED.clear();
      FastLED.show();
    }
    overlayRedFill();
  } else {
    showPartidaPlasma(phase);
  }
}

void updateFinalTimer() {
  if (currentState != FINAL || finalStartMs == 0) return;

  if (millis() - finalStartMs >= FINAL_DURATION_MS) {
    setLocalState(REPOSO, true);
  }
}

void sendRadioPacket(const RadioPacket &pkt) {
  esp_err_t err = esp_now_send(broadcastMac, (const uint8_t*)&pkt, sizeof(pkt));
  if (err != ESP_OK) {
    StaticJsonDocument<160> doc;
    doc["type"] = "event";
    doc["msg"] = "esp_now_send_error";
    doc["code"] = (int)err;
    serializeJson(doc, Serial);
    Serial.println();
  }
}

void broadcastLocalStatus() {
  RadioPacket pkt;
  pkt.magic = PACKET_MAGIC;
  pkt.type = PKT_STATUS;
  pkt.senderId = selfId;
  pkt.state = currentState;
  pkt.seq = localSeq;
  sendRadioPacket(pkt);
}

void broadcastSetAllState(NodeState state) {
  RadioPacket pkt;
  pkt.magic = PACKET_MAGIC;
  pkt.type = PKT_SET_ALL_STATE;
  pkt.senderId = selfId;
  pkt.state = state;
  pkt.seq = ++localSeq;
  sendRadioPacket(pkt);
}

void broadcastSetNodeState(uint32_t targetId, NodeState state) {
  RadioPacket pkt;
  pkt.magic = PACKET_MAGIC;
  pkt.type = PKT_SET_NODE_STATE;
  pkt.senderId = selfId;
  pkt.state = state;
  pkt.seq = targetId; // repropuesto como targetId para este tipo de paquete
  sendRadioPacket(pkt);
}

void broadcastCanStart(bool cs) {
  RadioPacket pkt;
  pkt.magic = PACKET_MAGIC;
  pkt.type = PKT_CAN_START;
  pkt.senderId = selfId;
  pkt.state = cs ? 1 : 0;
  pkt.seq = ++localSeq;
  sendRadioPacket(pkt);
}

void sendStartRequest() {
  RadioPacket pkt;
  pkt.magic = PACKET_MAGIC;
  pkt.type = PKT_START_REQUEST;
  pkt.senderId = selfId;
  pkt.state = 0;
  pkt.seq = ++localSeq;
  sendRadioPacket(pkt);
}

void requestAllStatuses() {
  RadioPacket pkt;
  pkt.magic = PACKET_MAGIC;
  pkt.type = PKT_REQUEST_STATUS;
  pkt.senderId = selfId;
  pkt.state = 0;
  pkt.seq = ++localSeq;
  sendRadioPacket(pkt);
}

void sendEventToWeb(const char* msg) {
  StaticJsonDocument<128> doc;
  doc["type"] = "event";
  doc["msg"] = msg;
  serializeJson(doc, Serial);
  Serial.println();
}

void sendStatusToWeb() {
  StaticJsonDocument<2048> doc;
  doc["type"] = "status";
  doc["selfId"] = selfId;
  doc["isMaster"] = isMaster ? 1 : 0;

  JsonArray arr = doc.createNestedArray("nodes");
  for (int i = 0; i < MAX_NODES; i++) {
    if (!nodes[i].online) continue;

    JsonObject n = arr.createNestedObject();
    n["id"] = nodes[i].id;
    n["state"] = nodes[i].state;
    n["online"] = 1;
    n["seq"] = nodes[i].seq;
  }

  serializeJson(doc, Serial);
  Serial.println();
}

// --------------------
// RX queue
// --------------------

bool anyNodeNeedsSetAll() {
  for (int i = 0; i < MAX_NODES; i++) {
    if (!nodes[i].online || nodes[i].id == selfId) continue;
    NodeState ns = (NodeState)nodes[i].state;
    switch (setAllPendingTarget) {
      case PARTIDA: if (ns == READY)   return true; break;
      case FINAL:   if (ns == PARTIDA) return true; break;
      case REPOSO:  if (ns != REPOSO)  return true; break;
      default: break;
    }
  }
  return false;
}

void updateSetAllRepeat() {
  if (!setAllPending || !isMaster) { setAllPending = false; return; }
  if (!anyNodeNeedsSetAll() || millis() - setAllPendingStartMs >= SET_ALL_MAX_MS) {
    setAllPending = false;
    return;
  }
  if (millis() - setAllPendingLastSendMs >= SET_ALL_REPEAT_MS) {
    setAllPendingLastSendMs = millis();
    broadcastSetAllState(setAllPendingTarget);
  }
}

bool enqueueRx(const RadioPacket &pkt) {
  uint8_t nextHead = (rxHead + 1) % RX_QUEUE_SIZE;
  if (nextHead == rxTail) {
    rxDropped++;
    return false;
  }
  rxQueue[rxHead] = pkt;
  rxHead = nextHead;
  return true;
}

bool dequeueRx(RadioPacket &pkt) {
  if (rxTail == rxHead) return false;
  pkt = rxQueue[rxTail];
  rxTail = (rxTail + 1) % RX_QUEUE_SIZE;
  return true;
}

void handleIncomingStatus(const RadioPacket &pkt) {
  if (pkt.senderId == selfId) return;

  upsertNode(pkt.senderId, pkt.state, pkt.seq, true, millis());
}

void handleIncomingSetAllState(const RadioPacket &pkt) {
  if (pkt.state > FINAL) return;

  NodeState requested = (NodeState)pkt.state;

  if (requested == PARTIDA && currentState != READY) {
    sendEventToWeb("PARTIDA ignorado: no READY");
    return;
  }

  if (requested == FINAL && currentState != PARTIDA) {
    sendEventToWeb("FIN ignorado: no PARTIDA");
    return;
  }

  setLocalState(requested, true);
}

void handleIncomingRequestStatus(const RadioPacket &pkt) {
  if (pkt.senderId == selfId) return;

  uint32_t jitter = 20 + (selfId % 13) * 15;
  pendingStatusReply = true;
  statusReplyDueMs = millis() + jitter;
}

void handleIncomingSetNodeState(const RadioPacket &pkt) {
  uint32_t targetId = pkt.seq; // seq repropuesto como targetId
  if (targetId != selfId) return; // no es para este nodo
  if (pkt.state > FINAL) return;

  NodeState requested = (NodeState)pkt.state;

  if (requested == PARTIDA && currentState != READY) {
    sendEventToWeb("set_node PARTIDA ignorado: no READY");
    return;
  }
  if (requested == FINAL && currentState != PARTIDA) {
    sendEventToWeb("set_node FIN ignorado: no PARTIDA");
    return;
  }
  setLocalState(requested, true);
}

void handleIncomingCanStart(const RadioPacket &pkt) {
  if (pkt.senderId == selfId) return;
  canStartFlag = (pkt.state != 0);
#ifdef STARTER_MODE
  showStarterIndicator(canStartFlag);
#endif
}

void handleIncomingStartRequest(const RadioPacket &pkt) {
  if (!isMaster) return;
  // Siempre notificar a la web (para log y para que decida si arrancar)
  StaticJsonDocument<128> doc;
  doc["type"]     = "event";
  doc["msg"]      = "start_request_rx";
  doc["canStart"] = canStartFlag ? 1 : 0;
  serializeJson(doc, Serial);
  Serial.println();
}

void processRxQueue() {
  RadioPacket pkt;
  while (dequeueRx(pkt)) {
    switch (pkt.type) {
      case PKT_STATUS:
        handleIncomingStatus(pkt);
        break;
      case PKT_SET_ALL_STATE:
        handleIncomingSetAllState(pkt);
        break;
      case PKT_REQUEST_STATUS:
        handleIncomingRequestStatus(pkt);
        break;
      case PKT_SET_NODE_STATE:
        handleIncomingSetNodeState(pkt);
        break;
      case PKT_CAN_START:
        handleIncomingCanStart(pkt);
        break;
      case PKT_START_REQUEST:
        handleIncomingStartRequest(pkt);
        break;
      default:
        break;
    }
  }

  static uint32_t lastReportedDrops = 0;
  if (rxDropped != lastReportedDrops) {
    lastReportedDrops = rxDropped;
    StaticJsonDocument<160> doc;
    doc["type"] = "event";
    doc["msg"] = "rx_queue_drop";
    doc["count"] = rxDropped;
    serializeJson(doc, Serial);
    Serial.println();
  }
}

void processPendingStatusReply() {
  if (!pendingStatusReply) return;
  if (millis() < statusReplyDueMs) return;

  pendingStatusReply = false;
  broadcastLocalStatus();
}

void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len != sizeof(RadioPacket)) return;

  RadioPacket pkt;
  memcpy(&pkt, data, sizeof(pkt));
  if (pkt.magic != PACKET_MAGIC) return;

  enqueueRx(pkt);
}

void onDataSent(const wifi_tx_info_t *tx_info, esp_now_send_status_t status) {
}

bool initEspNow() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.disconnect();

  if (esp_now_init() != ESP_OK) {
    Serial.println("Error inicializando ESP-NOW");
    return false;
  }

  esp_now_register_recv_cb(onDataRecv);
  esp_now_register_send_cb(onDataSent);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, broadcastMac, 6);
  peer.channel = 0;
  peer.encrypt = false;

  if (!esp_now_is_peer_exist(broadcastMac)) {
    if (esp_now_add_peer(&peer) != ESP_OK) {
      Serial.println("Error añadiendo peer broadcast");
      return false;
    }
  }

  return true;
}

void processJsonCommand(const String& line) {
  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, line);

  if (err) {
    sendEventToWeb("JSON invalido");
    return;
  }

  const char* cmd = doc["cmd"] | "";

  if (strcmp(cmd, "claim_master") == 0) {
    bool wasMaster = isMaster;
    isMaster = true;
    lastWebClaimMs = millis();

    if (!wasMaster) sendEventToWeb("Este dispositivo ahora es maestro");

    updateSelfInTable();
    requestAllStatuses();
    sendStatusToWeb();
    return;
  }

  if (strcmp(cmd, "get_status") == 0) {
    updateSelfInTable();
    requestAllStatuses();
    sendStatusToWeb();
    return;
  }

  if (strcmp(cmd, "set_self") == 0) {
  int state = doc["state"] | -1;

    if (state >= REPOSO && state <= FINAL) {
      NodeState requested = (NodeState)state;

      if (requested == PARTIDA) {
        if (currentState == READY) {
          setLocalState(requested, true);
        } else {
          sendEventToWeb("set_self PARTIDA ignorado: no READY");
        }
      } else if (requested == FINAL) {
        if (currentState == PARTIDA) {
          setLocalState(requested, true);
        } else {
          sendEventToWeb("set_self FIN ignorado: no PARTIDA");
        }
      } else {
        setLocalState(requested, true);
      }

      if (isMaster) sendStatusToWeb();
    }
    return;
  }

  if (strcmp(cmd, "set_node") == 0) {
    const char* idStr = doc["id"] | "0";
    uint32_t targetId = (uint32_t)strtoul(idStr, nullptr, 16);
    int state = doc["state"] | -1;
    if (targetId == 0 || state < REPOSO || state > FINAL) return;

    NodeState requested = (NodeState)state;
    if (targetId == selfId) {
      // Aplicar localmente (es el master)
      if (requested == PARTIDA) {
        if (currentState == READY) setLocalState(requested, true);
        else sendEventToWeb("set_node PARTIDA ignorado: no READY");
      } else if (requested == FINAL) {
        if (currentState == PARTIDA) setLocalState(requested, true);
        else sendEventToWeb("set_node FIN ignorado: no PARTIDA");
      } else {
        setLocalState(requested, true);
      }
    } else {
      broadcastSetNodeState(targetId, requested);
    }
    if (isMaster) sendStatusToWeb();
    return;
  }

  if (strcmp(cmd, "set_all") == 0) {
    int state = doc["state"] | -1;
    if (state >= REPOSO && state <= FINAL) {
      broadcastSetAllState((NodeState)state);

      setAllPending = true;
      setAllPendingTarget = (NodeState)state;
      setAllPendingStartMs = millis();
      setAllPendingLastSendMs = millis();

      NodeState requested = (NodeState)state;
      if (requested == PARTIDA) {
        if (currentState == READY) setLocalState(requested, true);
      } else if (requested == FINAL) {
        if (currentState == PARTIDA) setLocalState(requested, true);
      } else {
        setLocalState(requested, true);
      }

      if (isMaster) sendStatusToWeb();
    }
    return;
  }

  if (strcmp(cmd, "set_can_start") == 0) {
    bool cs = (doc["value"] | 0) != 0;
    canStartFlag = cs;
#ifdef STARTER_MODE
    showStarterIndicator(canStartFlag);
#endif
    broadcastCanStart(cs);
    lastCanStartBroadcastMs = millis();
    return;
  }
}

void readSerialCommands() {
  while (Serial.available()) {
    char c = (char)Serial.read();

    if (c == '\n') {
      serialLine.trim();
      if (serialLine.length() > 0) processJsonCommand(serialLine);
      serialLine = "";
    } else if (c != '\r') {
      serialLine += c;
    }
  }
}

void updateNodeTimeouts() {
  bool changed = false;

  for (int i = 0; i < MAX_NODES; i++) {
    if (!nodes[i].online) continue;
    if (nodes[i].id == selfId) continue;

    if (millis() - nodes[i].lastSeenMs > NODE_TIMEOUT_MS) {
      nodes[i].online = false;
      changed = true;
    }
  }

  if (changed && isMaster) sendStatusToWeb();
}

void updateMasterLease() {
  if (isMaster && (millis() - lastWebClaimMs > WEB_MASTER_TIMEOUT)) {
    isMaster = false;
  }
}

void updateStatusHeartbeat() {
  if (millis() - lastStatusHeartbeatMs >= STATUS_HEARTBEAT_MS) {
    lastStatusHeartbeatMs = millis();
    localSeq++;
    updateSelfInTable();
    broadcastLocalStatus();
    if (isMaster) sendStatusToWeb();
  }
}

void updateStatusRequestHeartbeat() {
  if (!isMaster) return;

  if (millis() - lastStatusRequestMs >= STATUS_REQUEST_MS) {
    lastStatusRequestMs = millis();
    requestAllStatuses();
  }
}

void updateStarterButton() {
  // Cooldown tras watchdog: evita bucle si el pin está físicamente pegado en LOW
  static unsigned long btnCooldownUntilMs = 0;
  if (millis() < btnCooldownUntilMs) return;

  bool sample = (digitalRead(STARTER_BUTTON_PIN) == LOW);

  if (sample != rawStartBtn) {
    rawStartBtn = sample;
    lastStartDebounceMs = millis();
  }

  if ((millis() - lastStartDebounceMs) < STARTER_DEBOUNCE_MS) {
    lastDebouncedStartBtn = debouncedStartBtn;
    return;
  }

  debouncedStartBtn = rawStartBtn;

  // Rastrear tiempo en HIGH estable (histéresis anti-ruido)
  if (!debouncedStartBtn) {
    startBtnStableHighMs = millis();
  }

  // Watchdog: pin pegado en LOW más de 2s → cortocircuito/hardware
  static unsigned long pressedSinceMs = 0;
  if (debouncedStartBtn) {
    if (pressedSinceMs == 0) pressedSinceMs = millis();
    if (millis() - pressedSinceMs > 2000UL) {
      rawStartBtn           = false;
      debouncedStartBtn     = false;
      lastStartDebounceMs   = millis();
      pressedSinceMs        = 0;
      btnCooldownUntilMs    = millis() + 5000UL;
      lastDebouncedStartBtn = false;
      return;
    }
  } else {
    pressedSinceMs = 0;
  }

  // Flanco de bajada (pulsación) → acción inmediata
  bool stableHighBefore = (millis() - startBtnStableHighMs) >= STARTER_DEBOUNCE_MS;
  if (debouncedStartBtn && !lastDebouncedStartBtn) {
    if (stableHighBefore) {
      if (!isMaster) {
        sendStartRequest();  // siempre notifica al maestro; él decide si puede arrancar
      } else {
        // Si el arrancador ES el maestro, notificar directamente a la web
        StaticJsonDocument<128> doc;
        doc["type"]     = "event";
        doc["msg"]      = "start_request_rx";
        doc["canStart"] = canStartFlag ? 1 : 0;
        serializeJson(doc, Serial);
        Serial.println();
      }
    }
  }

  // Flanco de subida (liberación) → solo log
  if (!debouncedStartBtn && lastDebouncedStartBtn) {
    sendEventToWeb("starter_btn_released");
  }

  lastDebouncedStartBtn = debouncedStartBtn;
}

void updateCanStartHeartbeat() {
  // Re-difunde la bandera canStart cada 2 s para que dispositivos
  // que se conecten tarde la reciban sin esperar un cambio de estado
  if (!isMaster) return;
  if (millis() - lastCanStartBroadcastMs >= 2000) {
    lastCanStartBroadcastMs = millis();
    broadcastCanStart(canStartFlag);
  }
}

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(500);

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(STARTER_BUTTON_PIN, INPUT_PULLUP);

  FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS);
#ifdef STARTER_MODE
  FastLED.addLeds<LED_TYPE, STARTER_IND_PIN, COLOR_ORDER>(starterLed, STARTER_IND_NUM);
#endif
  FastLED.setBrightness(BRIGHTNESS);
  FastLED.clear();
  FastLED.show();

  memset(nodes, 0, sizeof(nodes));

  WiFi.mode(WIFI_STA);
  selfId = makeSelfId();

  if (!initEspNow()) {
    Serial.println("Fallo ESP-NOW");
  }

  randomSeed((uint32_t)esp_random());
  lastStatusHeartbeatMs = millis() + random(0, 900);
  lastStatusRequestMs = millis();

  setLocalState(REPOSO, false);
  updateSelfInTable();

  printOwnInfo();
  sendEventToWeb("Firmware unificado listo");
}

void loop() {
  readSerialCommands();
  processRxQueue();
  processPendingStatusReply();
  updateButton();
  updateBlink();
  updateMustGoTimer();
  updatePartidaEffect();
  updateFinalTimer();
  updateNodeTimeouts();
  updateMasterLease();
  updateStatusHeartbeat();
  updateStatusRequestHeartbeat();
  updateSetAllRepeat();
#ifdef STARTER_MODE
  updateStarterButton();
#endif
  updateCanStartHeartbeat();
}