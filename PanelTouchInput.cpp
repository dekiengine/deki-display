#include "PanelTouchInput.h"

namespace DekiDisplay
{

PanelTouchInput::PanelTouchInput(std::unique_ptr<ITouchController> controller, const TouchMapper& mapper,
                                 int32_t intPin, NowFn now)
    : m_Controller(std::move(controller)),
      m_Mapper(mapper),
      m_IntPin(intPin),
      m_Now(now)
{
}

bool PanelTouchInput::Initialize()
{
    m_Initialized = m_Controller != nullptr;
    return m_Initialized;
}

void PanelTouchInput::Shutdown()
{
    m_Callbacks.clear();
    m_Initialized = false;
}

void PanelTouchInput::RegisterEventCallback(const DekiInput::InputEventCallback& callback)
{
    m_Callbacks.push_back(callback);
}

bool PanelTouchInput::GetPointerPosition(int32_t* x, int32_t* y) const
{
    if (!m_Pressed)
    {
        return false;
    }
    if (x)
    {
        *x = m_X;
    }
    if (y)
    {
        *y = m_Y;
    }
    return true;
}

void PanelTouchInput::Send(DekiInput::InputEventType type, int32_t x, int32_t y, bool pressed)
{
    DekiInput::InputEvent event{};
    event.type = type;
    event.x = x;
    event.y = y;
    event.pressed = pressed;
    event.timestamp = m_Now ? m_Now() : 0;
    for (const auto& callback : m_Callbacks)
    {
        if (callback)
        {
            callback(event);
        }
    }
}

void PanelTouchInput::Update()
{
    if (!m_Initialized)
    {
        return;
    }
    TouchPoint point;
    if (m_Controller->Read(&point, 1) == 0)
    {
        m_StaleFrames = 0;
        m_Stale = false;
        if (m_Pressed)
        {
            m_Pressed = false;
            Send(DekiInput::InputEventType::MouseButtonUp, m_X, m_Y, false);
        }
        return;
    }

    int32_t x = 0;
    int32_t y = 0;
    m_Mapper.Map(point.x, point.y, x, y);

    if (m_Stale)
    {
        if (x == m_X && y == m_Y)
        {
            return;
        }
        m_Stale = false;
    }
    if (!m_Pressed)
    {
        m_Pressed = true;
        m_StaleFrames = 0;
        m_X = x;
        m_Y = y;
        Send(DekiInput::InputEventType::MouseButtonDown, x, y, true);
        return;
    }
    if (x != m_X || y != m_Y)
    {
        m_StaleFrames = 0;
        m_X = x;
        m_Y = y;
        Send(DekiInput::InputEventType::MouseMove, x, y, false);
        return;
    }
    // With no INT line a controller may keep reporting the last touch after
    // the finger has gone: a point that has not moved for a few frames
    // counts as lifted.
    if (m_IntPin < 0 && m_Controller->MayRepeatLastTouch() && ++m_StaleFrames >= kStaleFrames)
    {
        m_Pressed = false;
        m_StaleFrames = 0;
        m_Stale = true;
        Send(DekiInput::InputEventType::MouseButtonUp, m_X, m_Y, false);
    }
}

}  // namespace DekiDisplay
