// deki-display's MIPI-DSI panel off a device, on a fake panel that records
// what it is sent and holds the frame buffers a platform would:
//
// - DsiPanelComponent: the start-up goes over the link in order, then the
//   video stream starts, the display is the engine's and the backlight goes
//   on; a boot scene runs where there is no DSI; a start-up that does not
//   read, or a panel with no pixel clock, is refused.
// - DsiDisplay: the game draws into the buffer not on screen, after the
//   refresh still reading it has ended; Present shows the buffer it is given,
//   or copies a buffer of the game's own into the free one.
// - DsiPanelConfig: the refresh period its timing gives.

#include <gtest/gtest.h>

#include "DsiDisplay.h"
#include "DsiPanelComponent.h"
#include "MipiDsi.h"

#include "DekiGPIO.h"

#include <deki/Engine.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace DekiDisplay;

namespace
{

// What the panel and the pins were asked to do, in order.
struct Step
{
    enum Kind
    {
        Command,
        Start,
        Show,
        Wait,
        Pin
    } kind;
    uint8_t cmd = 0;
    std::vector<uint8_t> params;
    int32_t index = -1;
    int pin = -1;
    bool high = false;
};
std::vector<Step> g_Steps;

std::vector<Step> Of(Step::Kind kind)
{
    std::vector<Step> out;
    for (const Step& s : g_Steps)
    {
        if (s.kind == kind)
        {
            out.push_back(s);
        }
    }
    return out;
}

class FakeDsiPanel : public IDsiPanel
{
public:
    FakeDsiPanel(int32_t width, int32_t height, int32_t buffers) : height(height)
    {
        for (int32_t i = 0; i < buffers; ++i)
        {
            // Not black: Start must clear them.
            m_Buffers.emplace_back(static_cast<size_t>(width) * height * 2, uint8_t{ 0xA5 });
        }
    }

    bool Command(uint8_t cmd, const uint8_t* params, size_t n) override
    {
        Step s{ Step::Command };
        s.cmd = cmd;
        s.params.assign(params, params + n);
        g_Steps.push_back(s);
        return !refuseCommands;
    }
    bool Start() override
    {
        g_Steps.push_back(Step{ Step::Start });
        return true;
    }
    int32_t GetFrameBufferCount() const override { return static_cast<int32_t>(m_Buffers.size()); }
    uint8_t* GetFrameBuffer(int32_t index) const override
    {
        return const_cast<uint8_t*>(m_Buffers[static_cast<size_t>(index)].data());
    }
    // As a video-mode panel: the buffer shown last goes up at the next
    // refresh; until then the one being sent stays busy too.
    bool Show(int32_t index) override
    {
        Step s{ Step::Show };
        s.index = index;
        g_Steps.push_back(s);
        shown = index;
        return true;
    }
    uint32_t GetBusyMask() const override { return (1u << sending) | (1u << shown); }
    bool WaitRefresh(uint32_t) override
    {
        g_Steps.push_back(Step{ Step::Wait });
        // A panel that never finishes a refresh times every wait out.
        if (!refreshes)
        {
            return false;
        }
        sending = shown;
        return true;
    }
    // A refresh ending on its own, between frames.
    void Refresh() { sending = shown; }
    bool GetFramesShown(uint64_t* frames, uint64_t* lastUs) const override
    {
        *frames = 7;
        *lastUs = 1234;
        return true;
    }
    bool StartBacklightPwm(int32_t pin, uint32_t hz, bool) override
    {
        pwmPin = pin;
        pwmHz = hz;
        return hasPwm;
    }
    bool SetBacklightLevel(float level) override
    {
        levels.push_back(level);
        return true;
    }
    // Row copies as a DMA does them: started, and done only when waited for,
    // so a display that reads or reuses the source too early is caught.
    bool StartRowsCopy(int32_t index, int32_t y, int32_t rows, const uint8_t* src, int32_t stride) override
    {
        if (!copiesRows)
        {
            return false;
        }
        WaitRowsCopy();
        pending = { true, index, y, rows, std::vector<uint8_t>() };
        pendingSrc = src;
        pendingStride = stride;
        ++rowCopies;
        return true;
    }
    void WaitRowsCopy() override
    {
        if (!pending.on)
        {
            return;
        }
        const size_t rowBytes = m_Buffers[0].size() / static_cast<size_t>(height);
        for (int32_t r = 0; r < pending.rows; ++r)
        {
            std::memcpy(m_Buffers[static_cast<size_t>(pending.index)].data() + (pending.y + r) * rowBytes,
                        pendingSrc + static_cast<size_t>(r) * pendingStride * 2, rowBytes);
        }
        pending.on = false;
    }
    bool CopyRowsBetween(int32_t from, int32_t to, int32_t y, int32_t rows) override
    {
        if (!copiesRows)
        {
            return false;
        }
        const size_t rowBytes = m_Buffers[0].size() / static_cast<size_t>(height);
        std::memcpy(m_Buffers[static_cast<size_t>(to)].data() + y * rowBytes,
                    m_Buffers[static_cast<size_t>(from)].data() + y * rowBytes, rows * rowBytes);
        rowsBetween += rows;
        return true;
    }
    struct Pending
    {
        bool on = false;
        int32_t index = 0, y = 0, rows = 0;
        std::vector<uint8_t> unused;
    };
    bool copiesRows = false;
    int rowsBetween = 0;  // rows copied from one frame buffer into another
    int32_t height = 0;
    Pending pending;
    const uint8_t* pendingSrc = nullptr;
    int32_t pendingStride = 0;
    int rowCopies = 0;

