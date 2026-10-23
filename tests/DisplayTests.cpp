// deki-display off a device: what each layer puts on the wire, recorded by
// fakes of the layer below it.
//
// - InitSequence: the text a boot scene holds and the tables the controller
//   defaults are ported in.
// - PanelController: reset, start-up, MADCTL, rotation offsets and windows,
//   as bytes on a recording IPanelIO.
// - SpiPanelIO: DC and chip select per transfer, on a recording SPI device.
// - PanelDisplay: byte order, bands, merged regions and the overlay, sent as
//   drawn and from a worker thread.
// - DisplayPanelComponent: a boot scene runs where there is no SPI.

#include <gtest/gtest.h>

#include "DisplayPanelComponent.h"
#include "InitSequence.h"
#include "PanelController.h"
#include "PanelDisplay.h"
#include "QspiPanelIO.h"
#include "SpiPanelIO.h"

#include "DekiGPIO.h"
#include "DekiSPI.h"

#include <deki/Engine.h>

#include <atomic>
#include <cstring>
#include <string>
#include <thread>
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
        Delay,
        Wait  // for the panel's refresh (the TE line)
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
    uint32_t GetQueuedCount() const override { return static_cast<uint32_t>(g_Events.size()); }
    size_t GetMaxChunkBytes() const override { return maxChunk; }
    bool PixelsBigEndian() const override { return bigEndian; }
};

class FakeGpio : public DekiGpio::IDekiGPIO
{
public:
    // A TE line: edges can be counted on it, and each wait sees a pulse
    // unless `pulsing` is off.
    int tePin = -1;
    bool pulsing = true;

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
    bool CountEdges(int pin, DekiGpio::Edge edge, DekiGpio::Pull) override
    {
        return pin == tePin && edge == DekiGpio::Edge::Rising;
    }
    uint32_t TakeEdges(int) override { return 0; }
    void StopCounting(int) override {}
    bool WaitForEdge(int pin, uint32_t) override
    {
        Event e{ Event::Wait };
        e.pin = pin;
        g_Events.push_back(e);
        return pin == tePin && pulsing;
    }
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
    EXPECT_EQ(cmds[seq.size()], "21");         // INVON
    EXPECT_EQ(cmds[seq.size() + 1], "3A 55");  // RGB565
    EXPECT_EQ(cmds[seq.size() + 2], "36 64");  // MADCTL: MV|MX|MH, red first
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
        int cmdBits = 0;
        int cmd = 0;
        int addrBits = 0;
        uint32_t addr = 0;
        DekiSpi::Lines lines = DekiSpi::Lines::Single;
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
        sent.push_back({ std::vector<uint8_t>(p, p + t.txBytes), t.dc, t.keepCs, queued, held, t.cmdBits, t.cmd,
                         t.addrBits, t.addr, t.lines });
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
    EXPECT_EQ(io.GetQueuedCount(), 3u);
    io.End();
    ASSERT_EQ(dev.sent.size(), 3u);
    EXPECT_EQ(dev.sent[0].dc, 0);
    EXPECT_TRUE(dev.sent[0].keepCs);
    EXPECT_TRUE(dev.sent[0].queued);  // behind whatever is still on the wire
    EXPECT_EQ(dev.sent[1].dc, 1);
    EXPECT_TRUE(dev.sent[1].keepCs);
    EXPECT_TRUE(dev.sent[1].queued);
    EXPECT_FALSE(dev.sent[2].keepCs);
    EXPECT_EQ(dev.held, 0);
    EXPECT_EQ(io.GetMaxChunkBytes(), 4096u);
    EXPECT_TRUE(io.PixelsBigEndian());
}

TEST(SpiPanelIO, InsideARunAWindowCommandIsQueuedBehindThePixels)
{
    RecordingDevice dev;
    SpiPanelIO io(&dev);
    const uint8_t window[] = { 0x00, 0x00, 0x00, 0xEF };
    const uint8_t gamma[14] = {};
    ASSERT_TRUE(io.Begin());
    ASSERT_TRUE(io.Command(0x2B, window, 4));
    ASSERT_TRUE(io.Command(0xE0, gamma, 14));  // too long to copy: sent and waited for
    io.End();
    ASSERT_EQ(dev.sent.size(), 4u);
    EXPECT_TRUE(dev.sent[0].queued);
    EXPECT_TRUE(dev.sent[1].queued);
    EXPECT_EQ(dev.sent[1].dc, 1);
    EXPECT_FALSE(dev.sent[2].queued);
    EXPECT_FALSE(dev.sent[3].queued);
    EXPECT_EQ(io.GetQueuedCount(), 2u);
    EXPECT_EQ(dev.held, 0);
}

// --- QspiPanelIO ---

