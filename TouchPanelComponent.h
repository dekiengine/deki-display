#pragma once

#include <cstdint>
#include <deki/SetupComponent.h>
#include <deki/reflection/Property.h>
#include "DekiDisplayPackage.h"

namespace DekiDisplay
{

/// The touch controllers this package reads. Scenes store the name.
/// CST816 covers the CST816S and CST816T.
enum class TouchController : uint8_t
{
    GT911 = 0,
    CST816 = 1
};

enum class TouchRotation : uint8_t
{
    None = 0,
    CW_90 = 1,
    CW_180 = 2,
    CW_270 = 3,
    Mirror = 4,
    Mirror_CW_90 = 5,
    Mirror_CW_180 = 6,
    Mirror_CW_270 = 7
};

/// Starts the panel's touch controller from the boot scene and gives it to
/// deki-input, which then sends a press, moves and a release at screen pixels,
/// turned as the display is.
///
/// Comes after the Display Panel component (it turns with the display) and
/// the I2C Bus component for its bus, in PlatformSetupComponent's list. On a
/// platform with no I2C (the editor, the simulator) it reports success and
/// starts nothing.
DEKI_CATEGORY("Display")
DEKI_DESCRIPTION("Reads a panel's touch controller (GT911, CST816, ...) on an I2C bus with Deki's own display "
                 "library, as pointer input.")
class DEKI_DISPLAY_API TouchPanelComponent : public Deki::SetupComponent
{
public:
    DEKI_EXPORT
    DEKI_TOOLTIP("The touch controller IC")
    TouchController controller = TouchController::GT911;

    DEKI_EXPORT
    DEKI_TOOLTIP("The I2C port it is on (an I2C Bus component must start that port before this one)")
    DEKI_RANGE(0, 3)
    int32_t i2cPort = 0;

    DEKI_EXPORT
    DEKI_TOOLTIP("Its I2C address; 0 takes the controller's usual one (GT911: 0x5D, falling back to 0x14; CST816: "
                 "0x15)")
    DEKI_RANGE(0, 127)
    int32_t i2cAddress = 0;

    DEKI_EXPORT
    DEKI_TOOLTIP("Interrupt pin: the controller pulls it low when a touch report is waiting (-1 = not wired, read "
                 "every frame)")
    DEKI_RANGE(-1, 48)
    int32_t intPin = -1;

    DEKI_EXPORT
    DEKI_TOOLTIP("Reset pin (-1 = not connected)")
    DEKI_RANGE(-1, 48)
    int32_t rstPin = -1;

    DEKI_GROUP("Touch Panel Bounds")
    DEKI_EXPORT
    DEKI_TOOLTIP("Minimum raw X from the touch controller, along the panel's width before the display is rotated")
    int32_t xMin = 0;

    DEKI_EXPORT
    DEKI_TOOLTIP("Maximum raw X from the touch controller, along the panel's width before the display is rotated (a "
                 "240x320 panel shown in landscape: 239)")
    int32_t xMax = 239;

    DEKI_EXPORT
    DEKI_TOOLTIP("Minimum raw Y from the touch controller, along the panel's height before the display is rotated")
    int32_t yMin = 0;

    DEKI_EXPORT
    DEKI_TOOLTIP("Maximum raw Y from the touch controller, along the panel's height before the display is rotated (a "
                 "240x320 panel shown in landscape: 319)")
    int32_t yMax = 319;

    DEKI_GROUP("Advanced")
    DEKI_EXPORT
    DEKI_TOOLTIP("How the touch controller is turned against the panel, added to the display rotation. None when the "
                 "controller reports in the panel's own orientation")
    TouchRotation offsetRotation = TouchRotation::None;

    void Setup(SetupCallback onComplete) override;
    const char* GetSetupName() const override { return "Touch Panel"; }
};

}  // namespace DekiDisplay
