#include "DisplayPanelComponent.h"

#include "DekiSPI.h"  // from deki-spi
#include "InitSequence.h"
#include "PanelDisplay.h"
#include "QspiPanelIO.h"
#include "SpiPanelIO.h"

#include <deki/Engine.h>
#include <deki/LogSystem.h>
#include <deki/SceneSystem.h>
#include <deki/Time.h>

#include <memory>

namespace DekiDisplay
{

namespace
{

// The started panel, kept for the life of the program, like the other
// drivers the boot scene starts. The device belongs to its SPI bus.
struct StartedPanel
{
    std::unique_ptr<IPanelIO> io;
    std::unique_ptr<PanelController> controller;
    std::unique_ptr<PanelDisplay> display;
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
    // No SPI on this platform (the editor, the simulator): there is no panel
    // to start, and the boot scene carries on without one.
    if (!DekiSpi::DekiSPI::HasFactory())
    {
        DEKI_LOG_INFO("DisplayPanelComponent: no SPI on this platform; no panel started");
        finish(true);
        return;
    }
    DekiSpi::IDekiSPIBus* bus = DekiSpi::DekiSPI::GetBus(spiHost);
    if (!bus)
    {
        DEKI_LOG_ERROR("DisplayPanelComponent: no SPI bus on host %d - add an SPI Bus component for it before the "
                       "display in the setup list",
                       (int)spiHost);
        finish(false);
        return;
    }
    const bool quad = busType == DisplayBus::QSPI;
    if (quad && !bus->IsQuad())
    {
        DEKI_LOG_ERROR("DisplayPanelComponent: a QSPI panel needs IO2 and IO3, and the SPI Bus on host %d has none",
                       (int)spiHost);
        finish(false);
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
    DekiSpi::DeviceConfig device;
    device.csPin = csPin;
    device.dcPin = quad ? -1 : dcPin;  // a quad panel names each write instead
    device.clockHz = spiWriteHz > 0 ? static_cast<uint32_t>(spiWriteHz) : 40000000u;
    device.mode = 0;
    device.halfDuplex = true;
    device.queueDepth = 8;
    DekiSpi::IDekiSPIDevice* dev = bus->AddDevice(device);
    if (!dev)
    {
        DEKI_LOG_ERROR("DisplayPanelComponent: SPI host %d refused the panel (CS %d, %d Hz): %s", (int)spiHost,
                       (int)csPin, (int)spiWriteHz, bus->GetLastError());
        finish(false);
        return;
    }

    auto panel = std::make_unique<StartedPanel>();
    if (quad)
    {
        panel->io = std::make_unique<QspiPanelIO>(dev);
    }
    else
    {
        panel->io = std::make_unique<SpiPanelIO>(dev);
    }
    panel->controller = std::make_unique<PanelController>(*panel->io, &Delay);

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
    DEKI_LOG_INFO("DisplayPanelComponent: %s %dx%d on %s host %d at %u Hz (CS %d, DC %d, RST %d), %s start-up",
                  GetControllerInfo(controller).name, (int)panelWidth, (int)panelHeight, quad ? "QSPI" : "SPI",
                  (int)spiHost, (unsigned)dev->GetClockHz(), (int)csPin, quad ? -1 : (int)dcPin, (int)rstPin,
                  init.empty() ? "default" : "the board's");
    if (!panel->controller->Init(config, &init))
    {
        finish(false);
        return;
    }

    panel->display = std::make_unique<PanelDisplay>(*panel->controller, *panel->io);
    PanelDisplay::Options options;
    options.usePsram = usePsram;
    options.doubleBuffer = doubleBuffer;
    options.backlightPin = blPin;
    options.backlightInvert = blInvert;
    if (!panel->display->Start(options))
    {
        finish(false);
        return;
    }
    // Off again, with a warning, where the platform cannot: frames are then
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
