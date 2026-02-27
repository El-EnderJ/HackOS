/**
 * @file subghz_analyzer_app.cpp
 * @brief Sub-GHz Analyzer – demo application using the HackOSApp SDK.
 *
 * This application demonstrates the full HackOSApp lifecycle and SDK:
 *
 *  - **on_alloc()**: Requests memory for signal display buffers via
 *    AppContext.
 *
 *  - **on_start()**: Builds a SceneManager with three scenes (Main Menu,
 *    Capture, Result) and registers Views in a ViewDispatcher.
 *
 *  - **on_event()**: Routes input to the active scene which in turn
 *    controls the RadioManager for signal capture.
 *
 *  - **on_free()**: Cleans up all resources; any leaked resources are
 *    swept by the AppContext safety net.
 *
 * Scenes:
 *  0 – Main Menu   (Start Capture / View Last / Back)
 *  1 – Capture     (live signal capture via RadioManager)
 *  2 – Result      (decoded signal details)
 */

#include "apps/subghz_analyzer_app.h"

#include <cstdio>
#include <new>

#include "hackos.h"

// ── Anonymous namespace for all internal implementation ──────────────────────

namespace
{

// ── Constants ────────────────────────────────────────────────────────────────

static constexpr const char *TAG_SUBGHZ = "SubGhzApp";

static constexpr size_t LINE_BUF_SIZE = 32U;

// Scene IDs
enum SceneId : uint32_t
{
    SCENE_MAIN_MENU = 0U,
    SCENE_CAPTURE   = 1U,
    SCENE_RESULT    = 2U,
    SCENE_COUNT     = 3U,
};

// View IDs
enum ViewId : uint32_t
{
    VIEW_MENU    = 0U,
    VIEW_CAPTURE = 1U,
    VIEW_RESULT  = 2U,
};

// Custom app-event IDs forwarded through the SceneManager
enum AppEvent : uint32_t
{
    EVENT_START_CAPTURE    = 100U,
    EVENT_VIEW_LAST_SIGNAL = 101U,
    EVENT_BACK             = 102U,
    EVENT_SIGNAL_FOUND     = 200U,
};

// ── Menu items ───────────────────────────────────────────────────────────────

static constexpr size_t MENU_ITEM_COUNT = 3U;
static const char *const kMenuLabels[MENU_ITEM_COUNT] = {
    "Start Capture",
    "View Last Signal",
    "Back",
};

// ── Forward declaration ──────────────────────────────────────────────────────

class SubGhzAnalyzerApp;

// ── CaptureView: live signal capture screen ─────────────────────────────────

class CaptureView final : public View
{
public:
    CaptureView()
        : capturing_(false),
          statusLine_{}
    {
        setStatusText("Press CENTER to scan");
    }

    void draw(Canvas *canvas) override
    {
        canvas->drawStr(2, 10, "Sub-GHz Capture");
        canvas->drawLine(0, 14, Canvas::WIDTH - 1, 14);

        if (capturing_)
        {
            canvas->drawStr(2, 28, "Scanning 433.92 MHz...");
            canvas->drawStr(2, 40, statusLine_);
        }
        else
        {
            canvas->drawStr(2, 30, statusLine_);
        }
    }

    bool input(InputEvent * /*event*/) override
    {
        return false; // Input is handled by scene callbacks
    }

    void setCapturing(bool active) { capturing_ = active; }
    void setStatusText(const char *text)
    {
        std::snprintf(statusLine_, sizeof(statusLine_), "%s", text);
    }

private:
    bool capturing_;
    char statusLine_[LINE_BUF_SIZE];
};

// ── ResultView: decoded signal details screen ───────────────────────────────

class ResultView final : public View
{
public:
    ResultView()
        : freqLine_{},
          protoLine_{},
          valueLine_{}
    {
    }

    void draw(Canvas *canvas) override
    {
        canvas->drawStr(2, 10, "Signal Details");
        canvas->drawLine(0, 14, Canvas::WIDTH - 1, 14);
        canvas->drawStr(2, 26, freqLine_);
        canvas->drawStr(2, 38, protoLine_);
        canvas->drawStr(2, 50, valueLine_);
    }

    bool input(InputEvent * /*event*/) override
    {
        return false;
    }

