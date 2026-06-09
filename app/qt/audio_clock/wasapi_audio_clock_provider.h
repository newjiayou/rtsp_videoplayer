#pragma once

#include "audio_clock_provider.h"

class WasapiAudioClockProvider final : public IAudioClockProvider {
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
    AudioClockDiagnostics diagnostics_{};
};
