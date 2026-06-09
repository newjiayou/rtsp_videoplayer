#include "alsa_audio_output.h"

#include <QDebug>

#include <algorithm>
#include <utility>

#if defined(EDGELIVE_HAVE_ALSA_NATIVE_OUTPUT)
#include <alsa/asoundlib.h>
#include <cerrno>
#endif

namespace {

constexpr unsigned int kLocalBufferTimeUs = 120000;
constexpr unsigned int kLiveBufferTimeUs = 120000;
constexpr unsigned int kLocalPeriodTimeUs = 30000;
constexpr unsigned int kLivePeriodTimeUs = 30000;
constexpr int kPcmWaitTimeoutMs = 20;

} // namespace

AlsaAudioOutput::AlsaAudioOutput() = default;

AlsaAudioOutput::~AlsaAudioOutput() {
    Stop();
}

#if defined(EDGELIVE_HAVE_ALSA_NATIVE_OUTPUT)

bool AlsaAudioOutput::Initialize(int sample_rate, int channels, int bytes_per_sample, bool local_playback_mode) {
    if (sample_rate < 8000 || sample_rate > 384000 || channels <= 0 || bytes_per_sample != 2) {
        qWarning() << "[EdgeLivePlayer] ALSA init skipped due to unsupported format:"
                   << "sr/ch/bps=" << sample_rate << channels << bytes_per_sample;
        return false;
    }

    Stop();

    std::lock_guard<std::mutex> lock(mutex_);
    sample_rate_ = sample_rate;
    channels_ = channels;
    bytes_per_sample_ = bytes_per_sample;
    frame_bytes_ = channels_ * bytes_per_sample_;
    local_playback_mode_ = local_playback_mode;
    initialized_ = OpenDeviceLocked();
    if (!initialized_) {
        CloseDeviceLocked();
        return false;
    }
    stop_worker_ = false;
    device_started_ = false;
    anchor_valid_ = false;
    total_written_frames_ = 0;
    anchor_media_pts_ms_ = 0;
    last_returned_clock_ms_ = 0;
    query_fail_count_ = 0;
    underrun_count_ = 0;
    ClearQueueLocked();
    EnsureWorkerLocked();
    qInfo() << "[EdgeLivePlayer] ALSA audio initialized:"
            << "sample_rate=" << sample_rate_
            << "channels=" << channels_
            << "buffer_frames=" << buffer_frames_
            << "period_frames=" << period_frames_
            << "buffer_ms=" << DurationMsForFramesLocked(static_cast<int64_t>(buffer_frames_))
            << "period_ms=" << DurationMsForFramesLocked(static_cast<int64_t>(period_frames_));
    return true;
}

void AlsaAudioOutput::Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || pcm_ == nullptr) {
        return;
    }
    stop_worker_ = false;
    EnsureWorkerLocked();
    cv_.notify_all();
}

void AlsaAudioOutput::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_worker_ = true;
        cv_.notify_all();
    }
    if (writer_thread_.joinable()) {
        writer_thread_.join();
    }

    std::lock_guard<std::mutex> lock(mutex_);
    worker_running_ = false;
    if (pcm_ != nullptr) {
        snd_pcm_drop(pcm_);
    }
    CloseDeviceLocked();
    initialized_ = false;
    anchor_valid_ = false;
    device_started_ = false;
    total_written_frames_ = 0;
    anchor_media_pts_ms_ = 0;
    last_returned_clock_ms_ = 0;
    query_fail_count_ = 0;
    underrun_count_ = 0;
    ClearQueueLocked();
}

void AlsaAudioOutput::Reset() {
    Stop();
}

void AlsaAudioOutput::QueueAudioFrame(int64_t pts_ms, QByteArray pcm) {
    if (pcm.isEmpty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || frame_bytes_ <= 0) {
        return;
    }

    const int64_t duration_ms = DurationMsForBytesLocked(pcm.size());
    latest_audio_pkt_pts_ms_ = pts_ms;
    latest_audio_pkt_end_pts_ms_ = pts_ms + duration_ms;
    audio_buffered_bytes_ += pcm.size();
    audio_chunks_.push_back(AudioChunk{std::move(pcm), pts_ms, 0});
    cv_.notify_all();
}

int64_t AlsaAudioOutput::GetAudioClockMs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ComputeClockLocked(nullptr, nullptr);
}

