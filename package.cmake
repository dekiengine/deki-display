# Package descriptor for deki-engine auto-discovery
set(PACKAGE_DISPLAY_NAME "Display")
set(PACKAGE_PREFIX "DekiDisplay")
set(PACKAGE_UPPER "DISPLAY")
set(PACKAGE_TARGET "deki-display")
set(PACKAGE_FILE_PREFIX "Display")
set(PACKAGE_ENTRY DekiDisplayPackage.cpp)
# Calls into deki-spi's bus registry and deki-gpio's pins.
set(PACKAGE_LINK_DEPS deki-spi deki-gpio)
