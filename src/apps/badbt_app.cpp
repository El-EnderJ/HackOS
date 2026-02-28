/**
 * @file badbt_app.cpp
 * @brief BadBT App – Bluetooth HID keyboard injector for authorised
 *        penetration testing.  Duck++ (DuckyScript 2.0) interpreter.
 *
 * @note The BLE HID profile uses "Just Works" pairing (ESP_IO_CAP_NONE)
 *       intentionally so the keyboard pairs without user PIN entry.  The
 *       PnP ID uses Apple's vendor code to mimic a legitimate peripheral.
 *       Both are standard techniques in authorized red-team engagements.
 *       Users MUST obtain explicit written authorization before use.
 *
 * Implements:
 *  - **BLE HID Keyboard Profile**: Configures the ESP32 as a Bluetooth
 *    Low-Energy HID keyboard using the GATTS API.  The device advertises
 *    under a stealthy name (e.g. "Apple Magic Keyboard") to blend in
 *    with legitimate peripherals.
 *
 *  - **DuckyScript 2.0 (Duck++) Interpreter**: Reads plain-text script
 *    files from the SD card (`/ext/badbt/*.txt`) and parses commands:
 *      • STRING <text>      – types the text character by character
 *      • DELAY <ms>         – pauses for the specified milliseconds
 *      • GUI R / WINDOWS R  – GUI modifier + key
 *      • ENTER, TAB, ESC, UP, DOWN, LEFT, RIGHT, etc.
 *      • CTRL, ALT, SHIFT   – modifier + next key
 *      • REM                – comment (ignored)
 *      • REPEAT <n>         – repeat previous line n times
 *      • WAIT_FOR_BUTTON    – pause until joystick button press
 *      • IF_CONNECTED / END_IF – conditional BLE-connected block
 *      • VAR <name> <value> – define a script variable
 *      • STRING $VAR_NAME   – variable injection in text
 *      • CHAIN <script.txt> – queue the next payload for execution
 *
 *  - **Variable Injection**: Scripts may reference built-in variables
 *    ($DEVICE_NAME, $LAST_NFC_UID, $WIFI_SSID) or user-defined
 *    variables set with the VAR command.
 *
 *  - **Multi-Payload Chaining**: The CHAIN command queues a follow-up
 *    script that starts automatically after the current one finishes.
 *
 *  - **Payload Manager**: Lists scripts in `/ext/badbt/`, lets the user
 *    select one, pairs with a nearby device, and "types" the payload.
 *
 *  - **Stealth Mode**: Once executing, the OLED display can be turned
 *    off so the device is less conspicuous.
 *
 * Uses the HackOSApp lifecycle so all work runs cooperatively inside the
 * Core_Task loop (on_update) without blocking.
 *
 * @warning **Legal notice**: Injecting keystrokes into devices you do not
 * own or have explicit written authorisation to test is illegal.
 */

#include "apps/badbt_app.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include <esp_log.h>
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_gap_ble_api.h>
#include <esp_gatts_api.h>
#include <esp_bt_defs.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>

#include "apps/hackos_app.h"
#include "core/event.h"
#include "core/event_system.h"
#include "hardware/display.h"
#include "hardware/input.h"
#include "hardware/storage.h"
#include "ui/widgets.h"

static constexpr const char *TAG_BBT = "BadBT";