    bool hasPwm = true;
    int32_t pwmPin = -1;
    uint32_t pwmHz = 0;
    std::vector<float> levels;

    int32_t shown = 0;
    int32_t sending = 0;
    bool refreshes = true;
    bool refuseCommands = false;

private:
    std::vector<std::vector<uint8_t>> m_Buffers;
};

class RecordingGpio : public DekiGpio::IDekiGPIO
{
public:
    bool SetOutput(int pin, bool high) override { return Write(pin, high); }
    bool SetInput(int, DekiGpio::Pull) override { return true; }
    bool Write(int pin, bool high) override
    {
        Step s{ Step::Pin };
        s.pin = pin;
        s.high = high;
        g_Steps.push_back(s);
        return true;
    }
    bool Read(int) override { return false; }
    bool CountEdges(int, DekiGpio::Edge, DekiGpio::Pull) override { return false; }
    uint32_t TakeEdges(int) override { return 0; }
    void StopCounting(int) override {}
};

// The factory DsiPanelComponent is given: the panel the test made, handed
// over once.
std::unique_ptr<FakeDsiPanel> g_NextPanel;
FakeDsiPanel* g_Panel = nullptr;
DsiPanelConfig g_Config;
std::unique_ptr<IDsiPanel> MakeFakePanel(const DsiPanelConfig& config)
{
    g_Config = config;
    g_Panel = g_NextPanel.get();
    return std::move(g_NextPanel);
}

// The CrowPanel Advanced 9"'s panel, an EK79007, as its boot scene starts it.
const char* kEK79007Init = R"(01; delay 20          # software reset
B2 10                    # two DSI lanes
80 8B; 81 78; 82 84; 83 88; 84 A8; 85 E3; 86 88
11; delay 120            # sleep out
)";

class DsiTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        g_Steps.clear();
        DekiGpio::DekiGPIO::SetCurrent(&m_Gpio);
    }
    void TearDown() override
    {
        DsiPanelComponent::Stop();
        DekiGpio::DekiGPIO::SetCurrent(nullptr);
        MipiDsi::SetFactory(nullptr);
        g_NextPanel.reset();
        g_Panel = nullptr;
    }

    // The 9" panel's size and timing.
    static void Configure(DsiPanelComponent& c)
    {
        c.initSequence = kEK79007Init;
        c.panelWidth = 1024;
        c.panelHeight = 600;
        c.dataLanes = 2;
        c.laneMbps = 900;
        c.pixelClockHz = 51000000;
        c.hsyncPulse = 70;
        c.hBackPorch = 160;
        c.hFrontPorch = 160;
        c.vsyncPulse = 10;
        c.vBackPorch = 23;
        c.vFrontPorch = 12;
        c.blPin = 31;
    }

    static bool RunSetup(DsiPanelComponent& c)
    {
        bool result = false;
        bool called = false;
        c.Setup(
            [&](bool ok)
            {
                called = true;
                result = ok;
            });
        EXPECT_TRUE(called);
        return result;
    }

    RecordingGpio m_Gpio;
};

}  // namespace

// --- DsiPanelConfig ---

TEST(DsiPanelConfig, TheRefreshPeriodComesFromTheTiming)
{
    DsiPanelConfig c;
    c.width = 1024;
    c.height = 600;
    c.pixelClockHz = 51000000;
    c.hsyncPulse = 70;
    c.hBackPorch = 160;
    c.hFrontPorch = 160;
    c.vsyncPulse = 10;
    c.vBackPorch = 23;
    c.vFrontPorch = 12;
    // (1024 + 390) x (600 + 45) pixel clocks: 17,882 us, 55.9 Hz.
    EXPECT_EQ(c.RefreshPeriodUs(), 17882u);
    c.pixelClockHz = 0;
    EXPECT_EQ(c.RefreshPeriodUs(), 0u);
}

// --- DsiPanelComponent ---

TEST_F(DsiTest, ABootSceneRunsWhereThereIsNoDsi)
{
    DsiPanelComponent c;
    Configure(c);
    EXPECT_TRUE(RunSetup(c));
    EXPECT_TRUE(g_Steps.empty());
}

