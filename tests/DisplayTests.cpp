// deki-display off a device: what each layer puts on the wire, recorded by
// fakes of the layer below it.
//
// - InitSequence: the text a boot scene holds and the tables the controller
//   defaults are ported in.
// - PanelController: reset, start-up, MADCTL, rotation offsets and windows,
//   as bytes on a recording IPanelIO.
// - SpiPanelIO: DC and chip select per transfer, on a recording SPI device.
// - PanelDisplay: byte order, bands, merged regions and the overlay.
// - DisplayPanelComponent: a boot scene runs where there is no SPI.

#include <gtest/gtest.h>

#include "DisplayPanelComponent.h"
#include "InitSequence.h"
#include "PanelController.h"
#include "PanelDisplay.h"
#include "SpiPanelIO.h"

#include "DekiGPIO.h"
#include "DekiSPI.h"

#include <deki/Engine.h>

#include <cstring>
#include <string>
#include <vector>

using namespace DekiDisplay;

namespace
{

// What happened, in order, across the panel IO and the pins.
struct Event
{
    enum Kind
    {
        Command,
        Start,
        Pixels,
        Pin,
        Delay
    } kind;
    uint8_t cmd = 0;
    std::vector<uint8_t> bytes;
    bool more = false;
    const void* data = nullptr;
    int pin = -1;
    bool high = false;
    uint32_t ms = 0;
};
std::vector<Event> g_Events;

void RecordDelay(uint32_t ms)
{
    Event e{ Event::Delay };
    e.ms = ms;
    g_Events.push_back(e);
}

class RecordingIO : public IPanelIO
{
public:
    bool bigEndian = true;
    size_t maxChunk = 32768;
    int depth = 0;
    int begins = 0;

    bool Begin() override
    {
        ++depth;
        ++begins;
        return true;
    }
    void End() override { --depth; }
    bool Command(uint8_t cmd, const uint8_t* params, size_t n) override
    {
        Event e{ Event::Command };
        e.cmd = cmd;
        e.bytes.assign(params, params + n);
        g_Events.push_back(e);
        return true;
    }
    bool StartPixels(uint8_t cmd) override
    {
        Event e{ Event::Start };
        e.cmd = cmd;
        g_Events.push_back(e);
        return true;
    }
    bool QueuePixels(const void* data, size_t bytes, bool more) override
    {
        EXPECT_LE(bytes, maxChunk);
        Event e{ Event::Pixels };
        const auto* p = static_cast<const uint8_t*>(data);
        e.bytes.assign(p, p + bytes);
        e.more = more;
        e.data = data;
        g_Events.push_back(e);
        return true;
    }
    bool WaitPixels(size_t) override { return true; }
    size_t GetMaxChunkBytes() const override { return maxChunk; }
    bool PixelsBigEndian() const override { return bigEndian; }
};

class FakeGpio : public DekiGpio::IDekiGPIO
{
public:
    bool SetOutput(int pin, bool high) override { return Write(pin, high); }
    bool SetInput(int, DekiGpio::Pull) override { return true; }
    bool Write(int pin, bool high) override
    {
        Event e{ Event::Pin };
        e.pin = pin;
        e.high = high;
        g_Events.push_back(e);
        return true;
    }
    bool Read(int) override { return false; }
    bool CountEdges(int, DekiGpio::Edge, DekiGpio::Pull) override { return false; }
    uint32_t TakeEdges(int) override { return 0; }
    void StopCounting(int) override {}
};

std::vector<Event> Of(Event::Kind kind)
{
    std::vector<Event> out;
    for (const Event& e : g_Events)
    {
        if (e.kind == kind)
        {
            out.push_back(e);
        }
    }
    return out;
}

// The commands sent, as "2A 00 00 01 3F" strings, for readable comparisons.
std::vector<std::string> Commands()
{
    std::vector<std::string> out;
    for (const Event& e : Of(Event::Command))
    {
        char buf[8];
        std::string s;
        snprintf(buf, sizeof(buf), "%02X", e.cmd);
        s = buf;
        for (uint8_t b : e.bytes)
        {
            snprintf(buf, sizeof(buf), " %02X", b);
            s += buf;
        }
        out.push_back(s);
    }
    return out;
}

// The pixels sent, concatenated.
std::vector<uint8_t> Wire()
{
    std::vector<uint8_t> out;
    for (const Event& e : Of(Event::Pixels))
    {
        out.insert(out.end(), e.bytes.begin(), e.bytes.end());
    }
    return out;
}

// The LilyGO T-Deck Plus's start-up, as its boot scene holds it.
const char* kTDeckInit = R"(# LilyGO T-Deck ST7789: LilyGO TFT_eSPI INIT_SEQUENCE_2, VCOM tuned on a T-Deck Plus
11; delay 120          # SLPOUT first
13                     # NORON
B2 0C 0C 00 33 33      # PORCTRL
B7 75                  # GCTRL: VGH 14.97 V, VGL -10.43 V
BB 19                  # VCOMS 0.725 V
C0 2C                  # LCMCTRL (so rgbOrder on)
C2 01; C3 13; C4 20; C6 0F; D0 A4 A1
E0 D0 0D 14 0D 0D 09 38 44 4E 3A 17 18 2F 30
E1 D0 09 0F 08 07 14 37 44 4D 38 15 16 2C 3E
29; delay 120
)";

class DisplayTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        g_Events.clear();
        DekiGpio::DekiGPIO::SetCurrent(&m_Gpio);
    }
    void TearDown() override
    {
        DekiGpio::DekiGPIO::SetCurrent(nullptr);
        DekiSpi::DekiSPI::SetFactory(nullptr);
    }

    // A small ST7789 panel, so frames are a few bytes.
    static PanelConfig SmallPanel(int32_t w, int32_t h)
    {
        PanelConfig c;
        c.panelWidth = w;
        c.panelHeight = h;
        c.rotation = 0;
        c.rgbOrder = true;
        return c;
    }

    // An RGB565 frame of `pixels` in memory's byte order, as the engine
    // draws it in this build.
    static std::vector<uint8_t> Frame565(const std::vector<uint16_t>& pixels)
    {
        std::vector<uint8_t> out;
        for (uint16_t p : pixels)
        {
            if (Deki::kRGB565BigEndian)
            {
                out.push_back(static_cast<uint8_t>(p >> 8));
                out.push_back(static_cast<uint8_t>(p));
            }
            else
            {
                out.push_back(static_cast<uint8_t>(p));
                out.push_back(static_cast<uint8_t>(p >> 8));
            }
        }
        return out;
    }

    FakeGpio m_Gpio;
};

}  // namespace

// --- InitSequence ---

TEST(InitSequenceText, ReadsTheTDeckSequence)
{
    InitSequence seq;
    std::string error;
    ASSERT_TRUE(ParseInitSequence(kTDeckInit, seq, error)) << error;
    ASSERT_EQ(seq.size(), 14u);
    EXPECT_EQ(seq[0].cmd, 0x11);
    EXPECT_TRUE(seq[0].params.empty());
    EXPECT_EQ(seq[0].delayMs, 120u);
    EXPECT_EQ(seq[2].cmd, 0xB2);
    EXPECT_EQ(seq[2].params, (std::vector<uint8_t>{ 0x0C, 0x0C, 0x00, 0x33, 0x33 }));
    EXPECT_EQ(seq[4].cmd, 0xBB);
    EXPECT_EQ(seq[4].params, (std::vector<uint8_t>{ 0x19 }));
    // Several commands on one line, split by ';'.
    EXPECT_EQ(seq[6].cmd, 0xC2);
    EXPECT_EQ(seq[10].cmd, 0xD0);
    EXPECT_EQ(seq[10].params, (std::vector<uint8_t>{ 0xA4, 0xA1 }));
    EXPECT_EQ(seq[11].params.size(), 14u);
    EXPECT_EQ(seq[13].cmd, 0x29);
    EXPECT_EQ(seq[13].delayMs, 120u);
}