    void loadFromRecord(const hackos::radio::SignalRecord &rec)
    {
        std::snprintf(freqLine_, sizeof(freqLine_), "Freq: %lu Hz",
                      static_cast<unsigned long>(rec.frequencyHz));
        std::snprintf(protoLine_, sizeof(protoLine_), "Proto: %s",
                      rec.protocolName);
        std::snprintf(valueLine_, sizeof(valueLine_), "Val: 0x%08lX",
                      static_cast<unsigned long>(rec.decodedValue));
    }

    void setNoSignal()
    {
        std::snprintf(freqLine_, sizeof(freqLine_), "No signal captured");
        protoLine_[0] = '\0';
        valueLine_[0] = '\0';
    }

private:
    char freqLine_[LINE_BUF_SIZE];
    char protoLine_[LINE_BUF_SIZE];
    char valueLine_[LINE_BUF_SIZE];
};

// ── MenuView: wraps MenuListView widget in a View ───────────────────────────

class SubGhzMenuView final : public View
{
public:
    SubGhzMenuView()
        : menu_(0, 20, Canvas::WIDTH, 36, 3)
    {
        menu_.setItems(kMenuLabels, MENU_ITEM_COUNT);
    }

    void draw(Canvas *canvas) override
    {
        canvas->drawStr(2, 10, "Sub-GHz Analyzer");
        canvas->drawLine(0, 14, Canvas::WIDTH - 1, 14);
        menu_.draw();
    }

    bool input(InputEvent * /*event*/) override
    {
        return false;
    }

    MenuListView &menu() { return menu_; }

private:
    MenuListView menu_;
};

// ═══════════════════════════════════════════════════════════════════════════════
// ── SubGhzAnalyzerApp ──────────────────────────────────────────────────────
// ═══════════════════════════════════════════════════════════════════════════════

class SubGhzAnalyzerApp final : public hackos::HackOSApp
{
public:
    SubGhzAnalyzerApp()
        : menuView_(nullptr),
          captureView_(nullptr),
          resultView_(nullptr),
          sceneManager_(nullptr),
          viewDispatcher_(),
          statusBar_(0, 0, 128, 8),
          needsRedraw_(true)
    {
    }

    // ── HackOSApp lifecycle ──────────────────────────────────────────────

    void on_alloc() override
    {
        // Phase 1: allocate views through the AppContext sandbox
        menuView_    = static_cast<SubGhzMenuView *>(ctx().alloc(sizeof(SubGhzMenuView)));
        captureView_ = static_cast<CaptureView *>(ctx().alloc(sizeof(CaptureView)));
        resultView_  = static_cast<ResultView *>(ctx().alloc(sizeof(ResultView)));

        if (menuView_ != nullptr)
        {
            new (menuView_) SubGhzMenuView();
        }
        if (captureView_ != nullptr)
        {
            new (captureView_) CaptureView();
        }
        if (resultView_ != nullptr)
        {
            new (resultView_) ResultView();
        }
    }

    void on_start() override
    {
        // Phase 2: set up views, scenes, status bar
        statusBar_.setConnectivity(false, false);
        statusBar_.setBatteryLevel(100U);
        statusBar_.setTime(0U, 0U);

        if (menuView_ != nullptr)
        {
            viewDispatcher_.addView(VIEW_MENU, menuView_);
        }
        if (captureView_ != nullptr)
        {
            viewDispatcher_.addView(VIEW_CAPTURE, captureView_);
        }
        if (resultView_ != nullptr)
        {
            viewDispatcher_.addView(VIEW_RESULT, resultView_);
        }

        // Build scene table
        static const SceneHandler handlers[SCENE_COUNT] = {
            {sceneMainMenuEnter, sceneMainMenuEvent, sceneMainMenuExit},
            {sceneCaptureEnter,  sceneCaptureEvent,  sceneCaptureExit},
            {sceneResultEnter,   sceneResultEvent,   sceneResultExit},
        };

        sceneManager_ = new (std::nothrow) SceneManager(handlers, SCENE_COUNT, this);
        if (sceneManager_ != nullptr)
        {
            sceneManager_->navigateTo(SCENE_MAIN_MENU);
        }

        needsRedraw_ = true;
    }