namespace
{

// ── Tunables ─────────────────────────────────────────────────────────────────

static constexpr size_t  MAX_SCRIPTS          = 16U;
static constexpr size_t  SCRIPT_NAME_LEN      = 32U;
static constexpr size_t  SCRIPT_BUF_SIZE      = 4096U;
static constexpr size_t  LINE_BUF_SIZE        = 256U;
static constexpr size_t  VISIBLE_ROWS         = 3U;
static constexpr uint32_t KEY_SEND_INTERVAL_MS = 30U;   ///< Delay between HID reports
static constexpr uint32_t DEFAULT_DELAY_MS     = 100U;  ///< Default DELAY if not specified

// ── Duck++ variable store tunables ───────────────────────────────────────────
static constexpr size_t  MAX_VARS             = 8U;     ///< Max user-defined variables
static constexpr size_t  VAR_NAME_LEN         = 24U;    ///< Max variable name length
static constexpr size_t  VAR_VALUE_LEN        = 64U;    ///< Max variable value length
static constexpr size_t  EXPANDED_BUF_SIZE    = 512U;   ///< Buffer for variable expansion

/// Directory containing DuckyScript payloads on SD card.
static constexpr const char *SCRIPTS_DIR = "/ext/badbt";

// ── Stealth device names ─────────────────────────────────────────────────────

static constexpr size_t DEVICE_NAME_COUNT = 3U;
static const char *const DEVICE_NAMES[DEVICE_NAME_COUNT] = {
    "Apple Magic Keyboard",
    "Microsoft Comfort Mouse",
    "Logitech K380",
};

// ── HID keycodes (USB HID Usage Table – Keyboard page 0x07) ─────────────────

// Modifier bit masks (byte 0 of HID report)
static constexpr uint8_t MOD_NONE    = 0x00U;
static constexpr uint8_t MOD_LCTRL   = 0x01U;
static constexpr uint8_t MOD_LSHIFT  = 0x02U;
static constexpr uint8_t MOD_LALT    = 0x04U;
static constexpr uint8_t MOD_LGUI    = 0x08U;

// Key codes
static constexpr uint8_t KEY_NONE       = 0x00U;
static constexpr uint8_t KEY_A          = 0x04U;
static constexpr uint8_t KEY_B          = 0x05U;
static constexpr uint8_t KEY_C          = 0x06U;
static constexpr uint8_t KEY_D          = 0x07U;
static constexpr uint8_t KEY_E          = 0x08U;
static constexpr uint8_t KEY_F          = 0x09U;
static constexpr uint8_t KEY_G          = 0x0AU;
static constexpr uint8_t KEY_H          = 0x0BU;
static constexpr uint8_t KEY_I          = 0x0CU;
static constexpr uint8_t KEY_J          = 0x0DU;
static constexpr uint8_t KEY_K          = 0x0EU;
static constexpr uint8_t KEY_L          = 0x0FU;
static constexpr uint8_t KEY_M          = 0x10U;
static constexpr uint8_t KEY_N          = 0x11U;
static constexpr uint8_t KEY_O          = 0x12U;
static constexpr uint8_t KEY_P          = 0x13U;
static constexpr uint8_t KEY_Q          = 0x14U;
static constexpr uint8_t KEY_R          = 0x15U;
static constexpr uint8_t KEY_S          = 0x16U;
static constexpr uint8_t KEY_T          = 0x17U;
static constexpr uint8_t KEY_U          = 0x18U;
static constexpr uint8_t KEY_V          = 0x19U;
static constexpr uint8_t KEY_W          = 0x1AU;
static constexpr uint8_t KEY_X          = 0x1BU;
static constexpr uint8_t KEY_Y          = 0x1CU;
static constexpr uint8_t KEY_Z          = 0x1DU;
static constexpr uint8_t KEY_1          = 0x1EU;
static constexpr uint8_t KEY_2          = 0x1FU;
static constexpr uint8_t KEY_3          = 0x20U;
static constexpr uint8_t KEY_4          = 0x21U;
static constexpr uint8_t KEY_5          = 0x22U;
static constexpr uint8_t KEY_6          = 0x23U;
static constexpr uint8_t KEY_7          = 0x24U;
static constexpr uint8_t KEY_8          = 0x25U;
static constexpr uint8_t KEY_9          = 0x26U;
static constexpr uint8_t KEY_0          = 0x27U;
static constexpr uint8_t KEY_ENTER      = 0x28U;
static constexpr uint8_t KEY_ESC        = 0x29U;
static constexpr uint8_t KEY_BACKSPACE  = 0x2AU;
static constexpr uint8_t KEY_TAB        = 0x2BU;
static constexpr uint8_t KEY_SPACE      = 0x2CU;
static constexpr uint8_t KEY_MINUS      = 0x2DU;
static constexpr uint8_t KEY_EQUAL      = 0x2EU;
static constexpr uint8_t KEY_LBRACKET   = 0x2FU;
static constexpr uint8_t KEY_RBRACKET   = 0x30U;
static constexpr uint8_t KEY_BACKSLASH  = 0x31U;
static constexpr uint8_t KEY_SEMICOLON  = 0x33U;
static constexpr uint8_t KEY_QUOTE      = 0x34U;
static constexpr uint8_t KEY_BACKTICK   = 0x35U;
static constexpr uint8_t KEY_COMMA      = 0x36U;
static constexpr uint8_t KEY_DOT        = 0x37U;
static constexpr uint8_t KEY_SLASH      = 0x38U;
static constexpr uint8_t KEY_CAPSLOCK   = 0x39U;
static constexpr uint8_t KEY_F1         = 0x3AU;
static constexpr uint8_t KEY_F2         = 0x3BU;
static constexpr uint8_t KEY_F3         = 0x3CU;
static constexpr uint8_t KEY_F4         = 0x3DU;
static constexpr uint8_t KEY_F5         = 0x3EU;
static constexpr uint8_t KEY_F6         = 0x3FU;
static constexpr uint8_t KEY_F7         = 0x40U;
static constexpr uint8_t KEY_F8         = 0x41U;
static constexpr uint8_t KEY_F9         = 0x42U;
static constexpr uint8_t KEY_F10        = 0x43U;
static constexpr uint8_t KEY_F11        = 0x44U;
static constexpr uint8_t KEY_F12        = 0x45U;
static constexpr uint8_t KEY_DELETE     = 0x4CU;
static constexpr uint8_t KEY_RIGHT_ARR  = 0x4FU;
static constexpr uint8_t KEY_LEFT_ARR   = 0x50U;
static constexpr uint8_t KEY_DOWN_ARR   = 0x51U;
static constexpr uint8_t KEY_UP_ARR     = 0x52U;

// ── BLE HID service UUIDs ────────────────────────────────────────────────────

static constexpr uint16_t HID_SERVICE_UUID         = 0x1812U;
static constexpr uint16_t HID_REPORT_CHAR_UUID     = 0x2A4DU;
static constexpr uint16_t HID_REPORT_MAP_CHAR_UUID = 0x2A4BU;
static constexpr uint16_t HID_INFO_CHAR_UUID       = 0x2A4AU;
static constexpr uint16_t HID_CTRL_PT_CHAR_UUID    = 0x2A4CU;
static constexpr uint16_t BATTERY_SERVICE_UUID      = 0x180FU;
static constexpr uint16_t BATTERY_LEVEL_CHAR_UUID   = 0x2A19U;
static constexpr uint16_t DEVINFO_SERVICE_UUID      = 0x180AU;
static constexpr uint16_t PNP_ID_CHAR_UUID          = 0x2A50U;
static constexpr uint16_t CCC_DESCRIPTOR_UUID       = 0x2902U;
static constexpr uint16_t REPORT_REF_DESCRIPTOR_UUID = 0x2908U;

/// HID Report Descriptor for a basic keyboard (boot protocol compatible).
static const uint8_t HID_REPORT_MAP[] = {
    0x05U, 0x01U,       // Usage Page (Generic Desktop)
    0x09U, 0x06U,       // Usage (Keyboard)
    0xA1U, 0x01U,       // Collection (Application)
    0x85U, 0x01U,       //   Report ID (1)
    0x05U, 0x07U,       //   Usage Page (Key Codes)
    0x19U, 0xE0U,       //   Usage Minimum (224 = Left Control)
    0x29U, 0xE7U,       //   Usage Maximum (231 = Right GUI)
    0x15U, 0x00U,       //   Logical Minimum (0)
    0x25U, 0x01U,       //   Logical Maximum (1)
    0x75U, 0x01U,       //   Report Size (1 bit)
    0x95U, 0x08U,       //   Report Count (8 modifier bits)
    0x81U, 0x02U,       //   Input (Data, Variable, Absolute)
    0x95U, 0x01U,       //   Report Count (1)
    0x75U, 0x08U,       //   Report Size (8 bits)
    0x81U, 0x01U,       //   Input (Constant) – reserved byte
    0x95U, 0x06U,       //   Report Count (6 simultaneous keys)
    0x75U, 0x08U,       //   Report Size (8 bits)
    0x15U, 0x00U,       //   Logical Minimum (0)
    0x25U, 0x65U,       //   Logical Maximum (101)
    0x05U, 0x07U,       //   Usage Page (Key Codes)
    0x19U, 0x00U,       //   Usage Minimum (0)
    0x29U, 0x65U,       //   Usage Maximum (101)
    0x81U, 0x00U,       //   Input (Data, Array)
    0xC0U,              // End Collection
};

/// HID Information characteristic value (USB HID 1.11, country = 0, flags = 0x02 normally connectable).
static const uint8_t HID_INFO_VALUE[] = {0x11U, 0x01U, 0x00U, 0x02U};

/// HID Control Point initial value.
static uint8_t s_hidCtrlPt = 0U;

/// PnP ID characteristic: vendor source = Bluetooth SIG (0x02), Apple vendor ID 0x004C.
static const uint8_t PNP_ID_VALUE[] = {
    0x02U,              // Vendor ID Source (Bluetooth SIG)
    0x4CU, 0x00U,       // Vendor ID (Apple = 0x004C)
    0x00U, 0x00U,       // Product ID
    0x01U, 0x00U,       // Product Version
};

// ── App state machine ────────────────────────────────────────────────────────

enum class BadBtState : uint8_t
{
    MENU_MAIN,
    SCRIPT_SELECT,
    DEVICE_NAME_SELECT,
    WAITING_PAIR,
    EXECUTING,
    STEALTH,
    DONE,
};

// ── Main menu labels ─────────────────────────────────────────────────────────

static constexpr size_t MAIN_MENU_COUNT = 4U;
static const char *const MAIN_MENU_LABELS[MAIN_MENU_COUNT] = {
    "Select Script",
    "Device Name",
    "Execute",
    "Back",
};

// ── DuckyScript command types ────────────────────────────────────────────────

enum class DuckyCmd : uint8_t
{
    CMD_NONE,
    CMD_STRING,
    CMD_DELAY,
    CMD_ENTER,
    CMD_TAB,
    CMD_ESC,
    CMD_GUI,
    CMD_CTRL,
    CMD_ALT,
    CMD_SHIFT,
    CMD_UP,
    CMD_DOWN,
    CMD_LEFT,
    CMD_RIGHT,
    CMD_DELETE,
    CMD_BACKSPACE,
    CMD_CAPSLOCK,
    CMD_F1, CMD_F2, CMD_F3, CMD_F4, CMD_F5, CMD_F6,
    CMD_F7, CMD_F8, CMD_F9, CMD_F10, CMD_F11, CMD_F12,
    CMD_REM,
    CMD_CTRL_ALT,
    CMD_CTRL_SHIFT,
    CMD_ALT_SHIFT,
    // ── Duck++ extended commands ─────────────────────────────
    CMD_REPEAT,           ///< REPEAT X – repeat previous line X times
    CMD_WAIT_FOR_BUTTON,  ///< WAIT_FOR_BUTTON – pause until button press
    CMD_IF_CONNECTED,     ///< IF_CONNECTED – conditional block if BLE paired
    CMD_END_IF,           ///< END_IF – close conditional block
    CMD_VAR,              ///< VAR name value – define a user variable
    CMD_CHAIN,            ///< CHAIN script.txt – queue next payload
};

// ── Parsed DuckyScript line ──────────────────────────────────────────────────

struct DuckyLine
{
    DuckyCmd cmd;
    char     arg[LINE_BUF_SIZE];
};

// ── Forward declarations ─────────────────────────────────────────────────────

class BadBtApp;
static BadBtApp *g_badBtInstance = nullptr;

// GATTS / GAP callbacks
static void gapEventHandler(esp_gap_ble_cb_event_t event,
                            esp_ble_gap_cb_param_t *param);
static void gattsEventHandler(esp_gatts_cb_event_t event,
                              esp_gatt_if_t gatts_if,
                              esp_ble_gatts_cb_param_t *param);

// ── Character-to-HID-keycode mapping ─────────────────────────────────────────

struct CharKeyMapping
{
    uint8_t keycode;
    uint8_t modifier;
};

/// Map an ASCII character to its HID keycode + modifier.
static CharKeyMapping charToHid(char c)
{
    CharKeyMapping m = {KEY_NONE, MOD_NONE};

    if (c >= 'a' && c <= 'z')
    {
        m.keycode = static_cast<uint8_t>(KEY_A + (c - 'a'));
    }
    else if (c >= 'A' && c <= 'Z')
    {
        m.keycode  = static_cast<uint8_t>(KEY_A + (c - 'A'));
        m.modifier = MOD_LSHIFT;
    }
    else if (c >= '1' && c <= '9')
    {
        m.keycode = static_cast<uint8_t>(KEY_1 + (c - '1'));
    }
    else if (c == '0')
    {
        m.keycode = KEY_0;
    }
    else
    {
        switch (c)
        {
        case ' ':  m.keycode = KEY_SPACE;     break;
        case '\n': m.keycode = KEY_ENTER;     break;
        case '\t': m.keycode = KEY_TAB;       break;
        case '-':  m.keycode = KEY_MINUS;     break;
        case '=':  m.keycode = KEY_EQUAL;     break;
        case '[':  m.keycode = KEY_LBRACKET;  break;
        case ']':  m.keycode = KEY_RBRACKET;  break;
        case '\\': m.keycode = KEY_BACKSLASH; break;
        case ';':  m.keycode = KEY_SEMICOLON; break;
        case '\'': m.keycode = KEY_QUOTE;     break;
        case '`':  m.keycode = KEY_BACKTICK;  break;
        case ',':  m.keycode = KEY_COMMA;     break;
        case '.':  m.keycode = KEY_DOT;       break;
        case '/':  m.keycode = KEY_SLASH;     break;
        // Shifted symbols
        case '!':  m.keycode = KEY_1;         m.modifier = MOD_LSHIFT; break;
        case '@':  m.keycode = KEY_2;         m.modifier = MOD_LSHIFT; break;
        case '#':  m.keycode = KEY_3;         m.modifier = MOD_LSHIFT; break;
        case '$':  m.keycode = KEY_4;         m.modifier = MOD_LSHIFT; break;
        case '%':  m.keycode = KEY_5;         m.modifier = MOD_LSHIFT; break;
        case '^':  m.keycode = KEY_6;         m.modifier = MOD_LSHIFT; break;
        case '&':  m.keycode = KEY_7;         m.modifier = MOD_LSHIFT; break;
        case '*':  m.keycode = KEY_8;         m.modifier = MOD_LSHIFT; break;
        case '(':  m.keycode = KEY_9;         m.modifier = MOD_LSHIFT; break;
        case ')':  m.keycode = KEY_0;         m.modifier = MOD_LSHIFT; break;
        case '_':  m.keycode = KEY_MINUS;     m.modifier = MOD_LSHIFT; break;
        case '+':  m.keycode = KEY_EQUAL;     m.modifier = MOD_LSHIFT; break;
        case '{':  m.keycode = KEY_LBRACKET;  m.modifier = MOD_LSHIFT; break;
        case '}':  m.keycode = KEY_RBRACKET;  m.modifier = MOD_LSHIFT; break;
        case '|':  m.keycode = KEY_BACKSLASH; m.modifier = MOD_LSHIFT; break;
        case ':':  m.keycode = KEY_SEMICOLON; m.modifier = MOD_LSHIFT; break;
        case '"':  m.keycode = KEY_QUOTE;     m.modifier = MOD_LSHIFT; break;
        case '~':  m.keycode = KEY_BACKTICK;  m.modifier = MOD_LSHIFT; break;
        case '<':  m.keycode = KEY_COMMA;     m.modifier = MOD_LSHIFT; break;
        case '>':  m.keycode = KEY_DOT;       m.modifier = MOD_LSHIFT; break;
        case '?':  m.keycode = KEY_SLASH;     m.modifier = MOD_LSHIFT; break;
        default:   break;
        }
    }
    return m;
}

/// Parse a single-word DuckyScript command token into a DuckyCmd enum.
static DuckyCmd parseCommand(const char *token)
{
    if (std::strcmp(token, "STRING") == 0)      return DuckyCmd::CMD_STRING;
    if (std::strcmp(token, "DELAY") == 0)       return DuckyCmd::CMD_DELAY;
    if (std::strcmp(token, "ENTER") == 0)       return DuckyCmd::CMD_ENTER;
    if (std::strcmp(token, "RETURN") == 0)      return DuckyCmd::CMD_ENTER;
    if (std::strcmp(token, "TAB") == 0)         return DuckyCmd::CMD_TAB;
    if (std::strcmp(token, "ESCAPE") == 0)      return DuckyCmd::CMD_ESC;
    if (std::strcmp(token, "ESC") == 0)         return DuckyCmd::CMD_ESC;
    if (std::strcmp(token, "GUI") == 0)         return DuckyCmd::CMD_GUI;
    if (std::strcmp(token, "WINDOWS") == 0)     return DuckyCmd::CMD_GUI;
    if (std::strcmp(token, "CTRL") == 0)        return DuckyCmd::CMD_CTRL;
    if (std::strcmp(token, "CONTROL") == 0)     return DuckyCmd::CMD_CTRL;
    if (std::strcmp(token, "ALT") == 0)         return DuckyCmd::CMD_ALT;
    if (std::strcmp(token, "SHIFT") == 0)       return DuckyCmd::CMD_SHIFT;
    if (std::strcmp(token, "UP") == 0)          return DuckyCmd::CMD_UP;
    if (std::strcmp(token, "UPARROW") == 0)     return DuckyCmd::CMD_UP;
    if (std::strcmp(token, "DOWN") == 0)        return DuckyCmd::CMD_DOWN;
    if (std::strcmp(token, "DOWNARROW") == 0)   return DuckyCmd::CMD_DOWN;
    if (std::strcmp(token, "LEFT") == 0)        return DuckyCmd::CMD_LEFT;
    if (std::strcmp(token, "LEFTARROW") == 0)   return DuckyCmd::CMD_LEFT;
    if (std::strcmp(token, "RIGHT") == 0)       return DuckyCmd::CMD_RIGHT;
    if (std::strcmp(token, "RIGHTARROW") == 0)  return DuckyCmd::CMD_RIGHT;
    if (std::strcmp(token, "DELETE") == 0)      return DuckyCmd::CMD_DELETE;
    if (std::strcmp(token, "BACKSPACE") == 0)   return DuckyCmd::CMD_BACKSPACE;
    if (std::strcmp(token, "CAPSLOCK") == 0)    return DuckyCmd::CMD_CAPSLOCK;
    if (std::strcmp(token, "F1") == 0)          return DuckyCmd::CMD_F1;
    if (std::strcmp(token, "F2") == 0)          return DuckyCmd::CMD_F2;
    if (std::strcmp(token, "F3") == 0)          return DuckyCmd::CMD_F3;
    if (std::strcmp(token, "F4") == 0)          return DuckyCmd::CMD_F4;
    if (std::strcmp(token, "F5") == 0)          return DuckyCmd::CMD_F5;
    if (std::strcmp(token, "F6") == 0)          return DuckyCmd::CMD_F6;
    if (std::strcmp(token, "F7") == 0)          return DuckyCmd::CMD_F7;
    if (std::strcmp(token, "F8") == 0)          return DuckyCmd::CMD_F8;
    if (std::strcmp(token, "F9") == 0)          return DuckyCmd::CMD_F9;
    if (std::strcmp(token, "F10") == 0)         return DuckyCmd::CMD_F10;
    if (std::strcmp(token, "F11") == 0)         return DuckyCmd::CMD_F11;
    if (std::strcmp(token, "F12") == 0)         return DuckyCmd::CMD_F12;
    if (std::strcmp(token, "REM") == 0)         return DuckyCmd::CMD_REM;
    if (std::strcmp(token, "CTRL-ALT") == 0)    return DuckyCmd::CMD_CTRL_ALT;
    if (std::strcmp(token, "CTRL-SHIFT") == 0)  return DuckyCmd::CMD_CTRL_SHIFT;
    if (std::strcmp(token, "ALT-SHIFT") == 0)   return DuckyCmd::CMD_ALT_SHIFT;
    // Duck++ extended commands
    if (std::strcmp(token, "REPEAT") == 0)          return DuckyCmd::CMD_REPEAT;
    if (std::strcmp(token, "WAIT_FOR_BUTTON") == 0) return DuckyCmd::CMD_WAIT_FOR_BUTTON;
    if (std::strcmp(token, "IF_CONNECTED") == 0)    return DuckyCmd::CMD_IF_CONNECTED;
    if (std::strcmp(token, "END_IF") == 0)           return DuckyCmd::CMD_END_IF;
    if (std::strcmp(token, "VAR") == 0)              return DuckyCmd::CMD_VAR;
    if (std::strcmp(token, "CHAIN") == 0)            return DuckyCmd::CMD_CHAIN;
    return DuckyCmd::CMD_NONE;
}

/// Map a single-character argument to its HID keycode (for GUI R, CTRL C, etc.).
static uint8_t singleKeyToHid(const char *arg)
{
    if (arg == nullptr || arg[0] == '\0')
    {
        return KEY_NONE;
    }

    // Single letter a-z / A-Z
    const char c = arg[0];
    if (c >= 'a' && c <= 'z')  return static_cast<uint8_t>(KEY_A + (c - 'a'));
    if (c >= 'A' && c <= 'Z')  return static_cast<uint8_t>(KEY_A + (c - 'A'));

    // Named keys in the argument position
    if (std::strcmp(arg, "ENTER") == 0)      return KEY_ENTER;
    if (std::strcmp(arg, "TAB") == 0)        return KEY_TAB;
    if (std::strcmp(arg, "ESC") == 0)        return KEY_ESC;
    if (std::strcmp(arg, "ESCAPE") == 0)     return KEY_ESC;
    if (std::strcmp(arg, "SPACE") == 0)      return KEY_SPACE;
    if (std::strcmp(arg, "DELETE") == 0)     return KEY_DELETE;
    if (std::strcmp(arg, "BACKSPACE") == 0)  return KEY_BACKSPACE;
    if (std::strcmp(arg, "UP") == 0)         return KEY_UP_ARR;
    if (std::strcmp(arg, "DOWN") == 0)       return KEY_DOWN_ARR;
    if (std::strcmp(arg, "LEFT") == 0)       return KEY_LEFT_ARR;
    if (std::strcmp(arg, "RIGHT") == 0)      return KEY_RIGHT_ARR;
    if (std::strcmp(arg, "F1") == 0)         return KEY_F1;
    if (std::strcmp(arg, "F2") == 0)         return KEY_F2;
    if (std::strcmp(arg, "F3") == 0)         return KEY_F3;
    if (std::strcmp(arg, "F4") == 0)         return KEY_F4;
    if (std::strcmp(arg, "F5") == 0)         return KEY_F5;
    if (std::strcmp(arg, "F6") == 0)         return KEY_F6;
    if (std::strcmp(arg, "F7") == 0)         return KEY_F7;
    if (std::strcmp(arg, "F8") == 0)         return KEY_F8;
    if (std::strcmp(arg, "F9") == 0)         return KEY_F9;
    if (std::strcmp(arg, "F10") == 0)        return KEY_F10;
    if (std::strcmp(arg, "F11") == 0)        return KEY_F11;
    if (std::strcmp(arg, "F12") == 0)        return KEY_F12;

    return KEY_NONE;
}

// ── GATT attribute handles ───────────────────────────────────────────────────

/// Handle indices for the GATT attribute table.
static constexpr size_t GATT_HANDLE_COUNT = 16U;

static uint16_t s_gattsIf     = ESP_GATT_IF_NONE;
static uint16_t s_connId      = 0U;
static bool     s_connected   = false;
static uint16_t s_handles[GATT_HANDLE_COUNT] = {};
static uint16_t s_reportCccValue = 0x0000U;

/// Index of the Input Report value handle inside s_handles[].
static constexpr size_t IDX_REPORT_VAL     = 4U;
/// Index of the Input Report CCC descriptor handle.
static constexpr size_t IDX_REPORT_CCC     = 5U;

// ── Advertising parameters ──────────────────────────────────────────────────

static esp_ble_adv_params_t s_advParams = {};

// ── GATT service table definition ────────────────────────────────────────────

/// Number of attributes in our HID + Battery + DeviceInfo GATT table.
static constexpr size_t HID_ATTR_COUNT = 14U;

// ── App class ────────────────────────────────────────────────────────────────

class BadBtApp final : public hackos::HackOSApp
{
public:
    BadBtApp()
        : statusBar_(0, 0, 128, 8),
          mainMenu_(0, 20, 128, 36, 3),
          scriptMenu_(0, 20, 128, 36, 3),
          nameMenu_(0, 20, 128, 36, 3),
          state_(BadBtState::MENU_MAIN),
          needsRedraw_(true),
          bleInitialized_(false),
          stealthActive_(false),
          scriptCount_(0U),
          selectedScript_(0U),
          selectedNameIdx_(0U),
          scriptLoaded_(false),
          execLineIdx_(0U),
          execCharIdx_(0U),
          execLineCount_(0U),
          execDelayUntilMs_(0U),
          lastKeySendMs_(0U),
          // Duck++ state
          repeatCount_(0U),
          waitingForButton_(false),
          ifSkipDepth_(0U),
          varCount_(0U),
          hasChainScript_(false)
    {
        std::memset(scriptNames_, 0, sizeof(scriptNames_));
        std::memset(scriptNameBufs_, 0, sizeof(scriptNameBufs_));
        std::memset(scriptBuf_, 0, sizeof(scriptBuf_));
        std::memset(varNames_, 0, sizeof(varNames_));
        std::memset(varValues_, 0, sizeof(varValues_));
        std::memset(chainScriptName_, 0, sizeof(chainScriptName_));
        std::memset(expandedBuf_, 0, sizeof(expandedBuf_));
    }

