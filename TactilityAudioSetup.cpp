#include "TactilityAudioSetup.h"

#include <deki/LogSystem.h>

#include "TactilityAudio.h"

#if defined(DEKI_PACKAGE_AUDIO) && !defined(DEKI_EDITOR)
#include "DekiAudioInit.h"  // from deki-audio
#endif

namespace DekiTactility
{

#if !defined(DEKI_EDITOR) && defined(DEKI_PACKAGE_TACTILITY)

void TactilityAudioSetup::Setup(SetupCallback onComplete)
{
#if defined(DEKI_PACKAGE_AUDIO)
    // deki-audio's loader and stream reading; safe to repeat.
    DekiAudioInitSystem();
    StartAudio(sampleRate, initialVolume);
#else
    DEKI_LOG_WARNING("TactilityAudioSetup: the project has no deki-audio package, so there is nothing to play");
#endif
    // Sound is optional: a device without a speaker, or with its output
    // switched off, still runs the game, silently.
    onComplete(true);
}

#else

void TactilityAudioSetup::Setup(SetupCallback onComplete)
{
    onComplete(true);
}

#endif

}  // namespace DekiTactility