AlsaAudioOutput::Diagnostics AlsaAudioOutput::GetDiagnostics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Diagnostics diagnostics;
    diagnostics.query_fail_count = query_fail_count_;
    diagnostics.underrun_count = underrun_count_;
    diagnostics.anchor_pts_ms = anchor_valid_ ? anchor_media_pts_ms_ : 0;
    diagnostics.latest_audio_pkt_pts_ms = latest_audio_pkt_pts_ms_;
    diagnostics.latest_audio_pkt_end_pts_ms = latest_audio_pkt_end_pts_ms_;
    diagnostics.buffer_total_ms = DurationMsForFramesLocked(static_cast<int64_t>(buffer_frames_));
    diagnostics.free_ms = std::max<int64_t>(0, diagnostics.buffer_total_ms - diagnostics.pending_ms);
    diagnostics.clock_ms = ComputeClockLocked(&diagnostics.processed_ms, &diagnostics.device_delay_ms);
    diagnostics.pending_ms = diagnostics.device_delay_ms;
    diagnostics.free_ms = std::max<int64_t>(0, diagnostics.buffer_total_ms - diagnostics.pending_ms);
    diagnostics.queued_audio_ms = DurationMsForBytesLocked(audio_buffered_bytes_);
    diagnostics.audio_path_lag_ms = latest_audio_pkt_end_pts_ms_ > 0
        ? std::max<int64_t>(0, latest_audio_pkt_end_pts_ms_ - diagnostics.clock_ms)
        : 0;
    diagnostics.software_prune_count = software_prune_count_;
    diagnostics.software_prune_duration_ms = software_prune_duration_ms_;
    diagnostics.last_software_prune_duration_ms = last_software_prune_duration_ms_;
    diagnostics.anchor_rebase_count = anchor_rebase_count_;
    diagnostics.last_anchor_rebase_duration_ms = last_anchor_rebase_duration_ms_;
    diagnostics.last_anchor_rebased = last_anchor_rebased_;
    return diagnostics;
}

bool AlsaAudioOutput::OpenDeviceLocked() {
    CloseDeviceLocked();

    int err = snd_pcm_open(&pcm_, "default", SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0 || pcm_ == nullptr) {
        qWarning() << "[EdgeLivePlayer] ALSA open failed:" << snd_strerror(err);
        pcm_ = nullptr;
        return false;
    }

    snd_pcm_hw_params_t* hw_params = nullptr;
    snd_pcm_hw_params_malloc(&hw_params);
    if (hw_params == nullptr) {
        qWarning() << "[EdgeLivePlayer] ALSA hw params alloc failed";
        return false;
    }

    err = snd_pcm_hw_params_any(pcm_, hw_params);
    if (err >= 0) {
        err = snd_pcm_hw_params_set_access(pcm_, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);
    }
    if (err >= 0) {
        err = snd_pcm_hw_params_set_format(pcm_, hw_params, SND_PCM_FORMAT_S16_LE);
    }
    unsigned int rate = static_cast<unsigned int>(sample_rate_);
    if (err >= 0) {
        err = snd_pcm_hw_params_set_rate_near(pcm_, hw_params, &rate, nullptr);
    }
    if (err >= 0) {
        err = snd_pcm_hw_params_set_channels(pcm_, hw_params, static_cast<unsigned int>(channels_));
    }

    unsigned int buffer_time_us = local_playback_mode_ ? kLocalBufferTimeUs : kLiveBufferTimeUs;
    unsigned int period_time_us = local_playback_mode_ ? kLocalPeriodTimeUs : kLivePeriodTimeUs;
    if (err >= 0) {
        err = snd_pcm_hw_params_set_buffer_time_near(pcm_, hw_params, &buffer_time_us, nullptr);
    }
    if (err >= 0) {
        err = snd_pcm_hw_params_set_period_time_near(pcm_, hw_params, &period_time_us, nullptr);
    }
    if (err >= 0) {
        err = snd_pcm_hw_params(pcm_, hw_params);
    }
    if (err >= 0) {
        snd_pcm_hw_params_get_buffer_size(hw_params, &buffer_frames_);
        snd_pcm_hw_params_get_period_size(hw_params, &period_frames_, nullptr);
    }
    snd_pcm_hw_params_free(hw_params);

    if (err < 0) {
        qWarning() << "[EdgeLivePlayer] ALSA hw params failed:" << snd_strerror(err);
        CloseDeviceLocked();
        return false;
    }

    snd_pcm_sw_params_t* sw_params = nullptr;
    snd_pcm_sw_params_malloc(&sw_params);
    if (sw_params == nullptr) {
        qWarning() << "[EdgeLivePlayer] ALSA sw params alloc failed";
        CloseDeviceLocked();
        return false;
    }
    err = snd_pcm_sw_params_current(pcm_, sw_params);
    if (err >= 0) {
        err = snd_pcm_sw_params_set_start_threshold(
            pcm_,
            sw_params,
            std::min<snd_pcm_uframes_t>(buffer_frames_, std::max<snd_pcm_uframes_t>(period_frames_, 1)));
    }
    if (err >= 0) {
        err = snd_pcm_sw_params_set_avail_min(pcm_, sw_params, std::max<snd_pcm_uframes_t>(1, period_frames_));
    }
    if (err >= 0) {
        err = snd_pcm_sw_params(pcm_, sw_params);
    }
    snd_pcm_sw_params_free(sw_params);
    if (err < 0) {
        qWarning() << "[EdgeLivePlayer] ALSA sw params failed:" << snd_strerror(err);
        CloseDeviceLocked();
        return false;
    }

    if (snd_pcm_prepare(pcm_) < 0) {
        qWarning() << "[EdgeLivePlayer] ALSA prepare failed";
        CloseDeviceLocked();
        return false;
    }
    return true;
}

