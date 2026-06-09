#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace edgelive {

enum class PlayerState {
    Created,
    Initialized,
    Playing,
    Stopped,
    Released,
    Error,
};

enum class MediaSourceType {
    Auto,
    File,
    Rtsp,
    Rtmp,
};

enum class PlaybackOutputMode {
    RealtimeVideoOnly,
    RealtimeVideoAudio,
    LocalPlayback,
};

struct MediaSourceOptions {
    PlaybackOutputMode output_mode{PlaybackOutputMode::RealtimeVideoOnly};
    std::string transport{"tcp"};
    int timeout_ms{5000};
    int buffer_size{1024 * 1024};
    int packet_queue_capacity{512};
    bool drop_oldest_on_full{true};
    bool low_latency_mode{false};
    int video_decode_queue_limit{8};
    int audio_decode_queue_limit{16};
    int video_render_queue_limit{8};
    bool fast_startup{false};
    bool defer_audio_until_first_video{false};
    int av_sync_dead_zone_ms{20};
    int av_sync_max_lead_ms{120};
    int av_sync_max_lag_ms{400};
    bool av_sync_drop_late_video{true};
    bool adaptive_jitter_enabled{true};
    int jitter_min_ms{150};
    int jitter_max_ms{1200};
    int jitter_target_ms_initial{300};
    int jitter_eval_window_ms{2000};
    int latency_cap_ms{2000};
    bool video_recovery_enabled{true};
    bool video_recovery_live_only{true};
    bool video_pts_backpressure_enabled{true};
    int video_pts_jump_threshold_ms{500};
    int video_pts_rollback_threshold_ms{300};
    int video_pts_drift_threshold_ms{200};
    int video_pts_invalid_max_streak{3};
    bool video_pts_hard_recover_to_keyframe{true};
};

struct MediaSource {
    MediaSourceType type{MediaSourceType::Auto};
    std::string url;
    MediaSourceOptions options{};
};

struct PlayerInitConfig {
    std::string log_tag{"EdgeLivePlayer"};
};

struct PlayRequest {
    std::string url;
    MediaSource source{};
};

enum class VideoPixelFormat {
    Bgra,
    Yuv420p,
    Nv12,
};

struct VideoFrame {
    int width{0};
    int height{0};
    int stride{0};
    int strides[3]{0, 0, 0};
    int64_t pts_ms{0};
    int64_t raw_pts_ms{0};
    int64_t enqueue_steady_ms{0};
    const uint8_t* data{nullptr};
    const uint8_t* planes[3]{nullptr, nullptr, nullptr};
    VideoPixelFormat format{VideoPixelFormat::Bgra};
    bool is_keyframe{false};
    bool pts_reanchored{false};
    std::shared_ptr<const std::vector<uint8_t>> buffer;
};

struct AudioFrame {
    int sample_rate{0};
    int channels{0};
    int bytes_per_sample{0};
    int64_t pts_ms{0};
    size_t data_size{0};
    const uint8_t* data{nullptr};
};

