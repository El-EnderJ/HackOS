# Contributing to HackOS

Thank you for your interest in contributing! HackOS follows a strict set of architectural rules to keep the firmware lean, deterministic, and maintainable on resource-constrained hardware. **Please read this document in full before opening a pull request.**

---

## Table of Contents

1. [Code of Conduct](#code-of-conduct)
2. [Getting Started](#getting-started)
3. [Branching Strategy](#branching-strategy)
4. [Naming Conventions](#naming-conventions)
5. [Architecture Rules](#architecture-rules)
   - [Singleton Pattern](#singleton-pattern)
   - [Event-Driven Pattern](#event-driven-pattern)
6. [App Development Rules](#app-development-rules)
7. [HAL Development Rules](#hal-development-rules)
8. [Code Style](#code-style)
9. [Commit Messages](#commit-messages)
10. [Pull Request Checklist](#pull-request-checklist)

---

## Code of Conduct

All contributors are expected to be respectful and constructive.  
Harassment, personal attacks, and discriminatory language will not be tolerated.

---

## Getting Started

```bash
# Fork and clone the repository
git clone https://github.com/<your-fork>/HackOS.git
cd HackOS

# Install PlatformIO
pip install platformio

# Verify the build passes before making changes
pio run
```

See the [Installation Guide in README.md](README.md#installation-guide-platformio) for full setup instructions.

---

## Branching Strategy

| Branch | Purpose |
|---|---|
| `main` | Stable, release-ready code |
| `develop` | Integration branch for in-progress work |
| `feature/<short-name>` | New features or apps |
| `fix/<short-name>` | Bug fixes |
| `docs/<short-name>` | Documentation-only changes |

Always branch from `develop`, not `main`.

---

## Naming Conventions

These conventions are **strictly enforced**. PRs that violate them will be blocked.

### Files

| Item | Convention | Example |
|---|---|---|
| App header | `include/apps/<snake_case>_app.h` | `file_manager_app.h` |
| App source | `src/apps/<snake_case>_app.cpp` | `file_manager_app.cpp` |
| HAL header | `include/hardware/<snake_case>.h` | `nfc_reader.h` |
| HAL source | `src/hardware/<snake_case>.cpp` | `nfc_reader.cpp` |
| Core header | `include/core/<snake_case>.h` | `event_system.h` |

### C++ Identifiers

| Item | Convention | Example |
|---|---|---|
| Classes | `PascalCase` | `FileManagerApp`, `StorageManager` |
| Member variables | `snake_case_` (trailing underscore) | `entryCount_`, `mounted_` |
| Local variables | `camelCase` | `entryName`, `hexUID` |
| Constants / `constexpr` | `SCREAMING_SNAKE_CASE` | `MAX_ENTRIES`, `LABEL_LEN` |
| Factory functions | `create<PascalCase>App()` | `createFileManagerApp()` |
| Log tags | `"<PascalCase>App"` string literal | `"FileManagerApp"` |
| Enums | `PascalCase` with `PascalCase` values | `enum class FMState : uint8_t { BROWSING }` |

### Singletons

Every singleton class **must** follow the exact pattern below. No exceptions.

```cpp
class MyDriver
{
public:
    static MyDriver &instance();
    // … public API …
private:
    MyDriver();   // private constructor – prevents external instantiation
    // … private state …
};
```

```cpp
MyDriver &MyDriver::instance()
{
    static MyDriver driver;   // Meyers singleton – thread-safe in C++11
    return driver;
}
```

---

## Architecture Rules

### Singleton Pattern

**Rule S-1**: All hardware drivers (`DisplayManager`, `InputManager`, `Wireless`, `NFCReader`, `IRTransceiver`, `RFTransceiver`, `StorageManager`) **must** be accessed exclusively through their `instance()` accessor.

**Rule S-2**: Singletons **must not** be constructed or stored in user code. Do not hold references or pointers to a singleton longer than the current function scope.

**Rule S-3**: No new global or `static` variables with non-trivial constructors. Use the Singleton pattern instead.

**Rule S-4**: `AppManager`, `EventSystem`, and `StateMachine` are also singletons. They follow the same rules.

### Event-Driven Pattern

**Rule E-1**: All inter-component communication **must** happen through `EventSystem::instance().postEvent()`. Direct method calls between an app and another app are forbidden.

**Rule E-2**: An app **must** subscribe to `EventSystem` in `onSetup()` and unsubscribe in `onDestroy()`:

```cpp
void onSetup() override
{
    (void)EventSystem::instance().subscribe(this);
    // …
}

void onDestroy() override
{
    EventSystem::instance().unsubscribe(this);
    // …
}
```

**Rule E-3**: `EVT_INPUT` events are **not** forwarded by `AppManager`. Apps **must** subscribe directly to receive input events.

**Rule E-4**: To navigate back to the Launcher, post `EVT_SYSTEM / SYSTEM_EVENT_BACK`:

```cpp
const Event evt{EventType::EVT_SYSTEM, SYSTEM_EVENT_BACK, 0, nullptr};
EventSystem::instance().postEvent(evt);
```

**Rule E-5**: To launch a sub-app from the Launcher, post `EVT_APP / APP_EVENT_LAUNCH(index)`:

```cpp
const Event evt{EventType::EVT_APP, APP_EVENT_LAUNCH,
                static_cast<int32_t>(menu_.selectedIndex()), nullptr};
EventSystem::instance().postEvent(evt);
```

**Rule E-6**: Never call `onEvent()` directly on another observer.

**Rule E-7**: Never post events with stack-allocated `data` pointers that may go out of scope before the event is consumed.

---

## App Development Rules

**Rule A-1**: Every app **must** be contained in:
- One header: `include/apps/<name>_app.h` (factory declaration only)
- One source: `src/apps/<name>_app.cpp` (full implementation)

**Rule A-2**: The concrete app class **must** live inside an anonymous `namespace {}` in the `.cpp` file. It must not be visible in the header.

**Rule A-3**: The factory function **must** use placement `new` with `std::nothrow` to avoid exceptions:

```cpp
AppBase *createMyApp()
{
    return new (std::nothrow) MyApp();
}
```

**Rule A-4**: `onDraw()` **must** check a `needsRedraw_` flag or widget dirty flags before clearing and re-painting the display. Do not call `DisplayManager::instance().clear()` unconditionally.

**Rule A-5**: `onLoop()` **must not** perform any blocking operation (no `delay()`, no blocking I²C/SPI). Use a state machine and return on every tick; block work in `onLoop()` only across multiple calls.

**Rule A-6**: Apps **must not** call `StorageManager::instance().writeFile()` with payloads larger than **4 KB** in a single call. Use `appendChunk()` with iteration for larger payloads to prevent the UI from freezing.

**Rule A-7**: Every app **must** provide a "Back" navigation path that posts `SYSTEM_EVENT_BACK`.

**Rule A-8**: Apps **must not** store pointers to other `AppBase` instances.

---

## HAL Development Rules

**Rule H-1**: HAL classes (in `include/hardware/`) **must** be Singletons (Rule S-1–S-4).

**Rule H-2**: HAL `init()` / `deinit()` methods **must** be idempotent (safe to call multiple times).

**Rule H-3**: All hardware-specific `#include` directives (e.g. `<SD.h>`, `<esp_wifi.h>`) belong exclusively in the `.cpp` implementation. Headers expose only the public API.

**Rule H-4**: HAL methods **must not** block for more than ~5 ms. Offload longer operations to an async callback or a FreeRTOS task, and signal completion via `EventSystem::instance().postEvent()`.

---

## Code Style

HackOS follows a modified Google C++ style with these overrides:

| Rule | Value |
|---|---|
| Standard | C++17 (`-std=gnu++17`) |
| Indentation | 4 spaces (no tabs) |
| Brace style | Allman (opening brace on new line) |
| Line length | ≤ 100 characters |
| Include order | 1. Implementation header, 2. C stdlib, 3. ESP-IDF/Arduino, 4. Project headers |
| `nullptr` | Always prefer over `NULL` or `0` |
| Cast style | `static_cast<>`, `reinterpret_cast<>` – no C-style casts |
| Integer literals | Use `U`/`UL`/`ULL` suffixes for unsigned constants |
| Compiler warnings | All code must compile cleanly with `-Wall -Wextra` |

---

## Commit Messages

Use [Conventional Commits](https://www.conventionalcommits.org/):

```
<type>(<scope>): <short summary>

[optional body]
[optional footer]
```

| Type | When to use |
|---|---|
| `feat` | New app or feature |
| `fix` | Bug fix |
| `refactor` | Code restructuring without behaviour change |
| `docs` | Documentation only |
| `chore` | Build system, dependencies, CI |
| `test` | Tests only |

Examples:

```
feat(apps): add FileManagerApp for SD card browsing
fix(storage): handle NULL path in listDir()
docs(readme): add architecture diagram
```

---

## Pull Request Checklist

Before opening a PR, confirm every item:

- [ ] `pio run` compiles with **zero errors and zero new warnings**
- [ ] New app follows all **Rule A-1 through A-8**
- [ ] All singletons accessed via `instance()` only (**Rule S-1**)
- [ ] All inter-component communication via `EventSystem` (**Rule E-1**)
- [ ] `onSetup()` subscribes and `onDestroy()` unsubscribes from `EventSystem`
- [ ] `onLoop()` contains no blocking calls
- [ ] `onDraw()` guards with dirty-flag check
- [ ] New app registered in `main.cpp` with `registerApp()`
- [ ] Header contains **only** the factory function declaration
- [ ] Concrete class is inside `namespace {}` in `.cpp`
- [ ] Factory uses `new (std::nothrow)`
- [ ] No C-style casts
- [ ] Commit messages follow Conventional Commits format
- [ ] `README.md` updated if a new app or HAL feature was added