    void on_event(Event *event) override
    {
        // Phase 3: route input events to the active scene
        if (event->type != EventType::EVT_INPUT)
        {
            return;
        }

        const auto input = static_cast<InputManager::InputEvent>(event->arg0);

        if (sceneManager_ == nullptr)
        {
            return;
        }

        // Map hardware input to scene events
        if (input == InputManager::InputEvent::BUTTON_PRESS)
        {
            if (sceneManager_->currentScene() == SCENE_MAIN_MENU && menuView_ != nullptr)
            {
                const size_t sel = menuView_->menu().selectedIndex();
                if (sel == 0U)
                {
                    sceneManager_->handleEvent(EVENT_START_CAPTURE);
                }
                else if (sel == 1U)
                {
                    sceneManager_->handleEvent(EVENT_VIEW_LAST_SIGNAL);
                }
                else
                {
                    sceneManager_->handleEvent(EVENT_BACK);
                }
            }
            else if (sceneManager_->currentScene() == SCENE_CAPTURE)
            {
                sceneManager_->handleEvent(EVENT_SIGNAL_FOUND);
            }
            else if (sceneManager_->currentScene() == SCENE_RESULT)
            {
                sceneManager_->handleEvent(EVENT_BACK);
            }
        }
        else if (input == InputManager::InputEvent::UP)
        {
            if (sceneManager_->currentScene() == SCENE_MAIN_MENU && menuView_ != nullptr)
            {
                menuView_->menu().moveSelection(-1);
                needsRedraw_ = true;
            }
        }
        else if (input == InputManager::InputEvent::DOWN)
        {
            if (sceneManager_->currentScene() == SCENE_MAIN_MENU && menuView_ != nullptr)
            {
                menuView_->menu().moveSelection(1);
                needsRedraw_ = true;
            }
        }
        else if (input == InputManager::InputEvent::LEFT)
        {
            if (sceneManager_->currentScene() != SCENE_MAIN_MENU)
            {
                sceneManager_->handleEvent(EVENT_BACK);
            }
            else
            {
                // At top level, request system back
                const Event evt{EventType::EVT_SYSTEM, SYSTEM_EVENT_BACK, 0, nullptr};
                EventSystem::instance().postEvent(evt);
            }
        }
    }

    void on_free() override
    {
        // Phase 4: explicit cleanup
        hackos::radio::RadioManager::instance().stopCapture();

        viewDispatcher_.removeView(VIEW_MENU);
        viewDispatcher_.removeView(VIEW_CAPTURE);
        viewDispatcher_.removeView(VIEW_RESULT);

        // Destroy placement-new'd views (AppContext::releaseAll frees memory)
        if (menuView_ != nullptr)
        {
            menuView_->~SubGhzMenuView();
        }
        if (captureView_ != nullptr)
        {
            captureView_->~CaptureView();
        }
        if (resultView_ != nullptr)
        {
            resultView_->~ResultView();
        }

        delete sceneManager_;
        sceneManager_ = nullptr;
    }

    void on_update() override
    {
        // Poll RadioManager for decoded signals during capture
        if (sceneManager_ != nullptr &&
            sceneManager_->currentScene() == SCENE_CAPTURE &&
            hackos::radio::RadioManager::instance().isCapturing())
        {
            if (hackos::radio::RadioManager::instance().hasLastRecord())
            {
                sceneManager_->handleEvent(EVENT_SIGNAL_FOUND);
            }
        }
    }

    void on_draw() override
    {
        if (!needsRedraw_ && !statusBar_.isDirty())
        {
            return;
        }

        DisplayManager::instance().clear();
        statusBar_.draw();

        Canvas canvas;
        canvas.clear();
        viewDispatcher_.draw(&canvas);
        // The canvas is drawn through the ViewDispatcher; the status bar
        // renders via the legacy widget API.

        DisplayManager::instance().present();
        statusBar_.clearDirty();
        needsRedraw_ = false;
    }

    // ── Accessors for scene callbacks ────────────────────────────────────

    ViewDispatcher &viewDispatcher() { return viewDispatcher_; }
    CaptureView *captureView() { return captureView_; }
    ResultView *resultView() { return resultView_; }
    SceneManager *sceneManager() { return sceneManager_; }
    void requestRedraw() { needsRedraw_ = true; }

private:
    SubGhzMenuView *menuView_;
    CaptureView *captureView_;
    ResultView *resultView_;
    SceneManager *sceneManager_;
    ViewDispatcher viewDispatcher_;
    StatusBar statusBar_;
    bool needsRedraw_;

