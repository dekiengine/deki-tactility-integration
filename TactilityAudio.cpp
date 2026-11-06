// Deki's audio on Tactility's audio stream.
//
// Tactility owns the speaker: an app does not drive the I2S pins as Deki's
// own firmware does (deki-audio's MAX98357A driver), it opens an output on
// the board's audio stream device and writes PCM to it. The stream resamples
// to the codec's native rate and applies the system volume, so the game can
// keep its own rate and its volume stays a software scale on top.
//
// The stream has one output slot for the whole device, so it must be closed
// when the game ends, or nothing else can play until a reboot. main() does
// that through ShutdownAudio() once the engine has stopped.

#include "TactilityAudio.h"

#if defined(DEKI_TACTILITY_TARGET) && !defined(DEKI_EDITOR) && defined(DEKI_PACKAGE_AUDIO)

#include <unistd.h>

#include <atomic>
#include <string>

#include <deki/LogSystem.h>
#include <deki/PackageConfig.h>

#include "DekiAudio.h"   // from deki-audio
#include "IDekiAudio.h"  // from deki-audio

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

class TactilityAudio : public DekiAudio::IDekiAudio
{
public:
    ~TactilityAudio() override { Shutdown(); }

    const char* GetPackageId() const override { return "audio"; }
    const char* GetPackageName() const override { return "Tactility Audio"; }
    void Update(float) override {}
    Deki::PackageState GetState() const override { return m_State; }
    const char* GetLastError() const override { return m_LastError.c_str(); }

    void Configure(const Deki::PackageConfig& config) override
    {
        m_SampleRate = config.GetInt("sampleRate", 16000);
        const int volume = config.GetInt("initialVolume", 80);
        m_Volume = volume < 0 ? 0.0f : (volume > 100 ? 1.0f : static_cast<float>(volume) * 0.01f);
    }

    bool Initialize() override
    {
        Shutdown();
        if (device_get_first_active_by_type(&AUDIO_STREAM_TYPE, &m_Device) != ERROR_NONE)
        {
            m_Device = nullptr;
            return Fail("this device has no audio stream (no speaker)");
        }
        if (!Open(m_SampleRate))
        {
            device_put(m_Device);
            m_Device = nullptr;
            return false;
        }
        m_Closing.store(false);
        m_State = Deki::PackageState::Running;
        return true;
    }

    // Waits out any write in progress, then gives the output back to the OS.
    void Shutdown() override
    {
        m_Closing.store(true);
        m_Stop.store(true);
        while (m_Writers.load() > 0)
        {
            usleep(1000);
        }
        Close();
        if (m_Device)
        {
            device_put(m_Device);
            m_Device = nullptr;
        }
        m_Playing = false;
        m_State = Deki::PackageState::Uninitialized;
    }

    bool PlayPCM(const int16_t* samples, size_t count, int sampleRate) override
    {
        if (!samples || count == 0)
        {
            return false;
        }
        // Counted before the check, so Shutdown() either sees this write or
        // this write sees it closing.
        m_Writers.fetch_add(1);
        if (m_Closing.load() || !m_Stream)
        {
            m_Writers.fetch_sub(1);
            return false;
        }
        // Tactility resamples from any rate, so a clip at another rate only
        // needs the stream reopened at it.
        if (sampleRate > 0 && sampleRate != m_StreamRate && !Open(sampleRate))
        {
            m_Writers.fetch_sub(1);
            return false;
        }

        m_Stop.store(false);
        m_Playing = true;
        // Scaled a chunk at a time on the stack, as deki-audio's own driver
        // does: a copy of a long clip would run internal RAM out.
        int16_t scaled[256];
        const float volume = m_Volume;
        bool ok = true;
        for (size_t done = 0; done < count && ok && !m_Stop.load();)
        {
            const size_t n = count - done < 256 ? count - done : 256;
            for (size_t i = 0; i < n; ++i)
            {
                int32_t s = static_cast<int32_t>(static_cast<float>(samples[done + i]) * volume);
                scaled[i] = static_cast<int16_t>(s > 32767 ? 32767 : (s < -32768 ? -32768 : s));
            }
            const uint8_t* data = reinterpret_cast<const uint8_t*>(scaled);
            size_t remaining = n * sizeof(int16_t);
            while (remaining > 0)
            {
                size_t written = 0;
                const error_t error =
                    audio_stream_write(m_Stream, data, remaining, &written, pdMS_TO_TICKS(kWriteTimeoutMs));
                if (error != ERROR_NONE || written == 0)
                {
                    m_LastError = std::string("audio stream write failed: ") + error_to_string(error);
                    ok = false;
                    break;
                }
                data += written;
                remaining -= written;
            }
            done += n;
        }
        m_Playing = false;
        m_Writers.fetch_sub(1);
        return ok;
    }

