#pragma once

// Deki's audio on Tactility's audio stream (TactilityAudio.cpp).

namespace DekiTactility
{

/// Opens the speaker stream and makes it deki-audio's current output. False
/// when there is no speaker, or the project has no deki-audio.
bool StartAudio(int sampleRate, int volumePercent);

/// Closes the speaker stream, if one is open. main() calls it once the game
/// has stopped: the stream belongs to the OS and would outlive the app.
void ShutdownAudio();

}  // namespace DekiTactility