    // ═════════════════════════════════════════════════════════════════════
    // Scene callbacks – static functions receiving the app as context
    // ═════════════════════════════════════════════════════════════════════

    // ── Scene 0: Main Menu ───────────────────────────────────────────────

    static void sceneMainMenuEnter(void *context)
    {
        auto *app = static_cast<SubGhzAnalyzerApp *>(context);
        app->viewDispatcher().switchToView(VIEW_MENU);
        app->requestRedraw();
    }

    static bool sceneMainMenuEvent(void *context, uint32_t eventId)
    {
        auto *app = static_cast<SubGhzAnalyzerApp *>(context);
        if (eventId == EVENT_START_CAPTURE)
        {
            app->sceneManager()->navigateTo(SCENE_CAPTURE);
            return true;
        }
        if (eventId == EVENT_VIEW_LAST_SIGNAL)
        {
            app->sceneManager()->navigateTo(SCENE_RESULT);
            return true;
        }
        if (eventId == EVENT_BACK)
        {
            const Event evt{EventType::EVT_SYSTEM, SYSTEM_EVENT_BACK, 0, nullptr};
            EventSystem::instance().postEvent(evt);
            return true;
        }
        return false;
    }

    static void sceneMainMenuExit(void * /*context*/)
    {
    }

    // ── Scene 1: Capture ─────────────────────────────────────────────────

    static void sceneCaptureEnter(void *context)
    {
        auto *app = static_cast<SubGhzAnalyzerApp *>(context);
        app->viewDispatcher().switchToView(VIEW_CAPTURE);
        if (app->captureView() != nullptr)
        {
            app->captureView()->setCapturing(true);
            app->captureView()->setStatusText("Waiting for signal...");
        }

        // Request RadioManager to start capturing on the first registered device
        auto *dev = hackos::radio::RadioManager::instance().findDevice("RF433");
        if (dev != nullptr)
        {
            hackos::radio::RadioManager::instance().startCapture(dev);
        }

        app->requestRedraw();
    }

    static bool sceneCaptureEvent(void *context, uint32_t eventId)
    {
        auto *app = static_cast<SubGhzAnalyzerApp *>(context);
        if (eventId == EVENT_SIGNAL_FOUND)
        {
            hackos::radio::RadioManager::instance().stopCapture();
            if (app->captureView() != nullptr)
            {
                app->captureView()->setCapturing(false);
                app->captureView()->setStatusText("Signal decoded!");
            }
            app->sceneManager()->navigateTo(SCENE_RESULT);
            return true;
        }
        if (eventId == EVENT_BACK)
        {
            hackos::radio::RadioManager::instance().stopCapture();
            app->sceneManager()->navigateBack();
            return true;
        }
        return false;
    }

    static void sceneCaptureExit(void *context)
    {
        auto *app = static_cast<SubGhzAnalyzerApp *>(context);
        hackos::radio::RadioManager::instance().stopCapture();
        if (app->captureView() != nullptr)
        {
            app->captureView()->setCapturing(false);
        }
    }

    // ── Scene 2: Result ──────────────────────────────────────────────────

    static void sceneResultEnter(void *context)
    {
        auto *app = static_cast<SubGhzAnalyzerApp *>(context);
        app->viewDispatcher().switchToView(VIEW_RESULT);

        if (app->resultView() != nullptr)
        {
            if (hackos::radio::RadioManager::instance().hasLastRecord())
            {
                app->resultView()->loadFromRecord(
                    hackos::radio::RadioManager::instance().lastRecord());
            }
            else
            {
                app->resultView()->setNoSignal();
            }
        }

        app->requestRedraw();
    }

    static bool sceneResultEvent(void *context, uint32_t eventId)
    {
        auto *app = static_cast<SubGhzAnalyzerApp *>(context);
        if (eventId == EVENT_BACK)
        {
            app->sceneManager()->navigateBack();
            return true;
        }
        return false;
    }

    static void sceneResultExit(void * /*context*/)
    {
    }
};

} // namespace

// ── Factory function ─────────────────────────────────────────────────────────

AppBase *createSubGhzAnalyzerApp()
{
    return new (std::nothrow) SubGhzAnalyzerApp();
}
