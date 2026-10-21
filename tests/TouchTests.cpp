// deki-display's touch off a device:
//
// - TouchMapper: raw coordinates to screen pixels, as LovyanGFX maps them.
// - GT911Touch: its start-up, address fallback, wake pulse and reports,
//   against an I2C bus that answers as a GT911 does.
// - PanelTouchInput: the press, moves and release it sends.
// - TouchPanelComponent: a boot scene runs where there is no I2C.

#include <gtest/gtest.h>

#include "DekiGPIO.h"
#include "DekiI2C.h"
#include "GT911Touch.h"
#include "PanelTouchInput.h"
#include "TouchMapper.h"
#include "TouchPanelComponent.h"

#include <deki/PackageConfig.h>

#include <map>
#include <memory>
#include <vector>

using namespace DekiDisplay;

namespace
{

// --- A GT911 on a fake I2C bus ---

class FakeGT911Bus : public DekiI2c::IDekiI2C
{
public:
    uint8_t answersOn = 0x5D;
    uint8_t status = 0;  // at 0x814E
    std::vector<uint8_t> points;  // from 0x814F
    int clears = 0;
    int reads = 0;
    std::vector<uint8_t> asked;  // addresses written to

    const char* GetPackageId() const override { return "i2c"; }
    const char* GetPackageName() const override { return "Fake GT911 bus"; }
    void Configure(const Deki::PackageConfig&) override {}
    bool Initialize() override { return true; }
    void Shutdown() override {}
    void Update(float) override {}
    Deki::PackageState GetState() const override { return Deki::PackageState::Running; }
    const char* GetLastError() const override { return ""; }
    int GetPort() const override { return 0; }
    int GetSdaPin() const override { return 18; }
    int GetSclPin() const override { return 8; }
    int GetFrequencyHz() const override { return 400000; }
    bool Probe(uint8_t addr) override { return addr == answersOn; }
    bool Read(uint8_t, uint8_t, uint8_t*, size_t) override { return false; }
    bool Write(uint8_t, uint8_t, const uint8_t*, size_t) override { return false; }

    bool WriteRaw(uint8_t addr, const uint8_t* tx, size_t len) override
    {
        asked.push_back(addr);
        if (addr != answersOn)
        {
            return false;
        }
        if (len == 3 && tx[0] == 0x81 && tx[1] == 0x4E && tx[2] == 0x00)
        {
            ++clears;
            status = 0;
        }
        return true;
    }
    bool WriteRead(uint8_t addr, const uint8_t* tx, size_t txLen, uint8_t* rx, size_t rxLen) override
    {
        if (addr != answersOn || txLen != 2 || tx[0] != 0x81)
        {
            return false;
        }
        ++reads;
        if (tx[1] == 0x4E)
        {
            rx[0] = status;
            return rxLen == 1;
        }
        if (tx[1] == 0x4F && rxLen <= points.size())
        {
            std::copy(points.begin(), points.begin() + rxLen, rx);
            return true;
        }
        return false;
    }

    // One finger at (x, y), as a report waiting.
    void Touch(uint16_t x, uint16_t y)
    {
        status = 0x81;
        points = { 0, static_cast<uint8_t>(x), static_cast<uint8_t>(x >> 8), static_cast<uint8_t>(y),
                   static_cast<uint8_t>(y >> 8), 20, 0, 0 };
    }
};

class FakeGpio : public DekiGpio::IDekiGPIO
{
public:
    std::map<int, bool> level;
    std::map<int, bool> output;
    std::vector<std::pair<int, bool>> writes;

    bool SetOutput(int pin, bool high) override
    {
        output[pin] = true;
        return Write(pin, high);
    }
    bool SetInput(int pin, DekiGpio::Pull) override
    {
        output[pin] = false;
        return true;
    }
    bool Write(int pin, bool high) override
    {
        level[pin] = high;
        writes.emplace_back(pin, high);
        return true;
    }
    bool Read(int pin) override { return level.count(pin) ? level[pin] : true; }
    bool CountEdges(int, DekiGpio::Edge, DekiGpio::Pull) override { return false; }
    uint32_t TakeEdges(int) override { return 0; }
    void StopCounting(int) override {}
};

uint32_t g_Now = 1000;
uint32_t Now()
{
    return g_Now;
}
std::vector<uint32_t> g_Delays;
void Delay(uint32_t ms)
{
    g_Delays.push_back(ms);
    g_Now += ms;
}

}  // namespace

// --- TouchMapper ---

TEST(TouchMapper, ThePanelsOwnOrientationIsUnchanged)
{
    TouchMapper m;  // 240x320, raw 0-239 and 0-319, not rotated
    int32_t x = 0;
    int32_t y = 0;
    m.Map(10, 20, x, y);
    EXPECT_EQ(x, 10);
    EXPECT_EQ(y, 20);
}