TEST(QspiPanelIO, ACommandIsARegisterWriteOnOneLine)
{
    RecordingDevice dev;
    QspiPanelIO io(&dev);
    const uint8_t madctl = 0x60;
    ASSERT_TRUE(io.Command(0x36, &madctl, 1));
    ASSERT_EQ(dev.sent.size(), 1u);
    // 02 00 36 00, then the parameter: one transfer, no DC.
    EXPECT_EQ(dev.sent[0].cmdBits, 8);
    EXPECT_EQ(dev.sent[0].cmd, 0x02);
    EXPECT_EQ(dev.sent[0].addrBits, 24);
    EXPECT_EQ(dev.sent[0].addr, 0x3600u);
    EXPECT_EQ(dev.sent[0].bytes, (std::vector<uint8_t>{ 0x60 }));
    EXPECT_EQ(dev.sent[0].lines, DekiSpi::Lines::Single);
    EXPECT_EQ(dev.sent[0].dc, -1);
    EXPECT_FALSE(dev.sent[0].keepCs);
    EXPECT_EQ(dev.held, 0);
}

TEST(QspiPanelIO, PixelsAreOneQuadWriteFromTheMemoryWriteToTheLastPart)
{
    RecordingDevice dev;
    QspiPanelIO io(&dev);
    const uint8_t pixels[8] = {};
    ASSERT_TRUE(io.Begin());
    ASSERT_TRUE(io.StartPixels(0x2C));
    EXPECT_TRUE(dev.sent.empty());  // the phases go with the first part
    ASSERT_TRUE(io.QueuePixels(pixels, 8, true));
    ASSERT_TRUE(io.QueuePixels(pixels, 8, false));
    io.End();
    ASSERT_EQ(dev.sent.size(), 2u);
    EXPECT_EQ(dev.sent[0].cmd, 0x32);
    EXPECT_EQ(dev.sent[0].addr, 0x2C00u);
    EXPECT_EQ(dev.sent[0].lines, DekiSpi::Lines::Quad);
    EXPECT_TRUE(dev.sent[0].keepCs);
    EXPECT_TRUE(dev.sent[0].queued);
    // The rest continues the write: no phases of its own.
    EXPECT_EQ(dev.sent[1].cmdBits, 0);
    EXPECT_EQ(dev.sent[1].addrBits, 0);
    EXPECT_EQ(dev.sent[1].lines, DekiSpi::Lines::Quad);
    EXPECT_FALSE(dev.sent[1].keepCs);
    EXPECT_EQ(io.GetQueuedCount(), 2u);
    EXPECT_EQ(dev.held, 0);
}

// --- The RM67162 ---

TEST_F(DisplayTest, TheRM67162StartsAsLovyanGFXStartsIt)
{
    RecordingIO io;
    PanelController panel(io, &RecordDelay);
    PanelConfig c;
    c.controller = Controller::RM67162;
    c.panelWidth = 240;
    c.panelHeight = 536;
    c.rotation = 1;      // landscape, as the T-Display S3 AMOLED shows it
    c.rgbOrder = false;  // no colour-order bit on this controller
    ASSERT_TRUE(panel.Init(c, nullptr));
    const std::vector<std::string> cmds = Commands();
    ASSERT_GE(cmds.size(), 3u);
    EXPECT_EQ(cmds[0], "FE 00");
    EXPECT_EQ(cmds[1], "11");
    EXPECT_EQ(cmds[cmds.size() - 3], "20");     // INVOFF
    EXPECT_EQ(cmds[cmds.size() - 2], "3A 55");  // RGB565
    EXPECT_EQ(cmds.back(), "36 60");            // landscape, no BGR
    EXPECT_EQ(panel.GetWidth(), 536);
    EXPECT_EQ(panel.GetHeight(), 240);
    EXPECT_EQ(panel.GetRowAlign(), 2);
}

TEST_F(DisplayTest, AnAmoledsOffsetIsNotTurnedWithTheRotation)
{
    RecordingIO io;
    PanelController panel(io, &RecordDelay);
    PanelConfig c;
    c.controller = Controller::RM67162;
    c.panelWidth = 240;
    c.panelHeight = 536;
    c.offsetX = 10;
    c.offsetY = 20;
    c.rotation = 1;
    ASSERT_TRUE(panel.Init(c, nullptr));
    g_Events.clear();
    ASSERT_TRUE(panel.SetWindow(0, 0, 536, 240));
    // Swapped with the axes, and otherwise as given (Panel_AMOLED).
    EXPECT_EQ(Commands(), (std::vector<std::string>{ "2A 00 14 02 2B", "2B 00 0A 00 F9" }));
}