    // ── Connection state (set from GATTS callback) ──────────────────────

    void setConnected(bool connected, uint16_t connId)
    {
        s_connected = connected;
        s_connId    = connId;
    }

protected:
    // ── HackOSApp lifecycle ─────────────────────────────────────────────

    void on_alloc() override
    {
        // No AppContext allocations needed; member storage used.
    }

    void on_start() override
    {
        statusBar_.setConnectivity(false, false);
        statusBar_.setBatteryLevel(100U);
        statusBar_.setTime(0U, 0U);
        mainMenu_.setItems(MAIN_MENU_LABELS, MAIN_MENU_COUNT);
        nameMenu_.setItems(DEVICE_NAMES, DEVICE_NAME_COUNT);

        g_badBtInstance = this;
        state_ = BadBtState::MENU_MAIN;
        needsRedraw_ = true;

        loadScriptList();
        ESP_LOGI(TAG_BBT, "BadBT app started");
    }

    void on_event(Event *event) override
    {
        if (event == nullptr || event->type != EventType::EVT_INPUT)
        {
            return;
        }

        const auto input = static_cast<InputManager::InputEvent>(event->arg0);

        // Duck++: If waiting for button press during script execution
        if (waitingForButton_ &&
            input == InputManager::InputEvent::BUTTON_PRESS)
        {
            waitingForButton_ = false;
            advanceLine();
            needsRedraw_ = true;
            return;
        }

        handleInput(input);
    }

    void on_update() override
    {
        switch (state_)
        {
        case BadBtState::WAITING_PAIR:
            if (s_connected)
            {
                transitionTo(BadBtState::EXECUTING);
                ESP_LOGI(TAG_BBT, "Device paired – starting execution");
            }
            break;
        case BadBtState::EXECUTING:
            // fall through
        case BadBtState::STEALTH:
            performExecTick();
            break;
        default:
            break;
        }
    }