TEST(TouchMapper, TheTDecksLandscapeScreen)
{
    // The T-Deck Plus: a 240x320 panel shown in landscape (rotation 1), its
    // GT911 reporting in the panel's own orientation.
    TouchMapper m;
    m.displayRotation = 1;
    int32_t x = 0;
    int32_t y = 0;
    m.Map(0, 0, x, y);
    EXPECT_EQ(x, 0);
    EXPECT_EQ(y, 239);
    m.Map(239, 319, x, y);
    EXPECT_EQ(x, 319);
    EXPECT_EQ(y, 0);
    m.Map(100, 50, x, y);
    EXPECT_EQ(x, 50);
    EXPECT_EQ(y, 139);
}

TEST(TouchMapper, TheRawRangeIsScaledToThePanel)
{
    TouchMapper m;  // a resistive controller's 12-bit readings
    m.xMin = 200;
    m.xMax = 3900;
    m.yMin = 300;
    m.yMax = 3800;
    int32_t x = 0;
    int32_t y = 0;
    m.Map(200, 300, x, y);
    EXPECT_EQ(x, 0);
    EXPECT_EQ(y, 0);
    m.Map(3900, 3800, x, y);
    EXPECT_EQ(x, 239);
    EXPECT_EQ(y, 319);
}

TEST(TouchMapper, AControllerTurnedWithTheDisplayCancelsOut)
{
    // Turned a quarter against the panel, reporting in landscape already:
    // with the display in landscape too, 3 + 1 quarter turns is no turn.
    TouchMapper m;
    m.xMax = 319;
    m.yMax = 239;
    m.displayRotation = 1;
    m.offsetRotation = 3;
    int32_t x = 0;
    int32_t y = 0;
    m.Map(319, 239, x, y);
    EXPECT_EQ(x, 319);
    EXPECT_EQ(y, 239);
    m.Map(5, 7, x, y);
    EXPECT_EQ(x, 5);
    EXPECT_EQ(y, 7);
}

// --- GT911Touch ---

class GT911Test : public ::testing::Test
{
protected:
    void SetUp() override
    {
        g_Now = 1000;
        g_Delays.clear();
    }
    FakeGT911Bus m_Bus;
    FakeGpio m_Gpio;
};

TEST_F(GT911Test, StartsOnItsAddressAndWakesWithAPulseOnInt)
{
    GT911Touch touch(&m_Bus, &m_Gpio, 0, 16, -1, &Delay, &Now);
    ASSERT_TRUE(touch.Init());
    EXPECT_EQ(touch.GetAddress(), 0x5D);
    EXPECT_EQ(m_Bus.clears, 1);
    // INT driven low, then high, then left as an input for the reports.
    ASSERT_GE(m_Gpio.writes.size(), 2u);
    EXPECT_EQ(m_Gpio.writes[0], std::make_pair(16, false));
    EXPECT_EQ(m_Gpio.writes[1], std::make_pair(16, true));
    EXPECT_FALSE(m_Gpio.output[16]);
}

TEST_F(GT911Test, FallsBackToTheOtherAddress)
{
    m_Bus.answersOn = 0x14;
    GT911Touch touch(&m_Bus, nullptr, 0, -1, -1, &Delay, &Now);
    ASSERT_TRUE(touch.Init());
    EXPECT_EQ(touch.GetAddress(), 0x14);
    EXPECT_EQ(m_Bus.asked.front(), 0x5D);
}

TEST_F(GT911Test, AControllerThatNeverAnswersDoesNotStart)
{
    m_Bus.answersOn = 0x33;
    GT911Touch touch(&m_Bus, nullptr, 0, -1, -1, &Delay, &Now);
    EXPECT_FALSE(touch.Init());
    TouchPoint p;
    EXPECT_EQ(touch.Read(&p, 1), 0);
}

TEST_F(GT911Test, AReportIsReadWhileIntIsLowThenCleared)
{
    GT911Touch touch(&m_Bus, &m_Gpio, 0, 16, -1, &Delay, &Now);
    ASSERT_TRUE(touch.Init());
    // Read every frame, as the game does: the first read after start finds
    // nothing (and would drop a report from long ago).
    TouchPoint p;
    m_Gpio.level[16] = false;
    EXPECT_EQ(touch.Read(&p, 1), 0);
    const int clears = m_Bus.clears;

    m_Bus.Touch(0x0123, 0x0045);  // a report is waiting
    g_Now += 16;
    ASSERT_EQ(touch.Read(&p, 1), 1);
    EXPECT_EQ(p.x, 0x0123);
    EXPECT_EQ(p.y, 0x0045);
    EXPECT_EQ(m_Bus.clears, clears + 1);

    // INT high: nothing new is read, and the last report holds.
    m_Gpio.level[16] = true;
    const int reads = m_Bus.reads;
    g_Now += 16;
    ASSERT_EQ(touch.Read(&p, 1), 1);
    EXPECT_EQ(m_Bus.reads, reads);
    EXPECT_EQ(p.x, 0x0123);

    // A report with no finger: lifted.
    m_Bus.status = 0x80;
    m_Gpio.level[16] = false;
    g_Now += 16;
    EXPECT_EQ(touch.Read(&p, 1), 0);
}