TEST_F(DisplayTest, AnAmoledsBacklightIsItsBrightness)
{
    RecordingIO io;
    PanelController panel(io, &RecordDelay);
    PanelConfig c;
    c.controller = Controller::RM67162;
    c.panelWidth = 4;
    c.panelHeight = 4;
    ASSERT_TRUE(panel.Init(c, nullptr));
    PanelDisplay display(panel, io);
    ASSERT_TRUE(display.Start(PanelDisplay::Options{}));  // no backlight pin
    g_Events.clear();
    display.SetBacklight(false);
    display.SetBacklight(true);
    EXPECT_EQ(Commands(), (std::vector<std::string>{ "51 00", "51 AF" }));

    // An LCD has no such command: nothing is sent without a backlight pin.
    RecordingIO lcdIO;
    PanelController lcd(lcdIO, &RecordDelay);
    ASSERT_TRUE(lcd.Init(SmallPanel(4, 4), nullptr));
    EXPECT_FALSE(lcd.HasBrightness());
    EXPECT_FALSE(lcd.SetBrightness(0x80));
}

TEST_F(DisplayTest, AnAmoledsRegionsStartAndEndOnEvenRows)
{
    RecordingIO io;
    PanelController panel(io, &RecordDelay);
    PanelConfig c;
    c.controller = Controller::RM67162;
    c.panelWidth = 4;
    c.panelHeight = 20;
    ASSERT_TRUE(panel.Init(c, nullptr));
    PanelDisplay display(panel, io);
    ASSERT_TRUE(display.Start(PanelDisplay::Options{}));
    g_Events.clear();
    const std::vector<uint8_t> frame(4 * 20 * 2, 0);
    const Deki::Rect rect[] = { { 0, 3, 4, 6 } };  // rows 3-5
    display.PresentRegions(frame.data(), 4, 20, Deki::ColorFormat::RGB565, rect, 1);
    EXPECT_EQ(Commands(), (std::vector<std::string>{ "2B 00 02 00 05" }));  // rows 2-5
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

// --- Async present ---

namespace
{

std::atomic<int> g_WorkersRunning{ 0 };

// The desktop's stand-in for the other core: a thread.
bool StartThread(void (*run)(void*), void* arg)
{
    ++g_WorkersRunning;
    std::thread(
        [run, arg]
        {
            run(arg);
            --g_WorkersRunning;
        })
        .detach();
    return true;
}
void NoYield()
{
}
const PanelDisplay::WorkerBackend kThreadWorker{ &StartThread, &NoYield };

}  // namespace

TEST_F(DisplayTest, AsyncPresentNeedsAWorker)
{
    RecordingIO io;
    PanelController panel(io, &RecordDelay);
    ASSERT_TRUE(panel.Init(SmallPanel(4, 4), nullptr));
    PanelDisplay display(panel, io);
    ASSERT_TRUE(display.Start(PanelDisplay::Options{}));
    PanelDisplay::SetWorkerBackend(nullptr);
    EXPECT_FALSE(display.SetAsyncPresent(true));
    EXPECT_FALSE(display.IsAsyncPresent());
}

TEST_F(DisplayTest, AsyncPresentSendsACopyOfARenderBuffer)
{
    // The T-Display S3 AMOLED's set-up: its frame is too big for internal
    // RAM, so the engine draws into a render buffer in PSRAM.
    RecordingIO io;
    PanelController panel(io, &RecordDelay);
    ASSERT_TRUE(panel.Init(SmallPanel(2, 2), nullptr));
    PanelDisplay display(panel, io);
    PanelDisplay::Options options;
    options.usePsram = true;
    ASSERT_TRUE(display.Start(options));
    PanelDisplay::SetWorkerBackend(&kThreadWorker);
    ASSERT_TRUE(display.SetAsyncPresent(true));
    g_Events.clear();

    uint8_t* buffer = display.GetRenderBuffer(nullptr, nullptr);
    ASSERT_NE(buffer, nullptr);
    const std::vector<uint8_t> frame = Frame565({ 0x1234, 0xABCD, 0xF800, 0x001F });
    memcpy(buffer, frame.data(), frame.size());
    display.Present(buffer, 2, 2, Deki::ColorFormat::RGB565);
    memset(buffer, 0, frame.size());  // the engine draws the next frame at once
    display.WaitForPresents();
    EXPECT_EQ(Wire(), (std::vector<uint8_t>{ 0x12, 0x34, 0xAB, 0xCD, 0xF8, 0x00, 0x00, 0x1F }));
    PanelDisplay::SetWorkerBackend(nullptr);
}

TEST_F(DisplayTest, AsyncFramesSendTheSameBytesAndLeaveTheFrameAlone)
{
    RecordingIO io;
    PanelController panel(io, &RecordDelay);
    ASSERT_TRUE(panel.Init(SmallPanel(2, 2), nullptr));
    PanelDisplay display(panel, io);
    ASSERT_TRUE(display.Start(PanelDisplay::Options{}));
    PanelDisplay::SetWorkerBackend(&kThreadWorker);
    ASSERT_TRUE(display.SetAsyncPresent(true));
    g_Events.clear();

    const std::vector<uint8_t> a = Frame565({ 0x1234, 0xABCD, 0xF800, 0x001F });
    const std::vector<uint8_t> b = Frame565({ 0x0001, 0x0002, 0x0003, 0x0004 });
    display.Present(a.data(), 2, 2, Deki::ColorFormat::RGB565);
    display.Present(b.data(), 2, 2, Deki::ColorFormat::RGB565);
    display.Present(a.data(), 2, 2, Deki::ColorFormat::RGB565);  // waits for a free copy
    display.WaitForPresents();
    EXPECT_EQ(Wire(), (std::vector<uint8_t>{ 0x12, 0x34, 0xAB, 0xCD, 0xF8, 0x00, 0x00, 0x1F,  //
                                             0x00, 0x01, 0x00, 0x02, 0x00, 0x03, 0x00, 0x04,  //
                                             0x12, 0x34, 0xAB, 0xCD, 0xF8, 0x00, 0x00, 0x1F }));
    EXPECT_EQ(a, Frame565({ 0x1234, 0xABCD, 0xF800, 0x001F }));

    ASSERT_TRUE(display.SetAsyncPresent(false));
    EXPECT_EQ(g_WorkersRunning.load(), 0);
    PanelDisplay::SetWorkerBackend(nullptr);
}

TEST_F(DisplayTest, AnAsyncStripIsOneWindowSwappedInItsCopy)
{
    RecordingIO io;
    io.bigEndian = !Deki::kRGB565BigEndian;  // swapped, in either byte order
    PanelController panel(io, &RecordDelay);
    ASSERT_TRUE(panel.Init(SmallPanel(2, 40), nullptr));
    PanelDisplay display(panel, io);
    ASSERT_TRUE(display.Start(PanelDisplay::Options{}));
    PanelDisplay::SetWorkerBackend(&kThreadWorker);
    ASSERT_TRUE(display.SetAsyncPresent(true));
    g_Events.clear();

    std::vector<uint16_t> pixels(2 * 40, 0);
    pixels[2 * 20] = 0x1234;  // the strip's first pixel
    const std::vector<uint8_t> frame = Frame565(pixels);
    const Deki::Rect strip[] = { { 0, 20, 2, 38 } };
    display.PresentRegions(frame.data(), 2, 40, Deki::ColorFormat::RGB565, strip, 1);
    display.WaitForPresents();

    EXPECT_EQ(Commands(), (std::vector<std::string>{ "2B 00 14 00 25" }));
    const std::vector<Event> parts = Of(Event::Pixels);
    ASSERT_EQ(parts.size(), 2u);  // 16 rows, then 2, while the first is sent
    EXPECT_TRUE(parts[0].more);
    EXPECT_FALSE(parts[1].more);
    const std::vector<uint8_t> wire = Wire();
    ASSERT_EQ(wire.size(), 2u * 18 * 2);
    // In the wire's order: the frame's bytes swapped.
    EXPECT_EQ(wire[0], frame[2 * 2 * 20 + 1]);
    EXPECT_EQ(wire[1], frame[2 * 2 * 20]);
    EXPECT_EQ(frame, Frame565(pixels));
    PanelDisplay::SetWorkerBackend(nullptr);
}

TEST_F(DisplayTest, OtherPresentsWaitForTheWorker)
{
    RecordingIO io;
    PanelController panel(io, &RecordDelay);
    ASSERT_TRUE(panel.Init(SmallPanel(2, 4), nullptr));
    PanelDisplay display(panel, io);
    ASSERT_TRUE(display.Start(PanelDisplay::Options{}));
    PanelDisplay::SetWorkerBackend(&kThreadWorker);
    ASSERT_TRUE(display.SetAsyncPresent(true));
    g_Events.clear();

    const std::vector<uint8_t> frame = Frame565({ 1, 2, 3, 4, 5, 6, 7, 8 });
    display.Present(frame.data(), 2, 4, Deki::ColorFormat::RGB565);
    // Two rectangles: sent as drawn, after the frame the worker has.
    const Deki::Rect rects[] = { { 0, 0, 2, 1 }, { 0, 3, 2, 4 } };
    display.PresentRegions(frame.data(), 2, 4, Deki::ColorFormat::RGB565, rects, 2);
    const std::vector<uint8_t> wire = Wire();
    ASSERT_EQ(wire.size(), (8u + 4u) * 2);
    EXPECT_EQ(wire[1], 1);  // the whole frame first
    EXPECT_EQ(wire[15], 8);
    EXPECT_EQ(wire[17], 1);  // then row 0
    EXPECT_EQ(wire[21], 7);  // and row 3
    PanelDisplay::SetWorkerBackend(nullptr);
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

// --- Tear-free (a TE pin) ---

namespace
{

// A screen pixel as a recognisable value: its row in the high byte, its
// column in the low.
uint16_t Place(int x, int y)
{
    return static_cast<uint16_t>((y << 8) | x);
}

// An RGB565 frame whose every pixel is Place(x, y).
std::vector<uint16_t> PlaceFrame(int w, int h)
{
    std::vector<uint16_t> out;
    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            out.push_back(Place(x, y));
        }
    }
    return out;
}

// The wire's pixels, high byte first, as values.
std::vector<uint16_t> WirePixels()
{
    const std::vector<uint8_t> bytes = Wire();
    std::vector<uint16_t> out;
    for (size_t i = 0; i + 1 < bytes.size(); i += 2)
    {
        out.push_back(static_cast<uint16_t>((bytes[i] << 8) | bytes[i + 1]));
    }
    return out;
}

// A 4 x 6 RM67162 (a small T-Display S3 AMOLED) at `rotation`.
PanelConfig SmallAmoled(uint8_t rotation)
{
    PanelConfig c;
    c.controller = Controller::RM67162;
    c.panelWidth = 4;
    c.panelHeight = 6;
    c.rotation = rotation;
    return c;
}

}  // namespace