struct PlayerStats {
    int64_t start_time_ms{0};
    uint32_t state_change_count{0};
    uint32_t play_count{0};
    uint32_t stop_count{0};
    uint32_t error_count{0};
    uint64_t demuxed_video_packets{0};
    uint64_t demuxed_audio_packets{0};
    uint64_t dropped_video_packets{0};
    uint64_t dropped_audio_packets{0};
    size_t video_queue_size{0};
    size_t audio_queue_size{0};
    int64_t dns_cost_ms{-1};
    int64_t connect_cost_ms{-1};
    int64_t handshake_cost_ms{-1};
    int64_t open_input_cost_ms{-1};
    int64_t stream_info_cost_ms{-1};
    int64_t first_read_cost_ms{-1};
    int64_t first_video_decode_cost_ms{-1};
    int64_t first_video_render_cost_ms{-1};
    int64_t first_audio_decode_cost_ms{-1};
    int64_t first_audio_play_cost_ms{-1};
    uint64_t audio_pop_packet_count{0};
    uint64_t audio_send_packet_count{0};
    uint64_t audio_send_packet_fail_count{0};
    uint64_t audio_receive_frame_fail_count{0};
    uint64_t audio_decoded_frame_count{0};
    uint64_t audio_invalid_param_skip_count{0};
    uint64_t audio_invalid_sample_rate_skip_count{0};
    uint64_t audio_swr_convert_fail_count{0};
    int audio_last_swr_convert_result{0};
    size_t max_video_queue_size{0};
    size_t max_audio_queue_size{0};
    bool audio_started_after_first_video{false};
    int64_t av_offset_avg_ms{0};
    int64_t av_offset_max_ms{0};
    int64_t av_offset_min_ms{0};
    uint64_t av_offset_sample_count{0};
    uint64_t video_sync_drop_count{0};
    int64_t last_av_offset_ms{0};
    int64_t sync_video_pts_ms{0};
    int64_t sync_audio_clock_ms{0};
    int64_t sync_raw_diff_ms{0};
    int64_t sync_final_delay_ms{0};
    std::string sync_action{"RENDER_NOW"};
    int64_t render_stat_wall_time_ms{0};
    int64_t render_stat_video_pts_delta_ms{0};
    int64_t render_stat_audio_clock_delta_ms{0};
    uint64_t render_stat_drops{0};
    int64_t render_stat_avg_diff_ms{0};
    int jitter_target_ms{0};
    int jitter_current_ms{0};
    double jitter_interarrival_std_ms{0.0};
    double loss_rate{0.0};
    double reorder_rate{0.0};
    uint64_t jitter_mode_switch_count{0};
    uint64_t latency_cap_trigger_count{0};
    uint64_t latency_cap_dropped_video_packets{0};
    uint64_t latency_cap_dropped_audio_packets{0};
    uint64_t video_recovery_enter_count{0};
    uint64_t video_recovery_exit_count{0};
    uint64_t video_recovery_drop_count{0};
    int64_t last_video_recovery_duration_ms{0};
    uint64_t video_decoded_frame_count{0};
    uint64_t video_rendered_frame_count{0};
    uint64_t audio_output_frame_count{0};
    int64_t video_decode_avg_cost_us{0};
    int64_t video_render_avg_cost_us{0};
    int64_t audio_decode_avg_cost_us{0};
    int64_t video_render_buffer_ms{0};
    double video_render_playback_rate{1.0};
    int64_t video_render_pid_error_ms{0};
    int64_t audio_latest_demux_pts_ms{0};
    int64_t audio_latest_queue_head_pts_ms{0};
    int64_t audio_latest_decode_pts_ms{0};
    int64_t audio_latest_submit_pts_ms{0};
    int64_t audio_decoder_pts_ms{0};
    int64_t audio_computed_output_pts_ms{0};
    int64_t audio_decode_to_submit_offset_ms{0};
    int64_t audio_submit_to_video_gap_ms{0};
    int64_t audio_demux_to_video_gap_ms{0};
    int64_t audio_queue_duration_ms{0};
    uint64_t audio_trim_drop_count{0};
    int64_t audio_trim_drop_duration_ms{0};
    uint64_t audio_latency_cap_drop_count{0};
    int64_t audio_latency_cap_drop_duration_ms{0};
    uint64_t audio_pts_rebase_count{0};
    std::string audio_pts_rebase_last_reason{"none"};
    int64_t video_packet_wait_avg_us{0};
    int64_t audio_packet_wait_avg_us{0};
    int video_decode_fps{0};
    int video_render_fps{0};
    int audio_output_fps{0};
    std::string audio_clock_provider{"unknown"};
    bool audio_clock_hardware_backed{false};
    uint64_t audio_clock_query_fail_count{0};
    uint64_t audio_clock_fallback_count{0};
    std::string video_pts_backpressure_mode{"Normal"};
    uint64_t video_pts_invalid_count{0};
    uint64_t video_pts_rollback_count{0};
    uint64_t video_pts_jump_count{0};
    uint64_t video_pts_drift_correction_count{0};
    uint64_t video_pts_sanitized_count{0};
    uint64_t video_pts_keyframe_reanchor_count{0};
    int64_t video_pts_last_raw_ms{0};
    int64_t video_pts_last_effective_ms{0};
    int64_t video_pts_last_drift_ms{0};
};

struct PlayerCallbacks {
    std::function<void(const VideoFrame&)> on_video_frame;
    std::function<void(const AudioFrame&)> on_audio_frame;
    std::function<int64_t()> get_audio_clock_ms;
    std::function<void(PlayerState)> on_state_changed;
    std::function<void(int, const std::string&)> on_error;
    std::function<void(const PlayerStats&)> on_stats;
};

class IPlayer {
public:
    virtual ~IPlayer() = default;
    virtual bool Init(const PlayerInitConfig& config, const PlayerCallbacks& callbacks) = 0;
    virtual bool Play(const PlayRequest& request) = 0;
    virtual bool Stop() = 0;
    virtual bool Release() = 0;
    virtual PlayerState GetState() const = 0;
    virtual PlayerStats GetStats() const = 0;
};

std::unique_ptr<IPlayer> CreatePlayer();

} // namespace edgelive