TEST(InitSequenceText, ADelayOnItsOwnLineWaitsAfterTheCommandBefore)
{
    InitSequence seq;
    std::string error;
    ASSERT_TRUE(ParseInitSequence("11\ndelay 120\n29 delay 20", seq, error)) << error;
    ASSERT_EQ(seq.size(), 2u);
    EXPECT_EQ(seq[0].delayMs, 120u);
    EXPECT_EQ(seq[1].delayMs, 20u);
}

TEST(InitSequenceText, TakesAPrefixCommasAndWindowsLineEnds)
{
    InitSequence seq;
    std::string error;
    ASSERT_TRUE(ParseInitSequence("0xB2 0x0C, 0x0c\r\nbb 19\r\n", seq, error)) << error;
    ASSERT_EQ(seq.size(), 2u);
    EXPECT_EQ(seq[0].params, (std::vector<uint8_t>{ 0x0C, 0x0C }));
    EXPECT_EQ(seq[1].cmd, 0xBB);
}

TEST(InitSequenceText, EmptyTextIsNoCommands)
{
    InitSequence seq;
    std::string error;
    EXPECT_TRUE(ParseInitSequence("  # only a comment\n\n", seq, error));
    EXPECT_TRUE(seq.empty());
}

TEST(InitSequenceText, MistakesNameTheirLine)
{
    InitSequence seq;
    std::string error;
    EXPECT_FALSE(ParseInitSequence("11\nG1 00", seq, error));
    EXPECT_NE(error.find("line 2"), std::string::npos) << error;
    EXPECT_FALSE(ParseInitSequence("11 123", seq, error));
    EXPECT_NE(error.find("line 1"), std::string::npos) << error;
    EXPECT_FALSE(ParseInitSequence("delay 10", seq, error));
    EXPECT_FALSE(ParseInitSequence("11 delay", seq, error));
    EXPECT_FALSE(ParseInitSequence("11 delay ten", seq, error));
    EXPECT_FALSE(ParseInitSequence("11 delay 10 29", seq, error));
}

TEST(InitSequenceTable, ReadsLovyanGFXsFormat)
{
    // 0x80 on the count: a delay follows; 255 means 500 ms.
    const uint8_t table[] = { 0x01, 0x80, 150, 0xB2, 2, 0x0C, 0x0D, 0x11, 0x80, 255, 0xFF, 0xFF };
    const InitSequence seq = InitSequenceFromTable(table);
    ASSERT_EQ(seq.size(), 3u);
    EXPECT_EQ(seq[0].delayMs, 150u);
    EXPECT_EQ(seq[1].params, (std::vector<uint8_t>{ 0x0C, 0x0D }));
    EXPECT_EQ(seq[1].delayMs, 0u);
    EXPECT_EQ(seq[2].delayMs, 500u);
}

TEST(InitSequenceTable, TheST7789DefaultIsLovyanGFXs)
{
    const InitSequence seq = InitSequenceFromTable(GetControllerInfo(Controller::ST7789).defaultInit);
    ASSERT_EQ(seq.size(), 13u);
    EXPECT_EQ(seq[0].cmd, 0xB7);
    EXPECT_EQ(seq[0].params, (std::vector<uint8_t>{ 0x35 }));
    EXPECT_EQ(seq[1].cmd, 0xBB);  // VCOMS 0x28, which ghosts on the T-Deck's module
    EXPECT_EQ(seq[1].params, (std::vector<uint8_t>{ 0x28 }));
    EXPECT_EQ(seq[10].cmd, 0x11);
    EXPECT_EQ(seq[10].delayMs, 130u);
    EXPECT_EQ(seq[12].cmd, 0x29);
}

// --- PanelController ---