TEST_F(DsiTest, TheStartUpGoesOverTheLinkThenTheStreamStarts)
{
    g_NextPanel = std::make_unique<FakeDsiPanel>(1024, 600, 2);
    MipiDsi::SetFactory(&MakeFakePanel);
    DsiPanelComponent c;
    Configure(c);
    ASSERT_TRUE(RunSetup(c));

    EXPECT_EQ(g_Config.width, 1024);
    EXPECT_EQ(g_Config.height, 600);
    EXPECT_EQ(g_Config.dataLanes, 2);
    EXPECT_EQ(g_Config.laneMbps, 900u);
    EXPECT_EQ(g_Config.pixelClockHz, 51000000u);
    EXPECT_EQ(g_Config.hBackPorch, 160);
    EXPECT_EQ(g_Config.vFrontPorch, 12);
    EXPECT_EQ(g_Config.frameBuffers, 2);

    const std::vector<Step> commands = Of(Step::Command);
    ASSERT_EQ(commands.size(), 10u);
    EXPECT_EQ(commands[0].cmd, 0x01);
    EXPECT_EQ(commands[1].cmd, 0xB2);
    EXPECT_EQ(commands[1].params, (std::vector<uint8_t>{ 0x10 }));
    EXPECT_EQ(commands[2].cmd, 0x80);
    EXPECT_EQ(commands[8].cmd, 0x86);
    EXPECT_EQ(commands[9].cmd, 0x11);

    // Every command before the stream starts; the backlight after it, once
    // buffer 0 is black and on screen.
    size_t lastCommand = 0, start = 0, lightOn = 0;
    for (size_t i = 0; i < g_Steps.size(); ++i)
    {
        const Step& s = g_Steps[i];
        if (s.kind == Step::Command)
        {
            lastCommand = i;
        }
        if (s.kind == Step::Start)
        {
            start = i;
        }
        if (s.kind == Step::Pin && s.pin == 31 && s.high)
        {
            lightOn = i;
        }
    }
    EXPECT_LT(lastCommand, start);
    EXPECT_LT(start, lightOn);
    EXPECT_EQ(g_Panel->shown, 0);
    for (int32_t b = 0; b < 2; ++b)
    {
        const uint8_t* fb = g_Panel->GetFrameBuffer(b);
        EXPECT_EQ(fb[0], 0);
        EXPECT_EQ(fb[1024 * 600 * 2 - 1], 0);
    }

    Deki::IDisplay* display = Deki::Engine::GetInstance().GetDisplay();
    ASSERT_NE(display, nullptr);
    int32_t w = 0, h = 0;
    display->GetDisplaySize(&w, &h);
    EXPECT_EQ(w, 1024);
    EXPECT_EQ(h, 600);
    EXPECT_EQ(display->GetColorFormat(), Deki::ColorFormat::RGB565);
    // Whole frames into its own buffers, never strips.
    EXPECT_FALSE(display->SupportsPartialPresent());
    EXPECT_EQ(display->GetRefreshPeriodUs(), 17882u);

    // Touch is laid over it at its size.
    int32_t tw = 0, th = 0;
    ASSERT_TRUE(DsiPanelComponent::GetStartedSize(tw, th));
    EXPECT_EQ(tw, 1024);
    EXPECT_EQ(th, 600);
    DsiPanelComponent::Stop();
    EXPECT_FALSE(DsiPanelComponent::GetStartedSize(tw, th));
    EXPECT_EQ(Deki::Engine::GetInstance().GetDisplay(), nullptr);
}

TEST_F(DsiTest, AStartUpThatDoesNotReadIsRefused)
{
    g_NextPanel = std::make_unique<FakeDsiPanel>(8, 4, 2);
    MipiDsi::SetFactory(&MakeFakePanel);
    DsiPanelComponent c;
    Configure(c);
    c.initSequence = "11; delay\n";
    EXPECT_FALSE(RunSetup(c));
    EXPECT_TRUE(Of(Step::Command).empty());
}

TEST_F(DsiTest, APanelWithoutItsPixelClockIsRefused)
{
    g_NextPanel = std::make_unique<FakeDsiPanel>(8, 4, 2);
    MipiDsi::SetFactory(&MakeFakePanel);
    DsiPanelComponent c;
    Configure(c);
    c.pixelClockHz = 0;
    EXPECT_FALSE(RunSetup(c));
    EXPECT_EQ(g_Panel, nullptr);  // the platform was not asked
}

TEST_F(DsiTest, ACommandThePanelRefusesStopsTheStart)
{
    g_NextPanel = std::make_unique<FakeDsiPanel>(8, 4, 2);
    g_NextPanel->refuseCommands = true;
    MipiDsi::SetFactory(&MakeFakePanel);
    DsiPanelComponent c;
    Configure(c);
    EXPECT_FALSE(RunSetup(c));
    EXPECT_EQ(Of(Step::Command).size(), 1u);
    EXPECT_TRUE(Of(Step::Start).empty());
}

// --- DsiDisplay ---

