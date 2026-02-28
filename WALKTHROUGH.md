# HackOS – Complete Project Walkthrough

> **Version captured**: Phase 7 MVP  
> **Target hardware**: ESP32 DevKit v1 (Xtensa LX6, 240 MHz, 520 KB SRAM, 4 MB Flash)  
> **Build system**: PlatformIO `esp32dev` / Arduino framework / C++17

---

## 1. Project Overview

HackOS is a cooperative, event-driven micro-OS designed to run on a bare ESP32 and expose a hacker-friendly multi-tool interface through a 128×64 OLED display and a 5-way joystick. It is architecturally split into four independent layers:

```
┌──────────────────────────────────────────────────────────┐
│                      Applications                        │
│  Launcher · WiFi · NFC · IR · RF · FileManager · Amiibo  │
│  BLE Audit · BadBT · GhostNet                            │
├──────────────────────────────────────────────────────────┤
│                      Core OS                             │
│  AppManager · EventSystem · StateMachine · GhostNetMgr   │
├──────────────────────────────────────────────────────────┤
│                Hardware Abstraction Layer                 │
│  Display · Input · Wireless · NFC · IR · RF · SD · ESPNOW│
├──────────────────────────────────────────────────────────┤
│                   ESP-IDF / FreeRTOS                      │
└──────────────────────────────────────────────────────────┘
```

---

## 2. Repository Layout

```
HackOS/
├── include/
│   ├── config.h                 GPIO pin constants (all constexpr)
│   ├── apps/
│   │   ├── app_base.h           Pure-virtual AppBase interface
│   │   ├── launcher_app.h       Factory: createLauncherApp()
│   │   ├── wifi_tools_app.h     Factory: createWifiToolsApp()
│   │   ├── nfc_tools_app.h      Factory: createNFCToolsApp()
│   │   ├── ir_tools_app.h       Factory: createIRToolsApp()
│   │   ├── rf_tools_app.h       Factory: createRFToolsApp()
│   │   ├── amiibo_app.h        Factory: createAmiiboApp()
│   │   ├── ghostnet_app.h     Factory: createGhostNetApp()
│   │   └── file_manager_app.h   Factory: createFileManagerApp()
│   ├── core/
│   │   ├── event.h              Event struct · EventType enum · event codes
│   │   ├── event_system.h       EventSystem singleton · IEventObserver
│   │   ├── app_manager.h        AppManager singleton
│   │   ├── ghostnet_manager.h   GhostNetManager (ESP-NOW mesh)
│   │   └── state_machine.h      GlobalState enum · StateMachine singleton
│   ├── hardware/
│   │   ├── display.h            DisplayManager (SSD1306 I²C)
│   │   ├── input.h              InputManager (joystick ADC + SW GPIO)
│   │   ├── wireless.h           Wireless (ESP-IDF WiFi STA + deauth)
│   │   ├── nfc_reader.h         NFCReader (PN532 SPI)
│   │   ├── ir_transceiver.h     IRTransceiver (IRremoteESP8266)
│   │   ├── rf_transceiver.h     RFTransceiver (rc-switch 433 MHz)
│   │   └── storage.h            StorageManager (SD SPI · listDir · write)
│   └── ui/
│       ├── widget.h             Widget base (x, y, w, h, dirty flag)
│       └── widgets.h            MenuListView · StatusBar · ProgressBar · DialogBox
├── src/
│   ├── main.cpp                 setup() + loop()
│   ├── apps/                    App implementations (mirror of include/apps/)
│   ├── core/                    Core implementations
│   ├── hardware/                HAL implementations
│   └── ui/                      Widget implementations
├── data/                        SPIFFS data (reserved, currently empty)
├── partitions.csv               Custom flash partition table
└── platformio.ini               PlatformIO build config
```

---

## 3. Boot Sequence

