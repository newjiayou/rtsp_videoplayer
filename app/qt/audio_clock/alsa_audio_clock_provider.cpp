#include "audio_clock/alsa_audio_clock_provider.h"

#include <QDebug>

QString AlsaAudioClockProvider::ProviderName() const {
    return QStringLiteral("alsa");
}

bool AlsaAudioClockProvider::Initialize(const AudioClockInitParams&) {
    diagnostics_ = {};
    diagnostics_.provider_name = ProviderName();
    diagnostics_.hardware_backed = false;
    qInfo() << "[EdgeLivePlayer] audio clock provider unavailable:"
            << ProviderName()
            << "Qt QAudioSink native ALSA PCM handle is not exposed in the current playback path";
    return false;
}

void AlsaAudioClockProvider::Reset() {
    diagnostics_ = {};
    diagnostics_.provider_name = ProviderName();
}

void AlsaAudioClockProvider::OnAudioSinkStarted(QAudioSink*) {}

void AlsaAudioClockProvider::OnAudioSamplesQueued(int64_t, int) {}

void AlsaAudioClockProvider::OnAudioSamplesDequeued(int64_t, int) {}

int64_t AlsaAudioClockProvider::GetAudioClockMs() {
    ++diagnostics_.query_fail_count;
    return 0;
}

bool AlsaAudioClockProvider::IsHardwareBacked() const {
    return false;
}

AudioClockDiagnostics AlsaAudioClockProvider::GetDiagnostics() const {
    return diagnostics_;
}
