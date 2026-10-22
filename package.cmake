# Package descriptor for deki-engine auto-discovery
set(PACKAGE_DISPLAY_NAME "Display")
set(PACKAGE_PREFIX "DekiDisplay")
set(PACKAGE_UPPER "DISPLAY")
set(PACKAGE_TARGET "deki-display")
set(PACKAGE_FILE_PREFIX "Display")
set(PACKAGE_ENTRY DekiDisplayPackage.cpp)
# Calls into deki-spi's bus registry, deki-gpio's pins, deki-i2c's buses, deki-input, and deki-rendering's
# scan-order registry.
set(PACKAGE_LINK_DEPS deki-spi deki-gpio deki-i2c deki-input deki-rendering)
