#include "DisplayPanelComponent.h"

#include "DekiSPI.h"  // from deki-spi
#include "InitSequence.h"
#include "PanelDisplay.h"
#include "ParallelPanelIO.h"
#include "QspiPanelIO.h"
#include "Rgb666PanelIO.h"
#include "SpiPanelIO.h"

#include <deki/Engine.h>
#include <deki/LogSystem.h>
#include <deki/SceneSystem.h>
#include <deki/Time.h>

#include <cstdio>
#include <memory>

namespace DekiDisplay
{

namespace
{

// The started panel, kept for the life of the program, like the other
// drivers the boot scene starts. An SPI device belongs to its SPI bus; a
// parallel bus belongs to the panel.
struct StartedPanel
{
    std::unique_ptr<IParallelBus> parallelBus;
    std::unique_ptr<IPanelIO> busIO;
    // Widens pixels to RGB666 for a controller that needs it on its bus.
    std::unique_ptr<IPanelIO> wideIO;
    std::unique_ptr<PanelController> controller;
    std::unique_ptr<PanelDisplay> display;

    IPanelIO& IO() const { return wideIO ? *wideIO : *busIO; }
};
std::unique_ptr<StartedPanel> s_Panel;

void Delay(uint32_t ms)
{
    Deki::Time::Delay(ms);
}

}  // namespace

const PanelController* DisplayPanelComponent::GetStartedController()
{
    return s_Panel ? s_Panel->controller.get() : nullptr;
}

void DisplayPanelComponent::Setup(SetupCallback onComplete)
{
    auto finish = [&](bool ok)
    {
        if (onComplete)
        {
            onComplete(ok);
        }
    };

    if (s_Panel)
    {
        DEKI_LOG_ERROR("DisplayPanelComponent: a display is already started; one panel per board");
        finish(false);
        return;
    }
    const bool parallel = busType == DisplayBus::Parallel8 || busType == DisplayBus::Parallel16;
    const bool quad = busType == DisplayBus::QSPI;
    // No such bus on this platform (the editor, the simulator): there is no
    // panel to start, and the boot scene carries on without one.
    if (parallel ? !ParallelBus::HasFactory() : !DekiSpi::DekiSPI::HasFactory())
    {
        DEKI_LOG_INFO("DisplayPanelComponent: no %s on this platform; no panel started",
                      parallel ? "parallel panel bus" : "SPI");
        finish(true);
        return;
    }

    InitSequence init;
    if (!initSequence.empty())
    {
        std::string error;
        if (!ParseInitSequence(initSequence, init, error))
        {
            DEKI_LOG_ERROR("DisplayPanelComponent: initSequence: %s; starting the %s with its default", error.c_str(),
                           GetControllerInfo(controller).name);
            init.clear();
        }
    }

    auto panel = std::make_unique<StartedPanel>();
    char wiring[64];
    if (parallel)
    {
        ParallelBusConfig bus;
        bus.width = busType == DisplayBus::Parallel16 ? 16 : 8;
        const int32_t pins[16] = { d0Pin, d1Pin, d2Pin,  d3Pin,  d4Pin,  d5Pin,  d6Pin,  d7Pin,
                                   d8Pin, d9Pin, d10Pin, d11Pin, d12Pin, d13Pin, d14Pin, d15Pin };
        for (int i = 0; i < 16; ++i)
        {
            bus.dataPins[i] = pins[i];
        }
        bus.wrPin = wrPin;
        bus.rdPin = rdPin;
        bus.dcPin = dcPin;
        bus.csPin = csPin;
        bus.clockHz = parallelWriteHz > 0 ? static_cast<uint32_t>(parallelWriteHz) : 20000000u;
        panel->parallelBus = ParallelBus::Create(bus);
        if (!panel->parallelBus)
        {
            DEKI_LOG_ERROR("DisplayPanelComponent: the %d-bit parallel bus did not start (WR %d, DC %d, D0 %d)",
                           (int)bus.width, (int)wrPin, (int)dcPin, (int)d0Pin);
            finish(false);
            return;
        }
        panel->busIO = std::make_unique<ParallelPanelIO>(*panel->parallelBus);
        snprintf(wiring, sizeof(wiring), "%d-bit parallel at %u Hz", (int)bus.width, (unsigned)bus.clockHz);
    }
    else
    {
        DekiSpi::IDekiSPIBus* bus = DekiSpi::DekiSPI::GetBus(spiHost);
        if (!bus)
        {
            DEKI_LOG_ERROR("DisplayPanelComponent: no SPI bus on host %d - add an SPI Bus component for it before the "
                           "display in the setup list",
                           (int)spiHost);
            finish(false);
            return;
        }
        if (quad && !bus->IsQuad())
        {
            DEKI_LOG_ERROR("DisplayPanelComponent: a QSPI panel needs IO2 and IO3, and the SPI Bus on host %d has "
                           "none",
                           (int)spiHost);
            finish(false);
            return;
        }
        DekiSpi::DeviceConfig device;
        device.csPin = csPin;
        device.dcPin = quad ? -1 : dcPin;  // a quad panel names each write instead
        device.clockHz = spiWriteHz > 0 ? static_cast<uint32_t>(spiWriteHz) : 40000000u;
        device.mode = 0;
        device.halfDuplex = true;
        device.queueDepth = 8;
        if (scanlineSync && !quad && tePin < 0)
        {
            device.readClockHz = spiReadHz > 0 ? static_cast<uint32_t>(spiReadHz) : 10000000u;
            device.threeWire = readsOnMosi;
        }
        DekiSpi::IDekiSPIDevice* dev = bus->AddDevice(device);
        if (!dev)
        {
            DEKI_LOG_ERROR("DisplayPanelComponent: SPI host %d refused the panel (CS %d, %d Hz): %s", (int)spiHost,
                           (int)csPin, (int)spiWriteHz, bus->GetLastError());
            finish(false);
            return;
        }
        if (quad)
        {
            panel->busIO = std::make_unique<QspiPanelIO>(dev);
        }
        else
        {
            panel->busIO = std::make_unique<SpiPanelIO>(dev);
        }
        snprintf(wiring, sizeof(wiring), "%s host %d at %u Hz", quad ? "QSPI" : "SPI", (int)spiHost,
                 (unsigned)dev->GetClockHz());
    }
    // A controller that takes 18 bits a pixel on this bus gets them widened.
    const bool pixels18 = GetControllerInfo(controller).spiNeeds18Bits && busType == DisplayBus::SPI;
    if (pixels18)
    {
        panel->wideIO = std::make_unique<Rgb666PanelIO>(*panel->busIO);
    }
    panel->controller = std::make_unique<PanelController>(panel->IO(), &Delay);

    PanelConfig config;
    config.controller = controller;
    config.panelWidth = panelWidth;
    config.panelHeight = panelHeight;
    config.memoryWidth = memoryWidth;
    config.memoryHeight = memoryHeight;
    config.offsetX = offsetX;
    config.offsetY = offsetY;
    config.rotation = static_cast<uint8_t>(rotation);
    config.invert = invertColor;
    config.rgbOrder = rgbOrder;
    config.resetPin = rstPin;
    config.hasChipSelect = csPin >= 0;
    config.pixels18 = pixels18;
    DEKI_LOG_INFO("DisplayPanelComponent: %s %dx%d on %s (CS %d, DC %d, RST %d), %s start-up%s",
                  GetControllerInfo(controller).name, (int)panelWidth, (int)panelHeight, wiring, (int)csPin,
                  quad ? -1 : (int)dcPin, (int)rstPin, init.empty() ? "default" : "the board's",
                  pixels18 ? ", RGB666" : "");
    if (!panel->controller->Init(config, &init))
    {
        finish(false);
        return;
    }

    panel->display = std::make_unique<PanelDisplay>(*panel->controller, panel->IO());
    PanelDisplay::Options options;
    options.usePsram = usePsram;
    options.doubleBuffer = doubleBuffer;
    options.backlightPin = blPin;
    options.backlightInvert = blInvert;
    options.tePin = tePin;
    options.scanlineSync = scanlineSync && busType == DisplayBus::SPI && tePin < 0;
    if (!panel->display->Start(options))
    {
        finish(false);
        return;
    }
    // Off again where the platform cannot, said in the log: frames are then
    // sent as they are drawn.
    if (asyncPresent)
    {
        panel->display->SetAsyncPresent(true);
    }

    s_Panel = std::move(panel);
    Deki::Engine::GetInstance().SetDisplay(s_Panel->display.get(), "DekiDisplay");
    // The display outlives scene changes.
    if (GetOwner())
    {
        Deki::Engine::GetInstance().GetSceneSystem().MarkPersistent(GetOwner());
    }
    finish(true);
}

}  // namespace DekiDisplay