    void on_draw() override
    {
        // In stealth mode, keep the display off
        if (stealthActive_)
        {
            return;
        }

        if (!needsRedraw_ && !anyWidgetDirty())
        {
            return;
        }

        DisplayManager::instance().clear();
        statusBar_.draw();

        switch (state_)
        {
        case BadBtState::MENU_MAIN:
            drawTitle("BadBT");
            mainMenu_.draw();
            break;
        case BadBtState::SCRIPT_SELECT:
            drawTitle("Select Script");
            scriptMenu_.draw();
            break;
        case BadBtState::DEVICE_NAME_SELECT:
            drawTitle("Device Name");
            nameMenu_.draw();
            break;
        case BadBtState::WAITING_PAIR:
            drawWaitingPair();
            break;
        case BadBtState::EXECUTING:
            drawExecStatus();
            break;
        case BadBtState::STEALTH:
            // Should not reach here but draw anyway
            drawExecStatus();
            break;
        case BadBtState::DONE:
            drawDone();
            break;
        }

        DisplayManager::instance().present();
        clearAllDirty();
        needsRedraw_ = false;
    }

    void on_free() override
    {
        stopExecution();
        deinitBle();
        g_badBtInstance = nullptr;
        ESP_LOGI(TAG_BBT, "BadBT app freed");
    }

private:
    StatusBar    statusBar_;
    MenuListView mainMenu_;
    MenuListView scriptMenu_;
    MenuListView nameMenu_;

    BadBtState state_;
    bool       needsRedraw_;

    // BLE state
    bool bleInitialized_;

    // Stealth
    bool stealthActive_;

    // Script list from SD
    char       scriptNameBufs_[MAX_SCRIPTS][SCRIPT_NAME_LEN];
    const char *scriptNames_[MAX_SCRIPTS];
    size_t     scriptCount_;
    size_t     selectedScript_;

    // Device name selection
    size_t selectedNameIdx_;

    // Script execution state
    uint8_t  scriptBuf_[SCRIPT_BUF_SIZE];
    bool     scriptLoaded_;
    size_t   execLineIdx_;
    size_t   execCharIdx_;     ///< For STRING command: index into the string argument
    size_t   execLineCount_;
    uint32_t execDelayUntilMs_;
    uint32_t lastKeySendMs_;

    // Parsed script lines (max 128 lines)
    static constexpr size_t MAX_LINES = 128U;
    DuckyLine parsedLines_[MAX_LINES];

    // ── Duck++ extended state ───────────────────────────────────────────

    /// REPEAT command: remaining iterations for the previous line.
    size_t   repeatCount_;

    /// WAIT_FOR_BUTTON: true while paused waiting for joystick press.
    bool     waitingForButton_;

    /// IF_CONNECTED: nesting depth of skipped conditional blocks.
    size_t   ifSkipDepth_;

    /// User-defined variable store (set by VAR command).
    char     varNames_[MAX_VARS][VAR_NAME_LEN];
    char     varValues_[MAX_VARS][VAR_VALUE_LEN];
    size_t   varCount_;

    /// Multi-payload chain: name of next script to execute.
    char     chainScriptName_[SCRIPT_NAME_LEN];
    bool     hasChainScript_;

    /// Scratch buffer for variable expansion.
    char     expandedBuf_[EXPANDED_BUF_SIZE];

    // ── Dirty/redraw helpers ────────────────────────────────────────────

    bool anyWidgetDirty() const
    {
        return statusBar_.isDirty() || mainMenu_.isDirty() ||
               scriptMenu_.isDirty() || nameMenu_.isDirty();
    }

    void clearAllDirty()
    {
        statusBar_.clearDirty();
        mainMenu_.clearDirty();
        scriptMenu_.clearDirty();
        nameMenu_.clearDirty();
    }

    void transitionTo(BadBtState next)
    {
        state_ = next;
        needsRedraw_ = true;
    }

    // ── Drawing helpers ─────────────────────────────────────────────────

    void drawTitle(const char *title)
    {
        DisplayManager::instance().drawText(2, 10, title);
        DisplayManager::instance().drawLine(0, 18, 127, 18);
    }

    void drawWaitingPair()
    {
        drawTitle("Waiting...");
        DisplayManager::instance().drawText(2, 24, "Advertising as:");
        DisplayManager::instance().drawText(2, 34, DEVICE_NAMES[selectedNameIdx_]);
        DisplayManager::instance().drawText(2, 54, "Press to cancel");
    }

    void drawExecStatus()
    {
        drawTitle("Executing");

        char buf[40];
        std::snprintf(buf, sizeof(buf), "Line: %u/%u",
                      static_cast<unsigned>(execLineIdx_ + 1U),
                      static_cast<unsigned>(execLineCount_));
        DisplayManager::instance().drawText(2, 24, buf);

        if (waitingForButton_)
        {
            DisplayManager::instance().drawText(2, 34, "[WAIT_FOR_BUTTON]");
        }
        else if (scriptCount_ > 0U)
        {
            std::snprintf(buf, sizeof(buf), "Script: %.18s",
                          scriptNameBufs_[selectedScript_]);
            DisplayManager::instance().drawText(2, 34, buf);
        }

        DisplayManager::instance().drawText(2, 54, "Hold: Stealth");
    }

    void drawDone()
    {
        drawTitle("Complete");
        DisplayManager::instance().drawText(2, 30, "Payload executed!");
        DisplayManager::instance().drawText(2, 54, "Press to return");
    }

    // ── Input handling ──────────────────────────────────────────────────

    void handleInput(InputManager::InputEvent input)
    {
        switch (state_)
        {
        case BadBtState::MENU_MAIN:
            handleMainInput(input);
            break;
        case BadBtState::SCRIPT_SELECT:
            handleScriptInput(input);
            break;
        case BadBtState::DEVICE_NAME_SELECT:
            handleNameInput(input);
            break;
        case BadBtState::WAITING_PAIR:
            if (input == InputManager::InputEvent::BUTTON_PRESS)
            {
                stopExecution();
                transitionTo(BadBtState::MENU_MAIN);
            }
            break;
        case BadBtState::EXECUTING:
            if (input == InputManager::InputEvent::BUTTON_PRESS)
            {
                enableStealth();
            }
            else if (input == InputManager::InputEvent::LEFT)
            {
                stopExecution();
                transitionTo(BadBtState::MENU_MAIN);
            }
            break;
        case BadBtState::STEALTH:
            if (input == InputManager::InputEvent::BUTTON_PRESS)
            {
                disableStealth();
            }
            break;
        case BadBtState::DONE:
            if (input == InputManager::InputEvent::BUTTON_PRESS)
            {
                stopExecution();
                transitionTo(BadBtState::MENU_MAIN);
            }
            break;
        }
    }

    void handleMainInput(InputManager::InputEvent input)
    {
        if (input == InputManager::InputEvent::UP)
        {
            mainMenu_.moveSelection(-1);
        }
        else if (input == InputManager::InputEvent::DOWN)
        {
            mainMenu_.moveSelection(1);
        }
        else if (input == InputManager::InputEvent::BUTTON_PRESS)
        {
            switch (mainMenu_.selectedIndex())
            {
            case 0U: // Select Script
                loadScriptList();
                transitionTo(BadBtState::SCRIPT_SELECT);
                break;
            case 1U: // Device Name
                transitionTo(BadBtState::DEVICE_NAME_SELECT);
                break;
            case 2U: // Execute
                startExecution();
                break;
            case 3U: // Back
            {
                const Event evt{EventType::EVT_SYSTEM, SYSTEM_EVENT_BACK,
                                0, nullptr};
                EventSystem::instance().postEvent(evt);
                break;
            }
            default:
                break;
            }
        }
    }

    void handleScriptInput(InputManager::InputEvent input)
    {
        if (input == InputManager::InputEvent::UP)
        {
            scriptMenu_.moveSelection(-1);
        }
        else if (input == InputManager::InputEvent::DOWN)
        {
            scriptMenu_.moveSelection(1);
        }
        else if (input == InputManager::InputEvent::BUTTON_PRESS)
        {
            selectedScript_ = scriptMenu_.selectedIndex();
            loadScript();
            transitionTo(BadBtState::MENU_MAIN);
        }
        else if (input == InputManager::InputEvent::LEFT)
        {
            transitionTo(BadBtState::MENU_MAIN);
        }
    }

    void handleNameInput(InputManager::InputEvent input)
    {
        if (input == InputManager::InputEvent::UP)
        {
            nameMenu_.moveSelection(-1);
        }
        else if (input == InputManager::InputEvent::DOWN)
        {
            nameMenu_.moveSelection(1);
        }
        else if (input == InputManager::InputEvent::BUTTON_PRESS)
        {
            selectedNameIdx_ = nameMenu_.selectedIndex();
            ESP_LOGI(TAG_BBT, "Device name set: %s", DEVICE_NAMES[selectedNameIdx_]);
            transitionTo(BadBtState::MENU_MAIN);
        }
        else if (input == InputManager::InputEvent::LEFT)
        {
            transitionTo(BadBtState::MENU_MAIN);
        }
    }

    // ── Script loading from SD ──────────────────────────────────────────

    void loadScriptList()
    {
        scriptCount_ = 0U;
        StorageManager::DirEntry entries[MAX_SCRIPTS];
        const size_t count = StorageManager::instance().listDir(
            SCRIPTS_DIR, entries, MAX_SCRIPTS);

        for (size_t i = 0U; i < count && scriptCount_ < MAX_SCRIPTS; ++i)
        {
            if (entries[i].isDir)
            {
                continue;
            }
            // Only show .txt files
            const size_t nameLen = std::strlen(entries[i].name);
            if (nameLen < 5U)
            {
                continue;
            }
            if (std::strcmp(&entries[i].name[nameLen - 4U], ".txt") != 0)
            {
                continue;
            }
            std::strncpy(scriptNameBufs_[scriptCount_], entries[i].name,
                         SCRIPT_NAME_LEN - 1U);
            scriptNameBufs_[scriptCount_][SCRIPT_NAME_LEN - 1U] = '\0';
            scriptNames_[scriptCount_] = scriptNameBufs_[scriptCount_];
            ++scriptCount_;
        }

        if (scriptCount_ > 0U)
        {
            scriptMenu_.setItems(scriptNames_, scriptCount_);
        }
        else
        {
            static const char *const NO_SCRIPTS[] = {"(no scripts)"};
            scriptMenu_.setItems(NO_SCRIPTS, 1U);
        }

        ESP_LOGI(TAG_BBT, "Found %u scripts in %s",
                 static_cast<unsigned>(scriptCount_), SCRIPTS_DIR);
    }

