# HackOS Plugin Development Guide

## Table of Contents

1. [Overview](#overview)
2. [Plugin Architecture](#plugin-architecture)
3. [Getting Started](#getting-started)
4. [Plugin JSON Specification](#plugin-json-specification)
5. [Action Types Reference](#action-types-reference)
6. [Configuration Block](#configuration-block)
7. [Plugin Lifecycle](#plugin-lifecycle)
8. [Web Dashboard Integration](#web-dashboard-integration)
9. [Best Practices](#best-practices)
10. [Troubleshooting](#troubleshooting)
11. [Advanced Examples](#advanced-examples)
12. [Contributing to the Plugin Store](#contributing-to-the-plugin-store)

---

## Overview

HackOS plugins are **JSON-based app definitions** that extend HackOS without writing C++ code. Each plugin file defines a new app that appears in the HackOS launcher alongside built-in apps. Plugins can:

- Control GPIO pins (toggle, set high/low)
- Generate PWM tones on the buzzer
- Set and display RF frequencies
- Log messages to the serial console
- Execute sequences of hardware actions

### Why Plugins?

Unlike traditional firmware apps that require recompilation, plugins are:

- **Dynamic**: Load at boot or hot-reload from the SD card
- **Portable**: Share a single `.json` file to distribute your app
- **Safe**: Actions are sandboxed to known-safe operation types
- **Simple**: No C++ knowledge required — just edit JSON

### Architecture Diagram

```
┌──────────────────────────────────────────────────┐
│                   HackOS Firmware                 │
│  ┌─────────────┐  ┌──────────────┐               │
│  │ AppManager   │  │ PluginManager │              │
│  │ (32 slots)   │←─│ (16 plugin    │              │
│  │              │  │  slots)       │              │
│  └──────┬───────┘  └──────┬───────┘              │
│         │                 │                       │
│         │    ┌────────────┘                       │
│         ↓    ↓                                    │
│  ┌──────────────┐   ┌──────────────┐             │
│  │ Built-in Apps │   │ Plugin Apps   │            │
│  │ (17 apps)     │   │ (dynamic)     │            │
│  └──────────────┘   └──────┬───────┘             │
│                            │                      │
│                     ┌──────┘                      │
│                     ↓                             │
│              ┌─────────────┐                      │
│              │ SD Card      │                     │
│              │ /ext/plugins/ │                    │
│              │  *.json       │                    │
│              └──────────────┘                     │
└──────────────────────────────────────────────────┘
```

---

## Plugin Architecture

### How It Works

1. **Boot Scan**: On startup, `PluginManager` scans `/ext/plugins/` on the SD card for `.json` files
2. **Parsing**: Each valid JSON file is parsed into a `PluginInfo` structure
3. **Registration**: Each plugin is registered with `AppManager` using a factory function template
4. **Runtime**: When launched, a `PluginApp` renders the plugin's actions as a menu on the OLED display
5. **Execution**: Users navigate actions with the joystick and execute them with the button press

### Component Roles

| Component | Role |
|-----------|------|
| `PluginManager` | Scans SD, parses JSON, manages plugin lifecycle |
| `PluginApp` | Generic app that renders any plugin's actions |
| `AppManager` | Registers plugins alongside built-in apps |
| `PluginManagerApp` | On-device OLED UI for managing plugins |
| `RemoteDashboardApp` | Web UI for plugin store (upload/browse/manage) |

---

## Getting Started

### Step 1: Create a Plugin File

Create a file named `my_plugin.json`:

```json
{
  "name": "my_plugin",
  "label": "My First Plugin",
  "version": "1.0.0",
  "author": "Your Name",
  "description": "My first HackOS plugin",
  "category": "tools",
  "actions": [
    {
      "type": "gpio_toggle",
      "pin": 2,
      "label": "Toggle LED"
    }
  ]
}
```

### Step 2: Install the Plugin

**Option A: SD Card**
Copy `my_plugin.json` to `/ext/plugins/` on the SD card.

**Option B: Web Dashboard**
1. Connect to `HackOS-Dashboard` WiFi
2. Open `http://192.168.4.1`
3. Go to **Plugin Store** tab
4. Upload your file or use the built-in creator

### Step 3: Activate

Either reboot HackOS or use the Plugin Manager app to reload plugins.

### Step 4: Use

Your plugin appears in the launcher menu. Navigate to it and press the button to see your actions.

---

## Plugin JSON Specification

### Required Fields

| Field | Type | Description | Max Length |
|-------|------|-------------|-----------|
| `name` | string | Unique identifier (no spaces, lowercase) | 31 chars |

### Optional Fields

| Field | Type | Default | Description | Max Length |
|-------|------|---------|-------------|-----------|
| `label` | string | Same as `name` | Display name in menus | 47 chars |
| `version` | string | `"1.0.0"` | Semantic version | 15 chars |
| `author` | string | `"Unknown"` | Creator name | 31 chars |
| `description` | string | `"No description"` | Brief description | 95 chars |
| `category` | string | `"general"` | Category for grouping | 15 chars |
| `config` | object | `{}` | Hardware configuration | — |
| `actions` | array | `[]` | List of executable actions | Max 8 |

### Complete Example

```json
{
  "name": "signal_helper",
  "label": "Signal Helper",
  "version": "1.2.0",
  "author": "HackOS Community",
  "description": "RF signal testing and GPIO debugging toolkit",
  "category": "rf",
  "config": {
    "pin": 25,
    "frequency": 433920000,
    "protocol": "OOK"
  },
  "actions": [
    {
      "type": "gpio_toggle",
      "pin": 25,
      "label": "Toggle TX Pin"
    },
    {
      "type": "freq_set",
      "value": 433920000,
      "label": "Show 433.92 MHz"
    },
    {
      "type": "pwm_tone",
      "pin": 27,
      "freq": 1000,
      "duration": 500,
      "label": "Test Beep"
    },
    {
      "type": "log_msg",
      "label": "Print RF Status"
    }
  ]
}
```

---

## Action Types Reference

### `gpio_toggle`

Toggles a GPIO pin between HIGH and LOW states.

```json
{
  "type": "gpio_toggle",
  "pin": 2,
  "label": "Toggle Onboard LED"
}
```

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `pin` | integer | Yes | GPIO pin number (0-39) |
| `label` | string | Yes | Display text (max 31 chars) |

**Notes:**
- The pin is automatically configured as OUTPUT
- First toggle reads the current state, then inverts it

### `gpio_high`

Sets a GPIO pin to HIGH (3.3V).

```json
{
  "type": "gpio_high",
  "pin": 2,
  "label": "LED ON"
}
```

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `pin` | integer | Yes | GPIO pin number |
| `label` | string | Yes | Display text |

### `gpio_low`

Sets a GPIO pin to LOW (0V).

```json
{
  "type": "gpio_low",
  "pin": 2,
  "label": "LED OFF"
}
```

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `pin` | integer | Yes | GPIO pin number |
| `label` | string | Yes | Display text |

### `pwm_tone`

Plays a PWM tone on a pin using LEDC channel 1.

```json
{
  "type": "pwm_tone",
  "pin": 27,
  "freq": 440,
  "duration": 300,
  "label": "Play A4 Note"
}
```

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `pin` | integer | Yes | GPIO pin for PWM output |
| `freq` | integer | Yes | Frequency in Hz |
| `duration` | integer | No | Duration in ms (0 = continuous) |
| `label` | string | Yes | Display text |

**Notes:**
- Uses LEDC channel 1 (channel 0 is used by Signal Analyzer)
- If `duration` is set, the tone stops automatically after that time
- Common frequencies: A4=440Hz, C5=523Hz, E5=659Hz

### `freq_set`

Displays a frequency value (informational, no hardware action).

```json
{
  "type": "freq_set",
  "value": 433920000,
  "label": "Set 433.92 MHz"
}
```

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `value` | integer | Yes | Frequency in Hz |
| `label` | string | Yes | Display text |

### `delay_ms`

Pauses execution for a specified time.

```json
{
  "type": "delay_ms",
  "value": 1000,
  "label": "Wait 1 second"
}
```

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `value` | integer | Yes | Delay in milliseconds (max 5000) |
| `label` | string | Yes | Display text |

**Safety:** The delay is capped at 5000ms to prevent UI lockup.

### `log_msg`

Prints the action's label to the serial console via `ESP_LOGI`.

```json
{
  "type": "log_msg",
  "label": "Hello from my plugin!"
}
```

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `label` | string | Yes | Message to log |

---

## Configuration Block

The optional `config` block provides metadata about the plugin's hardware requirements:

```json
{
  "config": {
    "pin": 25,
    "frequency": 433920000,
    "protocol": "OOK"
  }
}
```

| Field | Type | Description |
|-------|------|-------------|
| `pin` | integer | Primary GPIO pin used |
| `frequency` | integer | Operating frequency in Hz |
| `protocol` | string | Communication protocol (e.g., OOK, FSK, GPIO, PWM) |

This information is displayed in the Plugin Manager's detail view and the Web Plugin Store.

---

## Plugin Lifecycle

```
┌─────────┐     ┌──────────┐     ┌────────────┐
│ SD Card  │────→│ Scan &   │────→│ Register   │
│ .json    │     │ Parse    │     │ with       │
│ files    │     │          │     │ AppManager │
└─────────┘     └──────────┘     └─────┬──────┘
                                       │
                                       ↓
                                ┌──────────────┐
                                │ Available in  │
                                │ Launcher Menu │
                                └──────┬───────┘
                                       │ (user selects)
                                       ↓
                                ┌──────────────┐
                                │ PluginApp    │
                                │ renders      │
                                │ actions      │
                                └──────┬───────┘
                                       │ (user presses button)
                                       ↓
                                ┌──────────────┐
                                │ Execute      │
                                │ GPIO/PWM/etc │
                                └──────────────┘
```

### Boot Sequence

1. `StorageInit` creates `/ext/plugins/` if it doesn't exist
2. `PluginManager::scanAndLoad()` reads all `.json` files
3. `PluginManager::registerAll()` registers each plugin with `AppManager`
4. Plugins appear in the launcher alongside built-in apps

### Hot Reload

Plugins can be reloaded without rebooting:
- Via the **Plugin Manager** app: select "Reload Plugins"
- Via the **Web Dashboard**: POST to `/api/plugins/reload`
- New plugins are detected and registered automatically
- Already-loaded plugins are not duplicated

---

## Web Dashboard Integration

### Plugin Store Tab

The Web Dashboard includes a **Plugin Store** tab with:

1. **Plugin Grid**: Visual cards showing all installed plugins with:
   - Name, version, author
   - Enable/disable status badge
   - Description
   - Action count
   - Enable/Disable and Delete buttons

2. **Upload Zone**: Drag-and-drop or click-to-browse file upload for `.json` plugins

3. **Plugin Creator**: Built-in form to create new plugins:
   - Name, label, author, description fields
   - JSON editor for actions and config
   - One-click creation and installation

### REST API

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/plugins` | GET | List all loaded plugins |
| `/api/plugins/upload` | POST | Upload a new plugin file |
| `/api/plugins/toggle` | POST | Enable/disable a plugin |
| `/api/plugins/delete` | POST | Delete a plugin |
| `/api/plugins/reload` | POST | Reload plugins from SD |

#### GET /api/plugins

Response:
```json
{
  "plugins": [
    {
      "name": "led_blinker",
      "label": "LED Blinker",
      "version": "1.0.0",
      "author": "HackOS Team",
      "description": "Toggle the onboard LED",
      "category": "tools",
      "enabled": true,
      "actions": 3
    }
  ]
}
```

#### POST /api/plugins/upload

Body:
```json
{
  "filename": "my_plugin.json",
  "content": "{\"name\":\"my_plugin\",\"label\":\"My Plugin\",...}"
}
```

#### POST /api/plugins/toggle

Body:
```json
{
  "name": "my_plugin",
  "enabled": true
}
```

#### POST /api/plugins/delete

Body:
```json
{
  "name": "my_plugin"
}
```

---

## Best Practices

### Naming Conventions

- Use `snake_case` for the `name` field
- Keep names short and descriptive (max 31 chars)
- Use a meaningful `label` for display (max 47 chars)
- Prefix community plugins with your username: `username_plugin_name`

### Action Design

- **Limit actions to 8 per plugin** (hardware constraint)
- Use descriptive labels that fit on the 128px OLED display
- Group related actions together
- Always include a "safe" action first (e.g., read before write)

### Version Management

- Follow semantic versioning: `MAJOR.MINOR.PATCH`
- Increment MAJOR for breaking changes
- Increment MINOR for new actions
- Increment PATCH for bug fixes

### GPIO Safety

- **Never** use pins already in use by HackOS hardware:
  - GPIO 21/22: I2C (OLED display)
  - GPIO 34/35/36/39: Joystick (input only)
  - GPIO 5: SD Card CS
  - GPIO 14: IR Transmitter
  - GPIO 25: RF Transmitter
  - GPIO 27: Buzzer
- Check your ESP32 board's pinout before using a GPIO
- Use `gpio_toggle` instead of `gpio_high`/`gpio_low` for testing

### File Size

- Keep JSON files under 2KB (parser buffer limit)
- Use short but clear descriptions
- Don't include unnecessary whitespace in production plugins

---

## Troubleshooting

### Plugin Not Showing Up

1. **Check filename**: Must end in `.json`
2. **Check location**: Must be in `/ext/plugins/` on SD card
3. **Check JSON validity**: Use a JSON validator
4. **Check `name` field**: Must be present and unique
5. **Check slot limit**: Maximum 16 plugins + 32 total apps

### Plugin Actions Not Working

1. **Check pin numbers**: Ensure they're valid ESP32 GPIO pins
2. **Check action type**: Must be one of the supported types
3. **Check label**: Each action must have a non-empty `label`

### Web Upload Failing

1. **Check file extension**: Must be `.json`
2. **Check file size**: Must be under 4KB (POST buffer limit)
3. **Check JSON format**: Must be valid JSON
4. **Check SD card**: Must be mounted and writable

### Serial Debug

Enable serial monitoring at 115200 baud to see plugin loading logs:
```
[PluginMgr] Loaded plugin: LED Blinker (1.0.0)
[PluginMgr] Registered plugin app: led_blinker
[PluginMgr] Scan complete: 1 plugins loaded
```

---

## Advanced Examples

### Multi-Function Tool

```json
{
  "name": "pentest_helper",
  "label": "Pentest Helper",
  "version": "2.0.0",
  "author": "SecurityResearcher",
  "description": "Quick-access pentesting utilities",
  "category": "security",
  "config": {
    "pin": 25,
    "frequency": 433920000,
    "protocol": "OOK"
  },
  "actions": [
    {
      "type": "gpio_toggle",
      "pin": 25,
      "label": "Toggle RF TX"
    },
    {
      "type": "freq_set",
      "value": 433920000,
      "label": "433.92 MHz Mode"
    },
    {
      "type": "freq_set",
      "value": 315000000,
      "label": "315 MHz Mode"
    },
    {
      "type": "pwm_tone",
      "pin": 27,
      "freq": 2000,
      "duration": 100,
      "label": "Alert Beep"
    },
    {
      "type": "log_msg",
      "label": "Log Pentest Start"
    },
    {
      "type": "delay_ms",
      "value": 2000,
      "label": "Wait 2s"
    }
  ]
}
```

### Audio Sequencer

```json
{
  "name": "melody_player",
  "label": "Melody Player",
  "version": "1.0.0",
  "author": "MusicHacker",
  "description": "Play simple melodies on the buzzer",
  "category": "audio",
  "config": {
    "pin": 27,
    "protocol": "PWM"
  },
  "actions": [
    {
      "type": "pwm_tone",
      "pin": 27,
      "freq": 262,
      "duration": 400,
      "label": "C4"
    },
    {
      "type": "pwm_tone",
      "pin": 27,
      "freq": 294,
      "duration": 400,
      "label": "D4"
    },
    {
      "type": "pwm_tone",
      "pin": 27,
      "freq": 330,
      "duration": 400,
      "label": "E4"
    },
    {
      "type": "pwm_tone",
      "pin": 27,
      "freq": 349,
      "duration": 400,
      "label": "F4"
    },
    {
      "type": "pwm_tone",
      "pin": 27,
      "freq": 392,
      "duration": 400,
      "label": "G4"
    },
    {
      "type": "pwm_tone",
      "pin": 27,
      "freq": 440,
      "duration": 400,
      "label": "A4"
    },
    {
      "type": "pwm_tone",
      "pin": 27,
      "freq": 494,
      "duration": 400,
      "label": "B4"
    },
    {
      "type": "pwm_tone",
      "pin": 27,
      "freq": 523,
      "duration": 400,
      "label": "C5"
    }
  ]
}
```

---

## Contributing to the Plugin Store

### Repository Structure

The `plugins/` directory in the HackOS repository serves as the community plugin store:

```
plugins/
├── README.md           ← Overview and quick start
├── examples/           ← Official example plugins
│   ├── led_blinker.json
│   ├── buzzer_tool.json
│   ├── rf_433_presets.json
│   └── gpio_tester.json
└── community/          ← Community contributions (submit via PR)
    └── your_plugin.json
```

### Submission Process

1. **Fork** the [HackOS repository](https://github.com/El-EnderJ/HackOS)
2. Create your plugin file in `plugins/community/`
3. Validate your JSON
4. Test on hardware if possible
5. Submit a **Pull Request** with:
   - Clear title: `[Plugin] Your Plugin Name`
   - Description of what it does
   - Any hardware requirements
   - Screenshots if applicable

### Review Criteria

- Valid JSON format
- All required fields present
- Actions use supported types only
- Description is clear and accurate
- No malicious or dangerous GPIO operations
- Follows naming conventions

---

*Last updated: Phase 20 – Plugin System*
