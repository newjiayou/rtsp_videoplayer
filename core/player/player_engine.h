#pragma once

#include "sdk/player.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <limits>
#include <string>
#include <thread>
#include <vector>

struct AVCodecContext;
struct AVFormatContext;
struct SwsContext;
struct SwrContext;

namespace edgelive {

class PlayerEngine final : public IPlayer {
public:
    PlayerEngine();
    ~PlayerEngine() override;

    bool Init(const PlayerInitConfig& config, const PlayerCallbacks& callbacks) override;
    bool Play(const PlayRequest& request) override;
    bool Stop() override;
    bool Release() override;
    PlayerState GetState() const override;
    PlayerStats GetStats() const override;

private:
    enum class PlaybackMode {
        File,
        Live,
    };

    bool CleanupRuntimeResources();
    void ResetQueues(const MediaSource& source);
    void DemuxLoop();
    void DemuxFileLoop(const MediaSource& source);
    void DemuxLiveLoop(const MediaSource& source);
    void DemuxLoopImpl(const MediaSource& source, bool live_mode);
    void VideoDecodeLoop();
    void VideoDecodeFileLoop(const MediaSource& source);
    void VideoDecodeLiveLoop(const MediaSource& source);
    void VideoDecodeLoopImpl(const MediaSource& source, bool live_mode);
    void VideoRenderLoop();
    void VideoRenderFileLoop(const MediaSource& source);
    void VideoRenderLiveLoop(const MediaSource& source);
    void VideoRenderLoopImpl(const MediaSource& source, bool live_mode);
    void AudioDecodeLoop();
    void AudioDecodeFileLoop(const MediaSource& source);
    void AudioDecodeLiveLoop(const MediaSource& source);
    void AudioDecodeLoopImpl(const MediaSource& source, bool live_mode);
    void ChangeState(PlayerState state);
    void EmitError(int code, const std::string& message);
    void UpdateStats(const std::function<void(PlayerStats&)>& updater);
    PlaybackMode ResolvePlaybackMode(const MediaSource& source) const;
    bool ShouldOutputAudio(const MediaSource& source) const;
    bool ShouldEnableLowLatency(const MediaSource& source) const;
    bool ShouldHoldAudioForFastStartup() const;
    int64_t GetAudioMasterClockMs() const;
    int64_t ComputeAudioPlaybackDelayMs(int64_t audio_pts_ms) const;
    void OnVideoPacketObserved(int64_t arrival_ms, int64_t pts_ms);
    void MaybeUpdateJitterController(const MediaSource& source, int64_t now_ms);
    void ApplyLatencyCap(const MediaSource& source);
    int GetCurrentJitterTargetMs(const MediaSource& source) const;
    int GetCurrentAudioJitterTargetMs(const MediaSource& source) const;
    bool ShouldEnableVideoRecovery(const MediaSource& source) const;
    void EnterVideoRecovery(const std::string& reason);
    void ExitVideoRecovery();
    bool IsVideoRecoveryActive() const;
    bool ShouldDropPacketUntilKeyframe(bool is_keyframe, uint64_t& dropped_packets);
    bool ShouldEnableVideoPtsBackpressure(const MediaSource& source) const;
    const char* VideoPtsBackpressureModeToString(int mode) const;
    void ResetVideoPtsBackpressureState();
    int64_t GetVideoPtsEstimatedFrameDurationMs() const;

private:
    struct AvSyncState {
        int64_t anchor_monotonic_ms{0};
        int64_t anchor_audio_pts_ms{0};
        int64_t anchor_video_pts_ms{0};
        int64_t anchor_video_monotonic_ms{0};
        int64_t last_audio_pts_ms{0};
        int64_t submitted_audio_duration_ms{0};
        int64_t first_audio_pts_ms{std::numeric_limits<int64_t>::min()};
        int64_t first_video_pts_ms{std::numeric_limits<int64_t>::min()};
        int64_t av_offset_ms{0};
        bool av_offset_ready{false};
        bool audio_clock_started{false};
        bool video_clock_started{false};
    };
    struct JitterControlState {
        enum class Mode {
            Stable,
            AntiJitter
        };
        std::vector<int64_t> interarrival_ms;
        std::vector<int64_t> pts_delta_ms;
        int64_t last_arrival_ms{-1};
        int64_t last_pts_ms{-1};
        int64_t window_start_ms{0};
        uint64_t packet_count{0};
        uint64_t reorder_count{0};
        uint64_t loss_events{0};
        int stable_hits{0};
        int anti_jitter_hits{0};
        int target_buffer_ms{100};
        Mode mode{Mode::Stable};
    };
    struct VideoRecoveryState {
        bool active{false};
        bool waiting_for_keyframe{false};
        bool decoder_flushed{false};
        int64_t enter_steady_ms{0};
        uint64_t dropped_frames{0};
        uint64_t dropped_packets{0};
        uint32_t consecutive_decode_failures{0};
        uint32_t consecutive_corrupt_frames{0};
        bool catchup_active{false};
        int64_t catchup_begin_steady_ms{0};
        std::string last_reason;
    };
    struct VideoRateControlState {
        bool initialized{false};
        int64_t playback_pts_ms{0};
        int64_t last_steady_ms{0};
        int64_t last_frame_pts_ms{0};
        double playback_rate{1.0};
        double integral_error{0.0};
        double last_error_ms{0.0};
    };
    struct VideoPtsBackpressureState {
        enum class Mode {
            Normal,
            SanitizeOnly,
            SoftBackpressure,
            HardBackpressure,
            WaitKeyframeRecovery,
        };
        int64_t last_raw_pts_ms{0};
        int64_t last_effective_pts_ms{0};
        int64_t last_good_pts_ms{0};
        int64_t estimated_frame_duration_ms{40};
        int64_t drift_offset_ms{0};
        int64_t last_raw_steady_ms{0};
        int invalid_streak{0};
        int rollback_streak{0};
        int jump_streak{0};
        int64_t last_mode_change_steady_ms{0};
        int64_t short_window_start_steady_ms{0};
        int64_t long_window_start_steady_ms{0};
        int64_t short_window_diff_sum_ms{0};
        int64_t long_window_diff_sum_ms{0};
        uint64_t short_window_samples{0};
        uint64_t long_window_samples{0};
        Mode mode{Mode::Normal};
    };
    struct VideoPtsNormalizeResult {
        int64_t effective_pts_ms{0};
        int64_t raw_pts_ms{0};
        int64_t drift_ms{0};
        uint32_t flags{0};
        VideoPtsBackpressureState::Mode mode{VideoPtsBackpressureState::Mode::Normal};
        bool request_keyframe_recovery{false};
        bool request_render_clock_reset{false};
    };
    enum class RenderBackpressureAction {
        RenderNow,
        SleepShort,
        SleepLong,
        ClampPts,
        DropLateFrame,
        FreezePid,
        FlushToKeyframe,
    };
    VideoPtsNormalizeResult NormalizeVideoPtsForBackpressure(
        const MediaSource& source,
        int64_t raw_pts_ms,
        bool is_keyframe,
        int64_t frame_duration_hint_ms);
    RenderBackpressureAction DecideRenderBackpressureAction(
        VideoPtsBackpressureState::Mode mode,
        int64_t render_buffer_ms,
        int64_t av_diff_ms,
        int64_t drift_ms,
        const MediaSource& source) const;
    struct RenderStatWindow {
        int64_t wall_start_ms{0};
        int64_t video_pts_start_ms{0};
        int64_t audio_clock_start_ms{0};
        int64_t diff_sum_ms{0};
        uint64_t diff_samples{0};
        uint64_t drop_count{0};
        bool initialized{false};
    };
    struct VideoTimingDiagnostics {
        int64_t last_demux_arrival_ms{-1};
        int64_t last_demux_pts_ms{-1};
        int64_t last_decode_wall_ms{-1};
        int64_t last_decode_pts_ms{-1};
        int64_t last_render_wall_ms{-1};
        int64_t last_render_pts_ms{-1};
        int last_width{0};
        int last_height{0};
        int last_format{-1};
        uint64_t decoded_frames_since_log{0};
        uint64_t rendered_frames_since_log{0};
        uint64_t demux_packets_since_log{0};
    };