    void loadScript()
    {
        if (selectedScript_ >= scriptCount_)
        {
            scriptLoaded_ = false;
            return;
        }

        char path[80];
        std::snprintf(path, sizeof(path), "%s/%s",
                      SCRIPTS_DIR, scriptNameBufs_[selectedScript_]);

        size_t bytesRead = 0U;
        const bool ok = StorageManager::instance().readFile(
            path, scriptBuf_, SCRIPT_BUF_SIZE - 1U, &bytesRead);

        if (!ok || bytesRead == 0U)
        {
            ESP_LOGE(TAG_BBT, "Failed to read script: %s", path);
            scriptLoaded_ = false;
            return;
        }

        scriptBuf_[bytesRead] = '\0';
        parseScript(reinterpret_cast<const char *>(scriptBuf_), bytesRead);
        scriptLoaded_ = true;
        ESP_LOGI(TAG_BBT, "Loaded script: %s (%u bytes, %u lines)",
                 path, static_cast<unsigned>(bytesRead),
                 static_cast<unsigned>(execLineCount_));
    }

    // ── DuckyScript parser ──────────────────────────────────────────────

    void parseScript(const char *text, size_t len)
    {
        execLineCount_ = 0U;
        size_t pos = 0U;

        while (pos < len && execLineCount_ < MAX_LINES)
        {
            // Find end of current line
            size_t lineEnd = pos;
            while (lineEnd < len && text[lineEnd] != '\n' && text[lineEnd] != '\r')
            {
                ++lineEnd;
            }

            const size_t lineLen = lineEnd - pos;
            if (lineLen > 0U)
            {
                parseLine(&text[pos], lineLen);
            }

            // Skip line endings
            pos = lineEnd;
            while (pos < len && (text[pos] == '\n' || text[pos] == '\r'))
            {
                ++pos;
            }
        }
    }

    void parseLine(const char *line, size_t len)
    {
        if (len == 0U || execLineCount_ >= MAX_LINES)
        {
            return;
        }

        // Copy line to temporary buffer for tokenization
        char tmp[LINE_BUF_SIZE];
        const size_t copyLen = (len < LINE_BUF_SIZE - 1U) ? len : (LINE_BUF_SIZE - 1U);
        std::memcpy(tmp, line, copyLen);
        tmp[copyLen] = '\0';

        // Skip leading whitespace
        size_t start = 0U;
        while (start < copyLen && (tmp[start] == ' ' || tmp[start] == '\t'))
        {
            ++start;
        }
        if (start >= copyLen)
        {
            return; // Empty line
        }

        // Extract command token
        size_t tokenEnd = start;
        while (tokenEnd < copyLen && tmp[tokenEnd] != ' ' && tmp[tokenEnd] != '\t')
        {
            ++tokenEnd;
        }

        // Null-terminate the token
        const char savedChar = tmp[tokenEnd];
        tmp[tokenEnd] = '\0';

        DuckyLine &dl = parsedLines_[execLineCount_];
        dl.cmd = parseCommand(&tmp[start]);
        dl.arg[0] = '\0';

        if (dl.cmd == DuckyCmd::CMD_NONE)
        {
            return; // Unknown command – skip
        }

        // Restore and extract argument
        tmp[tokenEnd] = savedChar;
        size_t argStart = tokenEnd;
        while (argStart < copyLen && (tmp[argStart] == ' ' || tmp[argStart] == '\t'))
        {
            ++argStart;
        }

        if (argStart < copyLen)
        {
            const size_t argLen = copyLen - argStart;
            const size_t argCopy = (argLen < LINE_BUF_SIZE - 1U) ? argLen : (LINE_BUF_SIZE - 1U);
            std::memcpy(dl.arg, &tmp[argStart], argCopy);
            dl.arg[argCopy] = '\0';
        }

        ++execLineCount_;
    }

    // ── BLE HID stack init / deinit ─────────────────────────────────────

    void initBle()
    {
        if (bleInitialized_)
        {
            return;
        }

        // Release classic BT memory – we only use BLE
        esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);

