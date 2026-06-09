#include "audio_clock/audio_clock_provider.h"

#include "audio_clock/qt_estimated_audio_clock_provider.h"

#if defined(_WIN32)
#include "audio_clock/wasapi_audio_clock_provider.h"
#elif defined(__APPLE__)
#include "audio_clock/coreaudio_audio_clock_provider.h"
#elif defined(__linux__) && defined(EDGELIVE_HAVE_ALSA_AUDIO_CLOCK)
#include "audio_clock/alsa_audio_clock_provider.h"
#endif

#include <QDebug>

#include <memory>

namespace {

class AutoAudioClockProvider final : public IAudioClockProvider {
public:
    AutoAudioClockProvider(std::unique_ptr<IAudioClockProvider> primary,
                           std::unique_ptr<IAudioClockProvider> fallback)
        : primary_(std::move(primary)),
          fallback_(std::move(fallback)),
          active_(fallback_.get()) {}

    QString ProviderName() const override {
        return active_ != nullptr ? active_->ProviderName() : QStringLiteral("unknown");
    }

    bool Initialize(const AudioClockInitParams& params) override {
        params_ = params;
        fallback_count_ = 0;
        if (fallback_ == nullptr || !fallback_->Initialize(params_)) {
            active_ = nullptr;
            return false;
        }

        active_ = fallback_.get();
        if (primary_ != nullptr && primary_->Initialize(params_)) {
            active_ = primary_.get();
            qInfo() << "[EdgeLivePlayer] audio clock provider selected:" << active_->ProviderName();
            return true;
        }

        qInfo() << "[EdgeLivePlayer] audio clock provider selected:" << active_->ProviderName()
                << "(fallback)";
        return true;
    }

    void Reset() override {
        params_ = {};
        fallback_count_ = 0;
        if (primary_ != nullptr) {
            primary_->Reset();
        }
        if (fallback_ != nullptr) {
            fallback_->Reset();
        }
        active_ = fallback_.get();
    }

    void OnAudioSinkStarted(QAudioSink* sink) override {
        params_.sink = sink;
        if (primary_ != nullptr) {
            primary_->OnAudioSinkStarted(sink);
        }
        if (fallback_ != nullptr) {
            fallback_->OnAudioSinkStarted(sink);
        }
    }

    void OnAudioSamplesQueued(int64_t media_pts_ms, int sample_count) override {
        if (primary_ != nullptr) {
            primary_->OnAudioSamplesQueued(media_pts_ms, sample_count);
        }
        if (fallback_ != nullptr) {
            fallback_->OnAudioSamplesQueued(media_pts_ms, sample_count);
        }
    }

    void OnAudioSamplesDequeued(int64_t media_pts_ms, int sample_offset_samples) override {
        if (primary_ != nullptr) {
            primary_->OnAudioSamplesDequeued(media_pts_ms, sample_offset_samples);
        }
        if (fallback_ != nullptr) {
            fallback_->OnAudioSamplesDequeued(media_pts_ms, sample_offset_samples);
        }
    }

    int64_t GetAudioClockMs() override {
        if (active_ == nullptr) {
            return 0;
        }

        int64_t clock_ms = active_->GetAudioClockMs();
        if (active_ == primary_.get() && clock_ms <= 0 && fallback_ != nullptr) {
            ++fallback_count_;
            active_ = fallback_.get();
            qWarning() << "[EdgeLivePlayer] audio clock provider fallback:"
                       << "from=" << primary_->ProviderName()
                       << "to=" << active_->ProviderName();
            clock_ms = active_->GetAudioClockMs();
        }
        return clock_ms;
    }

    bool IsHardwareBacked() const override {
        return active_ != nullptr && active_->IsHardwareBacked();
    }

    AudioClockDiagnostics GetDiagnostics() const override {
        AudioClockDiagnostics diagnostics;
        if (active_ != nullptr) {
            diagnostics = active_->GetDiagnostics();
        }
        diagnostics.provider_name = ProviderName();
        diagnostics.hardware_backed = IsHardwareBacked();
        diagnostics.fallback_count = fallback_count_;
        return diagnostics;
    }

private:
    AudioClockInitParams params_{};
    std::unique_ptr<IAudioClockProvider> primary_;
    std::unique_ptr<IAudioClockProvider> fallback_;
    IAudioClockProvider* active_{nullptr};
    uint64_t fallback_count_{0};
};

} // namespace

std::unique_ptr<IAudioClockProvider> CreateAudioClockProvider() {
    std::unique_ptr<IAudioClockProvider> primary;
#if defined(_WIN32)
    primary = std::make_unique<WasapiAudioClockProvider>();
#elif defined(__APPLE__)
    primary = std::make_unique<CoreAudioClockProvider>();
#elif defined(__linux__) && defined(EDGELIVE_HAVE_ALSA_AUDIO_CLOCK)
    primary = std::make_unique<AlsaAudioClockProvider>();
#endif
    return std::make_unique<AutoAudioClockProvider>(
        std::move(primary),
        std::make_unique<QtEstimatedAudioClockProvider>());
}