TEST_F(DisplayTest, StartUpIsTheSequenceThenInversionColourAndRotation)
{
    RecordingIO io;
    PanelController panel(io, &RecordDelay);
    InitSequence seq;
    std::string error;
    ASSERT_TRUE(ParseInitSequence(kTDeckInit, seq, error));

    PanelConfig c;  // the T-Deck Plus: 240x320 shown in landscape, IPS, red first
    c.rotation = 1;
    c.invert = true;
    c.rgbOrder = true;
    ASSERT_TRUE(panel.Init(c, &seq));

    const std::vector<std::string> cmds = Commands();
    ASSERT_EQ(cmds.size(), seq.size() + 3);
    EXPECT_EQ(cmds[0], "11");
    EXPECT_EQ(cmds[4], "BB 19");
    EXPECT_EQ(cmds[seq.size()], "21");          // INVON
    EXPECT_EQ(cmds[seq.size() + 1], "3A 55");   // RGB565
    EXPECT_EQ(cmds[seq.size() + 2], "36 64");   // MADCTL: MV|MX|MH, red first
    EXPECT_EQ(panel.GetWidth(), 320);
    EXPECT_EQ(panel.GetHeight(), 240);
    EXPECT_EQ(io.depth, 0);
}

TEST_F(DisplayTest, NoSequenceStartsWithTheControllersDefault)
{
    RecordingIO io;
    PanelController panel(io, &RecordDelay);
    ASSERT_TRUE(panel.Init(PanelConfig{}, nullptr));
    EXPECT_EQ(Commands().front(), "B7 35");
    InitSequence empty;
    g_Events.clear();
    ASSERT_TRUE(panel.Init(PanelConfig{}, &empty));
    EXPECT_EQ(Commands().front(), "B7 35");
}

TEST_F(DisplayTest, MadctlSetsBgrUnlessThePanelIsRedFirst)
{
    RecordingIO io;
    PanelController panel(io, &RecordDelay);
    PanelConfig c;
    c.rgbOrder = false;
    ASSERT_TRUE(panel.Init(c, nullptr));
    EXPECT_EQ(Commands().back(), "36 08");
    g_Events.clear();
    panel.SetRotation(2);
    EXPECT_EQ(Commands().back(), "36 DC");  // MX|MH|MY|ML|BGR
}

TEST_F(DisplayTest, RotationSwapsTheSizeAndTheOffsets)
{
    // A 135x240 module in the middle of a 240x320 controller (the LilyGO
    // T-Display's), turned to landscape: LovyanGFX puts it at column 40, row 53.
    RecordingIO io;
    PanelController panel(io, &RecordDelay);
    PanelConfig c;
    c.panelWidth = 135;
    c.panelHeight = 240;
    c.offsetX = 52;
    c.offsetY = 40;
    c.rotation = 1;
    ASSERT_TRUE(panel.Init(c, nullptr));
    EXPECT_EQ(panel.GetWidth(), 240);
    EXPECT_EQ(panel.GetHeight(), 135);
    g_Events.clear();
    ASSERT_TRUE(panel.SetWindow(0, 0, 240, 135));
    const std::vector<std::string> cmds = Commands();
    ASSERT_EQ(cmds.size(), 2u);
    EXPECT_EQ(cmds[0], "2A 00 28 01 17");  // columns 40-279
    EXPECT_EQ(cmds[1], "2B 00 35 00 BB");  // rows 53-187
    ASSERT_EQ(Of(Event::Start).size(), 1u);
    EXPECT_EQ(Of(Event::Start)[0].cmd, 0x2C);
}

TEST_F(DisplayTest, AnUnchangedColumnRangeIsNotSentAgain)
{
    RecordingIO io;
    PanelController panel(io, &RecordDelay);
    ASSERT_TRUE(panel.Init(SmallPanel(320, 240), nullptr));
    g_Events.clear();
    panel.SetWindow(0, 0, 320, 16);
    panel.SetWindow(0, 16, 320, 32);
    const std::vector<std::string> cmds = Commands();
    ASSERT_EQ(cmds.size(), 3u);
    EXPECT_EQ(cmds[0], "2A 00 00 01 3F");
    EXPECT_EQ(cmds[1], "2B 00 00 00 0F");
    EXPECT_EQ(cmds[2], "2B 00 10 00 1F");
    // A rotation may move the window: it is sent in full again.
    panel.SetRotation(0);
    g_Events.clear();
    panel.SetWindow(0, 16, 320, 32);
    EXPECT_EQ(Commands().size(), 2u);
}