void AlsaAudioOutput::CloseDeviceLocked() {
    if (pcm_ != nullptr) {
        snd_pcm_close(pcm_);
        pcm_ = nullptr;
    }
    buffer_frames_ = 0;
    period_frames_ = 0;
}

void AlsaAudioOutput::ClearQueueLocked() {
    audio_chunks_.clear();
    audio_buffered_bytes_ = 0;
    latest_audio_pkt_pts_ms_ = 0;
    latest_audio_pkt_end_pts_ms_ = 0;
    software_prune_count_ = 0;
    software_prune_duration_ms_ = 0;
    last_software_prune_duration_ms_ = 0;
    anchor_rebase_count_ = 0;
    last_anchor_rebase_duration_ms_ = 0;
    last_anchor_rebased_ = false;
}

void AlsaAudioOutput::EnsureWorkerLocked() {
    if (worker_running_) {
        return;
    }
    worker_running_ = true;
    writer_thread_ = std::thread(&AlsaAudioOutput::WriterLoop, this);
}

void AlsaAudioOutput::WriterLoop() {
    while (true) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() { return stop_worker_ || !audio_chunks_.empty(); });
            if (stop_worker_) {
                break;
            }
        }

        while (true) {
            snd_pcm_t* pcm = nullptr;
            int frame_bytes = 0;
            int sample_rate = 0;
            snd_pcm_uframes_t period_frames = 0;
            const char* write_ptr = nullptr;
            snd_pcm_uframes_t frames_to_write = 0;
            int64_t anchor_pts_ms = 0;
            int64_t anchor_offset_frames = 0;
            bool needs_anchor = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (stop_worker_) {
                    return;
                }
                pcm = pcm_;
                frame_bytes = frame_bytes_;
                sample_rate = sample_rate_;
                period_frames = std::max<snd_pcm_uframes_t>(1, period_frames_);
                if (pcm == nullptr || frame_bytes <= 0 || sample_rate <= 0) {
                    break;
                }
                if (audio_chunks_.empty()) {
                    break;
                }

                AudioChunk& chunk = audio_chunks_.front();
                const qsizetype available_bytes = chunk.data.size() - chunk.offset;
                if (available_bytes <= 0) {
                    audio_chunks_.pop_front();
                    continue;
                }

                const snd_pcm_uframes_t chunk_remaining_frames = static_cast<snd_pcm_uframes_t>(
                    available_bytes / static_cast<qsizetype>(frame_bytes));
                if (chunk_remaining_frames == 0) {
                    audio_buffered_bytes_ -= available_bytes;
                    audio_chunks_.pop_front();
                    continue;
                }

                write_ptr = chunk.data.constData() + chunk.offset;
                frames_to_write = std::min(period_frames, chunk_remaining_frames);
                if (!anchor_valid_) {
                    anchor_offset_frames = static_cast<int64_t>(chunk.offset / frame_bytes_);
                    anchor_pts_ms = chunk.pts_ms + DurationMsForFramesLocked(anchor_offset_frames);
                    needs_anchor = true;
                }
            }
            if (frames_to_write == 0) {
                break;
            }

            snd_pcm_sframes_t avail_frames = snd_pcm_avail_update(pcm);
            if (avail_frames == -EPIPE) {
                std::lock_guard<std::mutex> lock(mutex_);
                ++underrun_count_;
                const int64_t buffered_ms = DurationMsForBytesLocked(audio_buffered_bytes_);
                qWarning() << "[EdgeLivePlayer] ALSA underrun detected before write:"
                           << "count=" << underrun_count_
                           << "buffered_ms=" << buffered_ms;
                snd_pcm_prepare(pcm_);
                device_started_ = false;
                continue;
            }
            if (avail_frames == -ESTRPIPE) {
                snd_pcm_resume(pcm);
                continue;
            }
            if (avail_frames < 0) {
                const int recover_ret = snd_pcm_recover(pcm, static_cast<int>(avail_frames), 1);
                if (recover_ret < 0) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    ++query_fail_count_;
                    qWarning() << "[EdgeLivePlayer] ALSA avail update failed:"
                               << snd_strerror(static_cast<int>(avail_frames));
                    break;
                }
                continue;
            }
            if (avail_frames == 0) {
                snd_pcm_wait(pcm, kPcmWaitTimeoutMs);
                continue;
            }
            frames_to_write = std::min<snd_pcm_uframes_t>(
                frames_to_write,
                static_cast<snd_pcm_uframes_t>(avail_frames));
            if (frames_to_write == 0) {
                snd_pcm_wait(pcm, kPcmWaitTimeoutMs);
                continue;
            }

            const snd_pcm_sframes_t write_ret = snd_pcm_writei(
                pcm,
                write_ptr,
                frames_to_write);
            if (write_ret == -EPIPE) {
                std::lock_guard<std::mutex> lock(mutex_);
                ++underrun_count_;
                const int64_t buffered_ms = DurationMsForBytesLocked(audio_buffered_bytes_);
                int64_t pending_ms = 0;
                ComputeClockLocked(nullptr, &pending_ms);
                qWarning() << "[EdgeLivePlayer] ALSA underrun during write:"
                           << "count=" << underrun_count_
                           << "buffered_ms=" << buffered_ms
                           << "pending_ms=" << pending_ms;
                snd_pcm_prepare(pcm_);
                device_started_ = false;
                continue;
            }
            if (write_ret == -ESTRPIPE) {
                snd_pcm_resume(pcm);
                continue;
            }
            if (write_ret < 0) {
                const int recover_ret = snd_pcm_recover(pcm, static_cast<int>(write_ret), 1);
                if (recover_ret < 0) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    ++query_fail_count_;
                    qWarning() << "[EdgeLivePlayer] ALSA write failed:" << snd_strerror(static_cast<int>(write_ret));
                    break;
                }
                continue;
            }

            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (audio_chunks_.empty()) {
                    continue;
                }
                AudioChunk& chunk = audio_chunks_.front();
                const qsizetype consumed_bytes = static_cast<qsizetype>(write_ret) * frame_bytes;
                chunk.offset += consumed_bytes;
                audio_buffered_bytes_ = std::max<qsizetype>(0, audio_buffered_bytes_ - consumed_bytes);
                if (chunk.offset >= chunk.data.size()) {
                    audio_chunks_.pop_front();
                }
                if (needs_anchor && !anchor_valid_) {
                    anchor_media_pts_ms_ = anchor_pts_ms;
                    anchor_valid_ = true;
                    qInfo() << "[EdgeLivePlayer] ALSA audio anchor established:"
                            << "pts_ms=" << anchor_media_pts_ms_
                            << "written_frames=" << total_written_frames_
                            << "chunk_offset_frames=" << anchor_offset_frames;
                }
                device_started_ = true;
                total_written_frames_ += static_cast<uint64_t>(write_ret);
            }
        }
    }
}