```
setup()
  │
  ├─ Serial.begin(115200)
  ├─ DisplayManager::instance().init()      SSD1306 via I²C
  ├─ InputManager::instance().init()        ADC calibration + GPIO
  ├─ StorageManager::instance().mount()     SD.begin() via SPI
  ├─ EventSystem::instance().init()         FreeRTOS queue (depth=16)
  ├─ AppManager::instance().init()          Subscribes to EventSystem
  │
  ├─ StateMachine: BOOT → SPLASH → LAUNCHER
  │
  ├─ registerApp("launcher",      createLauncherApp)
  ├─ registerApp("wifi_tools",    createWifiToolsApp)
  ├─ registerApp("ir_tools",      createIRToolsApp)
  ├─ registerApp("nfc_tools",     createNFCToolsApp)
  ├─ registerApp("rf_tools",      createRFToolsApp)
  ├─ registerApp("file_manager",  createFileManagerApp)
  ├─ registerApp("amiibo",        createAmiiboApp)
  │
  ├─ launchApp("launcher")        LauncherApp::onSetup()
  │
  └─ Log: RAM usage (free/total)

loop()  [every 10 ms, yields via vTaskDelay]
  ├─ EventSystem::dispatchPendingEvents()
  └─ AppManager::loop()
       ├─ InputManager::readInput()  → post EVT_INPUT if not CENTER
       ├─ activeApp_->onLoop()
       └─ activeApp_->onDraw()       [rate-limited to 30 FPS / 33 ms]
```

Target boot time: **< 3 seconds** to the main Launcher menu.  
Expected free RAM at the Launcher: **> 150 KB**.

---

## 4. Core Subsystems

### 4.1 EventSystem

`EventSystem` is a thread-safe FreeRTOS queue plus an observer list.

```
┌─────────────────────┐
│   postEvent(evt)    │ ──► FreeRTOS Queue
└─────────────────────┘
          │
          ▼ (dispatched by loop())
┌─────────────────────┐
│ dispatchPendingEvents│ ──► IEventObserver::onEvent() × N observers
└─────────────────────┘
```

| `EventType` | `arg0` | `arg1` | Usage |
|---|---|---|---|
| `EVT_INPUT` | `InputManager::InputEvent` | — | Joystick/button input |
| `EVT_SYSTEM` | `SYSTEM_EVENT_BACK = 1` | — | Navigate back |
| `EVT_APP` | `APP_EVENT_LAUNCH = 1` | app index | Launch app at index |
| `EVT_WIFI_SCAN_DONE` | AP count | — | Async scan complete |
| `EVT_XP_EARNED` | XP amount | — | Award XP to ExperienceManager |
| `EVT_GHOSTNET` | `GhostMsgType` | payload len / peer count | GhostNet mesh event |

**Observer limit**: 8 simultaneous observers (`MAX_OBSERVERS`).

### 4.2 AppManager

`AppManager` owns exactly one `AppBase*` (`activeApp_`) at a time.

| Method | Effect |
|---|---|
| `registerApp(name, factory)` | Adds an entry to the app table (max 8) |
| `launchApp(name)` | Destroys active app, creates new one via factory, calls `onSetup()` |
| `loop()` | Reads input → `onLoop()` → `onDraw()` (33 ms frame gate) |
| `onEvent(EVT_SYSTEM/BACK)` | `onDestroy()` → `goBack()` → re-launch "launcher" |
| `onEvent(EVT_APP/LAUNCH)` | `launchApp(appNameAt(arg1))` |

### 4.3 StateMachine

States: `BOOT → SPLASH → LAUNCHER → APP_RUNNING`

`pushState()` adds to a stack; `goBack()` pops it.  
`AppManager` uses `currentState()` to decide whether to push `APP_RUNNING`.

---

## 5. Hardware Abstraction Layer (HAL)

All HAL classes are Meyers Singletons (thread-safe static local).

### 5.1 DisplayManager (`display.h / display.cpp`)

Wraps Adafruit SSD1306 (128×64, I²C, SDA=21, SCL=22).