TEST(PanelTurn, MadctlMapsTurnAndComeBack)
{
    // Rotation 1 on a 240 x 536 panel: MV then MX, so the screen's top-left
    // pixel is the panel's top-right one and the screen's columns go down
    // the panel's rows.
    const PixelMap m = MadctlMap(0x60, 240, 536);
    int32_t u = 0, v = 0;
    m.Apply(0, 0, u, v);
    EXPECT_EQ(u, 239);
    EXPECT_EQ(v, 0);
    m.Apply(535, 239, u, v);
    EXPECT_EQ(u, 0);
    EXPECT_EQ(v, 535);

    // Every rotation of the RM67162's table comes back where it started.
    for (uint8_t madctl : GetControllerInfo(Controller::RM67162).madctl)
    {
        const PixelMap there = MadctlMap(madctl, 240, 536);
        const PixelMap round = there.Then(there.Inverse());
        for (int32_t x : { 0, 7, 239 })
        {
            for (int32_t y : { 0, 100, 239 })
            {
                round.Apply(x, y, u, v);
                EXPECT_EQ(u, x) << "MADCTL " << int(madctl);
                EXPECT_EQ(v, y) << "MADCTL " << int(madctl);
            }
        }
    }
}

TEST_F(DisplayTest, WithoutATePinNothingChanges)
{
    RecordingIO io;
    PanelController panel(io, &RecordDelay);
    ASSERT_TRUE(panel.Init(SmallAmoled(1), nullptr));
    PanelDisplay display(panel, io);
    ASSERT_TRUE(display.Start(PanelDisplay::Options{}));
    EXPECT_FALSE(display.IsTearFree());
    EXPECT_FALSE(display.IsTurned());
    EXPECT_EQ(DekiRendering::FindScanOrderedDisplay(&display), nullptr);
    g_Events.clear();
    const std::vector<uint8_t> frame = Frame565(PlaceFrame(6, 4));
    display.Present(frame.data(), 6, 4, Deki::ColorFormat::RGB565);
    EXPECT_TRUE(Of(Event::Wait).empty());
    EXPECT_EQ(WirePixels(), PlaceFrame(6, 4)) << "the controller turns it";
}