TEST_F(DsiTest, TheGameDrawsIntoTheBufferNotOnScreen)
{
    FakeDsiPanel panel(8, 4, 2);
    DsiDisplay display(panel, 8, 4, 16667);
    ASSERT_TRUE(display.Start({}));
    ASSERT_EQ(display.GetShownIndex(), 0);

    for (int frame = 0; frame < 4; ++frame)
    {
        const int32_t onScreen = display.GetShownIndex();
        g_Steps.clear();
        int32_t w = 0, h = 0;
        uint8_t* buffer = display.GetRenderBuffer(&w, &h);
        EXPECT_EQ(w, 8);
        EXPECT_EQ(h, 4);
        // Not the one shown, nor one the panel may still be sending: with
        // two, after a frame each waits for the refresh sending the other.
        EXPECT_EQ(buffer, panel.GetFrameBuffer(1 - onScreen));
        EXPECT_EQ(panel.GetBusyMask() & (1u << (1 - onScreen)), 0u);
        if (frame > 0)
        {
            EXPECT_EQ(Of(Step::Wait).size(), 1u);
        }
        // Asked again before Present: the same buffer, no more waiting.
        EXPECT_EQ(display.GetRenderBuffer(nullptr, nullptr), buffer);

        display.Present(buffer, 8, 4, Deki::ColorFormat::RGB565);
        const std::vector<Step> shows = Of(Step::Show);
        ASSERT_EQ(shows.size(), 1u);
        EXPECT_EQ(shows[0].index, 1 - onScreen);
        EXPECT_EQ(display.GetShownIndex(), 1 - onScreen);
    }
}

TEST_F(DsiTest, WithThreeBuffersDrawingNeverWaits)
{
    FakeDsiPanel panel(8, 4, 3);
    DsiDisplay display(panel, 8, 4, 16667);
    ASSERT_TRUE(display.Start({}));
    g_Steps.clear();
    for (int frame = 0; frame < 9; ++frame)
    {
        uint8_t* buffer = display.GetRenderBuffer(nullptr, nullptr);
        int32_t index = -1;
        for (int32_t i = 0; i < 3; ++i)
        {
            if (panel.GetFrameBuffer(i) == buffer)
            {
                index = i;
            }
        }
        ASSERT_GE(index, 0);
        // Never one the panel may read.
        EXPECT_EQ(panel.GetBusyMask() & (1u << index), 0u) << "frame " << frame;
        display.Present(buffer, 8, 4, Deki::ColorFormat::RGB565);
        // Refreshes end now and then between frames, or not at all.
        if (frame % 3 == 1)
        {
            panel.Refresh();
        }
    }
    EXPECT_TRUE(Of(Step::Wait).empty());
}

TEST_F(DsiTest, AFrameInTheGamesOwnBufferIsCopiedIntoTheFreeOne)
{
    FakeDsiPanel panel(8, 4, 2);
    DsiDisplay display(panel, 8, 4, 16667);
    ASSERT_TRUE(display.Start({}));

    std::vector<uint8_t> own(8 * 4 * 2);
    for (size_t i = 0; i < own.size(); ++i)
    {
        own[i] = static_cast<uint8_t>(i);
    }
    display.Present(own.data(), 8, 4, Deki::ColorFormat::RGB565);
    EXPECT_EQ(display.GetShownIndex(), 1);
    EXPECT_EQ(std::memcmp(panel.GetFrameBuffer(1), own.data(), own.size()), 0);
}

TEST_F(DsiTest, AFrameOfAnotherSizeOrFormatIsNotShown)
{
    FakeDsiPanel panel(8, 4, 2);
    DsiDisplay display(panel, 8, 4, 16667);
    ASSERT_TRUE(display.Start({}));
    g_Steps.clear();
    std::vector<uint8_t> wide(8 * 4 * 4);
    display.Present(wide.data(), 8, 4, Deki::ColorFormat::ARGB8888);
    display.Present(wide.data(), 4, 4, Deki::ColorFormat::RGB565);
    EXPECT_TRUE(Of(Step::Show).empty());
    EXPECT_EQ(display.GetShownIndex(), 0);
}

TEST_F(DsiTest, WithOneBufferFramesAreNotTimedToTheRefresh)
{
    FakeDsiPanel panel(8, 4, 1);
    DsiDisplay display(panel, 8, 4, 16667);
    ASSERT_TRUE(display.Start({}));
    EXPECT_EQ(display.GetRenderBuffer(nullptr, nullptr), panel.GetFrameBuffer(0));
    EXPECT_EQ(display.GetRefreshPeriodUs(), 0u);
}

TEST_F(DsiTest, APanelThatNeverRefreshesStillGetsFrames)
{
    // A wait that times out is logged and drawing carries on: a stuck panel
    // must not hang the game.
    FakeDsiPanel panel(8, 4, 2);
    panel.refreshes = false;
    DsiDisplay display(panel, 8, 4, 16667);
    ASSERT_TRUE(display.Start({}));
    uint8_t* buffer = display.GetRenderBuffer(nullptr, nullptr);
    ASSERT_NE(buffer, nullptr);
    display.Present(buffer, 8, 4, Deki::ColorFormat::RGB565);
    EXPECT_EQ(display.GetShownIndex(), 1);
}

