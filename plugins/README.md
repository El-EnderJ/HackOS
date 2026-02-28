# 🔌 HackOS Plugin Store

Welcome to the HackOS community plugin repository! This is where the community can submit, share, and discover plugins for HackOS.

## 📦 What Are Plugins?

Plugins are JSON configuration files that define custom apps for HackOS. Each plugin adds a new app to the launcher menu that can perform GPIO operations, play tones, set frequencies, and more — all without writing C++ code.

## 🚀 How to Install Plugins

### Method 1: Web Dashboard (Easiest)
1. Connect to HackOS WiFi (`HackOS-Dashboard` / `hackos1337`)
2. Open `http://192.168.4.1` in your browser
3. Go to the **Plugin Store** tab
4. Upload your `.json` plugin file or create one using the built-in editor

### Method 2: SD Card (Manual)
1. Copy the `.json` plugin file to `/ext/plugins/` on the SD card
2. Reboot HackOS or use the **Plugin Manager** app to reload

### Method 3: Plugin Manager App
1. Navigate to **Plugin Manager** in the HackOS launcher
2. Select **Reload Plugins** to scan for new plugins on the SD card

## 📂 Directory Structure

```
plugins/
├── README.md           ← You are here
├── examples/           ← Example plugins by the HackOS team
│   ├── led_blinker.json
│   ├── buzzer_tool.json
│   ├── rf_433_presets.json
│   └── gpio_tester.json
└── community/          ← Community-submitted plugins (submit via PR!)
    └── .gitkeep
```

## 🤝 Contributing Plugins

We welcome plugin submissions from the community! To submit a plugin:

1. **Fork** this repository
2. Create your plugin `.json` file in `plugins/community/`
3. Make sure it follows the [Plugin Specification](#plugin-specification)
4. Open a **Pull Request** with your plugin

### PR Guidelines
- One plugin per PR
- Include a clear description of what the plugin does
- Test your plugin on actual hardware if possible
- Follow the naming convention: `your_plugin_name.json`

## 📋 Plugin Specification

See [docs/PLUGINS.md](../docs/PLUGINS.md) for the full specification.

### Quick Reference

```json
{
  "name": "my_plugin",
  "label": "My Plugin",
  "version": "1.0.0",
  "author": "YourName",
  "description": "A short description",
  "category": "tools",
  "config": {
    "pin": 25,
    "frequency": 433920000,
    "protocol": "OOK"
  },
  "actions": [
    {
      "type": "gpio_toggle",
      "pin": 2,
      "label": "Toggle LED"
    }
  ]
}
```

### Available Action Types

| Action Type    | Description                    | Parameters              |
|---------------|--------------------------------|-------------------------|
| `gpio_toggle` | Toggle a GPIO pin HIGH/LOW     | `pin`                   |
| `gpio_high`   | Set a GPIO pin to HIGH         | `pin`                   |
| `gpio_low`    | Set a GPIO pin to LOW          | `pin`                   |
| `pwm_tone`    | Play a PWM tone                | `pin`, `freq`, `duration` |
| `freq_set`    | Set/display a frequency value  | `value`                 |
| `delay_ms`    | Wait for N milliseconds        | `value` (max 5000)      |
| `log_msg`     | Print a message to serial log  | `label` (message text)  |

### Categories

Use one of: `tools`, `rf`, `audio`, `debug`, `network`, `security`, `general`

## ⚠️ Safety Notes

- GPIO operations directly control hardware pins — double-check your pin numbers
- PWM tones are limited to safe frequencies
- `delay_ms` is capped at 5000ms to prevent UI lockup
- Plugins run in the same process space as HackOS — they cannot crash independently

## 📜 License

All plugins in this repository are shared under the same license as HackOS. By submitting a plugin, you agree to license it under the project's terms.
