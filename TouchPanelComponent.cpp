#include "TouchPanelComponent.h"

#include "DekiGPIO.h"   // from deki-gpio
#include "DekiI2C.h"    // from deki-i2c
#include "DekiInput.h"  // from deki-input
#include "DekiSPI.h"    // from deki-spi
#include "CST816Touch.h"
#include "DisplayPanelComponent.h"
#include "FT5x06Touch.h"
#include "GT911Touch.h"
#include "PanelTouchInput.h"
#include "XPT2046Touch.h"

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

void TouchDelay(uint32_t ms)
{
    Deki::Time::Delay(ms);
}

uint32_t TouchNow()
{
    return Deki::Time::GetTime();
}

// Starts `touch`, trying a few times: a controller just powered may need a
// moment after the bus comes up.
bool StartWithRetries(ITouchController& touch)
{
    for (int attempt = 0; attempt < 5; ++attempt)
    {
        if (touch.Init())
        {
            return true;
        }
        TouchDelay(50);
    }
    return false;
}

}  // namespace

void TouchPanelComponent::Setup(SetupCallback onComplete)
{
    auto finish = [&](bool ok)
    {
        if (onComplete)
        {
            onComplete(ok);
        }
    };

    const bool onSpi = controller == TouchController::XPT2046;
    // No such bus on this platform (the editor, the simulator): no touch to
    // start, and the boot scene carries on without it.
    if (onSpi ? !DekiSpi::DekiSPI::HasFactory() : !DekiI2c::DekiI2C::HasFactory())
    {
        DEKI_LOG_INFO("TouchPanelComponent: no %s on this platform; no touch started", onSpi ? "SPI" : "I2C");
        finish(true);
        return;
    }
    const PanelController* display = DisplayPanelComponent::GetStartedController();
    if (!display)
    {
        DEKI_LOG_ERROR("TouchPanelComponent: no display started - the Display Panel component goes before touch in "
                       "the setup list");
        finish(false);
        return;
    }

    DekiGpio::IDekiGPIO* gpio = DekiGpio::DekiGPIO::GetCurrent();
    std::unique_ptr<ITouchController> touch;
    char where[48];
    if (onSpi)
    {
        DekiSpi::IDekiSPIBus* bus = DekiSpi::DekiSPI::GetBus(spiHost);
        if (!bus)
        {
            DEKI_LOG_ERROR("TouchPanelComponent: no SPI bus on host %d - add an SPI Bus component for it before touch",
                           (int)spiHost);
            finish(false);
            return;
        }
        DekiSpi::DeviceConfig device;
        device.csPin = csPin;
        device.clockHz = spiHz > 0 ? static_cast<uint32_t>(spiHz) : 1000000u;
        device.mode = 0;
        device.halfDuplex = false;  // each reply comes back while the next command goes out
        device.queueDepth = 1;
        DekiSpi::IDekiSPIDevice* dev = bus->AddDevice(device);
        if (!dev)
        {
            DEKI_LOG_ERROR("TouchPanelComponent: SPI host %d refused the XPT2046 (CS %d): %s", (int)spiHost, (int)csPin,
                           bus->GetLastError());
            finish(false);
            return;
        }
        auto xpt = std::make_unique<XPT2046Touch>(dev, gpio, intPin);
        xpt->Init();
        touch = std::move(xpt);
        snprintf(where, sizeof(where), "on SPI host %d (CS %d)", (int)spiHost, (int)csPin);
    }
    else
    {
        DekiI2c::IDekiI2C* bus = DekiI2c::DekiI2C::GetBus(i2cPort);
        if (!bus)
        {
            DEKI_LOG_ERROR("TouchPanelComponent: no I2C bus on port %d - add an I2C Bus component for it before touch",
                           (int)i2cPort);
            finish(false);
            return;
        }
        const auto addressWanted = static_cast<uint8_t>(i2cAddress);
        uint8_t address = 0;
        if (controller == TouchController::CST816)
        {
            // It sleeps, unanswering, until touched, so it cannot be found now:
            // it is set up when it first answers, and a read it does not answer
            // is no touch (a NACK costs no timeout).
            auto cst = std::make_unique<CST816Touch>(bus, gpio, addressWanted, intPin, rstPin, &TouchDelay);
            cst->Init();
            address = addressWanted ? addressWanted : CST816Touch::kAddress;
            touch = std::move(cst);
        }
        else if (controller == TouchController::FT5x06)
        {
            auto ft = std::make_unique<FT5x06Touch>(bus, gpio, addressWanted, intPin, rstPin, &TouchDelay);
            if (!StartWithRetries(*ft))
            {
                DEKI_LOG_ERROR("TouchPanelComponent: the FT5x06 on I2C port %d does not answer (0x%02X); no touch",
                               (int)i2cPort, ft->GetAddress());
                finish(true);
                return;
            }
            address = ft->GetAddress();
            touch = std::move(ft);
        }
        else
        {
            auto gt = std::make_unique<GT911Touch>(bus, gpio, addressWanted, intPin, rstPin, &TouchDelay, &TouchNow);
            if (!StartWithRetries(*gt))
            {
                // Not registered: reading a controller that does not answer
                // costs an I2C timeout every frame. The board runs on without
                // touch.
                DEKI_LOG_ERROR("TouchPanelComponent: the GT911 on I2C port %d does not answer (0x5D or 0x14); no "
                               "touch",
                               (int)i2cPort);
                finish(true);
                return;
            }
            address = gt->GetAddress();
            touch = std::move(gt);
        }
        snprintf(where, sizeof(where), "at 0x%02X on I2C port %d", address, (int)i2cPort);
    }

    TouchMapper mapper;
    mapper.xMin = xMin;
    mapper.xMax = xMax;
    mapper.yMin = yMin;
    mapper.yMax = yMax;
    mapper.panelWidth = display->GetConfig().panelWidth;
    mapper.panelHeight = display->GetConfig().panelHeight;
    // The screen's, which tear-free presenting keeps while the controller is
    // written in its own orientation.
    mapper.displayRotation = display->GetScreenRotation();
    mapper.offsetRotation = static_cast<uint8_t>(offsetRotation);

    auto input = std::make_unique<PanelTouchInput>(std::move(touch), mapper, intPin, &TouchNow);
    if (!input->Initialize() || !DekiInput::DekiInput::SetInput(std::move(input), "DekiDisplayTouch"))
    {
        DEKI_LOG_ERROR("TouchPanelComponent: deki-input did not take the touch input");
        finish(false);
        return;
    }
    static const char* const kNames[] = { "GT911", "CST816", "FT5x06", "XPT2046" };
    DEKI_LOG_INFO("TouchPanelComponent: %s %s (INT %d, RST %d), as touch input", kNames[static_cast<int>(controller)],
                  where, (int)intPin, (int)rstPin);

    // Touch outlives scene changes, as the display does.
    if (GetOwner())
    {
        Deki::Engine::GetInstance().GetSceneSystem().MarkPersistent(GetOwner());
    }
    finish(true);
}

}  // namespace DekiDisplay