TEST_F(DsiTest, APwmBacklightIsDimmedAndKeepsItsBrightness)
{
    FakeDsiPanel panel(8, 4, 2);
    DsiDisplay display(panel, 8, 4, 16667);
    DsiDisplay::Options options;
    options.backlightPin = 31;
    options.backlightPwmHz = 30000;
    options.brightness = 0.98f;
    ASSERT_TRUE(display.Start(options));
    EXPECT_TRUE(display.HasBrightness());
    EXPECT_EQ(panel.pwmPin, 31);
    EXPECT_EQ(panel.pwmHz, 30000u);
    EXPECT_TRUE(Of(Step::Pin).empty());  // the pin is the PWM's, not switched
    ASSERT_EQ(panel.levels.size(), 1u);
    EXPECT_FLOAT_EQ(panel.levels[0], 0.98f);

    display.SetBacklight(false);
    display.SetBrightness(0.5f);  // off: remembered, not shown
    display.SetBacklight(true);
    ASSERT_EQ(panel.levels.size(), 3u);
    EXPECT_FLOAT_EQ(panel.levels[1], 0.0f);
    EXPECT_FLOAT_EQ(panel.levels[2], 0.5f);
}

TEST_F(DsiTest, WithoutPlatformPwmTheBacklightPinIsSwitched)
{
    FakeDsiPanel panel(8, 4, 2);
    panel.hasPwm = false;
    DsiDisplay display(panel, 8, 4, 16667);
    DsiDisplay::Options options;
    options.backlightPin = 31;
    options.backlightPwmHz = 30000;
    ASSERT_TRUE(display.Start(options));
    EXPECT_FALSE(display.HasBrightness());
    const std::vector<Step> pins = Of(Step::Pin);
    ASSERT_FALSE(pins.empty());
    EXPECT_TRUE(pins.back().high);
    EXPECT_TRUE(panel.levels.empty());
}

TEST_F(DsiTest, TheBacklightFollowsTheEngine)
{
    FakeDsiPanel panel(8, 4, 2);
    DsiDisplay display(panel, 8, 4, 16667);
    DsiDisplay::Options options;
    options.backlightPin = 31;
    options.backlightInvert = true;
    ASSERT_TRUE(display.Start(options));
    g_Steps.clear();
    display.SetBacklight(false);
    display.SetBacklight(true);
    const std::vector<Step> pins = Of(Step::Pin);
    ASSERT_EQ(pins.size(), 2u);
    EXPECT_TRUE(pins[0].high);  // inverted: high is off
    EXPECT_FALSE(pins[1].high);

    uint64_t frames = 0, at = 0;
    EXPECT_TRUE(display.GetFramesShown(&frames, &at));
    EXPECT_EQ(frames, 7u);
    EXPECT_EQ(at, 1234u);
}

namespace
{

// One frame in two strips of two rows: each drawn in a buffer the display
// lends, filled with `top` and `bottom`, or left out where -1. Only rows
// [changedTop, changedBottom) of the second strip changed, when given.
void DrawStrips(DsiDisplay& display, int top, int bottom, int32_t changedTop = 2, int32_t changedBottom = 4)
{
    bool first = true;
    for (int s = 0; s < 2; ++s)
    {
        const int value = s == 0 ? top : bottom;
        if (value < 0)
        {
            continue;
        }
        uint8_t* buffer = display.GetStripBuffer(8 * 2 * 2);
        ASSERT_NE(buffer, nullptr);
        std::memset(buffer, value, 8 * 2 * 2);
        DekiRendering::ScanStrip strip;
        strip.pixels = buffer;
        strip.stride = 8;
        strip.strip = Deki::Rect{ 0, s * 2, 8, s * 2 + 2 };
        strip.changed = s == 1 ? Deki::Rect{ 0, changedTop, 8, changedBottom } : strip.strip;
        strip.firstOfFrame = first;
        first = false;
        display.PresentStrip(strip);
    }
    display.EndStrips();
}

// The value of every pixel's bytes in row `y` of what is on screen, -1 when
// they differ.
int ShownRow(FakeDsiPanel& panel, DsiDisplay& display, int32_t y)
{
    const uint8_t* row = panel.GetFrameBuffer(display.GetShownIndex()) + y * 8 * 2;
    for (int i = 1; i < 8 * 2; ++i)
    {
        if (row[i] != row[0])
        {
            return -1;
        }
    }
    return row[0];
}

}  // namespace

class DsiStripsTest : public DsiTest, public ::testing::WithParamInterface<bool>
{
};

