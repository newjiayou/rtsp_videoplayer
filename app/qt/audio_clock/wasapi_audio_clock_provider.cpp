#include "audio_clock/wasapi_audio_clock_provider.h"

#include <QDebug>

QString WasapiAudioClockProvider::ProviderName() const {
    return QStringLiteral("wasapi");
}

bool WasapiAudioClockProvider::Initialize(const AudioClockInitParams&) {
    diagnostics_ = {};
    diagnostics_.provider_name = ProviderName();
    diagnostics_.hardware_backed = false;
    qInfo() << "[EdgeLivePlayer] audio clock provider unavailable:"
            << ProviderName()
            << "Qt QAudioSink native WASAPI stream handle is not exposed in the current playback path";
    return false;
}

void WasapiAudioClockProvider::Reset() {
    diagnostics_ = {};
    diagnostics_.provider_name = ProviderName();
}

void WasapiAudioClockProvider::OnAudioSinkStarted(QAudioSink*) {}

void WasapiAudioClockProvider::OnAudioSamplesQueued(int64_t, int) {}

void WasapiAudioClockProvider::OnAudioSamplesDequeued(int64_t, int) {}

int64_t WasapiAudioClockProvider::GetAudioClockMs() {
    ++diagnostics_.query_fail_count;
    return 0;
}

bool WasapiAudioClockProvider::IsHardwareBacked() const {
    return false;
}

AudioClockDiagnostics WasapiAudioClockProvider::GetDiagnostics() const {
    return diagnostics_;
}