TEST_F(GT911Test, AReportFromLongAgoIsDroppedForAFreshOne)
{
    GT911Touch touch(&m_Bus, nullptr, 0, -1, -1, &Delay, &Now);
    ASSERT_TRUE(touch.Init());
    m_Bus.Touch(10, 20);  // left over: cleared before anything is read
    g_Now += 500;
    TouchPoint p;
    EXPECT_EQ(touch.Read(&p, 1), 0);
    // It waited a little for a fresh report, 1 ms at a time.
    EXPECT_FALSE(g_Delays.empty());
}

// --- PanelTouchInput ---

namespace
{

// Reports whatever the test sets.
class ScriptedTouch : public ITouchController
{
public:
    bool down = false;
    uint16_t x = 0;
    uint16_t y = 0;
    bool Init() override { return true; }
    int Read(TouchPoint* points, int max) override
    {
        if (!down || max < 1)
        {
            return 0;
        }
        points[0].x = x;
        points[0].y = y;
        return 1;
    }
};

struct Recorded
{
    DekiInput::InputEventType type;
    int32_t x;
    int32_t y;
};

}  // namespace

TEST(PanelTouchInput, APressMovesAndARelease)
{
    auto scripted = std::make_unique<ScriptedTouch>();
    ScriptedTouch* touch = scripted.get();
    PanelTouchInput input(std::move(scripted), TouchMapper{}, 16, &Now);
    ASSERT_TRUE(input.Initialize());
    std::vector<Recorded> events;
    input.RegisterEventCallback([&](const DekiInput::InputEvent& e) { events.push_back({ e.type, e.x, e.y }); });

    input.Update();  // nothing touched
    touch->down = true;
    touch->x = 10;
    touch->y = 20;
    input.Update();
    input.Update();  // held still: nothing new
    touch->x = 12;
    input.Update();
    int32_t px = 0;
    int32_t py = 0;
    EXPECT_TRUE(input.GetPointerPosition(&px, &py));
    EXPECT_EQ(px, 12);
    touch->down = false;
    input.Update();
    EXPECT_FALSE(input.GetPointerPosition(&px, &py));

    ASSERT_EQ(events.size(), 3u);
    EXPECT_EQ(events[0].type, DekiInput::InputEventType::MouseButtonDown);
    EXPECT_EQ(events[0].x, 10);
    EXPECT_EQ(events[1].type, DekiInput::InputEventType::MouseMove);
    EXPECT_EQ(events[1].x, 12);
    EXPECT_EQ(events[2].type, DekiInput::InputEventType::MouseButtonUp);
    EXPECT_EQ(events[2].x, 12);
}

TEST(PanelTouchInput, WithoutIntAStillPointIsLiftedAndNotPressedAgain)
{
    auto scripted = std::make_unique<ScriptedTouch>();
    ScriptedTouch* touch = scripted.get();
    PanelTouchInput input(std::move(scripted), TouchMapper{}, -1, &Now);
    ASSERT_TRUE(input.Initialize());
    std::vector<Recorded> events;
    input.RegisterEventCallback([&](const DekiInput::InputEvent& e) { events.push_back({ e.type, e.x, e.y }); });

    touch->down = true;
    touch->x = 30;
    touch->y = 40;
    for (int i = 0; i < 10; ++i)
    {
        input.Update();  // the controller keeps reporting the same point
    }
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].type, DekiInput::InputEventType::MouseButtonDown);
    EXPECT_EQ(events[1].type, DekiInput::InputEventType::MouseButtonUp);

    touch->x = 31;  // a finger again, somewhere new
    input.Update();
    ASSERT_EQ(events.size(), 3u);
    EXPECT_EQ(events[2].type, DekiInput::InputEventType::MouseButtonDown);
}

// --- TouchPanelComponent ---

namespace
{
DekiI2c::IDekiI2C* NoI2C()
{
    return nullptr;
}
bool RunSetup(TouchPanelComponent& c)
{
    bool reported = false;
    bool called = false;
    c.Setup(
        [&](bool ok)
        {
            reported = ok;
            called = true;
        });
    EXPECT_TRUE(called);
    return reported;
}
}  // namespace

TEST(TouchPanelComponent, ABootSceneRunsWhereThereIsNoI2C)
{
    DekiI2c::DekiI2C::SetFactory(nullptr);
    TouchPanelComponent c;
    EXPECT_TRUE(RunSetup(c));
}

TEST(TouchPanelComponent, TouchNeedsTheDisplayStartedFirst)
{
    DekiI2c::DekiI2C::SetFactory(&NoI2C);
    TouchPanelComponent c;
    EXPECT_FALSE(RunSetup(c));
    DekiI2c::DekiI2C::SetFactory(nullptr);
}