TEST_P(DsiStripsTest, StripsArePutTogetherIntoAFrameShownWhole)
{
    FakeDsiPanel panel(8, 4, 3);
    panel.copiesRows = GetParam();
    DsiDisplay display(panel, 8, 4, 16667);
    DsiDisplay::Options options;
    options.strips = true;
    ASSERT_TRUE(display.Start(options));
    EXPECT_EQ(DekiRendering::FindScanOrderedDisplay(&display), &display);
    EXPECT_TRUE(display.LendsStripBuffers());

    g_Steps.clear();
    DrawStrips(display, 0x11, 0x22);
    // One Show, of the whole frame, after both strips.
    ASSERT_EQ(Of(Step::Show).size(), 1u);
    EXPECT_NE(display.GetShownIndex(), 0);
    EXPECT_EQ(ShownRow(panel, display, 0), 0x11);
    EXPECT_EQ(ShownRow(panel, display, 1), 0x11);
    EXPECT_EQ(ShownRow(panel, display, 2), 0x22);
    EXPECT_EQ(ShownRow(panel, display, 3), 0x22);
    if (GetParam())
    {
        EXPECT_EQ(panel.rowCopies, 2);  // by the platform, a strip each
    }

    // Next frame: the top strip did not change, and of the bottom one only
    // its last row. The rest is the frame shown before.
    const int32_t before = display.GetShownIndex();
    DrawStrips(display, -1, 0x33, 3, 4);
    EXPECT_NE(display.GetShownIndex(), before);
    EXPECT_EQ(ShownRow(panel, display, 0), 0x11);
    EXPECT_EQ(ShownRow(panel, display, 1), 0x11);
    EXPECT_EQ(ShownRow(panel, display, 2), 0x22);
    EXPECT_EQ(ShownRow(panel, display, 3), 0x33);

    // A whole frame between strip frames is shown as before.
    uint8_t* whole = display.GetRenderBuffer(nullptr, nullptr);
    std::memset(whole, 0x44, 8 * 4 * 2);
    display.Present(whole, 8, 4, Deki::ColorFormat::RGB565);
    for (int32_t y = 0; y < 4; ++y)
    {
        EXPECT_EQ(ShownRow(panel, display, y), 0x44);
    }
    DrawStrips(display, 0x55, -1);
    EXPECT_EQ(ShownRow(panel, display, 0), 0x55);
    EXPECT_EQ(ShownRow(panel, display, 3), 0x44);

    display.Shutdown();
    EXPECT_EQ(DekiRendering::FindScanOrderedDisplay(&display), nullptr);
}

INSTANTIATE_TEST_SUITE_P(ByCpuAndByPlatform, DsiStripsTest, ::testing::Bool());

// A frame drawn in part copies from the frame shown only the rows its buffer
// does not hold as shown: with three buffers, those the two frames before it
// drew. Every row it did not draw was copied before, a whole frame's worth.
TEST_F(DsiTest, AFrameDrawnInPartCopiesOnlyTheRowsItsBufferMissed)
{
    FakeDsiPanel panel(8, 4, 3);
    panel.copiesRows = true;
    DsiDisplay display(panel, 8, 4, 16667);
    DsiDisplay::Options options;
    options.strips = true;
    ASSERT_TRUE(display.Start(options));
    for (int f = 0; f < 3; ++f)
    {
        DrawStrips(display, 0x11, 0x22);  // every buffer gets the whole picture
    }

    // Only the last row changes, frame after frame: nothing else is copied.
    panel.rowsBetween = 0;
    for (int f = 0; f < 6; ++f)
    {
        DrawStrips(display, -1, 0x30 + f, 3, 4);
        EXPECT_EQ(ShownRow(panel, display, 0), 0x11) << f;
        EXPECT_EQ(ShownRow(panel, display, 2), 0x22) << f;
        EXPECT_EQ(ShownRow(panel, display, 3), 0x30 + f) << f;
    }
    // At most a buffer the warm-up did not reach, once (3 rows). Copying every
    // row not drawn would be 3 a frame: 18.
    EXPECT_LE(panel.rowsBetween, 3);

    // The top strip changes once; the buffers drawn after it, which missed
    // it, get its two rows from the frame shown, and no more.
    DrawStrips(display, 0x66, -1);
    panel.rowsBetween = 0;
    for (int f = 0; f < 3; ++f)
    {
        DrawStrips(display, -1, 0x40 + f, 3, 4);
        EXPECT_EQ(ShownRow(panel, display, 0), 0x66) << f;
        EXPECT_EQ(ShownRow(panel, display, 1), 0x66) << f;
        EXPECT_EQ(ShownRow(panel, display, 2), 0x22) << f;
        EXPECT_EQ(ShownRow(panel, display, 3), 0x40 + f) << f;
    }
    // Each buffer that missed it, two rows each: 2 or 4, as the panel frees them.
    EXPECT_GE(panel.rowsBetween, 2);
    EXPECT_LE(panel.rowsBetween, 2 * 2);

    // A whole frame leaves every other buffer stale everywhere.
    uint8_t* whole = display.GetRenderBuffer(nullptr, nullptr);
    std::memset(whole, 0x77, 8 * 4 * 2);
    display.Present(whole, 8, 4, Deki::ColorFormat::RGB565);
    DrawStrips(display, -1, 0x50, 3, 4);
    for (int32_t y = 0; y < 3; ++y)
    {
        EXPECT_EQ(ShownRow(panel, display, y), 0x77) << y;
    }
    EXPECT_EQ(ShownRow(panel, display, 3), 0x50);
    display.Shutdown();
}