    // Ends the clip being played after the chunk in flight.
    void Stop() override { m_Stop.store(true); }

    bool IsPlaying() const override { return m_Playing; }

    void SetVolume(float volume) override { m_Volume = volume < 0.0f ? 0.0f : (volume > 1.0f ? 1.0f : volume); }
    float GetVolume() const override { return m_Volume; }

    bool IsHardwareConnected() const override { return m_Stream != nullptr; }

private:
    bool Open(int sampleRate)
    {
        Close();
        AudioStreamConfig config{};
        config.sample_rate = static_cast<uint32_t>(sampleRate);
        config.bits_per_sample = 16;
        config.channels = 1;
        const error_t error = audio_stream_open_output(m_Device, &config, &m_Stream);
        if (error != ERROR_NONE)
        {
            m_Stream = nullptr;
            // Taken by another app, or switched off in the OS's audio settings.
            return Fail(std::string("cannot open the speaker: ") + error_to_string(error));
        }
        m_StreamRate = sampleRate;
        return true;
    }

    void Close()
    {
        if (m_Stream)
        {
            audio_stream_close(m_Stream);
            m_Stream = nullptr;
        }
    }

    bool Fail(const std::string& error)
    {
        m_LastError = error;
        m_State = Deki::PackageState::Error;
        return false;
    }

    Device* m_Device = nullptr;
    AudioStreamHandle m_Stream = nullptr;
    int m_SampleRate = 16000;
    int m_StreamRate = 0;
    float m_Volume = 0.8f;
    bool m_Playing = false;
    std::atomic<bool> m_Closing{ true };
    std::atomic<bool> m_Stop{ false };
    std::atomic<int> m_Writers{ 0 };
    Deki::PackageState m_State = Deki::PackageState::Uninitialized;
    std::string m_LastError;
};

// Made once and never freed: the game may hold the pointer until it stops.
TactilityAudio* s_Audio = nullptr;

}  // namespace

bool StartAudio(int sampleRate, int volumePercent)
{
    if (!s_Audio)
    {
        s_Audio = new TactilityAudio();
    }
    Deki::PackageConfig config;
    config.packageId = "audio";
    config.enabled = true;
    config.settings["sampleRate"] = std::to_string(sampleRate);
    config.settings["initialVolume"] = std::to_string(volumePercent);
    s_Audio->Configure(config);
    if (!s_Audio->Initialize())
    {
        DEKI_LOG_WARNING("TactilityAudio: %s; playing silently", s_Audio->GetLastError());
        return false;
    }
    DekiAudio::DekiAudio::SetCurrent(s_Audio);
    DEKI_LOG_INFO("TactilityAudio: speaker open at %d Hz", sampleRate);
    return true;
}

void ShutdownAudio()
{
    if (!s_Audio)
    {
        return;
    }
    DekiAudio::DekiAudio::SetCurrent(nullptr);
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

void ShutdownAudio() {}

}  // namespace DekiTactility

#endif