Key API:
```cpp
bool init();
void clear();
void drawText(int16_t x, int16_t y, const char *text);
void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1);
void fillRect(int16_t x, int16_t y, int16_t w, int16_t h);
void present();   // flush buffer to display
```

### 5.2 InputManager (`input.h / input.cpp`)

Reads the KY-023 joystick (ADC X=34, Y=35) and push-button (GPIO=32).

```cpp
enum class InputEvent : uint8_t { CENTER, UP, DOWN, LEFT, RIGHT, BUTTON_PRESS };
InputEvent readInput();   // called every loop tick
```

`InputManager::InputEvent::CENTER` means "no input" – AppManager suppresses posting an event in this case.

### 5.3 Wireless (`wireless.h / wireless.cpp`)

Wraps the ESP-IDF WiFi stack in STA mode.

```cpp
bool init();
bool startScan();          // non-blocking; posts EVT_WIFI_SCAN_DONE when done
const ApRecord *aps();     // heap-allocated array, valid after scan
bool sendDeauth(bssid, channel, count);
void deinit();
```

`ApRecord` fields: `ssid[33]`, `bssid[6]`, `rssi`, `channel`, `authmode`.  
Maximum tracked APs: **16** (`MAX_APS`).

### 5.4 NFCReader (`nfc_reader.h / nfc_reader.cpp`)

Wraps Adafruit PN532 (SPI, CS=17).

```cpp
bool init();
bool isReady() const;
bool readUID(uint8_t *buf, uint8_t *len, uint16_t timeoutMs);
bool authenticateBlock(uid, uidLen, blockNumber);
bool readBlock(blockNumber, data[16]);
bool emulateNtag215(const uint8_t *dump, uint16_t timeoutMs);  // Phase 12
bool writeNtag215(const uint8_t *dump);                         // Phase 12
void deinit();
```

Constants: `MIFARE_1K_SECTORS = 16`, `BLOCKS_PER_SECTOR = 4`, `BYTES_PER_BLOCK = 16`.

### 5.5 IRTransceiver (`ir_transceiver.h / ir_transceiver.cpp`)

Wraps IRremoteESP8266 (TX=4, RX=15).

```cpp
void initReceive();
void initTransmit();
bool decode(uint64_t &value, decode_type_t &proto, uint16_t &bits);
bool hasLastCode() const;
uint64_t lastValue() const;
decode_type_t lastProtocol() const;
uint16_t lastBits() const;
void send(value, protocol, bits);
void deinit();
```

### 5.6 RFTransceiver (`rf_transceiver.h / rf_transceiver.cpp`)

Wraps rc-switch library (433 MHz OOK, TX=25, RX=16).

```cpp
void initReceive();
void initTransmit();
bool read(unsigned long &code, unsigned int &bits, unsigned int &delay);
bool hasLastCode() const;
unsigned long lastCode() const;
unsigned int lastBitLength() const;
void send(code, bitLength);
void deinit();
```

### 5.7 StorageManager (`storage.h / storage.cpp`)

Wraps the Arduino SD library (SPI VSPI, CS=5).

```cpp
bool mount();
void unmount();
bool isMounted() const;
const char *lastError() const;

// Phase 7 additions:
size_t listDir(const char *path, DirEntry *entries, size_t maxEntries);
bool writeFile(const char *path, const uint8_t *data, size_t len);
bool appendChunk(const char *path, const uint8_t *data, size_t len);
```

`DirEntry` fields: `name[64]`, `isDir`, `size` (bytes, 0 for directories).

`appendChunk()` is the preferred API for apps – callers iterate over data in small pieces (< 4 KB each) and yield between calls to keep the UI responsive.

---

## 6. UI Widget Library

All widgets inherit from `Widget` (base class in `widget.h`):

```cpp
class Widget {
public:
    bool isDirty() const;
    void clearDirty();
    void markDirty();
    virtual void draw() = 0;
protected:
    Widget(int16_t x, int16_t y, int16_t width, int16_t height);
    int16_t x_, y_, w_, h_;
    bool dirty_;
};
```