// Part of a strip, the same part frame after frame (a widget animating): only
// it is written, nothing is copied from the frame shown, and the pixels round
// it are right. Moved elsewhere, the columns it left are brought up to date.
TEST_F(DsiTest, PartsOfStripsCopyOnlyWhatTheirBufferMissed)
{
    FakeDsiPanel panel(8, 4, 3);
    panel.copiesRows = true;
    DsiDisplay display(panel, 8, 4, 16667);
    DsiDisplay::Options options;
    options.strips = true;
    ASSERT_TRUE(display.Start(options));
    EXPECT_TRUE(display.SendsPartOfAStrip());
    for (int f = 0; f < 3; ++f)
    {
        DrawStrips(display, 0x11, 0x22);  // every buffer gets the whole picture
    }
    auto part = [&](int value, int32_t x0, int32_t x1)
    {
        uint8_t* buffer = display.GetStripBuffer(8 * 2 * 2);
        ASSERT_NE(buffer, nullptr);
        std::memset(buffer, value, 8 * 2 * 2);
        DekiRendering::ScanStrip strip;
        strip.pixels = buffer;
        strip.stride = 8;
        strip.strip = Deki::Rect{ 0, 2, 8, 4 };
        strip.changed = Deki::Rect{ x0, 2, x1, 4 };
        strip.firstOfFrame = true;
        display.PresentStrip(strip);
        display.EndStrips();
    };
    auto at = [&](int32_t x, int32_t y)
    { return panel.GetFrameBuffer(display.GetShownIndex())[(static_cast<size_t>(y) * 8 + x) * 2]; };
    auto expectRows = [&](int left, int32_t x0, int32_t x1, int inside, int right, const char* what)
    {
        for (int32_t y = 0; y < 4; ++y)
        {
            for (int32_t x = 0; x < 8; ++x)
            {
                const int want = y < 2 ? 0x11 : (x < x0 ? left : (x < x1 ? inside : right));
                EXPECT_EQ(at(x, y), want) << what << ", x " << x << ", y " << y;
            }
        }
    };

    panel.rowsBetween = 0;
    for (int f = 0; f < 6; ++f)
    {
        part(0x30 + f, 2, 5);
        expectRows(0x22, 2, 5, 0x30 + f, 0x22, "the same part");
    }
    // At most once the two rows above, into a buffer the warm-up did not
    // reach. Copying the strip's rows each frame, as before, was 2 a frame.
    EXPECT_LE(panel.rowsBetween, 2) << "nothing copied between buffers but once";

    // Moved right: the columns it left keep its last value in every buffer.
    for (int f = 0; f < 4; ++f)
    {
        part(0x60 + f, 5, 8);
        for (int32_t y = 2; y < 4; ++y)
        {
            for (int32_t x = 0; x < 8; ++x)
            {
                const int want = x < 2 ? 0x22 : (x < 5 ? 0x35 : 0x60 + f);
                EXPECT_EQ(at(x, y), want) << "moved, frame " << f << ", x " << x << ", y " << y;
            }
        }
    }
    display.Shutdown();
}

// Two parts of one strip in a frame, apart: each lands where it belongs, and
// what lies between them is the frame shown before in every buffer, though
// one of the buffers held it stale.
TEST_F(DsiTest, TwoPartsOfAStripKeepWhatIsBetweenThem)
{
    FakeDsiPanel panel(8, 4, 3);
    panel.copiesRows = true;
    DsiDisplay display(panel, 8, 4, 16667);
    DsiDisplay::Options options;
    options.strips = true;
    ASSERT_TRUE(display.Start(options));
    for (int f = 0; f < 3; ++f)
    {
        DrawStrips(display, 0x11, 0x22);
    }
    auto send = [&](int value, int32_t x0, int32_t x1, bool first)
    {
        uint8_t* buffer = display.GetStripBuffer(8 * 2 * 2);
        ASSERT_NE(buffer, nullptr);
        std::memset(buffer, value, 8 * 2 * 2);
        DekiRendering::ScanStrip strip;
        strip.pixels = buffer;
        strip.stride = 8;
        strip.strip = Deki::Rect{ 0, 2, 8, 4 };
        strip.changed = Deki::Rect{ x0, 2, x1, 4 };
        strip.firstOfFrame = first;
        display.PresentStrip(strip);
    };
    auto at = [&](int32_t x, int32_t y)
    { return panel.GetFrameBuffer(display.GetShownIndex())[(static_cast<size_t>(y) * 8 + x) * 2]; };

    // The middle changes once: the buffers drawn after hold it stale.
    send(0x50, 3, 5, true);
    display.EndStrips();
    for (int f = 0; f < 5; ++f)
    {
        send(0x30 + f, 0, 2, true);
        send(0x60 + f, 6, 8, false);
        display.EndStrips();
        for (int32_t y = 2; y < 4; ++y)
        {
            for (int32_t x = 0; x < 8; ++x)
            {
                const int want = x < 2 ? 0x30 + f : (x < 3 ? 0x22 : (x < 5 ? 0x50 : (x < 6 ? 0x22 : 0x60 + f)));
                EXPECT_EQ(at(x, y), want) << "frame " << f << ", x " << x << ", y " << y;
            }
        }
        EXPECT_EQ(at(0, 0), 0x11);
    }
    display.Shutdown();
}