int64_t AlsaAudioOutput::DurationMsForBytesLocked(qsizetype bytes) const {
    if (sample_rate_ <= 0 || frame_bytes_ <= 0 || bytes <= 0) {
        return 0;
    }
    return static_cast<int64_t>(bytes) * 1000 / static_cast<int64_t>(sample_rate_ * frame_bytes_);
}

int64_t AlsaAudioOutput::DurationMsForFramesLocked(int64_t frames) const {
    if (sample_rate_ <= 0 || frames <= 0) {
        return 0;
    }
    return frames * 1000 / sample_rate_;
}

int64_t AlsaAudioOutput::DurationMsForFramesClampedLocked(snd_pcm_sframes_t frames) const {
    return DurationMsForFramesLocked(std::max<int64_t>(0, static_cast<int64_t>(frames)));
}

int64_t AlsaAudioOutput::ComputeClockLocked(int64_t* processed_ms, int64_t* delay_ms) const {
    if (processed_ms != nullptr) {
        *processed_ms = 0;
    }
    if (delay_ms != nullptr) {
        *delay_ms = 0;
    }
    if (!anchor_valid_ || pcm_ == nullptr || sample_rate_ <= 0) {
        return 0;
    }

    snd_pcm_sframes_t delay_frames = 0;
    int err = snd_pcm_delay(pcm_, &delay_frames);
    if (err < 0) {
        err = snd_pcm_recover(pcm_, err, 0);
        if (err < 0) {
            ++query_fail_count_;
            return last_returned_clock_ms_;
        }
        if (snd_pcm_delay(pcm_, &delay_frames) < 0) {
            ++query_fail_count_;
            return last_returned_clock_ms_;
        }
    }

    const int64_t written_frames = static_cast<int64_t>(total_written_frames_);
    const int64_t pending_frames = std::max<int64_t>(0, static_cast<int64_t>(delay_frames));
    const int64_t played_frames = std::max<int64_t>(0, written_frames - pending_frames);
    const int64_t current_processed_ms = DurationMsForFramesLocked(played_frames);
    const int64_t current_delay_ms = DurationMsForFramesClampedLocked(delay_frames);
    int64_t clock_ms = anchor_media_pts_ms_ + current_processed_ms;
    if (clock_ms < last_returned_clock_ms_) {
        clock_ms = last_returned_clock_ms_;
    }
    if (processed_ms != nullptr) {
        *processed_ms = current_processed_ms;
    }
    if (delay_ms != nullptr) {
        *delay_ms = current_delay_ms;
    }
    last_returned_clock_ms_ = clock_ms;
    return clock_ms;
}

