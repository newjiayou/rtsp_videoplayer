#pragma once

#include <QAudioDevice>
#include <QAudioFormat>
#include <QString>

#include <cstdint>
#include <memory>

class QAudioSink;

struct AudioClockInitParams {
    int sample_rate{0};
    int channels{0};
    int bytes_per_sample{0};
    bool local_playback_mode{false};
    QAudioDevice output_device;
    QAudioFormat format;
    QAudioSink* sink{nullptr};
};

struct AudioClockDiagnostics {
    QString provider_name{"unknown"};
    bool hardware_backed{false};
    uint64_t query_fail_count{0};
    uint64_t fallback_count{0};
    int64_t anchor_pts_ms{0};
    int64_t processed_ms{0};
    int64_t device_delay_ms{0};
    int64_t clock_ms{0};
};

class IAudioClockProvider {
public:
    virtual ~IAudioClockProvider() = default;

    virtual QString ProviderName() const = 0;
    virtual bool Initialize(const AudioClockInitParams& params) = 0;
    virtual void Reset() = 0;
    virtual void OnAudioSinkStarted(QAudioSink* sink) = 0;
    virtual void OnAudioSamplesQueued(int64_t media_pts_ms, int sample_count) = 0;
    virtual void OnAudioSamplesDequeued(int64_t media_pts_ms, int sample_offset_samples) = 0;
    virtual int64_t GetAudioClockMs() = 0;
    virtual bool IsHardwareBacked() const = 0;
    virtual AudioClockDiagnostics GetDiagnostics() const = 0;
};

std::unique_ptr<IAudioClockProvider> CreateAudioClockProvider();