TEST_F(DisplayTest, ResetHoldsTheLineLowThenWaitsForTheController)
{
    RecordingIO io;
    PanelController panel(io, &RecordDelay);
    PanelConfig c;
    c.resetPin = 5;
    ASSERT_TRUE(panel.Init(c, nullptr));
    ASSERT_GE(g_Events.size(), 4u);
    EXPECT_EQ(g_Events[0].kind, Event::Pin);
    EXPECT_EQ(g_Events[0].pin, 5);
    EXPECT_FALSE(g_Events[0].high);
    EXPECT_EQ(g_Events[1].kind, Event::Delay);
    EXPECT_EQ(g_Events[1].ms, 8u);
    EXPECT_EQ(g_Events[2].kind, Event::Pin);
    EXPECT_TRUE(g_Events[2].high);
    EXPECT_EQ(g_Events[3].kind, Event::Delay);
    EXPECT_EQ(g_Events[3].ms, 64u);
    EXPECT_EQ(g_Events[4].kind, Event::Command);
}

TEST_F(DisplayTest, AWriteEndsWithANopOnlyWithoutChipSelect)
{
    RecordingIO io;
    PanelController panel(io, &RecordDelay);
    PanelConfig c;
    ASSERT_TRUE(panel.Init(c, nullptr));
    g_Events.clear();
    panel.BeginWrite();
    panel.EndWrite();
    EXPECT_TRUE(Commands().empty());

    c.hasChipSelect = false;
    ASSERT_TRUE(panel.Init(c, nullptr));
    g_Events.clear();
    panel.BeginWrite();
    panel.EndWrite();
    EXPECT_EQ(Commands(), (std::vector<std::string>{ "00" }));
}

// --- SpiPanelIO ---

namespace
{

class RecordingDevice : public DekiSpi::IDekiSPIDevice
{
public:
    struct Sent
    {
        std::vector<uint8_t> bytes;
        int dc;
        bool keepCs;
        bool queued;
        int held;
    };
    std::vector<Sent> sent;
    int held = 0;

    bool Transmit(const DekiSpi::Transfer& t) override { return Record(t, false); }
    bool Queue(const DekiSpi::Transfer& t) override { return Record(t, true); }
    bool Wait(size_t, uint32_t) override { return true; }
    size_t InFlight() const override { return 0; }
    bool Acquire() override
    {
        ++held;
        return true;
    }
    void Release() override { --held; }
    size_t GetMaxTransferBytes() const override { return 4096; }
    uint32_t GetClockHz() const override { return 80000000; }

private:
    bool Record(const DekiSpi::Transfer& t, bool queued)
    {
        // Chip select may only stay low while the bus is held.
        EXPECT_TRUE(!t.keepCs || held > 0);
        const auto* p = static_cast<const uint8_t*>(t.tx);
        sent.push_back({ std::vector<uint8_t>(p, p + t.txBytes), t.dc, t.keepCs, queued, held });
        return true;
    }
};

}  // namespace

TEST(SpiPanelIO, ACommandIsDcLowThenItsParametersDcHigh)
{
    RecordingDevice dev;
    SpiPanelIO io(&dev);
    const uint8_t params[] = { 0x00, 0x00, 0x01, 0x3F };
    ASSERT_TRUE(io.Command(0x2A, params, 4));
    ASSERT_EQ(dev.sent.size(), 2u);
    EXPECT_EQ(dev.sent[0].bytes, (std::vector<uint8_t>{ 0x2A }));
    EXPECT_EQ(dev.sent[0].dc, 0);
    EXPECT_TRUE(dev.sent[0].keepCs);  // CS low from the command into its parameters
    EXPECT_EQ(dev.sent[1].bytes.size(), 4u);
    EXPECT_EQ(dev.sent[1].dc, 1);
    EXPECT_FALSE(dev.sent[1].keepCs);
    EXPECT_EQ(dev.held, 0);

    dev.sent.clear();
    ASSERT_TRUE(io.Command(0x29, nullptr, 0));
    ASSERT_EQ(dev.sent.size(), 1u);
    EXPECT_FALSE(dev.sent[0].keepCs);
}