// A frame that missed its refresh's start begins where its write cannot
// catch that refresh up: refresh minus write, plus a millisecond's margin.
TEST(TearFreeStart, AFrameThatMissedARefreshWaitsOnlyForItsWindow)
{
    constexpr uint32_t kRefresh = 16780;  // the T-Display S3 AMOLED's, 59.6 Hz
    EXPECT_EQ(PanelDisplay::TearFreeStartUs(kRefresh, 10500), kRefresh - 10500 + 1000);
    EXPECT_EQ(PanelDisplay::TearFreeStartUs(kRefresh, 3000), kRefresh - 3000 + 1000);
    // Not known yet: the next refresh's start, as before.
    EXPECT_EQ(PanelDisplay::TearFreeStartUs(kRefresh, 0), kRefresh);
    // As slow as a refresh, or slower: no start is tear-free, so at once.
    EXPECT_EQ(PanelDisplay::TearFreeStartUs(kRefresh, kRefresh - 1000), 0u);
    EXPECT_EQ(PanelDisplay::TearFreeStartUs(kRefresh, 25000), 0u);
    // Inside the refresh, and later the faster the write.
    EXPECT_LT(PanelDisplay::TearFreeStartUs(kRefresh, 12000), PanelDisplay::TearFreeStartUs(kRefresh, 8000));
}

