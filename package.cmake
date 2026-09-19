# Package descriptor for deki-engine auto-discovery
set(PACKAGE_DISPLAY_NAME "Tactility")
set(PACKAGE_PREFIX "DekiTactility")
set(PACKAGE_UPPER "TACTILITY")
set(PACKAGE_TARGET "deki-tactility")
set(PACKAGE_FILE_PREFIX "Tactility")
set(PACKAGE_SOURCES
    TactilityDisplaySetup.cpp
    TactilityInputSetup.cpp
    TactilityDisplay.cpp
    TactilityWindowDisplay.cpp
    TactilityInput.cpp
    TactilityFileSystem.cpp
    TactilityApp.cpp
)
set(PACKAGE_ENTRY TactilityPackage.cpp)
set(PACKAGE_LINK_DEPS deki-input)
