#pragma once

#include <cstdint>

#include <deki/SetupComponent.h>
#include <deki/reflection/Property.h>

#include "TactilityPackage.h"

namespace DekiTactility
{

/// Boot component that plays Deki's audio (deki-audio's IDekiAudio) through
/// Tactility's audio stream: the device's speaker, shared with the OS, which
/// resamples to the hardware's rate and owns the system volume. Does nothing
/// in a project without deki-audio, or on a device with no speaker.
DEKI_CATEGORY("Tactility")
DEKI_DESCRIPTION("Plays Deki's audio through Tactility's speaker.")
class DEKI_TACTILITY_API TactilityAudioSetup : public Deki::SetupComponent
{
public:
    DEKI_EXPORT
    DEKI_TOOLTIP("Samples per second the game plays at. Tactility resamples to the speaker's own rate, so this is the "
                 "rate the game's sounds were made at.")
    DEKI_UNIT(Frequency)
    DEKI_RANGE(8000, 48000)
    int32_t sampleRate = 16000;

    /// Software playback volume, 0..100 percent, on top of the system volume.
    DEKI_EXPORT
    DEKI_TOOLTIP("The game's own volume at startup, 0 to 100. The device's system volume still applies on top.")
    DEKI_RANGE(0, 100)
    int32_t initialVolume = 80;

    void Setup(SetupCallback onComplete) override;
    const char* GetSetupName() const override { return "Tactility Audio"; }
};

}  // namespace DekiTactility