TEST(SpiPanelIO, ChipSelectStaysLowFromTheMemoryWriteToTheLastPart)
{
    RecordingDevice dev;
    SpiPanelIO io(&dev);
    const uint8_t pixels[8] = {};
    ASSERT_TRUE(io.Begin());
    ASSERT_TRUE(io.StartPixels(0x2C));
    ASSERT_TRUE(io.QueuePixels(pixels, 8, true));
    ASSERT_TRUE(io.QueuePixels(pixels, 8, false));
    io.End();
    ASSERT_EQ(dev.sent.size(), 3u);
    EXPECT_EQ(dev.sent[0].dc, 0);
    EXPECT_TRUE(dev.sent[0].keepCs);
    EXPECT_FALSE(dev.sent[0].queued);
    EXPECT_EQ(dev.sent[1].dc, 1);
    EXPECT_TRUE(dev.sent[1].keepCs);
    EXPECT_TRUE(dev.sent[1].queued);
    EXPECT_FALSE(dev.sent[2].keepCs);
    EXPECT_EQ(dev.held, 0);
    EXPECT_EQ(io.GetMaxChunkBytes(), 4096u);
    EXPECT_TRUE(io.PixelsBigEndian());
}

// --- PanelDisplay ---

TEST_F(DisplayTest, AnRGB565FrameGoesOutHighByteFirst)
{
    RecordingIO io;
    PanelController panel(io, &RecordDelay);
    ASSERT_TRUE(panel.Init(SmallPanel(2, 2), nullptr));
    PanelDisplay display(panel, io);
    ASSERT_TRUE(display.Start(PanelDisplay::Options{}));
    g_Events.clear();

    const std::vector<uint8_t> frame = Frame565({ 0x1234, 0xABCD, 0xF800, 0x001F });
    display.Present(frame.data(), 2, 2, Deki::ColorFormat::RGB565);
    EXPECT_EQ(Wire(), (std::vector<uint8_t>{ 0x12, 0x34, 0xAB, 0xCD, 0xF8, 0x00, 0x00, 0x1F }));
    // The frame is the engine's: never changed by the swap.
    EXPECT_EQ(frame, Frame565({ 0x1234, 0xABCD, 0xF800, 0x001F }));
}

TEST_F(DisplayTest, AFrameIsOneWindowFilledInBands)
{
    RecordingIO io;
    io.bigEndian = !Deki::kRGB565BigEndian;  // swapped, so through the bands in either byte order
    PanelController panel(io, &RecordDelay);
    ASSERT_TRUE(panel.Init(SmallPanel(8, 40), nullptr));
    PanelDisplay display(panel, io);
    PanelDisplay::Options options;
    options.bandRows = 16;
    ASSERT_TRUE(display.Start(options));
    g_Events.clear();

    const std::vector<uint8_t> frame(8 * 40 * 2, 0);
    display.Present(frame.data(), 8, 40, Deki::ColorFormat::RGB565);
    // Start cleared the panel through the same window, so only the memory
    // write is sent again.
    EXPECT_TRUE(Commands().empty());
    EXPECT_EQ(Of(Event::Start).size(), 1u);
    const std::vector<Event> parts = Of(Event::Pixels);
    ASSERT_EQ(parts.size(), 3u);
    EXPECT_EQ(parts[0].bytes.size(), 8u * 16 * 2);
    EXPECT_EQ(parts[2].bytes.size(), 8u * 8 * 2);
    EXPECT_TRUE(parts[0].more);
    EXPECT_TRUE(parts[1].more);
    EXPECT_FALSE(parts[2].more);
    // Two bands, alternating.
    EXPECT_NE(parts[0].data, parts[1].data);
    EXPECT_EQ(parts[0].data, parts[2].data);
}

TEST_F(DisplayTest, ABandIsNeverLongerThanOneTransfer)
{
    RecordingIO io;
    io.maxChunk = 8 * 2 * 3;  // three rows
    PanelController panel(io, &RecordDelay);
    ASSERT_TRUE(panel.Init(SmallPanel(8, 10), nullptr));
    PanelDisplay display(panel, io);
    ASSERT_TRUE(display.Start(PanelDisplay::Options{}));
    g_Events.clear();
    const std::vector<uint8_t> frame(8 * 10 * 2, 0);
    display.Present(frame.data(), 8, 10, Deki::ColorFormat::RGB565);
    EXPECT_EQ(Of(Event::Pixels).size(), 4u);  // 3, 3, 3, 1 rows
}

