#include "TouchPanelComponent.h"

#include "DekiGPIO.h"   // from deki-gpio
#include "DekiI2C.h"    // from deki-i2c
#include "DekiInput.h"  // from deki-input
#include "DisplayPanelComponent.h"
#include "GT911Touch.h"
#include "PanelTouchInput.h"

#include <deki/Engine.h>
#include <deki/LogSystem.h>
#include <deki/SceneSystem.h>
#include <deki/Time.h>

#include <memory>

namespace DekiDisplay
{

namespace
{

void Delay(uint32_t ms)
{
    Deki::Time::Delay(ms);
}

uint32_t Now()
{
    return Deki::Time::GetTime();
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

    // No I2C on this platform (the editor, the simulator): no touch to start,
    // and the boot scene carries on without it.
    if (!DekiI2c::DekiI2C::HasFactory())
    {
        DEKI_LOG_INFO("TouchPanelComponent: no I2C on this platform; no touch started");
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
    DekiI2c::IDekiI2C* bus = DekiI2c::DekiI2C::GetBus(i2cPort);
    if (!bus)
    {
        DEKI_LOG_ERROR("TouchPanelComponent: no I2C bus on port %d - add an I2C Bus component for it before touch",
                       (int)i2cPort);
        finish(false);
        return;
    }

    auto touch = std::make_unique<GT911Touch>(bus, DekiGpio::DekiGPIO::GetCurrent(), static_cast<uint8_t>(i2cAddress),
                                              intPin, rstPin, &Delay, &Now);
    // A controller just powered may need a moment after the bus comes up.
    bool started = false;
    for (int attempt = 0; attempt < 5 && !started; ++attempt)
    {
        started = touch->Init();
        if (!started)
        {
            Delay(50);
        }
    }
    if (!started)
    {
        // Not registered: reading a controller that does not answer costs an
        // I2C timeout every frame. The board runs on without touch.
        DEKI_LOG_ERROR("TouchPanelComponent: the GT911 on I2C port %d does not answer (0x5D or 0x14); no touch",
                       (int)i2cPort);
        finish(true);
        return;
    }
    const uint8_t address = touch->GetAddress();

    TouchMapper mapper;
    mapper.xMin = xMin;
    mapper.xMax = xMax;
    mapper.yMin = yMin;
    mapper.yMax = yMax;
    mapper.panelWidth = display->GetConfig().panelWidth;
    mapper.panelHeight = display->GetConfig().panelHeight;
    mapper.displayRotation = display->GetRotation();
    mapper.offsetRotation = static_cast<uint8_t>(offsetRotation);

    auto input = std::make_unique<PanelTouchInput>(std::move(touch), mapper, intPin, &Now);
    if (!input->Initialize() || !DekiInput::DekiInput::SetInput(std::move(input), "DekiDisplayTouch"))
    {
        DEKI_LOG_ERROR("TouchPanelComponent: deki-input did not take the touch input");
        finish(false);
        return;
    }
    DEKI_LOG_INFO("TouchPanelComponent: GT911 at 0x%02X on I2C port %d (INT %d, RST %d), as touch input", address,
                  (int)i2cPort, (int)intPin, (int)rstPin);

    // Touch outlives scene changes, as the display does.
    if (GetOwner())
    {
        Deki::Engine::GetInstance().GetSceneSystem().MarkPersistent(GetOwner());
    }
    finish(true);
}

}  // namespace DekiDisplay