| Widget | Purpose | Key methods |
|---|---|---|
| `MenuListView` | Scrollable selection list | `setItems()`, `moveSelection()`, `selectedIndex()`, `itemCount()` |
| `StatusBar` | Top bar (SD icon, WiFi icon, battery, time) | `setConnectivity()`, `setBatteryLevel()`, `setTime()` |
| `ProgressBar` | Horizontal fill bar | `setProgress(uint8_t percent)` |
| `DialogBox` | Modal overlay with title + message | `setText()`, `setVisible()` |

All `setXxx()` methods mark the widget dirty. `onDraw()` checks `isDirty()` to minimise unnecessary `DisplayManager::clear()` → `present()` cycles.

---

## 7. Applications – Detailed State Machines

### 7.1 LauncherApp

**States**: stateless (always shows app list)

```
onSetup():  populate menu from AppManager::appNameAt()
onEvent():  UP/DOWN → move selection
            BUTTON_PRESS → post EVT_APP/LAUNCH(selectedIndex)
```

The Launcher lists **all registered apps** including itself.  
Index 0 is always `"launcher"`.

### 7.2 WiFiToolsApp

```
MENU_PRINCIPAL ──(Scan Networks)──► SCANNING
SCANNING       ──(EVT_WIFI_SCAN_DONE)──► AP_LIST
AP_LIST        ──(SELECT ap)──► ATTACK_MENU
ATTACK_MENU    ──(Deauth)──► DEAUTHING
               ──(Evil Twin)──► ATTACK_MENU  [mock]
               ──(Info)──► ATTACK_MENU       [log only]
               ──(Save AP)──► ATTACK_MENU    [write to /captures/wifi_scan.txt]
DEAUTHING      ──(50 bursts or PRESS)──► ATTACK_MENU
```

AP labels: `"<SSID_22chars> <RSSI>dBm"` – heap-allocated, freed in `onDestroy()`.

### 7.3 NFCToolsApp

```
MAIN_MENU  ──(Read UID)──► READING_UID
           ──(Dump Mifare)──► DUMPING (requires UID)
           ──(Save UID)──► MAIN_MENU  [write to /captures/nfc_uid.txt]
           ──(Back)──► SYSTEM_EVENT_BACK
READING_UID ──(card present)──► show UID hex
            ──(PRESS/LEFT)──► MAIN_MENU
DUMPING    ──(sector loop)──► DUMP_DONE
           ──(PRESS/LEFT)──► DUMP_DONE  [abort]
DUMP_DONE  ──(PRESS/LEFT)──► MAIN_MENU
```

Mifare 1K layout: 16 sectors × 4 blocks × 16 bytes = 1024 bytes total.

### 7.4 IRToolsApp

```
MAIN_MENU  ──(IR Sniffer)──► SNIFFER
           ──(IR Cloner)──► CLONER
           ──(Save Code)──► MAIN_MENU  [write to /captures/ir_codes.txt]
           ──(Back)──► SYSTEM_EVENT_BACK
SNIFFER    ──(code detected)──► update protoName_/codeHex_
           ──(PRESS/LEFT)──► MAIN_MENU
CLONER     ──(first loop)──► send last captured code
           ──(PRESS/LEFT)──► MAIN_MENU
```

### 7.5 RFToolsApp

```
MAIN_MENU    ──(RF Receiver)──► RECEIVING
             ──(RF Transmitter)──► TRANSMITTING
             ──(Back)──► SYSTEM_EVENT_BACK
RECEIVING    ──(code detected)──► update codeLine_/bitLine_
             ──(PRESS/LEFT)──► MAIN_MENU
TRANSMITTING ──(first loop, has code)──► send once, set txSent_
             ──(PRESS/LEFT)──► MAIN_MENU
```

### 7.6 FileManagerApp

