#pragma once

#include <QByteArray>
#include <QString>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>

struct _snd_pcm;
typedef struct _snd_pcm snd_pcm_t;
typedef long snd_pcm_sframes_t;
typedef unsigned long snd_pcm_uframes_t;

class AlsaAudioOutput {
public:
    struct Diagnostics {
        QString provider_name{"alsa_native"};
        bool hardware_backed{true};
        uint64_t query_fail_count{0};
        uint64_t fallback_count{0};
        uint64_t underrun_count{0};
        int64_t anchor_pts_ms{0};
        int64_t processed_ms{0};
        int64_t device_delay_ms{0};
        int64_t clock_ms{0};
        int64_t latest_audio_pkt_pts_ms{0};
        int64_t latest_audio_pkt_end_pts_ms{0};
        int64_t queued_audio_ms{0};
        int64_t audio_path_lag_ms{0};
        int64_t buffer_total_ms{0};
        int64_t pending_ms{0};
        int64_t free_ms{0};
        uint64_t software_prune_count{0};
        int64_t software_prune_duration_ms{0};
        int64_t last_software_prune_duration_ms{0};
        uint64_t anchor_rebase_count{0};
        int64_t last_anchor_rebase_duration_ms{0};
        bool last_anchor_rebased{false};
    };

    AlsaAudioOutput();
    ~AlsaAudioOutput();

    bool Initialize(int sample_rate, int channels, int bytes_per_sample, bool local_playback_mode);
    void Start();
    void Stop();
    void Reset();
    void QueueAudioFrame(int64_t pts_ms, QByteArray pcm);
    int64_t GetAudioClockMs() const;
    Diagnostics GetDiagnostics() const;

private:
    struct AudioChunk {
        QByteArray data;
        int64_t pts_ms{0};
        qsizetype offset{0};
    };

    bool OpenDeviceLocked();
    void CloseDeviceLocked();
    void ClearQueueLocked();
    void EnsureWorkerLocked();
    void WriterLoop();
    int64_t DurationMsForBytesLocked(qsizetype bytes) const;
    int64_t DurationMsForFramesLocked(int64_t frames) const;
    int64_t DurationMsForFramesClampedLocked(snd_pcm_sframes_t frames) const;
    int64_t ComputeClockLocked(int64_t* processed_ms, int64_t* delay_ms) const;
private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread writer_thread_;
    bool worker_running_{false};
    bool stop_worker_{false};
    bool device_started_{false};
    bool anchor_valid_{false};
    bool initialized_{false};
    snd_pcm_t* pcm_{nullptr};
    int sample_rate_{0};
    int channels_{0};
    int bytes_per_sample_{0};
    int frame_bytes_{0};
    bool local_playback_mode_{false};
    snd_pcm_uframes_t buffer_frames_{0};
    snd_pcm_uframes_t period_frames_{0};
    uint64_t total_written_frames_{0};
    int64_t anchor_media_pts_ms_{0};
    mutable int64_t last_returned_clock_ms_{0};
    mutable uint64_t query_fail_count_{0};
    uint64_t underrun_count_{0};
    std::deque<AudioChunk> audio_chunks_;
    qsizetype audio_buffered_bytes_{0};
    int64_t latest_audio_pkt_pts_ms_{0};
    int64_t latest_audio_pkt_end_pts_ms_{0};
    uint64_t software_prune_count_{0};
    int64_t software_prune_duration_ms_{0};
    int64_t last_software_prune_duration_ms_{0};
    uint64_t anchor_rebase_count_{0};
    int64_t last_anchor_rebase_duration_ms_{0};
    bool last_anchor_rebased_{false};
};