TEST_F(DisplayTest, ATurnedPanelIsSentInItsOwnRowsFromItsRefresh)
{
    m_Gpio.tePin = 9;
    RecordingIO io;
    PanelController panel(io, &RecordDelay);
    ASSERT_TRUE(panel.Init(SmallAmoled(1), nullptr));
    g_Events.clear();
    PanelDisplay display(panel, io);
    PanelDisplay::Options options;
    options.tePin = 9;
    ASSERT_TRUE(display.Start(options));

    // TE on, then the controller back to its own orientation.
    const std::vector<std::string> start = Commands();
    ASSERT_GE(start.size(), 2u);
    EXPECT_EQ(start[0], "35 00");
    EXPECT_EQ(start[1], "36 00");
    EXPECT_TRUE(display.IsTearFree());
    EXPECT_TRUE(display.IsTurned());
    // The controller is written in its own orientation; the screen, and touch
    // with it, stays turned.
    EXPECT_EQ(panel.GetRotation(), 0);
    EXPECT_EQ(panel.GetScreenRotation(), 1);
    EXPECT_EQ(DekiRendering::FindScanOrderedDisplay(&display), &display);
    // The panel's rows run along the screen's columns, left to right.
    EXPECT_EQ(display.GetScanOrder(), DekiRendering::ScanOrder::ColumnsRight);
    int32_t w = 0, h = 0;
    display.GetDisplaySize(&w, &h);
    EXPECT_EQ(w, 6);
    EXPECT_EQ(h, 4);

    g_Events.clear();
    const std::vector<uint8_t> frame = Frame565(PlaceFrame(6, 4));
    display.Present(frame.data(), 6, 4, Deki::ColorFormat::RGB565);

    // The wait comes first, then the whole panel in its own rows: panel row
    // v is screen column v, from the screen's bottom row up.
    ASSERT_FALSE(g_Events.empty());
    EXPECT_EQ(g_Events.front().kind, Event::Wait);
    EXPECT_EQ(Of(Event::Wait).size(), 1u);
    std::vector<uint16_t> expected;
    for (int v = 0; v < 6; ++v)
    {
        for (int u = 0; u < 4; ++u)
        {
            expected.push_back(Place(v, 3 - u));
        }
    }
    EXPECT_EQ(WirePixels(), expected);
    EXPECT_EQ(frame, Frame565(PlaceFrame(6, 4))) << "the frame is the engine's";

    display.Shutdown();
    EXPECT_EQ(DekiRendering::FindScanOrderedDisplay(&display), nullptr);
}

TEST_F(DisplayTest, ATurnedStripSendsThePanelRowsOfWhatChanged)
{
    m_Gpio.tePin = 9;
    RecordingIO io;
    PanelController panel(io, &RecordDelay);
    ASSERT_TRUE(panel.Init(SmallAmoled(1), nullptr));
    PanelDisplay display(panel, io);
    PanelDisplay::Options options;
    options.tePin = 9;
    ASSERT_TRUE(display.Start(options));
    g_Events.clear();

    // Screen columns 2-3, all rows, as the renderer draws them; only part of
    // column 2 changed.
    std::vector<uint16_t> strip;
    for (int y = 0; y < 4; ++y)
    {
        strip.push_back(Place(2, y));
        strip.push_back(Place(3, y));
    }
    const std::vector<uint8_t> pixels = Frame565(strip);
    DekiRendering::ScanStrip s;
    s.pixels = pixels.data();
    s.stride = 2;
    s.strip = Deki::Rect{ 2, 0, 4, 4 };
    s.changed = Deki::Rect{ 2, 1, 3, 3 };
    s.firstOfFrame = true;
    display.PresentStrip(s);

    // Panel row 2, whole: what the changed pixels are in.
    EXPECT_EQ(Commands(), (std::vector<std::string>{ "2B 00 02 00 02" }));
    EXPECT_EQ(WirePixels(), (std::vector<uint16_t>{ Place(2, 3), Place(2, 2), Place(2, 1), Place(2, 0) }));
    EXPECT_EQ(Of(Event::Wait).size(), 1u);

    // The frame's next strip does not wait.
    g_Events.clear();
    s.changed = s.strip;
    s.firstOfFrame = false;
    display.PresentStrip(s);
    EXPECT_TRUE(Of(Event::Wait).empty());
    EXPECT_EQ(Commands(), (std::vector<std::string>{ "2B 00 02 00 03" }));
}

TEST_F(DisplayTest, AStripAcrossThePanelsRowsIsNotSentTurned)
{
    m_Gpio.tePin = 9;
    RecordingIO io;
    PanelController panel(io, &RecordDelay);
    ASSERT_TRUE(panel.Init(SmallAmoled(1), nullptr));
    PanelDisplay display(panel, io);
    PanelDisplay::Options options;
    options.tePin = 9;
    ASSERT_TRUE(display.Start(options));
    g_Events.clear();

    // A strip of screen rows holds part of every panel row only.
    const std::vector<uint8_t> pixels(6 * 2 * 2, 0);
    DekiRendering::ScanStrip s;
    s.pixels = pixels.data();
    s.stride = 6;
    s.strip = Deki::Rect{ 0, 0, 6, 2 };
    s.changed = s.strip;
    display.PresentStrip(s);
    EXPECT_TRUE(Of(Event::Pixels).empty());
}