#else

bool AlsaAudioOutput::Initialize(int sample_rate, int channels, int bytes_per_sample, bool local_playback_mode) {
    Q_UNUSED(sample_rate);
    Q_UNUSED(channels);
    Q_UNUSED(bytes_per_sample);
    Q_UNUSED(local_playback_mode);
    qWarning() << "[EdgeLivePlayer] ALSA native output unavailable:"
               << "missing ALSA development headers at build time";
    return false;
}

void AlsaAudioOutput::Start() {}

void AlsaAudioOutput::Stop() {}

void AlsaAudioOutput::Reset() {}

void AlsaAudioOutput::QueueAudioFrame(int64_t pts_ms, QByteArray pcm) {
    Q_UNUSED(pts_ms);
    Q_UNUSED(pcm);
}

int64_t AlsaAudioOutput::GetAudioClockMs() const {
    return 0;
}

AlsaAudioOutput::Diagnostics AlsaAudioOutput::GetDiagnostics() const {
    Diagnostics diagnostics;
    diagnostics.hardware_backed = false;
    diagnostics.provider_name = QStringLiteral("alsa_native_unavailable");
    return diagnostics;
}

bool AlsaAudioOutput::OpenDeviceLocked() {
    return false;
}

void AlsaAudioOutput::CloseDeviceLocked() {}

void AlsaAudioOutput::ClearQueueLocked() {}

void AlsaAudioOutput::EnsureWorkerLocked() {}

void AlsaAudioOutput::WriterLoop() {}

int64_t AlsaAudioOutput::DurationMsForBytesLocked(qsizetype bytes) const {
    Q_UNUSED(bytes);
    return 0;
}

int64_t AlsaAudioOutput::DurationMsForFramesLocked(int64_t frames) const {
    Q_UNUSED(frames);
    return 0;
}

int64_t AlsaAudioOutput::DurationMsForFramesClampedLocked(snd_pcm_sframes_t frames) const {
    Q_UNUSED(frames);
    return 0;
}

int64_t AlsaAudioOutput::ComputeClockLocked(int64_t* processed_ms, int64_t* delay_ms) const {
    if (processed_ms != nullptr) {
        *processed_ms = 0;
    }
    if (delay_ms != nullptr) {
        *delay_ms = 0;
    }
    return 0;
}

#endif
