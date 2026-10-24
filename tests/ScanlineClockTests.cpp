// ScanlineClock against a simulated panel: its line count as an ST7789 gives
// it (a dummy bit, a high byte that is always 0, the low byte), held through
// the blanking, and a clock that moves with every read.

#include <gtest/gtest.h>

#include "ScanlineClock.h"

#include <cstdint>

using namespace DekiDisplay;

namespace
{

// The simulated panel: a refresh of kPeriodUs, its blanking first (the count
// holds at 1), then 320 rows of kRowUs.
constexpr uint32_t kPeriodUs = 16600;
constexpr uint32_t kBlankingUs = 600;
constexpr uint32_t kRowUs = 50;
constexpr uint64_t kPhaseUs = 3700;  // when a refresh started
uint64_t g_Now = 0;

uint64_t Now()
{
    return g_Now += 3;  // reading the clock takes a little time
}

uint32_t AgeAt(uint64_t t)
{
    return static_cast<uint32_t>((t + kPeriodUs - kPhaseUs % kPeriodUs) % kPeriodUs);
}

class SimulatedPanel : public IPanelIO
{
public:
    bool readable = true;
    int reads = 0;

    bool Begin() override { return true; }
    void End() override {}
    bool Command(uint8_t, const uint8_t*, size_t) override { return true; }
    bool StartPixels(uint8_t) override { return true; }
    bool QueuePixels(const void*, size_t, bool) override { return true; }
    bool WaitPixels(size_t) override { return true; }
    uint32_t GetQueuedCount() const override { return 0; }
    size_t GetMaxChunkBytes() const override { return 4096; }
    bool PixelsBigEndian() const override { return true; }
    bool Read(uint8_t cmd, uint8_t* out, size_t n) override
    {
        if (!readable || cmd != 0x45 || n != 3)
        {
            return false;
        }
        ++reads;
        g_Now += 12;  // a read at 10 MHz, with the driver's time
        const uint32_t age = AgeAt(g_Now);
        const uint32_t line = age < kBlankingUs ? 1 : 1 + (age - kBlankingUs) / kRowUs;
        const uint8_t low = static_cast<uint8_t>(line);
        out[0] = 0x80;  // the dummy bit, then the high byte's top seven bits
        out[1] = static_cast<uint8_t>(low >> 1);
        out[2] = static_cast<uint8_t>((low & 1) << 7);
        return true;
    }
};

}  // namespace

TEST(ScanlineClock, DecodesTheLowByteAfterTheDummyBit)
{
    // As read from a LilyGO T-Deck's ST7789.
    const uint8_t a[3] = { 0x80, 0x0E, 0x00 };
    const uint8_t b[3] = { 0x80, 0x75, 0x80 };
    EXPECT_EQ(ScanlineClock::DecodeLow(a), 28);
    EXPECT_EQ(ScanlineClock::DecodeLow(b), 235);
}

TEST(ScanlineClock, FindsTheRefreshFromTheScanline)
{
    g_Now = 1000;
    SimulatedPanel panel;
    ScanlineClock clock(panel, &Now, nullptr);
    ASSERT_TRUE(clock.Calibrate());
    // To within a few reads, and not fooled by the low byte rolling over
    // from row 255 to 256.
    EXPECT_NEAR(static_cast<double>(clock.GetPeriodUs()), kPeriodUs, 40.0);
    for (int i = 0; i < 5; ++i)
    {
        g_Now += 3137;
        EXPECT_NEAR(static_cast<double>(clock.AgeUs(g_Now)), AgeAt(g_Now), 40.0);
    }
}

TEST(ScanlineClock, WaitsForTheNextStartAndKeepsTheClockRight)
{
    g_Now = 1000;
    SimulatedPanel panel;
    ScanlineClock clock(panel, &Now, nullptr);
    ASSERT_TRUE(clock.Calibrate());
    for (int i = 0; i < 20; ++i)
    {
        g_Now += 5000 + 431 * i;  // frames of all lengths
        clock.WaitForStart();
        // Just after a start: within the blanking it is caught in.
        EXPECT_LT(AgeAt(g_Now), kBlankingUs) << i;
    }
    EXPECT_NEAR(static_cast<double>(clock.GetPeriodUs()), kPeriodUs, 4.0);
}

TEST(ScanlineClock, WaitsForAnAge)
{
    g_Now = 1000;
    SimulatedPanel panel;
    ScanlineClock clock(panel, &Now, nullptr);
    ASSERT_TRUE(clock.Calibrate());
    g_Now += 7000;
    const uint32_t before = AgeAt(g_Now);
    clock.WaitForAge(before + 2500);
    EXPECT_NEAR(static_cast<double>(AgeAt(g_Now)), before + 2500, 60.0);
    // Older already: at once.
    const uint64_t at = g_Now;
    clock.WaitForAge(100);
    EXPECT_LT(g_Now - at, 20u);
}

TEST(ScanlineClock, APanelThatCannotBeReadIsNotCalibrated)
{
    g_Now = 1000;
    SimulatedPanel panel;
    panel.readable = false;
    ScanlineClock clock(panel, &Now, nullptr);
    EXPECT_FALSE(clock.Calibrate());
    EXPECT_FALSE(clock.IsCalibrated());
}