TEST_F(DisplayTest, UprightThePanelOnlyWaitsForItsRefresh)
{
    m_Gpio.tePin = 9;
    RecordingIO io;
    PanelController panel(io, &RecordDelay);
    ASSERT_TRUE(panel.Init(SmallAmoled(0), nullptr));
    PanelDisplay display(panel, io);
    PanelDisplay::Options options;
    options.tePin = 9;
    ASSERT_TRUE(display.Start(options));
    EXPECT_TRUE(display.IsTearFree());
    EXPECT_FALSE(display.IsTurned());
    EXPECT_EQ(display.GetScanOrder(), DekiRendering::ScanOrder::RowsDown);
    g_Events.clear();

    const std::vector<uint8_t> frame = Frame565(PlaceFrame(4, 6));
    display.Present(frame.data(), 4, 6, Deki::ColorFormat::RGB565);
    ASSERT_FALSE(g_Events.empty());
    EXPECT_EQ(g_Events.front().kind, Event::Wait);
    EXPECT_EQ(WirePixels(), PlaceFrame(4, 6));

    // Strips of rows: the first of a frame waits, the rest do not.
    g_Events.clear();
    DekiRendering::ScanStrip s;
    s.pixels = frame.data() + 2 * 4 * 2;  // rows 2-3
    s.stride = 4;
    s.strip = Deki::Rect{ 0, 2, 4, 4 };
    s.changed = s.strip;
    s.firstOfFrame = true;
    display.PresentStrip(s);
    s.firstOfFrame = false;
    display.PresentStrip(s);
    EXPECT_EQ(Of(Event::Wait).size(), 1u);
}

TEST_F(DisplayTest, UpsideDownThePanelIsSentFromItsFirstRow)
{
    m_Gpio.tePin = 9;
    RecordingIO io;
    PanelController panel(io, &RecordDelay);
    ASSERT_TRUE(panel.Init(SmallAmoled(2), nullptr));
    PanelDisplay display(panel, io);
    PanelDisplay::Options options;
    options.tePin = 9;
    ASSERT_TRUE(display.Start(options));
    EXPECT_TRUE(display.IsTurned());
    EXPECT_EQ(display.GetScanOrder(), DekiRendering::ScanOrder::RowsUp);
    g_Events.clear();
    const std::vector<uint8_t> frame = Frame565(PlaceFrame(4, 6));
    display.Present(frame.data(), 4, 6, Deki::ColorFormat::RGB565);
    std::vector<uint16_t> expected;
    for (int y = 5; y >= 0; --y)
    {
        for (int x = 3; x >= 0; --x)
        {
            expected.push_back(Place(x, y));
        }
    }
    EXPECT_EQ(WirePixels(), expected);
}

TEST_F(DisplayTest, ATeLineThatDoesNotPulseIsSaidOnceAndNotWaitedFor)
{
    m_Gpio.tePin = 9;
    m_Gpio.pulsing = false;
    RecordingIO io;
    PanelController panel(io, &RecordDelay);
    ASSERT_TRUE(panel.Init(SmallAmoled(1), nullptr));
    PanelDisplay display(panel, io);
    PanelDisplay::Options options;
    options.tePin = 9;
    ASSERT_TRUE(display.Start(options));
    g_Events.clear();
    const std::vector<uint8_t> frame = Frame565(PlaceFrame(6, 4));
    display.Present(frame.data(), 6, 4, Deki::ColorFormat::RGB565);
    display.Present(frame.data(), 6, 4, Deki::ColorFormat::RGB565);
    EXPECT_EQ(Of(Event::Wait).size(), 1u);
    EXPECT_FALSE(display.IsTearFree());
    // Still in the panel's own rows.
    ASSERT_EQ(WirePixels().size(), 2u * 6 * 4);
    EXPECT_EQ(WirePixels()[0], Place(0, 3));
}

TEST_F(DisplayTest, ATePinThatCannotBeReadLeavesThePanelAsItWas)
{
    // The fake counts edges on pin 9 only.
    m_Gpio.tePin = 9;
    RecordingIO io;
    PanelController panel(io, &RecordDelay);
    ASSERT_TRUE(panel.Init(SmallAmoled(1), nullptr));
    PanelDisplay display(panel, io);
    PanelDisplay::Options options;
    options.tePin = 10;
    ASSERT_TRUE(display.Start(options));
    EXPECT_FALSE(display.IsTearFree());
    EXPECT_FALSE(display.IsTurned());
    EXPECT_EQ(DekiRendering::FindScanOrderedDisplay(&display), nullptr);
}

