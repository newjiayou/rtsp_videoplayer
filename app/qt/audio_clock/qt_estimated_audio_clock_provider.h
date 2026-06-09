#pragma once

#include "audio_clock_provider.h"

#include <mutex>

class QtEstimatedAudioClockProvider final : public IAudioClockProvider {
public:
    QString ProviderName() const override;
    bool Initialize(const AudioClockInitParams& params) override;
    void Reset() override;
    void OnAudioSinkStarted(QAudioSink* sink) override;
    void OnAudioSamplesQueued(int64_t media_pts_ms, int sample_count) override;
    void OnAudioSamplesDequeued(int64_t media_pts_ms, int sample_offset_samples) override;
    int64_t GetAudioClockMs() override;
    bool IsHardwareBacked() const override;
    AudioClockDiagnostics GetDiagnostics() const override;

private:
    int64_t GetAudioClockMsLocked(AudioClockDiagnostics* diagnostics) const;
    int64_t DurationMsForSamplesLocked(int sample_count) const;

private:
    mutable std::mutex mutex_;
    QAudioSink* sink_{nullptr};
    int sample_rate_{0};
    int channels_{0};
    int bytes_per_sample_{0};
    bool local_playback_mode_{false};
    int64_t anchor_media_pts_ms_{0};
    qint64 anchor_processed_us_{0};
    int64_t latest_audio_end_pts_ms_{0};
    mutable int64_t last_returned_clock_ms_{0};
    mutable uint64_t query_fail_count_{0};
    bool anchor_valid_{false};
};