```
onSetup():
  if SD not mounted → NO_SD state
  else → BROWSING, listDir("/")

BROWSING:
  UP/DOWN → scroll MenuListView
  BUTTON_PRESS:
    if entry is directory → descend (append to currentPath_, re-listDir)
    if entry is file → no-op (size shown in label)
  LEFT:
    if currentPath_ == "/" → SYSTEM_EVENT_BACK
    else → strip last component, re-listDir

NO_SD:
  BUTTON_PRESS / LEFT → SYSTEM_EVENT_BACK
```

Entry label format:
- Directory: `[name]`
- File < 1 KB: `name               NNB`
- File ≥ 1 KB: `name               NNK`

Maximum entries per directory: **16**.  
Maximum path depth: **128 characters**.

### 7.7 AmiiboApp (Phase 12)

```
MAIN_MENU    ──(Browse Amiibo)──► BROWSING
             ──(Generate Keys)──► GENERATE_KEYS
             ──(Write to Tag)──► WRITING
             ──(Back)──► SYSTEM_EVENT_BACK
BROWSING     ──(SELECT dir)──► descend, re-listDir
             ──(SELECT .bin 540B)──► FILE_SELECTED
             ──(LEFT)──► navigate up / MAIN_MENU
FILE_SELECTED──(OK/PRESS)──► EMULATING
             ──(DOWN)──► WRITING (write to blank NTAG215)
             ──(LEFT)──► BROWSING
EMULATING    ──(PRESS/LEFT)──► FILE_SELECTED
WRITING      ──(PRESS/LEFT)──► MAIN_MENU
GENERATE_KEYS──(PRESS/LEFT)──► MAIN_MENU
```

**SD directory**: `/ext/nfc/amiibo/` – user organises `.bin` files in sub-folders (e.g. `Zelda/`, `Mario/`).
**Dump format**: Raw 540-byte NTAG215 binary image (135 pages × 4 bytes).
**Heap allocation**: Single `heap_caps_malloc(540, MALLOC_CAP_8BIT)` – freed in `onDestroy()`.
**NFC commands handled**: READ (0x30), FAST_READ (0x3A), PWD_AUTH (0x1B), GET_VERSION (0x60).

### 7.8 GhostNetApp (Phase 16)

```
MAIN_MENU    ──(Radar)──► RADAR
             ──(Chat)──► CHAT_VIEW
             ──(Remote Exec)──► REMOTE_EXEC
             ──(Sync Data)──► SYNC_VIEW
             ──(Back)──► SYSTEM_EVENT_BACK
RADAR        ──(PRESS/LEFT)──► MAIN_MENU
CHAT_VIEW    ──(UP/DOWN)──► scroll messages
             ──(RIGHT)──► cycle quick-send message
             ──(PRESS)──► send quick message
             ──(LEFT)──► MAIN_MENU
REMOTE_EXEC  ──(UP/DOWN)──► navigate command menu
             ──(PRESS BLE Spam)──► send CMD_BLE_SPAM to all peers
             ──(PRESS Deauth)──► send CMD_WIFI_DEAUTH to all peers
             ──(PRESS Stop)──► send CMD_STOP to all peers
             ──(LEFT/Back)──► MAIN_MENU
SYNC_VIEW    ──(PRESS)──► broadcast sync ping to peers
             ──(LEFT)──► MAIN_MENU
```

**ESP-NOW**: Zero-configuration mesh – devices auto-discover via periodic beacons.
**Encryption**: PMK/LMK key pair shared across all HackOS devices.
**Max peers**: 8 simultaneous, pruned after 30 s timeout.
**Packet format**: 2-byte magic `{'G','N'}` + type + seqNo + srcMAC + srcName + payload (≤220 bytes).

### 7.9 GhostNetManager (Core)

Singleton managing the ESP-NOW mesh layer:
- **Auto-discovery**: Broadcast beacons every 5 s; peers reply with ACK.
- **Peer table**: Up to 8 peers tracked with MAC, RSSI, name, and last-seen timestamp.
- **Chat**: Ring buffer of 16 messages; text sent/received to/from all peers.
- **Data sync**: Arbitrary payload broadcast with type tag (WiFi handshake, NFC UID, etc.).
- **Remote execution**: Master issues `GhostCmd` (BLE_SPAM, WIFI_DEAUTH, STOP); Nodes acknowledge.