TEST_F(DisplayTest, TheOverlayLandsWhereItIsOnTheScreenWhenTurned)
{
    m_Gpio.tePin = 9;
    RecordingIO io;
    PanelController panel(io, &RecordDelay);
    ASSERT_TRUE(panel.Init(SmallAmoled(1), nullptr));
    PanelDisplay display(panel, io);
    PanelDisplay::Options options;
    options.tePin = 9;
    ASSERT_TRUE(display.Start(options));
    void* overlay = display.CreateUIOverlay(6, 4);
    ASSERT_NE(overlay, nullptr);
    std::vector<uint32_t> pixels(6 * 4, 0);
    pixels[0 * 6 + 5] = 0xFFFFFFFF;  // screen (5, 0): opaque white
    ASSERT_TRUE(display.UpdateUIOverlay(overlay, 0, 0, 6, 4, pixels.data()));
    display.SetActiveUIOverlay(overlay);
    g_Events.clear();

    const std::vector<uint8_t> frame = Frame565(PlaceFrame(6, 4));
    display.Present(frame.data(), 6, 4, Deki::ColorFormat::RGB565);
    const std::vector<uint16_t> wire = WirePixels();
    ASSERT_EQ(wire.size(), 24u);
    // Screen (5, 0) is panel row 5, the row's last pixel (u = 3).
    EXPECT_EQ(wire[5 * 4 + 3], 0xFFFF);
    EXPECT_EQ(wire[5 * 4 + 2], Place(5, 1));
    display.SetActiveUIOverlay(nullptr);
    display.DestroyUIOverlay(overlay);
}

TEST_F(DisplayTest, AsyncTurnedFramesAndStripsSendTheSameBytes)
{
    m_Gpio.tePin = 9;
    RecordingIO io;
    io.bigEndian = !Deki::kRGB565BigEndian;  // swapped, in either byte order
    PanelController panel(io, &RecordDelay);
    ASSERT_TRUE(panel.Init(SmallAmoled(1), nullptr));
    PanelDisplay display(panel, io);
    PanelDisplay::Options options;
    options.tePin = 9;
    ASSERT_TRUE(display.Start(options));

    // As drawn: the reference.
    const std::vector<uint8_t> frame = Frame565(PlaceFrame(6, 4));
    g_Events.clear();
    display.Present(frame.data(), 6, 4, Deki::ColorFormat::RGB565);
    const std::vector<uint8_t> asDrawn = Wire();

    PanelDisplay::SetWorkerBackend(&kThreadWorker);
    ASSERT_TRUE(display.SetAsyncPresent(true));
    g_Events.clear();
    display.Present(frame.data(), 6, 4, Deki::ColorFormat::RGB565);
    display.WaitForPresents();
    EXPECT_EQ(Wire(), asDrawn);
    EXPECT_EQ(Of(Event::Wait).size(), 1u);

    // The same frame as three strips of two columns: one wait, the same
    // bytes.
    g_Events.clear();
    for (int left = 0; left < 6; left += 2)
    {
        std::vector<uint16_t> strip;
        for (int y = 0; y < 4; ++y)
        {
            strip.push_back(Place(left, y));
            strip.push_back(Place(left + 1, y));
        }
        const std::vector<uint8_t> pixels = Frame565(strip);
        DekiRendering::ScanStrip s;
        s.pixels = pixels.data();
        s.stride = 2;
        s.strip = Deki::Rect{ left, 0, left + 2, 4 };
        s.changed = s.strip;
        s.firstOfFrame = left == 0;
        display.PresentStrip(s);  // copied before it returns
    }
    display.WaitForPresents();
    EXPECT_EQ(Wire(), asDrawn);
    EXPECT_EQ(Of(Event::Wait).size(), 1u);

    // The same strips drawn in the strip copies it lends, sent with no copy:
    // the same bytes. One lent and given back unused leaves the next lent
    // copy free to draw in.
    ASSERT_TRUE(display.LendsStripBuffers());
    {
        uint8_t* unused = display.GetStripBuffer(2 * 4 * sizeof(uint16_t));
        ASSERT_NE(unused, nullptr);
        DekiRendering::ScanStrip back;
        back.pixels = unused;
        back.stride = 2;
        back.strip = Deki::Rect{ 0, 0, 2, 4 };
        display.PresentStrip(back);  // nothing changed: given back
    }
    g_Events.clear();
    for (int left = 0; left < 6; left += 2)
    {
        uint8_t* buffer = display.GetStripBuffer(2 * 4 * sizeof(uint16_t));
        ASSERT_NE(buffer, nullptr);
        std::vector<uint16_t> strip;
        for (int y = 0; y < 4; ++y)
        {
            strip.push_back(Place(left, y));
            strip.push_back(Place(left + 1, y));
        }
        const std::vector<uint8_t> pixels = Frame565(strip);
        std::memcpy(buffer, pixels.data(), pixels.size());  // drawn in place
        DekiRendering::ScanStrip s;
        s.pixels = buffer;
        s.stride = 2;
        s.strip = Deki::Rect{ left, 0, left + 2, 4 };
        s.changed = s.strip;
        s.firstOfFrame = left == 0;
        display.PresentStrip(s);
    }
    display.WaitForPresents();
    EXPECT_EQ(Wire(), asDrawn);
    EXPECT_EQ(Of(Event::Wait).size(), 1u);
    ASSERT_TRUE(display.SetAsyncPresent(false));
    EXPECT_FALSE(display.LendsStripBuffers());
    PanelDisplay::SetWorkerBackend(nullptr);
}
