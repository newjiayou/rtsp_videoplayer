#include "audio_clock/qt_estimated_audio_clock_provider.h"

#include <QAudioSink>
#include <QDateTime>
#include <QDebug>

#include <algorithm>

QString QtEstimatedAudioClockProvider::ProviderName() const {
    return QStringLiteral("qt_estimated");
}

bool QtEstimatedAudioClockProvider::Initialize(const AudioClockInitParams& params) {
    std::lock_guard<std::mutex> lock(mutex_);
    sample_rate_ = params.sample_rate;
    channels_ = params.channels;
    bytes_per_sample_ = params.bytes_per_sample;
    local_playback_mode_ = params.local_playback_mode;
    sink_ = params.sink;
    anchor_media_pts_ms_ = 0;
    anchor_processed_us_ = 0;
    latest_audio_end_pts_ms_ = 0;
    last_returned_clock_ms_ = 0;
    query_fail_count_ = 0;
    anchor_valid_ = false;
    return sample_rate_ > 0 && channels_ > 0 && bytes_per_sample_ > 0;
}

void QtEstimatedAudioClockProvider::Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    sink_ = nullptr;
    sample_rate_ = 0;
    channels_ = 0;
    bytes_per_sample_ = 0;
    local_playback_mode_ = false;
    anchor_media_pts_ms_ = 0;
    anchor_processed_us_ = 0;
    latest_audio_end_pts_ms_ = 0;
    last_returned_clock_ms_ = 0;
    query_fail_count_ = 0;
    anchor_valid_ = false;
}

void QtEstimatedAudioClockProvider::OnAudioSinkStarted(QAudioSink* sink) {
    std::lock_guard<std::mutex> lock(mutex_);
    sink_ = sink;
    anchor_processed_us_ = 0;
    anchor_valid_ = false;
    last_returned_clock_ms_ = 0;
}

void QtEstimatedAudioClockProvider::OnAudioSamplesQueued(int64_t media_pts_ms, int sample_count) {
    std::lock_guard<std::mutex> lock(mutex_);
    latest_audio_end_pts_ms_ = media_pts_ms + DurationMsForSamplesLocked(sample_count);
}

void QtEstimatedAudioClockProvider::OnAudioSamplesDequeued(int64_t media_pts_ms, int sample_offset_samples) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (anchor_valid_ || sink_ == nullptr || sample_rate_ <= 0) {
        return;
    }
    anchor_media_pts_ms_ = media_pts_ms + DurationMsForSamplesLocked(sample_offset_samples);
    anchor_processed_us_ = sink_->processedUSecs();
    anchor_valid_ = true;
    qInfo() << "[EdgeLivePlayer] audio clock anchor:"
            << "provider=" << ProviderName()
            << "pts_ms=" << anchor_media_pts_ms_
            << "processed_us=" << anchor_processed_us_;
}

int64_t QtEstimatedAudioClockProvider::GetAudioClockMs() {
    std::lock_guard<std::mutex> lock(mutex_);
    AudioClockDiagnostics diagnostics;
    const int64_t clock_ms = GetAudioClockMsLocked(&diagnostics);
    if (!local_playback_mode_ && diagnostics.anchor_pts_ms > 0) {
        static qint64 last_audio_clock_detail_log_ms = 0;
        const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
        if (now_ms - last_audio_clock_detail_log_ms >= 1000) {
            last_audio_clock_detail_log_ms = now_ms;
            qInfo() << "[EdgeLivePlayer] audio clock detail:"
                    << "provider=" << ProviderName()
                    << "anchor_pts_ms=" << diagnostics.anchor_pts_ms
                    << "processed_ms=" << diagnostics.processed_ms
                    << "device_delay_ms=" << diagnostics.device_delay_ms
                    << "last_media_pts_ms=" << latest_audio_end_pts_ms_
                    << "final_clock_ms=" << clock_ms;
        }
    }
    return clock_ms;
}

bool QtEstimatedAudioClockProvider::IsHardwareBacked() const {
    return false;
}

AudioClockDiagnostics QtEstimatedAudioClockProvider::GetDiagnostics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    AudioClockDiagnostics diagnostics;
    diagnostics.provider_name = ProviderName();
    diagnostics.hardware_backed = false;
    diagnostics.query_fail_count = query_fail_count_;
    diagnostics.clock_ms = GetAudioClockMsLocked(&diagnostics);
    return diagnostics;
}

int64_t QtEstimatedAudioClockProvider::GetAudioClockMsLocked(AudioClockDiagnostics* diagnostics) const {
    if (diagnostics != nullptr) {
        diagnostics->provider_name = ProviderName();
        diagnostics->hardware_backed = false;
        diagnostics->query_fail_count = query_fail_count_;
        diagnostics->anchor_pts_ms = anchor_valid_ ? anchor_media_pts_ms_ : 0;
    }
    if (sink_ == nullptr || !anchor_valid_ || sample_rate_ <= 0 || channels_ <= 0 || bytes_per_sample_ <= 0) {
        return 0;
    }

    const int bytes_per_second = sample_rate_ * channels_ * bytes_per_sample_;
    if (bytes_per_second <= 0) {
        return 0;
    }

    const qint64 processed_us = sink_->processedUSecs();
    if (processed_us < anchor_processed_us_) {
        ++query_fail_count_;
        return last_returned_clock_ms_;
    }
    const qint64 delta_us = processed_us - anchor_processed_us_;
    const int64_t consumed_clock_ms = anchor_media_pts_ms_ + delta_us / 1000;
    const qint64 bytes_free = std::max<qint64>(0, sink_->bytesFree());
    const qint64 buffer_size = std::max<qint64>(0, sink_->bufferSize());
    const qint64 pending_bytes = std::max<qint64>(0, buffer_size - bytes_free);
    const int64_t device_delay_ms = static_cast<int64_t>(pending_bytes * 1000 / bytes_per_second);
    int64_t final_clock_ms = std::max<int64_t>(0, consumed_clock_ms - device_delay_ms);
    if (last_returned_clock_ms_ > 0 && final_clock_ms < last_returned_clock_ms_) {
        final_clock_ms = last_returned_clock_ms_;
    }

    if (diagnostics != nullptr) {
        diagnostics->processed_ms = delta_us / 1000;
        diagnostics->device_delay_ms = device_delay_ms;
    }
    last_returned_clock_ms_ = final_clock_ms;
    return final_clock_ms;
}

int64_t QtEstimatedAudioClockProvider::DurationMsForSamplesLocked(int sample_count) const {
    if (sample_count <= 0 || sample_rate_ <= 0) {
        return 0;
    }
    return static_cast<int64_t>(sample_count) * 1000 / sample_rate_;
}
