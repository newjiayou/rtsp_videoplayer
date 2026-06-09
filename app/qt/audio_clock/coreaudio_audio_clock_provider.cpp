#include "audio_clock/coreaudio_audio_clock_provider.h"

#include <QDebug>

QString CoreAudioClockProvider::ProviderName() const {
    return QStringLiteral("coreaudio");
}

bool CoreAudioClockProvider::Initialize(const AudioClockInitParams&) {
    diagnostics_ = {};
    diagnostics_.provider_name = ProviderName();
    diagnostics_.hardware_backed = false;
    qInfo() << "[EdgeLivePlayer] audio clock provider unavailable:"
            << ProviderName()
            << "Qt QAudioSink native CoreAudio stream handle is not exposed in the current playback path";
    return false;
}

void CoreAudioClockProvider::Reset() {
    diagnostics_ = {};
    diagnostics_.provider_name = ProviderName();
}

void CoreAudioClockProvider::OnAudioSinkStarted(QAudioSink*) {}

void CoreAudioClockProvider::OnAudioSamplesQueued(int64_t, int) {}

void CoreAudioClockProvider::OnAudioSamplesDequeued(int64_t, int) {}

int64_t CoreAudioClockProvider::GetAudioClockMs() {
    ++diagnostics_.query_fail_count;
    return 0;
}

bool CoreAudioClockProvider::IsHardwareBacked() const {
    return false;
}

AudioClockDiagnostics CoreAudioClockProvider::GetDiagnostics() const {
    return diagnostics_;
}