        esp_bt_controller_config_t btCfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        esp_err_t err = esp_bt_controller_init(&btCfg);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG_BBT, "BT controller init failed: %d", err);
            return;
        }
        err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG_BBT, "BT controller enable failed: %d", err);
            esp_bt_controller_deinit();
            return;
        }
        err = esp_bluedroid_init();
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG_BBT, "Bluedroid init failed: %d", err);
            esp_bt_controller_disable();
            esp_bt_controller_deinit();
            return;
        }
        err = esp_bluedroid_enable();
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG_BBT, "Bluedroid enable failed: %d", err);
            esp_bluedroid_deinit();
            esp_bt_controller_disable();
            esp_bt_controller_deinit();
            return;
        }

        // Register GAP and GATTS callbacks
        esp_ble_gap_register_callback(gapEventHandler);
        esp_ble_gatts_register_callback(gattsEventHandler);

        // Set device name to the selected stealth name
        esp_ble_gap_set_device_name(DEVICE_NAMES[selectedNameIdx_]);

        // Configure security for pairing
        esp_ble_auth_req_t authReq = ESP_LE_AUTH_BOND;
        esp_ble_io_cap_t   ioCap   = ESP_IO_CAP_NONE;
        uint8_t keySize = 16U;
        uint8_t initKey = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
        uint8_t rspKey  = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;

        esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE,
                                       &authReq, sizeof(authReq));
        esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE,
                                       &ioCap, sizeof(ioCap));
        esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE,
                                       &keySize, sizeof(keySize));
        esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY,
                                       &initKey, sizeof(initKey));
        esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY,
                                       &rspKey, sizeof(rspKey));

        // Register GATT application – triggers ESP_GATTS_REG_EVT
        esp_ble_gatts_app_register(0U);

        // Configure advertising parameters (connectable)
        s_advParams.adv_int_min     = 0x20U;
        s_advParams.adv_int_max     = 0x40U;
        s_advParams.adv_type        = ADV_TYPE_IND; // Connectable undirected
        s_advParams.own_addr_type   = BLE_ADDR_TYPE_PUBLIC;
        s_advParams.channel_map     = ADV_CHNL_ALL;
        s_advParams.adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY;

        bleInitialized_ = true;
        ESP_LOGI(TAG_BBT, "BLE HID stack initialized");
    }

    void deinitBle()
    {
        if (!bleInitialized_)
        {
            return;
        }

        esp_ble_gap_stop_advertising();
        esp_bluedroid_disable();
        esp_bluedroid_deinit();
        esp_bt_controller_disable();
        esp_bt_controller_deinit();
        bleInitialized_ = false;
        s_connected = false;
        ESP_LOGI(TAG_BBT, "BLE HID stack deinitialized");
    }

    // ── Execution control ───────────────────────────────────────────────

    void startExecution()
    {
        if (!scriptLoaded_)
        {
            ESP_LOGW(TAG_BBT, "No script loaded");
            return;
        }

        execLineIdx_   = 0U;
        execCharIdx_   = 0U;
        execDelayUntilMs_ = 0U;
        lastKeySendMs_ = 0U;

        // Reset Duck++ state
        repeatCount_     = 0U;
        waitingForButton_ = false;
        ifSkipDepth_     = 0U;
        varCount_        = 0U;
        hasChainScript_  = false;
        std::memset(chainScriptName_, 0, sizeof(chainScriptName_));

        initBle();
        startAdvertising();
        transitionTo(BadBtState::WAITING_PAIR);
        ESP_LOGI(TAG_BBT, "Advertising – waiting for pairing");
    }

    void stopExecution()
    {
        if (stealthActive_)
        {
            disableStealth();
        }
        deinitBle();
    }

    void startAdvertising()
    {
        // Build advertising data with HID appearance
        esp_ble_adv_data_t advData = {};
        advData.set_scan_rsp      = false;
        advData.include_name      = true;
        advData.include_txpower   = false;
        advData.min_interval      = 0x0006U;
        advData.max_interval      = 0x0010U;
        advData.appearance        = 0x03C1U; // HID Keyboard appearance
        advData.flag              = (ESP_BLE_ADV_FLAG_GEN_DISC |
                                     ESP_BLE_ADV_FLAG_BREDR_NOT_SPT);

        esp_ble_gap_config_adv_data(&advData);
        // Advertising starts from the GAP callback after data is set
    }

    // ── Stealth mode ────────────────────────────────────────────────────

    void enableStealth()
    {
        stealthActive_ = true;
        DisplayManager::instance().clear();
        DisplayManager::instance().present();
        transitionTo(BadBtState::STEALTH);
        ESP_LOGI(TAG_BBT, "Stealth mode ON – display off");
    }

    void disableStealth()
    {
        stealthActive_ = false;
        transitionTo(BadBtState::EXECUTING);
        needsRedraw_ = true;
        ESP_LOGI(TAG_BBT, "Stealth mode OFF – display on");
    }

    // ── HID report sending ──────────────────────────────────────────────

    /// Send an 8-byte HID keyboard report: [modifier, reserved, key1..key6]
    void sendKeyReport(uint8_t modifier, uint8_t keycode)
    {
        if (!s_connected || s_gattsIf == ESP_GATT_IF_NONE)
        {
            return;
        }

        uint8_t report[8] = {modifier, 0x00U, keycode, 0, 0, 0, 0, 0};

        esp_ble_gatts_send_indicate(
            s_gattsIf, s_connId,
            s_handles[IDX_REPORT_VAL],
            sizeof(report), report, false);
    }

    /// Send key-down followed by key-up (release).
    void sendKeyPress(uint8_t modifier, uint8_t keycode)
    {
        sendKeyReport(modifier, keycode);
        // Small delay then release
        sendKeyReport(MOD_NONE, KEY_NONE);
    }

    // ── Duck++ variable management ──────────────────────────────────────

    /// Set a user variable.  Overwrites if the name already exists.
    void setVar(const char *name, const char *value)
    {
        if (name == nullptr || name[0] == '\0')
        {
            return;
        }

        // Check for existing variable to overwrite
        for (size_t i = 0U; i < varCount_; ++i)
        {
            if (std::strcmp(varNames_[i], name) == 0)
            {
                std::strncpy(varValues_[i], value, VAR_VALUE_LEN - 1U);
                varValues_[i][VAR_VALUE_LEN - 1U] = '\0';
                return;
            }
        }

        // Add new variable if there's room
        if (varCount_ < MAX_VARS)
        {
            std::strncpy(varNames_[varCount_], name, VAR_NAME_LEN - 1U);
            varNames_[varCount_][VAR_NAME_LEN - 1U] = '\0';
            std::strncpy(varValues_[varCount_], value, VAR_VALUE_LEN - 1U);
            varValues_[varCount_][VAR_VALUE_LEN - 1U] = '\0';
            ++varCount_;
            ESP_LOGD(TAG_BBT, "VAR %s = %s", name, value);
        }
        else
        {
            ESP_LOGW(TAG_BBT, "Variable store full (max %u)", static_cast<unsigned>(MAX_VARS));
        }
    }

    /// Look up a variable by name.  Returns the value or nullptr.
    const char *getVar(const char *name) const
    {
        if (name == nullptr)
        {
            return nullptr;
        }

        // Built-in system variables
        if (std::strcmp(name, "DEVICE_NAME") == 0)
        {
            return DEVICE_NAMES[selectedNameIdx_];
        }
        if (std::strcmp(name, "LAST_NFC_UID") == 0)
        {
            // Placeholder – populated from NFC subsystem captures
            return "(no_uid)";
        }
        if (std::strcmp(name, "WIFI_SSID") == 0)
        {
            // Placeholder – populated from WiFi subsystem
            return "(no_ssid)";
        }

        // User-defined variables
        for (size_t i = 0U; i < varCount_; ++i)
        {
            if (std::strcmp(varNames_[i], name) == 0)
            {
                return varValues_[i];
            }
        }
        return nullptr;
    }

    /// Expand $VAR_NAME tokens in a string.  Returns pointer to expandedBuf_.
    const char *expandVars(const char *input)
    {
        if (input == nullptr)
        {
            expandedBuf_[0] = '\0';
            return expandedBuf_;
        }

        size_t outIdx = 0U;
        size_t inIdx  = 0U;
        const size_t inLen = std::strlen(input);

        while (inIdx < inLen && outIdx < EXPANDED_BUF_SIZE - 1U)
        {
            if (input[inIdx] == '$')
            {
                // Extract variable name (alphanumeric + underscore)
                size_t nameStart = inIdx + 1U;
                size_t nameEnd   = nameStart;
                while (nameEnd < inLen &&
                       ((input[nameEnd] >= 'A' && input[nameEnd] <= 'Z') ||
                        (input[nameEnd] >= 'a' && input[nameEnd] <= 'z') ||
                        (input[nameEnd] >= '0' && input[nameEnd] <= '9') ||
                        input[nameEnd] == '_'))
                {
                    ++nameEnd;
                }

                if (nameEnd > nameStart)
                {
                    char varName[VAR_NAME_LEN];
                    const size_t nameLen = nameEnd - nameStart;
                    const size_t copyLen = (nameLen < VAR_NAME_LEN - 1U)
                                               ? nameLen : (VAR_NAME_LEN - 1U);
                    std::memcpy(varName, &input[nameStart], copyLen);
                    varName[copyLen] = '\0';

                    const char *val = getVar(varName);
                    if (val != nullptr)
                    {
                        const size_t valLen = std::strlen(val);
                        const size_t space  = EXPANDED_BUF_SIZE - 1U - outIdx;
                        const size_t toCopy = (valLen < space) ? valLen : space;
                        std::memcpy(&expandedBuf_[outIdx], val, toCopy);
                        outIdx += toCopy;
                    }
                    else
                    {
                        // Unknown variable – keep the literal $NAME
                        const size_t tokenLen = nameEnd - inIdx;
                        const size_t space    = EXPANDED_BUF_SIZE - 1U - outIdx;
                        const size_t toCopy   = (tokenLen < space) ? tokenLen : space;
                        std::memcpy(&expandedBuf_[outIdx], &input[inIdx], toCopy);
                        outIdx += toCopy;
                    }
                    inIdx = nameEnd;
                }
                else
                {
                    // Bare '$' with no valid name following
                    expandedBuf_[outIdx++] = '$';
                    ++inIdx;
                }
            }
            else
            {
                expandedBuf_[outIdx++] = input[inIdx++];
            }
        }

        expandedBuf_[outIdx] = '\0';
        return expandedBuf_;
    }

    /// Load a chained script by filename (from SCRIPTS_DIR).
    bool loadChainScript(const char *filename)
    {
        if (filename == nullptr || filename[0] == '\0')
        {
            return false;
        }

        char path[80];
        std::snprintf(path, sizeof(path), "%s/%s", SCRIPTS_DIR, filename);

        size_t bytesRead = 0U;
        const bool ok = StorageManager::instance().readFile(
            path, scriptBuf_, SCRIPT_BUF_SIZE - 1U, &bytesRead);

        if (!ok || bytesRead == 0U)
        {
            ESP_LOGE(TAG_BBT, "Failed to read chained script: %s", path);
            return false;
        }

        scriptBuf_[bytesRead] = '\0';
        parseScript(reinterpret_cast<const char *>(scriptBuf_), bytesRead);
        execLineIdx_  = 0U;
        execCharIdx_  = 0U;
        execDelayUntilMs_ = 0U;
        repeatCount_  = 0U;
        waitingForButton_ = false;
        ifSkipDepth_  = 0U;
        // Preserve variables across chain; reset chain target
        hasChainScript_ = false;
        std::memset(chainScriptName_, 0, sizeof(chainScriptName_));

        ESP_LOGI(TAG_BBT, "Chained script loaded: %s (%u bytes, %u lines)",
                 path, static_cast<unsigned>(bytesRead),
                 static_cast<unsigned>(execLineCount_));
        return true;
    }

    // ── Script execution tick ───────────────────────────────────────────

    void performExecTick()
    {
        if (!scriptLoaded_ || execLineIdx_ >= execLineCount_)
        {
            if (execLineIdx_ >= execLineCount_ && scriptLoaded_)
            {
                // Duck++: Multi-payload chaining
                if (hasChainScript_)
                {
                    ESP_LOGI(TAG_BBT, "Chaining to next script: %s", chainScriptName_);
                    if (loadChainScript(chainScriptName_))
                    {
                        needsRedraw_ = true;
                        return; // Continue execution with the chained script
                    }
                    ESP_LOGW(TAG_BBT, "Chain script failed – finishing");
                }

                transitionTo(BadBtState::DONE);
                ESP_LOGI(TAG_BBT, "Script execution complete");
                EventSystem::instance().postEvent(
                    {EventType::EVT_XP_EARNED, XP_BADBT_RUN, 0, nullptr});
            }
            return;
        }

        // Duck++: WAIT_FOR_BUTTON pauses execution
        if (waitingForButton_)
        {
            return;
        }

        const uint32_t nowMs = static_cast<uint32_t>(
            xTaskGetTickCount() * portTICK_PERIOD_MS);

        // Handle pending delay
        if (execDelayUntilMs_ > 0U && nowMs < execDelayUntilMs_)
        {
            return; // Still waiting
        }
        execDelayUntilMs_ = 0U;

        // Rate-limit key sends
        if ((nowMs - lastKeySendMs_) < KEY_SEND_INTERVAL_MS)
        {
            return;
        }

        const DuckyLine &dl = parsedLines_[execLineIdx_];

        // ── Duck++: IF_CONNECTED skip logic ─────────────────────────────
        if (ifSkipDepth_ > 0U)
        {
            // We are inside a skipped IF_CONNECTED block
            if (dl.cmd == DuckyCmd::CMD_IF_CONNECTED)
            {
                ++ifSkipDepth_; // Nested IF – increase depth
            }
            else if (dl.cmd == DuckyCmd::CMD_END_IF)
            {
                --ifSkipDepth_;
            }
            advanceLine();
            return;
        }

        switch (dl.cmd)
        {
        case DuckyCmd::CMD_REM:
            // Comment – skip
            advanceLine();
            break;

        case DuckyCmd::CMD_DELAY:
        {
            uint32_t delayMs = DEFAULT_DELAY_MS;
            if (dl.arg[0] != '\0')
            {
                char *endPtr = nullptr;
                const long v = std::strtol(dl.arg, &endPtr, 10);
                if (endPtr != dl.arg && v > 0)
                {
                    delayMs = static_cast<uint32_t>(v);
                }
            }
            execDelayUntilMs_ = nowMs + delayMs;
            advanceLine();
            break;
        }

        case DuckyCmd::CMD_STRING:
        {
            // Duck++: Expand variables in the string argument
            const char *text = expandVars(dl.arg);
            const size_t textLen = std::strlen(text);

            // Type one character per tick
            if (execCharIdx_ < textLen)
            {
                const CharKeyMapping m = charToHid(text[execCharIdx_]);
                sendKeyPress(m.modifier, m.keycode);
                lastKeySendMs_ = nowMs;
                ++execCharIdx_;
            }
            else
            {
                advanceLine();
            }
            break;
        }

        case DuckyCmd::CMD_ENTER:
            sendKeyPress(MOD_NONE, KEY_ENTER);
            lastKeySendMs_ = nowMs;
            advanceLine();
            break;

        case DuckyCmd::CMD_TAB:
            sendKeyPress(MOD_NONE, KEY_TAB);
            lastKeySendMs_ = nowMs;
            advanceLine();
            break;

        case DuckyCmd::CMD_ESC:
            sendKeyPress(MOD_NONE, KEY_ESC);
            lastKeySendMs_ = nowMs;
            advanceLine();
            break;

        case DuckyCmd::CMD_GUI:
        {
            const uint8_t key = singleKeyToHid(dl.arg);
            sendKeyPress(MOD_LGUI, key);
            lastKeySendMs_ = nowMs;
            advanceLine();
            break;
        }

        case DuckyCmd::CMD_CTRL:
        {
            const uint8_t key = singleKeyToHid(dl.arg);
            sendKeyPress(MOD_LCTRL, key);
            lastKeySendMs_ = nowMs;
            advanceLine();
            break;
        }

        case DuckyCmd::CMD_ALT:
        {
            const uint8_t key = singleKeyToHid(dl.arg);
            sendKeyPress(MOD_LALT, key);
            lastKeySendMs_ = nowMs;
            advanceLine();
            break;
        }

        case DuckyCmd::CMD_SHIFT:
        {
            const uint8_t key = singleKeyToHid(dl.arg);
            sendKeyPress(MOD_LSHIFT, key);
            lastKeySendMs_ = nowMs;
            advanceLine();
            break;
        }

        case DuckyCmd::CMD_CTRL_ALT:
        {
            const uint8_t key = singleKeyToHid(dl.arg);
            sendKeyPress(MOD_LCTRL | MOD_LALT, key);
            lastKeySendMs_ = nowMs;
            advanceLine();
            break;
        }

        case DuckyCmd::CMD_CTRL_SHIFT:
        {
            const uint8_t key = singleKeyToHid(dl.arg);
            sendKeyPress(MOD_LCTRL | MOD_LSHIFT, key);
            lastKeySendMs_ = nowMs;
            advanceLine();
            break;
        }

        case DuckyCmd::CMD_ALT_SHIFT:
        {
            const uint8_t key = singleKeyToHid(dl.arg);
            sendKeyPress(MOD_LALT | MOD_LSHIFT, key);
            lastKeySendMs_ = nowMs;
            advanceLine();
            break;
        }

        case DuckyCmd::CMD_UP:
            sendKeyPress(MOD_NONE, KEY_UP_ARR);
            lastKeySendMs_ = nowMs;
            advanceLine();
            break;

        case DuckyCmd::CMD_DOWN:
            sendKeyPress(MOD_NONE, KEY_DOWN_ARR);
            lastKeySendMs_ = nowMs;
            advanceLine();
            break;

        case DuckyCmd::CMD_LEFT:
            sendKeyPress(MOD_NONE, KEY_LEFT_ARR);
            lastKeySendMs_ = nowMs;
            advanceLine();
            break;

        case DuckyCmd::CMD_RIGHT:
            sendKeyPress(MOD_NONE, KEY_RIGHT_ARR);
            lastKeySendMs_ = nowMs;
            advanceLine();
            break;

        case DuckyCmd::CMD_DELETE:
            sendKeyPress(MOD_NONE, KEY_DELETE);
            lastKeySendMs_ = nowMs;
            advanceLine();
            break;

        case DuckyCmd::CMD_BACKSPACE:
            sendKeyPress(MOD_NONE, KEY_BACKSPACE);
            lastKeySendMs_ = nowMs;
            advanceLine();
            break;

        case DuckyCmd::CMD_CAPSLOCK:
            sendKeyPress(MOD_NONE, KEY_CAPSLOCK);
            lastKeySendMs_ = nowMs;
            advanceLine();
            break;

        case DuckyCmd::CMD_F1:  case DuckyCmd::CMD_F2:  case DuckyCmd::CMD_F3:
        case DuckyCmd::CMD_F4:  case DuckyCmd::CMD_F5:  case DuckyCmd::CMD_F6:
        case DuckyCmd::CMD_F7:  case DuckyCmd::CMD_F8:  case DuckyCmd::CMD_F9:
        case DuckyCmd::CMD_F10: case DuckyCmd::CMD_F11: case DuckyCmd::CMD_F12:
        {
            const uint8_t fKey = static_cast<uint8_t>(
                KEY_F1 + (static_cast<uint8_t>(dl.cmd) -
                          static_cast<uint8_t>(DuckyCmd::CMD_F1)));
            sendKeyPress(MOD_NONE, fKey);
            lastKeySendMs_ = nowMs;
            advanceLine();
            break;
        }

        // ── Duck++ extended commands ────────────────────────────────

        case DuckyCmd::CMD_REPEAT:
        {
            if (repeatCount_ == 0U)
            {
                // First encounter – parse the repeat count
                uint32_t count = 1U;
                if (dl.arg[0] != '\0')
                {
                    char *endPtr = nullptr;
                    const long v = std::strtol(dl.arg, &endPtr, 10);
                    if (endPtr != dl.arg && v > 0)
                    {
                        count = static_cast<uint32_t>(v);
                    }
                }
                repeatCount_ = count;
            }

            if (repeatCount_ > 0U && execLineIdx_ > 0U)
            {
                --repeatCount_;
                // Jump back to the previous line to re-execute it
                execLineIdx_ -= 1U;
                execCharIdx_ = 0U;
            }
            else
            {
                repeatCount_ = 0U;
                advanceLine();
            }
            break;
        }

        case DuckyCmd::CMD_WAIT_FOR_BUTTON:
            waitingForButton_ = true;
            ESP_LOGI(TAG_BBT, "WAIT_FOR_BUTTON – paused");
            needsRedraw_ = true;
            break;

        case DuckyCmd::CMD_IF_CONNECTED:
            if (!s_connected)
            {
                // Not connected – skip until matching END_IF
                ifSkipDepth_ = 1U;
                ESP_LOGD(TAG_BBT, "IF_CONNECTED: false – skipping block");
            }
            advanceLine();
            break;

        case DuckyCmd::CMD_END_IF:
            // If we reach here, we were inside an active IF block – just continue
            advanceLine();
            break;

        case DuckyCmd::CMD_VAR:
        {
            // Parse "VAR name value" – arg contains "name value"
            char nameBuf[VAR_NAME_LEN];
            nameBuf[0] = '\0';
            const char *argPtr = dl.arg;

            // Extract variable name
            size_t ni = 0U;
            while (*argPtr != '\0' && *argPtr != ' ' && *argPtr != '\t' &&
                   ni < VAR_NAME_LEN - 1U)
            {
                nameBuf[ni++] = *argPtr++;
            }
            nameBuf[ni] = '\0';

            // Skip whitespace
            while (*argPtr == ' ' || *argPtr == '\t')
            {
                ++argPtr;
            }

            setVar(nameBuf, argPtr);
            advanceLine();
            break;
        }

        case DuckyCmd::CMD_CHAIN:
        {
            // Queue the next script filename for execution after current
            const char *fname = dl.arg;
            if (fname[0] != '\0')
            {
                std::strncpy(chainScriptName_, fname, SCRIPT_NAME_LEN - 1U);
                chainScriptName_[SCRIPT_NAME_LEN - 1U] = '\0';
                hasChainScript_ = true;
                ESP_LOGI(TAG_BBT, "CHAIN queued: %s", chainScriptName_);
            }
            advanceLine();
            break;
        }

        default:
            advanceLine();
            break;
        }

        needsRedraw_ = true;
    }

    void advanceLine()
    {
        ++execLineIdx_;
        execCharIdx_ = 0U;
    }

    // ── GATTS service creation ──────────────────────────────────────────

    void createHidService(esp_gatt_if_t gattsIf)
    {
        s_gattsIf = gattsIf;

        // Create the attribute table for the HID service
        // We use manual attr creation for maximum control
        esp_gatts_attr_db_t attrTab[HID_ATTR_COUNT];
        std::memset(attrTab, 0, sizeof(attrTab));

        // ── Attribute 0: HID Service declaration ────────────
        static const uint16_t primaryServiceUuid = ESP_GATT_UUID_PRI_SERVICE;
        static const uint16_t hidSvcUuid = HID_SERVICE_UUID;
        attrTab[0].attr_control.auto_rsp = ESP_GATT_AUTO_RSP;
        attrTab[0].att_desc.uuid_length  = sizeof(uint16_t);
        attrTab[0].att_desc.uuid_p       = (uint8_t *)&primaryServiceUuid;
        attrTab[0].att_desc.perm         = ESP_GATT_PERM_READ;
        attrTab[0].att_desc.max_length   = sizeof(uint16_t);
        attrTab[0].att_desc.length       = sizeof(uint16_t);
        attrTab[0].att_desc.value        = (uint8_t *)&hidSvcUuid;

        // ── Attribute 1: HID Report Map char declaration ────
        static const uint16_t charDeclUuid = ESP_GATT_UUID_CHAR_DECLARE;
        static const uint8_t charPropRead = ESP_GATT_CHAR_PROP_BIT_READ;
        attrTab[1].attr_control.auto_rsp = ESP_GATT_AUTO_RSP;
        attrTab[1].att_desc.uuid_length  = sizeof(uint16_t);
        attrTab[1].att_desc.uuid_p       = (uint8_t *)&charDeclUuid;
        attrTab[1].att_desc.perm         = ESP_GATT_PERM_READ;
        attrTab[1].att_desc.max_length   = sizeof(uint8_t);
        attrTab[1].att_desc.length       = sizeof(uint8_t);
        attrTab[1].att_desc.value        = (uint8_t *)&charPropRead;

        // ── Attribute 2: HID Report Map char value ──────────
        static const uint16_t reportMapUuid = HID_REPORT_MAP_CHAR_UUID;
        attrTab[2].attr_control.auto_rsp = ESP_GATT_AUTO_RSP;
        attrTab[2].att_desc.uuid_length  = sizeof(uint16_t);
        attrTab[2].att_desc.uuid_p       = (uint8_t *)&reportMapUuid;
        attrTab[2].att_desc.perm         = ESP_GATT_PERM_READ;
        attrTab[2].att_desc.max_length   = sizeof(HID_REPORT_MAP);
        attrTab[2].att_desc.length       = sizeof(HID_REPORT_MAP);
        attrTab[2].att_desc.value        = (uint8_t *)HID_REPORT_MAP;

        // ── Attribute 3: HID Input Report char declaration ──
        static const uint8_t charPropReadNotify = ESP_GATT_CHAR_PROP_BIT_READ |
                                                   ESP_GATT_CHAR_PROP_BIT_NOTIFY;
        attrTab[3].attr_control.auto_rsp = ESP_GATT_AUTO_RSP;
        attrTab[3].att_desc.uuid_length  = sizeof(uint16_t);
        attrTab[3].att_desc.uuid_p       = (uint8_t *)&charDeclUuid;
        attrTab[3].att_desc.perm         = ESP_GATT_PERM_READ;
        attrTab[3].att_desc.max_length   = sizeof(uint8_t);
        attrTab[3].att_desc.length       = sizeof(uint8_t);
        attrTab[3].att_desc.value        = (uint8_t *)&charPropReadNotify;

        // ── Attribute 4: HID Input Report value ─────────────
        static const uint16_t reportCharUuid = HID_REPORT_CHAR_UUID;
        static uint8_t reportValue[8] = {};
        attrTab[4].attr_control.auto_rsp = ESP_GATT_AUTO_RSP;
        attrTab[4].att_desc.uuid_length  = sizeof(uint16_t);
        attrTab[4].att_desc.uuid_p       = (uint8_t *)&reportCharUuid;
        attrTab[4].att_desc.perm         = ESP_GATT_PERM_READ;
        attrTab[4].att_desc.max_length   = sizeof(reportValue);
        attrTab[4].att_desc.length       = sizeof(reportValue);
        attrTab[4].att_desc.value        = reportValue;

        // ── Attribute 5: CCC descriptor for Input Report ────
        static const uint16_t cccUuid = CCC_DESCRIPTOR_UUID;
        attrTab[5].attr_control.auto_rsp = ESP_GATT_AUTO_RSP;
        attrTab[5].att_desc.uuid_length  = sizeof(uint16_t);
        attrTab[5].att_desc.uuid_p       = (uint8_t *)&cccUuid;
        attrTab[5].att_desc.perm         = ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE;
        attrTab[5].att_desc.max_length   = sizeof(uint16_t);
        attrTab[5].att_desc.length       = sizeof(uint16_t);
        attrTab[5].att_desc.value        = (uint8_t *)&s_reportCccValue;

        // ── Attribute 6: Report Reference descriptor ────────
        static const uint16_t reportRefUuid = REPORT_REF_DESCRIPTOR_UUID;
        static const uint8_t reportRef[2] = {0x01U, 0x01U}; // Report ID 1, Input
        attrTab[6].attr_control.auto_rsp = ESP_GATT_AUTO_RSP;
        attrTab[6].att_desc.uuid_length  = sizeof(uint16_t);
        attrTab[6].att_desc.uuid_p       = (uint8_t *)&reportRefUuid;
        attrTab[6].att_desc.perm         = ESP_GATT_PERM_READ;
        attrTab[6].att_desc.max_length   = sizeof(reportRef);
        attrTab[6].att_desc.length       = sizeof(reportRef);
        attrTab[6].att_desc.value        = (uint8_t *)reportRef;

        // ── Attribute 7: HID Info char declaration ──────────
        attrTab[7].attr_control.auto_rsp = ESP_GATT_AUTO_RSP;
        attrTab[7].att_desc.uuid_length  = sizeof(uint16_t);
        attrTab[7].att_desc.uuid_p       = (uint8_t *)&charDeclUuid;
        attrTab[7].att_desc.perm         = ESP_GATT_PERM_READ;
        attrTab[7].att_desc.max_length   = sizeof(uint8_t);
        attrTab[7].att_desc.length       = sizeof(uint8_t);
        attrTab[7].att_desc.value        = (uint8_t *)&charPropRead;

        // ── Attribute 8: HID Info char value ────────────────
        static const uint16_t hidInfoUuid = HID_INFO_CHAR_UUID;
        attrTab[8].attr_control.auto_rsp = ESP_GATT_AUTO_RSP;
        attrTab[8].att_desc.uuid_length  = sizeof(uint16_t);
        attrTab[8].att_desc.uuid_p       = (uint8_t *)&hidInfoUuid;
        attrTab[8].att_desc.perm         = ESP_GATT_PERM_READ;
        attrTab[8].att_desc.max_length   = sizeof(HID_INFO_VALUE);
        attrTab[8].att_desc.length       = sizeof(HID_INFO_VALUE);
        attrTab[8].att_desc.value        = (uint8_t *)HID_INFO_VALUE;

        // ── Attribute 9: HID Control Point char declaration ─
        static const uint8_t charPropWriteNR = ESP_GATT_CHAR_PROP_BIT_WRITE_NR;
        attrTab[9].attr_control.auto_rsp = ESP_GATT_AUTO_RSP;
        attrTab[9].att_desc.uuid_length  = sizeof(uint16_t);
        attrTab[9].att_desc.uuid_p       = (uint8_t *)&charDeclUuid;
        attrTab[9].att_desc.perm         = ESP_GATT_PERM_READ;
        attrTab[9].att_desc.max_length   = sizeof(uint8_t);
        attrTab[9].att_desc.length       = sizeof(uint8_t);
        attrTab[9].att_desc.value        = (uint8_t *)&charPropWriteNR;

        // ── Attribute 10: HID Control Point char value ──────
        static const uint16_t ctrlPtUuid = HID_CTRL_PT_CHAR_UUID;
        attrTab[10].attr_control.auto_rsp = ESP_GATT_AUTO_RSP;
        attrTab[10].att_desc.uuid_length  = sizeof(uint16_t);
        attrTab[10].att_desc.uuid_p       = (uint8_t *)&ctrlPtUuid;
        attrTab[10].att_desc.perm         = ESP_GATT_PERM_WRITE;
        attrTab[10].att_desc.max_length   = sizeof(s_hidCtrlPt);
        attrTab[10].att_desc.length       = sizeof(s_hidCtrlPt);
        attrTab[10].att_desc.value        = &s_hidCtrlPt;

        // ── Attribute 11: Battery Service declaration ───────
        static const uint16_t batSvcUuid = BATTERY_SERVICE_UUID;
        attrTab[11].attr_control.auto_rsp = ESP_GATT_AUTO_RSP;
        attrTab[11].att_desc.uuid_length  = sizeof(uint16_t);
        attrTab[11].att_desc.uuid_p       = (uint8_t *)&primaryServiceUuid;
        attrTab[11].att_desc.perm         = ESP_GATT_PERM_READ;
        attrTab[11].att_desc.max_length   = sizeof(uint16_t);
        attrTab[11].att_desc.length       = sizeof(uint16_t);
        attrTab[11].att_desc.value        = (uint8_t *)&batSvcUuid;

        // ── Attribute 12: Battery Level char declaration ────
        attrTab[12].attr_control.auto_rsp = ESP_GATT_AUTO_RSP;
        attrTab[12].att_desc.uuid_length  = sizeof(uint16_t);
        attrTab[12].att_desc.uuid_p       = (uint8_t *)&charDeclUuid;
        attrTab[12].att_desc.perm         = ESP_GATT_PERM_READ;
        attrTab[12].att_desc.max_length   = sizeof(uint8_t);
        attrTab[12].att_desc.length       = sizeof(uint8_t);
        attrTab[12].att_desc.value        = (uint8_t *)&charPropRead;

        // ── Attribute 13: Battery Level char value ──────────
        static const uint16_t batLvlUuid = BATTERY_LEVEL_CHAR_UUID;
        static uint8_t batLevel = 100U;
        attrTab[13].attr_control.auto_rsp = ESP_GATT_AUTO_RSP;
        attrTab[13].att_desc.uuid_length  = sizeof(uint16_t);
        attrTab[13].att_desc.uuid_p       = (uint8_t *)&batLvlUuid;
        attrTab[13].att_desc.perm         = ESP_GATT_PERM_READ;
        attrTab[13].att_desc.max_length   = sizeof(batLevel);
        attrTab[13].att_desc.length       = sizeof(batLevel);
        attrTab[13].att_desc.value        = &batLevel;

        esp_ble_gatts_create_attr_tab(attrTab, gattsIf, HID_ATTR_COUNT, 0U);
    }

    // ── GATTS / GAP callback friend access ──────────────────────────────

    friend void gapEventHandler(esp_gap_ble_cb_event_t event,
                                esp_ble_gap_cb_param_t *param);
    friend void gattsEventHandler(esp_gatts_cb_event_t event,
                                  esp_gatt_if_t gatts_if,
                                  esp_ble_gatts_cb_param_t *param);
};