TEST_F(DisplayTest, StartClearsThePanelBeforeTheBacklightComesOn)
{
    RecordingIO io;
    PanelController panel(io, &RecordDelay);
    ASSERT_TRUE(panel.Init(SmallPanel(4, 4), nullptr));
    g_Events.clear();
    PanelDisplay display(panel, io);
    PanelDisplay::Options options;
    options.backlightPin = 42;
    ASSERT_TRUE(display.Start(options));

    ASSERT_GE(g_Events.size(), 3u);
    EXPECT_EQ(g_Events.front().kind, Event::Pin);
    EXPECT_FALSE(g_Events.front().high);
    EXPECT_EQ(g_Events.back().kind, Event::Pin);
    EXPECT_TRUE(g_Events.back().high);
    EXPECT_EQ(Wire(), std::vector<uint8_t>(4 * 4 * 2, 0));

    g_Events.clear();
    display.SetBacklight(false);
    ASSERT_EQ(Of(Event::Pin).size(), 1u);
    EXPECT_FALSE(Of(Event::Pin)[0].high);
}

TEST_F(DisplayTest, RegionsBecomeMergedRowBands)
{
    RecordingIO io;
    PanelController panel(io, &RecordDelay);
    ASSERT_TRUE(panel.Init(SmallPanel(4, 30), nullptr));
    PanelDisplay display(panel, io);
    ASSERT_TRUE(display.Start(PanelDisplay::Options{}));
    g_Events.clear();

    const std::vector<uint8_t> frame(4 * 30 * 2, 0);
    // Out of order and overlapping; one is empty.
    const Deki::Rect rects[] = { { 1, 20, 3, 22 }, { 0, 2, 2, 5 }, { 2, 4, 4, 8 }, { 1, 9, 1, 12 } };
    display.PresentRegions(frame.data(), 4, 30, Deki::ColorFormat::RGB565, rects, 4);
    // The columns are the full width Start cleared, so only the rows change.
    EXPECT_EQ(Commands(), (std::vector<std::string>{ "2B 00 02 00 07", "2B 00 14 00 15" }));
    EXPECT_EQ(Of(Event::Start).size(), 2u);
    EXPECT_EQ(Wire().size(), (6u + 2u) * 4 * 2);
}

TEST_F(DisplayTest, NoRegionsSendNothing)
{
    RecordingIO io;
    PanelController panel(io, &RecordDelay);
    ASSERT_TRUE(panel.Init(SmallPanel(4, 4), nullptr));
    PanelDisplay display(panel, io);
    ASSERT_TRUE(display.Start(PanelDisplay::Options{}));
    g_Events.clear();
    const std::vector<uint8_t> frame(4 * 4 * 2, 0);
    display.PresentRegions(frame.data(), 4, 4, Deki::ColorFormat::RGB565, nullptr, 0);
    EXPECT_TRUE(g_Events.empty());
}

TEST_F(DisplayTest, TheOverlayIsBlendedIntoWhatIsSentNotTheFrame)
{
    RecordingIO io;
    PanelController panel(io, &RecordDelay);
    ASSERT_TRUE(panel.Init(SmallPanel(2, 1), nullptr));
    PanelDisplay display(panel, io);
    ASSERT_TRUE(display.Start(PanelDisplay::Options{}));

    void* overlay = display.CreateUIOverlay(2, 1);
    ASSERT_NE(overlay, nullptr);
    const uint32_t pixels[2] = { 0x00000000, 0xFFFFFFFF };  // clear, then opaque white
    ASSERT_TRUE(display.UpdateUIOverlay(overlay, 0, 0, 2, 1, pixels));
    display.SetActiveUIOverlay(overlay);
    g_Events.clear();

    const std::vector<uint8_t> frame = Frame565({ 0xF800, 0x001F });
    display.Present(frame.data(), 2, 1, Deki::ColorFormat::RGB565);
    EXPECT_EQ(Wire(), (std::vector<uint8_t>{ 0xF8, 0x00, 0xFF, 0xFF }));
    EXPECT_EQ(frame, Frame565({ 0xF800, 0x001F }));

    display.SetActiveUIOverlay(nullptr);
    display.DestroyUIOverlay(overlay);
    g_Events.clear();
    display.Present(frame.data(), 2, 1, Deki::ColorFormat::RGB565);
    EXPECT_EQ(Wire(), (std::vector<uint8_t>{ 0xF8, 0x00, 0x00, 0x1F }));
}

