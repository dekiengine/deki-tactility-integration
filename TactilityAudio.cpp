// Deki's audio on Tactility's audio stream.
//
// Tactility owns the speaker: an app does not drive the I2S pins as Deki's
// own firmware does (deki-audio's MAX98357A driver), it opens an output on
// the board's audio stream device and writes PCM to it. The stream resamples
// to the codec's native rate and applies the system volume, so the game can
// keep its own rate and the mixer's volume stays a software scale on top.
//
// The stream has one output slot for the whole device, so it must be closed
// when the game ends, or nothing else can play until a reboot. main() does
// that through ShutdownAudio() once the engine has stopped.

#include "TactilityAudio.h"

#if defined(DEKI_TACTILITY_TARGET) && !defined(DEKI_EDITOR) && defined(DEKI_PACKAGE_AUDIO)

#include <string>

#include <deki/LogSystem.h>
#include <deki/PackageConfig.h>

#include "AudioOutput.h"  // from deki-audio
#include "IDekiAudio.h"   // from deki-audio
#include "Mixer.h"        // from deki-audio
#include "PumpWorker.h"   // from deki-audio

extern "C"
{
#include <tactility/device.h>
#include <tactility/drivers/audio_stream.h>
#include <tactility/error.h>
}

namespace DekiTactility
{
namespace
{

// A write that cannot go out in this long gives up; the stream may have
// been disabled from the OS's audio settings.
constexpr uint32_t kWriteTimeoutMs = 1000;
// Frames mixed per write: about 12 ms at 22050 Hz.
constexpr size_t kChunkFrames = 256;

// The speaker as deki-audio's output: a thread of the game's own mixes a
// chunk and writes it to the stream, the write waiting for room. The SDK asks
// for exactly that: stream writes from the app's own task, never the main or
// LVGL thread.
class TactilityAudio : public DekiAudio::IDekiAudio
{
public:
    ~TactilityAudio() override { Shutdown(); }

    const char* GetPackageId() const override { return "audio"; }
    const char* GetPackageName() const override { return "Tactility Audio"; }
    void Update(float) override {}
    Deki::PackageState GetState() const override { return m_State; }
    const char* GetLastError() const override { return m_LastError.c_str(); }

    void Configure(const Deki::PackageConfig& config) override { m_SampleRate = config.GetInt("sampleRate", 22050); }

    bool Initialize() override
    {
        Shutdown();
        if (device_get_first_active_by_type(&AUDIO_STREAM_TYPE, &m_Device) != ERROR_NONE)
        {
            m_Device = nullptr;
            return Fail("this device has no audio stream (no speaker)");
        }
        AudioStreamConfig config{};
        config.sample_rate = static_cast<uint32_t>(m_SampleRate);
        config.bits_per_sample = 16;
        config.channels = 1;
        const error_t error = audio_stream_open_output(m_Device, &config, &m_Stream);
        if (error != ERROR_NONE)
        {
            m_Stream = nullptr;
            device_put(m_Device);
            m_Device = nullptr;
            // Taken by another app, or switched off in the OS's audio settings.
            return Fail(std::string("cannot open the speaker: ") + error_to_string(error));
        }
        m_State = Deki::PackageState::Running;
        return true;
    }

    // Stops the thread, then gives the output back to the OS.
    void Shutdown() override
    {
        m_Pump.Stop();
        if (m_Stream)
        {
            audio_stream_close(m_Stream);
            m_Stream = nullptr;
        }
        if (m_Device)
        {
            device_put(m_Device);
            m_Device = nullptr;
        }
        m_State = Deki::PackageState::Uninitialized;
    }

    int GetSampleRate() const override { return m_SampleRate; }
    int GetChannels() const override { return 1; }

    bool Start() override
    {
        if (!m_Stream)
        {
            m_LastError = "no speaker stream";
            return false;
        }
        if (m_Pump.IsRunning())
        {
            return true;
        }
        auto write = [](void* self, const int16_t* samples, size_t frames)
        { return static_cast<TactilityAudio*>(self)->Write(samples, frames); };
        if (!m_Pump.Start("deki_audio", 1, kChunkFrames, write, this))
        {
            m_LastError = "no memory or thread for the audio";
            return false;
        }
        return true;
    }

    void Stop() override { m_Pump.Stop(); }
    bool IsRunning() const override { return m_Pump.IsRunning(); }
    bool IsHardwareConnected() const override { return m_Stream != nullptr; }

private:
    bool Write(const int16_t* samples, size_t frames)
    {
        const uint8_t* data = reinterpret_cast<const uint8_t*>(samples);
        size_t remaining = frames * sizeof(int16_t);
        while (remaining > 0)
        {
            size_t written = 0;
            const error_t error =
                audio_stream_write(m_Stream, data, remaining, &written, pdMS_TO_TICKS(kWriteTimeoutMs));
            if (error != ERROR_NONE || written == 0)
            {
                return false;
            }
            data += written;
            remaining -= written;
        }
        return true;
    }

    bool Fail(const std::string& error)
    {
        m_LastError = error;
        m_State = Deki::PackageState::Error;
        return false;
    }

    DekiAudio::PumpWorker m_Pump;
    Device* m_Device = nullptr;
    AudioStreamHandle m_Stream = nullptr;
    int m_SampleRate = 22050;
    Deki::PackageState m_State = Deki::PackageState::Uninitialized;
    std::string m_LastError;
};

// Made once and never freed: the mixer may hold the pointer until the game
// stops.
TactilityAudio* s_Audio = nullptr;

}  // namespace

bool StartAudio(int sampleRate, int volumePercent)
{
    // The mixer first, so it outlives the output.
    DekiAudio::Mixer& mixer = DekiAudio::Mixer::Instance();
    if (!s_Audio)
    {
        s_Audio = new TactilityAudio();
    }
    Deki::PackageConfig config;
    config.packageId = "audio";
    config.enabled = true;
    config.settings["sampleRate"] = std::to_string(sampleRate);
    s_Audio->Configure(config);
    const int volume = volumePercent < 0 ? 0 : (volumePercent > 100 ? 100 : volumePercent);
    mixer.SetMasterVolume(static_cast<float>(volume) * 0.01f);
    if (!s_Audio->Initialize())
    {
        DEKI_LOG_WARNING("TactilityAudio: %s; playing silently", s_Audio->GetLastError());
        return false;
    }
    DekiAudio::AudioOutput::SetCurrent(s_Audio);
    DEKI_LOG_INFO("TactilityAudio: speaker open at %d Hz", sampleRate);
    return true;
}

void ShutdownAudio()
{
    if (!s_Audio)
    {
        return;
    }
    DekiAudio::AudioOutput::SetCurrent(nullptr);
    s_Audio->Shutdown();
}

}  // namespace DekiTactility

#else

namespace DekiTactility
{

bool StartAudio(int, int)
{
    return false;
}

void ShutdownAudio()
{
}

}  // namespace DekiTactility

#endif