// ── GAP event handler (static, runs in BT task context) ──────────────────────

static void gapEventHandler(esp_gap_ble_cb_event_t event,
                            esp_ble_gap_cb_param_t *param)
{
    switch (event)
    {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        esp_ble_gap_start_advertising(&s_advParams);
        ESP_LOGD(TAG_BBT, "ADV data set – advertising started");
        break;

    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS)
        {
            ESP_LOGI(TAG_BBT, "Advertising started OK");
        }
        else
        {
            ESP_LOGW(TAG_BBT, "Advertising start failed: %d",
                     param->adv_start_cmpl.status);
        }
        break;

    case ESP_GAP_BLE_SEC_REQ_EVT:
        // Accept pairing request
        esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
        ESP_LOGI(TAG_BBT, "Security request – accepted");
        break;

    case ESP_GAP_BLE_AUTH_CMPL_EVT:
        if (param->ble_security.auth_cmpl.success)
        {
            ESP_LOGI(TAG_BBT, "Authentication complete (bonded)");
        }
        else
        {
            ESP_LOGW(TAG_BBT, "Authentication failed: %d",
                     param->ble_security.auth_cmpl.fail_reason);
        }
        break;

    default:
        break;
    }
}

// ── GATTS event handler (static, runs in BT task context) ────────────────────