TEST_F(DisplayTest, ARGB8888IsConvertedToRGB565)
{
    RecordingIO io;
    PanelController panel(io, &RecordDelay);
    ASSERT_TRUE(panel.Init(SmallPanel(3, 1), nullptr));
    PanelDisplay display(panel, io);
    ASSERT_TRUE(display.Start(PanelDisplay::Options{}));
    g_Events.clear();
    const uint32_t frame[3] = { 0xFFFF0000, 0xFF00FF00, 0xFF0000FF };
    display.Present(reinterpret_cast<const uint8_t*>(frame), 3, 1, Deki::ColorFormat::ARGB8888);
    EXPECT_EQ(Wire(), (std::vector<uint8_t>{ 0xF8, 0x00, 0x07, 0xE0, 0x00, 0x1F }));
}

TEST_F(DisplayTest, AFrameAlreadyInTheWiresOrderGoesOutWithNoCopy)
{
    RecordingIO io;
    io.bigEndian = Deki::kRGB565BigEndian;  // the wire takes the frame's own order
    PanelController panel(io, &RecordDelay);
    ASSERT_TRUE(panel.Init(SmallPanel(4, 4), nullptr));
    PanelDisplay display(panel, io);
    ASSERT_TRUE(display.Start(PanelDisplay::Options{}));
    g_Events.clear();
    const std::vector<uint8_t> frame = Frame565(std::vector<uint16_t>(16, 0x1234));
    display.Present(frame.data(), 4, 4, Deki::ColorFormat::RGB565);
    const std::vector<Event> parts = Of(Event::Pixels);
    ASSERT_EQ(parts.size(), 1u);
    EXPECT_EQ(parts[0].data, frame.data());
    EXPECT_EQ(parts[0].bytes, frame);
}

TEST_F(DisplayTest, RenderBuffersAreOfferedAndSwappedWhenAsked)
{
    RecordingIO io;
    PanelController panel(io, &RecordDelay);
    ASSERT_TRUE(panel.Init(SmallPanel(4, 4), nullptr));
    PanelDisplay none(panel, io);
    ASSERT_TRUE(none.Start(PanelDisplay::Options{}));
    EXPECT_EQ(none.GetRenderBuffer(nullptr, nullptr), nullptr);

    PanelDisplay two(panel, io);
    PanelDisplay::Options options;
    options.doubleBuffer = true;
    ASSERT_TRUE(two.Start(options));
    int32_t w = 0;
    int32_t h = 0;
    uint8_t* first = two.GetRenderBuffer(&w, &h);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(w, 4);
    EXPECT_EQ(h, 4);
    two.Present(first, 4, 4, Deki::ColorFormat::RGB565);
    uint8_t* second = two.GetRenderBuffer(nullptr, nullptr);
    EXPECT_NE(second, nullptr);
    EXPECT_NE(second, first);
}

// --- DisplayPanelComponent ---

namespace
{
DekiSpi::IDekiSPIBus* NoBus()
{
    return nullptr;
}
bool RunSetup(DisplayPanelComponent& c)
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

TEST_F(DisplayTest, ABootSceneRunsWhereThereIsNoSpi)
{
    DekiSpi::DekiSPI::SetFactory(nullptr);
    DisplayPanelComponent c;
    EXPECT_TRUE(RunSetup(c));
}

TEST_F(DisplayTest, APanelNeedsItsBusStartedFirst)
{
    DekiSpi::DekiSPI::SetFactory(&NoBus);
    DisplayPanelComponent c;
    c.spiHost = 1;
    EXPECT_FALSE(RunSetup(c));
}
