#include "DsiPanelComponent.h"

#include "DekiGPIO.h"  // from deki-gpio
#include "DsiDisplay.h"
#include "InitSequence.h"
#include "MipiDsi.h"

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
// drivers the boot scene starts.
struct StartedDsiPanel
{
    std::unique_ptr<IDsiPanel> panel;
    std::unique_ptr<DsiDisplay> display;
};
std::unique_ptr<StartedDsiPanel> s_DsiPanel;

}  // namespace

void DsiPanelComponent::Stop()
{
    if (!s_DsiPanel)
    {
        return;
    }
    Deki::Engine& engine = Deki::Engine::GetInstance();
    if (engine.GetDisplay() == s_DsiPanel->display.get())
    {
        engine.SetDisplay(nullptr, "");  // shuts the display down
    }
    s_DsiPanel.reset();
}

bool DsiPanelComponent::GetStartedSize(int32_t& width, int32_t& height)
{
    if (!s_DsiPanel)
    {
        return false;
    }
    s_DsiPanel->display->GetDisplaySize(&width, &height);
    return true;
}

void DsiPanelComponent::Setup(SetupCallback onComplete)
{
    auto finish = [&](bool ok)
    {
        if (onComplete)
        {
            onComplete(ok);
        }
    };

    if (s_DsiPanel)
    {
        DEKI_LOG_ERROR("DsiPanelComponent: a DSI panel is already started; one per board");
        finish(false);
        return;
    }
    // No DSI on this platform (the editor, the simulator): there is no panel
    // to start, and the boot scene carries on without one.
    if (!MipiDsi::HasFactory())
    {
        DEKI_LOG_INFO("DsiPanelComponent: no MIPI-DSI on this platform; no panel started");
        finish(true);
        return;
    }

    InitSequence init;
    {
        std::string error;
        if (!ParseInitSequence(initSequence, init, error))
        {
            // There is no default to fall back on: the start-up belongs to
            // the panel, and a wrong one shows nothing.
            DEKI_LOG_ERROR("DsiPanelComponent: initSequence: %s", error.c_str());
            finish(false);
            return;
        }
    }
    if (init.empty())
    {
        DEKI_LOG_WARNING("DsiPanelComponent: initSequence is empty; the panel's controller is not started, which few "
                         "panels survive");
    }

    DsiPanelConfig config;
    config.width = panelWidth;
    config.height = panelHeight;
    config.dataLanes = static_cast<uint8_t>(dataLanes);
    config.laneMbps = static_cast<uint32_t>(laneMbps);
    config.pixelClockHz = static_cast<uint32_t>(pixelClockHz);
    config.hsyncPulse = static_cast<uint16_t>(hsyncPulse);
    config.hBackPorch = static_cast<uint16_t>(hBackPorch);
    config.hFrontPorch = static_cast<uint16_t>(hFrontPorch);
    config.vsyncPulse = static_cast<uint16_t>(vsyncPulse);
    config.vBackPorch = static_cast<uint16_t>(vBackPorch);
    config.vFrontPorch = static_cast<uint16_t>(vFrontPorch);
    config.frameBuffers = static_cast<uint8_t>(frameBuffers < 1 ? 1 : (frameBuffers > 3 ? 3 : frameBuffers));
    if (config.pixelClockHz == 0 || config.width <= 0 || config.height <= 0)
    {
        DEKI_LOG_ERROR("DsiPanelComponent: the panel needs its size and pixel clock");
        finish(false);
        return;
    }

    // A wired reset: low, then released, before the link starts.
    DekiGpio::IDekiGPIO* gpio = DekiGpio::DekiGPIO::GetCurrent();
    if (rstPin >= 0 && gpio)
    {
        gpio->SetOutput(rstPin, false);
        Deki::Time::Delay(10);
        gpio->Write(rstPin, true);
        Deki::Time::Delay(20);
    }

    auto started = std::make_unique<StartedDsiPanel>();
    started->panel = MipiDsi::Create(config);
    if (!started->panel)
    {
        finish(false);  // the platform said why
        return;
    }

    DEKI_LOG_INFO("DsiPanelComponent: %dx%d on %d lane%s at %d Mbps, %d MHz pixel clock, %s start-up (%u commands)",
                  static_cast<int>(panelWidth), static_cast<int>(panelHeight), static_cast<int>(dataLanes),
                  dataLanes == 1 ? "" : "s", static_cast<int>(laneMbps), static_cast<int>(pixelClockHz / 1000000),
                  init.empty() ? "no" : "the board's", static_cast<unsigned>(init.size()));
    for (const InitStep& step : init)
    {
        if (!started->panel->Command(step.cmd, step.params.data(), step.params.size()))
        {
            DEKI_LOG_ERROR("DsiPanelComponent: the panel did not take command %02X of its start-up", step.cmd);
            finish(false);
            return;
        }
        if (step.delayMs > 0)
        {
            Deki::Time::Delay(step.delayMs);
        }
    }
    if (!started->panel->Start())
    {
        finish(false);
        return;
    }

    started->display = std::make_unique<DsiDisplay>(*started->panel, panelWidth, panelHeight,
                                                    config.RefreshPeriodUs());
    DsiDisplay::Options options;
    options.backlightPin = blPin;
    options.backlightInvert = blInvert;
    options.backlightPwmHz = blPwmHz > 0 ? static_cast<uint32_t>(blPwmHz) : 0u;
    options.brightness = static_cast<float>(blBrightness) / 100.0f;
    options.strips = drawInStrips;
    if (!started->display->Start(options))
    {
        finish(false);
        return;
    }

    s_DsiPanel = std::move(started);
    Deki::Engine::GetInstance().SetDisplay(s_DsiPanel->display.get(), "DekiDisplay DSI");
    // The display outlives scene changes.
    if (GetOwner())
    {
        Deki::Engine::GetInstance().GetSceneSystem().MarkPersistent(GetOwner());
    }
    finish(true);
}

}  // namespace DekiDisplay