    mutable std::mutex mutex_;
    mutable std::mutex startup_mutex_;
    mutable std::mutex av_sync_mutex_;
    std::condition_variable startup_cv_;
    PlayerInitConfig config_{};
    PlayerCallbacks callbacks_{};
    PlayerStats stats_{};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> demux_eof_{false};
    std::thread demux_thread_;
    std::thread video_decode_thread_;
    std::thread video_render_thread_;
    std::thread audio_decode_thread_;
    PlayerState state_{PlayerState::Created};
    PlayRequest active_request_{};
    PlaybackMode playback_mode_{PlaybackMode::File};

    AVFormatContext* fmt_ctx_{nullptr};
    AVCodecContext* video_codec_ctx_{nullptr};
    AVCodecContext* audio_codec_ctx_{nullptr};
    SwsContext* sws_ctx_{nullptr};
    SwrContext* swr_ctx_{nullptr};
    int video_stream_index_{-1};
    int audio_stream_index_{-1};
    std::atomic<bool> first_video_rendered_{false};
    int64_t last_stats_emit_ms_{0};
    AvSyncState av_sync_state_{};
    mutable std::mutex jitter_mutex_;
    JitterControlState jitter_state_{};
    mutable std::mutex video_recovery_mutex_;
    VideoRecoveryState video_recovery_state_{};
    mutable std::mutex render_stat_mutex_;
    RenderStatWindow render_stat_window_{};
    mutable std::mutex video_rate_mutex_;
    VideoRateControlState video_rate_control_state_{};
    mutable std::mutex video_pts_backpressure_mutex_;
    VideoPtsBackpressureState video_pts_backpressure_state_{};
    mutable std::mutex video_diag_mutex_;
    VideoTimingDiagnostics video_timing_diag_{};
    mutable std::atomic<int64_t> last_sync_log_ms_{0};
    mutable std::atomic<int64_t> last_audio_clock_log_ms_{0};
    mutable std::atomic<int64_t> last_audio_drift_log_ms_{0};
    mutable std::atomic<int64_t> last_video_demux_diag_log_ms_{0};
    mutable std::atomic<int64_t> last_video_decode_diag_log_ms_{0};
    mutable std::atomic<int64_t> last_video_render_diag_log_ms_{0};

    class PacketQueue;
    class VideoFrameQueue;
    std::unique_ptr<PacketQueue> video_packet_queue_;
    std::unique_ptr<PacketQueue> audio_packet_queue_;
    std::unique_ptr<VideoFrameQueue> video_frame_queue_;
};

} // namespace edgelive
