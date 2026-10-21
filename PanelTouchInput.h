#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "IDekiInput.h"  // from deki-input
#include "ITouchController.h"
#include "TouchMapper.h"

namespace DekiDisplay
{

/// A touch controller as a deki-input source: reads it once a frame and
/// sends a press, moves and a release at screen pixels, as a mouse would.
/// The same events, in the same order, as deki-lovyangfx-integration's
/// LovyanGFXTouch, so a scene behaves the same on either.
class PanelTouchInput : public DekiInput::IDekiInput
{
public:
    using NowFn = uint32_t (*)();

    /// `intPin` -1: the controller has no INT line, and a finger left still
    /// for kStaleFrames frames counts as lifted (some controllers keep
    /// reporting the last touch without one); that point is then no new
    /// press until it moves.
    PanelTouchInput(std::unique_ptr<ITouchController> controller, const TouchMapper& mapper, int32_t intPin,
                    NowFn now);

    bool Initialize() override;
    void Shutdown() override;
    void Update() override;
    void RegisterEventCallback(const DekiInput::InputEventCallback& callback) override;
    bool IsInitialized() const override { return m_Initialized; }
    bool GetPointerPosition(int32_t* x, int32_t* y) const override;
    bool IsKeyPressed(uint32_t) const override { return false; }

    static constexpr int kStaleFrames = 3;

private:
    void Send(DekiInput::InputEventType type, int32_t x, int32_t y, bool pressed);

    std::unique_ptr<ITouchController> m_Controller;
    TouchMapper m_Mapper;
    int32_t m_IntPin;
    NowFn m_Now;
    bool m_Initialized = false;
    bool m_Pressed = false;
    int32_t m_X = 0;
    int32_t m_Y = 0;
    int m_StaleFrames = 0;
    // Released as stale: the same point is not a new press until it moves or
    // the controller reports no touch.
    bool m_Stale = false;
    std::vector<DekiInput::InputEventCallback> m_Callbacks;
};

}  // namespace DekiDisplay