// Hundreds of frames of changes at random, sent as the renderer sends them
// (whole rows, or parts of a strip, two apart in one strip sometimes, strips
// with nothing changed left out): after every frame the panel shows exactly
// the picture, whatever each of the three buffers held stale. Each part is
// drawn in a strip buffer whose other pixels are garbage, as the renderer
// leaves them.
TEST_F(DsiTest, FramesOfRandomPartsAlwaysShowThePicture)
{
    constexpr int32_t W = 24, H = 12, kStrip = 3;
    for (bool byPlatform : { false, true })
    {
        FakeDsiPanel panel(W, H, 3);
        panel.copiesRows = byPlatform;
        DsiDisplay display(panel, W, H, 16667);
        DsiDisplay::Options options;
        options.strips = true;
        ASSERT_TRUE(display.Start(options));
        std::vector<uint8_t> picture(static_cast<size_t>(W) * H, 0x10);
        uint32_t seed = 12345u + (byPlatform ? 7u : 0u);
        auto rnd = [&](int n)
        {
            seed = seed * 1103515245u + 12345u;
            return static_cast<int>((seed >> 16) % static_cast<uint32_t>(n));
        };
        auto send = [&](const Deki::Rect& part, int32_t top, bool& first)
        {
            uint8_t* buffer = display.GetStripBuffer(static_cast<size_t>(W) * kStrip * 2);
            ASSERT_NE(buffer, nullptr);
            std::memset(buffer, 0xEE, static_cast<size_t>(W) * kStrip * 2);  // what it held before
            for (int32_t y = part.top; y < part.bottom; ++y)
            {
                for (int32_t x = part.left; x < part.right; ++x)
                {
                    const uint8_t v = picture[static_cast<size_t>(y) * W + x];
                    buffer[(static_cast<size_t>(y - top) * W + x) * 2] = v;
                    buffer[(static_cast<size_t>(y - top) * W + x) * 2 + 1] = v;
                }
            }
            DekiRendering::ScanStrip strip;
            strip.pixels = buffer;
            strip.stride = W;
            strip.strip = Deki::Rect{ 0, top, W, top + kStrip };
            strip.changed = part;
            strip.firstOfFrame = first;
            first = false;
            display.PresentStrip(strip);
        };
        // The first frame whole.
        bool first = true;
        for (int32_t top = 0; top < H; top += kStrip)
        {
            send(Deki::Rect{ 0, top, W, top + kStrip }, top, first);
        }
        display.EndStrips();
        for (int frame = 0; frame < 400; ++frame)
        {
            // One to three changes, each a rect of a new value.
            std::vector<Deki::Rect> changes;
            const int n = 1 + rnd(3);
            for (int c = 0; c < n; ++c)
            {
                const int32_t x0 = rnd(W), y0 = rnd(H);
                const Deki::Rect r{ x0, y0, std::min(W, x0 + 1 + rnd(W / 2)), std::min(H, y0 + 1 + rnd(4)) };
                const uint8_t v = static_cast<uint8_t>(0x20 + rnd(200));
                for (int32_t y = r.top; y < r.bottom; ++y)
                {
                    for (int32_t x = r.left; x < r.right; ++x)
                    {
                        picture[static_cast<size_t>(y) * W + x] = v;
                    }
                }
                changes.push_back(r);
            }
            first = true;
            for (int32_t top = 0; top < H; top += kStrip)
            {
                // The strip's parts: its changes cut to it, merged where they
                // overlap along it, so the parts lie apart.
                std::vector<Deki::Rect> parts;
                for (const Deki::Rect& r : changes)
                {
                    const Deki::Rect c{ r.left, std::max(r.top, top), r.right, std::min(r.bottom, top + kStrip) };
                    if (c.bottom > c.top)
                    {
                        parts.push_back(c);
                    }
                }
                std::sort(parts.begin(), parts.end(),
                          [](const Deki::Rect& a, const Deki::Rect& b) { return a.left < b.left; });
                std::vector<Deki::Rect> merged;
                for (const Deki::Rect& c : parts)
                {
                    if (!merged.empty() && c.left <= merged.back().right)
                    {
                        Deki::Rect& m = merged.back();
                        m = Deki::Rect{ m.left, std::min(m.top, c.top), std::max(m.right, c.right),
                                        std::max(m.bottom, c.bottom) };
                    }
                    else
                    {
                        merged.push_back(c);
                    }
                }
                for (const Deki::Rect& part : merged)
                {
                    send(part, top, first);
                }
            }
            if (!first)
            {
                display.EndStrips();
            }
            const uint8_t* shown = panel.GetFrameBuffer(display.GetShownIndex());
            int wrong = 0;
            for (int32_t i = 0; i < W * H; ++i)
            {
                wrong += shown[i * 2] != picture[static_cast<size_t>(i)] ? 1 : 0;
            }
            ASSERT_EQ(wrong, 0) << "frame " << frame << (byPlatform ? ", copied by the platform" : "");
        }
        display.Shutdown();
    }
}

TEST_F(DsiTest, WithoutStripsTheDisplayLendsNoStripBuffers)
{
    FakeDsiPanel panel(8, 4, 2);
    DsiDisplay display(panel, 8, 4, 16667);
    ASSERT_TRUE(display.Start({}));
    EXPECT_EQ(DekiRendering::FindScanOrderedDisplay(&display), nullptr);
    EXPECT_FALSE(display.LendsStripBuffers());
    EXPECT_EQ(display.GetStripBuffer(64), nullptr);
}