---

## 8. SD Card Capture Files

| App | Path | Format |
|---|---|---|
| WiFi Tools → Save AP | `/captures/wifi_scan.txt` | `SSID: …\nBSSID: …\nRSSI: … dBm\nChannel: …\nAuth: …\n` |
| NFC Tools → Save UID | `/captures/nfc_uid.txt` | `UID: XX:XX:XX:XX\n` |
| IR Tools → Save Code | `/captures/ir_codes.txt` | `Proto: …\nCode: 0x…\nBits: …\n` |
| Amiibo Master → Keys | `/ext/nfc/amiibo/amiibo_keys.bin` | Binary placeholder (80 bytes) |

All files are **appended** (not overwritten) so multiple captures accumulate.  
Create the `/captures/` directory on the SD card before first use.

---

## 9. Memory Budget

| Region | Budget | Notes |
|---|---|---|
| Free heap at Launcher | > 150 KB | Logged via `ESP.getFreeHeap()` at startup |
| WiFi AP label heap | max 16 × 32 B = 512 B | Freed in `onDestroy()` |
| Amiibo dump buffer | 540 B | `heap_caps_malloc` in `onSetup()`, freed in `onDestroy()` |
| GhostNet peer table | 8 × ~32 B = 256 B | Static in GhostNetManager singleton |
| GhostNet chat ring | 16 × ~84 B ≈ 1.3 KB | Static ring buffer |
| ESP-NOW overhead | ~2 KB | ESP-IDF internal buffers |
| EventSystem queue | 16 × sizeof(Event) ≈ 320 B | FreeRTOS static queue |
| OLED frame buffer | 1 KB (SSD1306 128×64 / 8) | Inside Adafruit GFX |
| Stack per FreeRTOS task | default Arduino task: 8 KB | No custom tasks created |

---

## 10. Build Flags and Compiler Warnings

```ini
build_flags =
    -std=gnu++17
    -Wall
    -Wextra
```

All files compile cleanly under `-Wall -Wextra`. Intentionally-ignored return values are silenced with `(void)` casts (e.g., `(void)EventSystem::instance().subscribe(this)`).

---

## 11. Known Limitations (MVP)

| Item | Detail |
|---|---|
| No clock source | `setTime()` is called with hardcoded `0, 0` – no RTC module |
| Battery level | Hardcoded to 100% – no ADC battery monitor |
| Evil Twin | Logged only; AP is not actually hosted |
| SD writes are synchronous | `writeFile` / `appendChunk` block the loop; keep payloads < 4 KB |
| No OTA | Firmware updates require USB flashing via PlatformIO |
| Max 16 apps | `AppManager::MAX_APPS = 16`; `EventSystem::MAX_OBSERVERS = 8` |
| SD `/captures/` must exist | The directory is not auto-created; format card with it pre-made |
| Amiibo keys placeholder | `amiibo_keys.bin` is a zeroed placeholder; real keys must be obtained separately |
| Amiibo emulation blocking | `emulateNtag215()` blocks the loop for up to 30 s waiting for a reader |
| GhostNet shared keys | PMK/LMK are hardcoded; all HackOS devices share the same keys |
| GhostNet RSSI | ESP-NOW recv callback does not expose RSSI; a placeholder value is used |
| GhostNet max peers | Limited to 8 simultaneous peers by ESP-NOW peer table |

---

## 12. Roadmap (Post-MVP)

- [ ] RTC integration (DS3231) for accurate timestamps on captures
- [ ] Battery ADC monitor feeding StatusBar  
- [ ] Bluetooth BLE scanner app  
- [ ] OTA update via WiFi  
- [ ] SPIFFS-backed settings store  
- [ ] Auto-create `/captures/` directory on SD mount  
- [ ] Async SD write task (FreeRTOS) with a write queue to fully decouple UI from I/O  