static void gattsEventHandler(esp_gatts_cb_event_t event,
                              esp_gatt_if_t gatts_if,
                              esp_ble_gatts_cb_param_t *param)
{
    switch (event)
    {
    case ESP_GATTS_REG_EVT:
        if (param->reg.status == ESP_GATT_OK)
        {
            s_gattsIf = gatts_if;
            if (g_badBtInstance != nullptr)
            {
                g_badBtInstance->createHidService(gatts_if);
            }
            ESP_LOGI(TAG_BBT, "GATTS registered, creating HID service");
        }
        break;

    case ESP_GATTS_CREAT_ATTR_TAB_EVT:
        if (param->add_attr_tab.status == ESP_GATT_OK &&
            param->add_attr_tab.num_handle == HID_ATTR_COUNT)
        {
            std::memcpy(s_handles, param->add_attr_tab.handles,
                        sizeof(uint16_t) * HID_ATTR_COUNT);
            esp_ble_gatts_start_service(s_handles[0]);
            ESP_LOGI(TAG_BBT, "Attribute table created (%u handles)",
                     static_cast<unsigned>(HID_ATTR_COUNT));
        }
        else
        {
            ESP_LOGE(TAG_BBT, "Attr table creation failed: status=%d, handles=%d",
                     param->add_attr_tab.status,
                     param->add_attr_tab.num_handle);
        }
        break;

    case ESP_GATTS_START_EVT:
        ESP_LOGI(TAG_BBT, "GATT service started");
        break;

    case ESP_GATTS_CONNECT_EVT:
        if (g_badBtInstance != nullptr)
        {
            g_badBtInstance->setConnected(true, param->connect.conn_id);
        }
        // Request encryption without MITM (matches ESP_IO_CAP_NONE "Just Works")
        esp_ble_set_encryption(param->connect.remote_bda,
                               ESP_BLE_SEC_ENCRYPT_NO_MITM);
        ESP_LOGI(TAG_BBT, "Client connected (conn_id=%u)",
                 static_cast<unsigned>(param->connect.conn_id));
        break;

    case ESP_GATTS_DISCONNECT_EVT:
        if (g_badBtInstance != nullptr)
        {
            g_badBtInstance->setConnected(false, 0U);
        }
        // Re-start advertising for reconnection
        esp_ble_gap_start_advertising(&s_advParams);
        ESP_LOGI(TAG_BBT, "Client disconnected – re-advertising");
        break;

    case ESP_GATTS_WRITE_EVT:
        // Handle CCC writes (enable/disable notifications)
        if (param->write.handle == s_handles[IDX_REPORT_CCC])
        {
            if (param->write.len == 2U)
            {
                s_reportCccValue = static_cast<uint16_t>(
                    param->write.value[0] | (param->write.value[1] << 8U));
                ESP_LOGD(TAG_BBT, "Report CCC written: 0x%04X",
                         s_reportCccValue);
            }
        }
        break;

    default:
        break;
    }
}

} // namespace

// ── Factory ──────────────────────────────────────────────────────────────────

AppBase *createBadBtApp()
{
    return new (std::nothrow) BadBtApp();
}
