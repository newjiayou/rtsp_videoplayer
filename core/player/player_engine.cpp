#include "player_engine.h"

#include "core/log/logger.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <netdb.h>
#include <sys/socket.h>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace edgelive {

namespace {

constexpr AVSampleFormat kOutputSampleFormat = AV_SAMPLE_FMT_S16;
constexpr int kFullPlaybackMinQueueCapacity = 2048;
constexpr int kLowLatencyQueueCapacity = 512;
constexpr int kContinuityPlaybackMinQueueCapacity = 512;
constexpr int kFastStartupAudioWaitMs = 150;
constexpr int kJitterUpdateIntervalMs = 300;
constexpr int kFallbackAudioSampleRate = 48000;
constexpr int kRtspBufferedMaxDelayUs = 400000;
constexpr int kRtspBufferedReorderQueueSize = 128;
constexpr uint32_t kVideoRecoveryFailureThreshold = 3;
constexpr size_t kMaxSafeDeepQueuePackets = 1024;
constexpr size_t kCriticalQueuePackets = 1536;
constexpr int64_t kVideoLateCatchupThresholdMs = 800;
constexpr int64_t kVideoCatchupMaxWaitMs = 10000;
constexpr int64_t kLiveAudioLeadWindowMs = 180;
constexpr int64_t kAudioClockTrustToleranceMs = 150;
constexpr int64_t kVideoRateControlTargetBufferMs = 300;
constexpr int64_t kVideoRateControlMinBufferMs = 180;
constexpr int64_t kVideoRateControlMaxBufferMs = 500;
constexpr double kVideoRateControlMinPlaybackRate = 0.95;
constexpr double kVideoRateControlMaxPlaybackRate = 1.20;
constexpr double kVideoRateControlKp = 0.0006;
constexpr double kVideoRateControlKi = 0.00008;
constexpr double kVideoRateControlKd = 0.00025;
constexpr int64_t kVideoPtsMinFrameDurationMs = 10;
constexpr int64_t kVideoPtsMaxFrameDurationMs = 100;
constexpr int64_t kVideoPtsHardBackpressureBufferMs = 650;
constexpr int64_t kVideoPtsSoftBackpressureBufferMs = 420;
constexpr int64_t kVideoPtsLateDropThresholdMs = 250;
constexpr int64_t kVideoPtsShortDriftWindowMs = 1000;
constexpr int64_t kVideoPtsLongDriftWindowMs = 4000;
constexpr int64_t kVideoPtsMaxDriftCorrectionStepMs = 15;
constexpr int64_t kVideoPtsLargeJumpClampMultiplier = 3;
constexpr int64_t kVideoPtsKeyframeReanchorThresholdMs = 150;
constexpr int64_t kVideoPtsTimelineReanchorThresholdMs = 1500;
constexpr int64_t kLiveRenderMaxSleepMs = 5;
constexpr int64_t kRenderQueueMinTargetBufferMs = 260;
constexpr int64_t kRenderQueueDefaultTargetBufferMs = 320;
constexpr int64_t kRenderQueueMaxTargetBufferMs = 420;
constexpr int64_t kRenderQueueMinSchedulingHeadroomMs = 70;
constexpr int64_t kRenderQueueMaxSchedulingHeadroomMs = 140;
constexpr int64_t kRenderQueueExtraFrameSlots = 2;

struct VideoBrakeDecision {
    int64_t wait_ms{0};
    const char* level{"none"};
};

enum class VideoPtsAnomalyFlags : uint32_t {
    None = 0,
    Invalid = 1u << 0,
    Rollback = 1u << 1,
    Jump = 1u << 2,
    Drift = 1u << 3,
    Sanitized = 1u << 4,
    KeyframeReanchor = 1u << 5,
    TimelineReanchor = 1u << 6,
};

inline VideoPtsAnomalyFlags operator|(VideoPtsAnomalyFlags lhs, VideoPtsAnomalyFlags rhs) {
    return static_cast<VideoPtsAnomalyFlags>(
        static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
}

inline VideoPtsAnomalyFlags& operator|=(VideoPtsAnomalyFlags& lhs, VideoPtsAnomalyFlags rhs) {
    lhs = lhs | rhs;
    return lhs;
}

inline bool HasVideoPtsFlag(VideoPtsAnomalyFlags flags, VideoPtsAnomalyFlags flag) {
    return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(flag)) != 0;
}


VideoBrakeDecision ComputeAdaptiveVideoBrake(
    int64_t av_delta_ms,
    int dead_zone_ms,
    int max_lead_ms) {
    VideoBrakeDecision decision;
    if (av_delta_ms <= dead_zone_ms) {
        return decision;
    }

    const int64_t lead_ms = av_delta_ms - dead_zone_ms;
    if (lead_ms <= 24) {
        decision.level = "light";
        decision.wait_ms = std::max<int64_t>(1, lead_ms / 4);
    } else if (lead_ms <= 48) {
        decision.level = "medium";
        decision.wait_ms = std::max<int64_t>(4, lead_ms / 3);
    } else if (lead_ms <= 96) {
        decision.level = "heavy";
        decision.wait_ms = std::max<int64_t>(10, lead_ms / 2);
    } else {
        decision.level = "hard";
        decision.wait_ms = std::max<int64_t>(20, (lead_ms * 2) / 3);
    }

    decision.wait_ms = std::min<int64_t>(decision.wait_ms, max_lead_ms);
    return decision;
}

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

int64_t SteadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string FfmpegErrorToString(int error_code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(error_code, buffer, sizeof(buffer));
    return std::string(buffer);
}

bool IsGpuConvertibleVideoFormat(AVPixelFormat format) {
    return format == AV_PIX_FMT_YUV420P || format == AV_PIX_FMT_YUVJ420P || format == AV_PIX_FMT_NV12;
}

int GpuConvertibleBufferSize(AVPixelFormat format, int width, int height) {
    if (format == AV_PIX_FMT_YUV420P || format == AV_PIX_FMT_YUVJ420P) {
        return width * height + 2 * ((width + 1) / 2) * ((height + 1) / 2);
    }
    if (format == AV_PIX_FMT_NV12) {
        return width * height + width * ((height + 1) / 2);
    }
    return 0;
}

void CopyFramePlane(
    uint8_t* dst,
    int dst_stride,
    const uint8_t* src,
    int src_stride,
    int row_bytes,
    int rows) {
    for (int y = 0; y < rows; ++y) {
        std::memcpy(dst + y * dst_stride, src + y * src_stride, static_cast<size_t>(row_bytes));
    }
}

bool StartsWith(const std::string& value, const char* prefix) {
    return value.rfind(prefix, 0) == 0;
}

int ParseDefaultPort(MediaSourceType type) {
    switch (type) {
    case MediaSourceType::Rtsp:
        return 554;
    case MediaSourceType::Rtmp:
        return 1935;
    case MediaSourceType::Auto:
    case MediaSourceType::File:
        return 0;
    }
    return 0;
}

bool ParseHostAndPort(const MediaSource& source, std::string& host, int& port) {
    const std::string& url = source.url;
    const size_t scheme_pos = url.find("://");
    if (scheme_pos == std::string::npos) {
        return false;
    }
    size_t host_begin = scheme_pos + 3;
    size_t host_end = url.find_first_of("/?", host_begin);
    const std::string authority = url.substr(host_begin, host_end - host_begin);
    if (authority.empty()) {
        return false;
    }

    const size_t at_pos = authority.rfind('@');
    const std::string host_port = at_pos == std::string::npos ? authority : authority.substr(at_pos + 1);
    if (host_port.empty()) {
        return false;
    }

    if (host_port.front() == '[') {
        const size_t close = host_port.find(']');
        if (close == std::string::npos) {
            return false;
        }
        host = host_port.substr(1, close - 1);
        if (close + 1 < host_port.size() && host_port[close + 1] == ':') {
            port = std::atoi(host_port.substr(close + 2).c_str());
        }
    } else {
        const size_t colon = host_port.rfind(':');
        if (colon != std::string::npos && host_port.find(':') == colon) {
            host = host_port.substr(0, colon);
            port = std::atoi(host_port.substr(colon + 1).c_str());
        } else {
            host = host_port;
        }
    }

    if (host.empty()) {
        return false;
    }
    if (port <= 0) {
        port = ParseDefaultPort(source.type);
    }
    return port > 0;
}

void ProbeNetworkTimings(const MediaSource& source, int timeout_ms, int64_t& dns_cost_ms, int64_t& connect_cost_ms) {
    dns_cost_ms = -1;
    connect_cost_ms = -1;
    if (source.type != MediaSourceType::Rtsp && source.type != MediaSourceType::Rtmp) {
        return;
    }

    std::string host;
    int port = 0;
    if (!ParseHostAndPort(source, host, port)) {
        return;
    }

    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;

    addrinfo* result = nullptr;
    const int64_t dns_begin_ms = NowMs();
    const int ret = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &result);
    dns_cost_ms = NowMs() - dns_begin_ms;
    if (ret != 0 || result == nullptr) {
        return;
    }

    for (addrinfo* ai = result; ai != nullptr; ai = ai->ai_next) {
        const int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }

        timeval tv{};
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        const int64_t connect_begin_ms = NowMs();
        const int connect_ret = connect(fd, ai->ai_addr, ai->ai_addrlen);
        connect_cost_ms = NowMs() - connect_begin_ms;
        close(fd);
        if (connect_ret == 0) {
            break;
        }
    }

    freeaddrinfo(result);
}

bool EndsWithIgnoreCase(const std::string& value, const char* suffix) {
    if (suffix == nullptr) {
        return false;
    }
    const std::string suffix_str(suffix);
    if (value.size() < suffix_str.size()) {
        return false;
    }

    const size_t offset = value.size() - suffix_str.size();
    for (size_t i = 0; i < suffix_str.size(); ++i) {
        const unsigned char lhs = static_cast<unsigned char>(value[offset + i]);
        const unsigned char rhs = static_cast<unsigned char>(suffix_str[i]);
        if (std::tolower(lhs) != std::tolower(rhs)) {
            return false;
        }
    }
    return true;
}

MediaSourceType DetectSourceType(const std::string& url) {
    if (StartsWith(url, "rtsp://")) {
        return MediaSourceType::Rtsp;
    }
    if (StartsWith(url, "rtmp://")) {
        return MediaSourceType::Rtmp;
    }
    return MediaSourceType::File;
}

std::string MediaSourceTypeToString(MediaSourceType type) {
    switch (type) {
    case MediaSourceType::Auto:
        return "auto";
    case MediaSourceType::File:
        return "file";
    case MediaSourceType::Rtsp:
        return "rtsp";
    case MediaSourceType::Rtmp:
        return "rtmp";
    }
    return "unknown";
}

std::string PlaybackOutputModeToString(PlaybackOutputMode mode) {
    switch (mode) {
    case PlaybackOutputMode::RealtimeVideoOnly:
        return "realtime_video_only";
    case PlaybackOutputMode::RealtimeVideoAudio:
        return "realtime_video_audio";
    case PlaybackOutputMode::LocalPlayback:
        return "local_playback";
    }
    return "unknown";
}

MediaSource NormalizeMediaSource(const PlayRequest& request) {
    MediaSource source = request.source;
    if (source.url.empty()) {
        source.url = request.url;
    }
    if (source.type == MediaSourceType::Auto) {
        source.type = DetectSourceType(source.url);
    }
    return source;
}

AVDictionary* BuildOpenOptions(const MediaSource& source) {
    AVDictionary* options = nullptr;
    const int64_t timeout_us = static_cast<int64_t>(source.options.timeout_ms) * 1000;
    const std::string timeout_str = std::to_string(timeout_us);
    const std::string buffer_size_str = std::to_string(source.options.buffer_size);
    const bool low_latency_mode = source.options.low_latency_mode;

    av_dict_set(&options, "rw_timeout", timeout_str.c_str(), 0);
    av_dict_set(&options, "buffer_size", buffer_size_str.c_str(), 0);
    if (low_latency_mode && source.type != MediaSourceType::Rtsp) {
        av_dict_set(&options, "fflags", "nobuffer", 0);
        av_dict_set(&options, "flags", "low_delay", 0);
        av_dict_set(&options, "flush_packets", "1", 0);
        // Keep startup short, but leave enough probe budget for RTSP/H264 SPS/PPS.
        av_dict_set(&options, "probesize", "262144", 0);
        av_dict_set(&options, "analyzeduration", "500000", 0);
    }

    if (source.type == MediaSourceType::Rtsp) {
        av_dict_set(&options, "rtsp_transport", source.options.transport.c_str(), 0);
        av_dict_set(&options, "rtsp_flags", "prefer_tcp", 0);
        av_dict_set(&options, "stimeout", timeout_str.c_str(), 0);
        av_dict_set(&options, "max_delay", std::to_string(kRtspBufferedMaxDelayUs).c_str(), 0);
        av_dict_set(&options, "reorder_queue_size", std::to_string(kRtspBufferedReorderQueueSize).c_str(), 0);
    } else if (source.type == MediaSourceType::Rtmp) {
        const std::string timeout_ms_str = std::to_string(source.options.timeout_ms);
        av_dict_set(&options, "timeout", timeout_ms_str.c_str(), 0);
        if (low_latency_mode) {
            av_dict_set(&options, "rtmp_buffer", "0", 0);
            av_dict_set(&options, "rtmp_live", "live", 0);
        }
    }

    return options;
}

std::string DescribeSource(const MediaSource& source) {
    std::ostringstream oss;
    oss << "type=" << MediaSourceTypeToString(source.type)
        << " output_mode=" << PlaybackOutputModeToString(source.options.output_mode)
        << " url=" << source.url
        << " transport=" << source.options.transport
        << " timeout_ms=" << source.options.timeout_ms
        << " buffer_size=" << source.options.buffer_size
        << " queue_capacity=" << source.options.packet_queue_capacity
        << " adaptive_jitter=" << (source.options.adaptive_jitter_enabled ? "true" : "false")
        << " jitter_min_ms=" << source.options.jitter_min_ms
        << " jitter_max_ms=" << source.options.jitter_max_ms
        << " jitter_init_ms=" << source.options.jitter_target_ms_initial
        << " latency_cap_ms=" << source.options.latency_cap_ms
        << " video_recovery=" << (source.options.video_recovery_enabled ? "true" : "false")
        << " video_recovery_live_only=" << (source.options.video_recovery_live_only ? "true" : "false")
        << " drop_oldest=" << (source.options.drop_oldest_on_full ? "true" : "false")
        << " low_latency=" << (source.options.low_latency_mode ? "true" : "false")
        << " fast_startup=" << (source.options.fast_startup ? "true" : "false")
        << " defer_audio_until_first_video=" << (source.options.defer_audio_until_first_video ? "true" : "false")
        << " video_decode_queue_limit=" << source.options.video_decode_queue_limit
        << " audio_decode_queue_limit=" << source.options.audio_decode_queue_limit
        << " video_render_queue_limit=" << source.options.video_render_queue_limit
        << " jitter_eval_window_ms=" << source.options.jitter_eval_window_ms
        << " av_sync_dead_zone_ms=" << source.options.av_sync_dead_zone_ms
        << " av_sync_max_lead_ms=" << source.options.av_sync_max_lead_ms
        << " av_sync_max_lag_ms=" << source.options.av_sync_max_lag_ms
        << " av_sync_drop_late_video=" << (source.options.av_sync_drop_late_video ? "true" : "false");
    return oss.str();
}

bool ShouldUseFullPlayback(const MediaSource& source) {
    return source.options.output_mode == PlaybackOutputMode::LocalPlayback ||
        source.type == MediaSourceType::File || EndsWithIgnoreCase(source.url, ".mp4");
}

int64_t PtsToMs(int64_t pts, AVRational time_base) {
    if (pts == AV_NOPTS_VALUE) {
        return 0;
    }
    return static_cast<int64_t>(pts * av_q2d(time_base) * 1000.0);
}

bool HasMeaningfulPacketTimestamp(const AVPacket& packet) {
    return (packet.pts != AV_NOPTS_VALUE && packet.pts > 0) ||
        (packet.dts != AV_NOPTS_VALUE && packet.dts > 0);
}

bool HasOnlyZeroOrMissingPacketTimestamp(const AVPacket& packet) {
    const bool pts_zero_or_missing = packet.pts == AV_NOPTS_VALUE || packet.pts == 0;
    const bool dts_zero_or_missing = packet.dts == AV_NOPTS_VALUE || packet.dts == 0;
    return pts_zero_or_missing && dts_zero_or_missing;
}

int64_t ResolveVideoFramePtsMs(const AVFrame* frame, const AVStream* stream) {
    if (frame == nullptr || stream == nullptr) {
        return 0;
    }

    const AVRational time_base = stream->time_base;
    const int64_t best_effort_ms = PtsToMs(frame->best_effort_timestamp, time_base);
    if (best_effort_ms > 0) {
        return best_effort_ms;
    }

    const int64_t pts_ms = PtsToMs(frame->pts, time_base);
    if (pts_ms > 0) {
        return pts_ms;
    }

    const int64_t pkt_dts_ms = PtsToMs(frame->pkt_dts, time_base);
    if (pkt_dts_ms > 0) {
        return pkt_dts_ms;
    }

    if (frame->best_effort_timestamp == 0) {
        return 0;
    }
    if (frame->pts == 0) {
        return 0;
    }
    if (frame->pkt_dts == 0) {
        return 0;
    }
    return best_effort_ms;
}

int64_t ResolveAudioFramePtsMs(const AVFrame* frame, const AVStream* stream) {
    if (frame == nullptr || stream == nullptr) {
        return 0;
    }

    const AVRational time_base = stream->time_base;
    const int64_t best_effort_ms = PtsToMs(frame->best_effort_timestamp, time_base);
    if (best_effort_ms > 0) {
        return best_effort_ms;
    }

    const int64_t pts_ms = PtsToMs(frame->pts, time_base);
    if (pts_ms > 0) {
        return pts_ms;
    }

    const int64_t pkt_dts_ms = PtsToMs(frame->pkt_dts, time_base);
    if (pkt_dts_ms > 0) {
        return pkt_dts_ms;
    }

    return 0;
}

bool HasMeaningfulAudioFrameTimestamp(const AVFrame* frame, const AVStream* stream) {
    return ResolveAudioFramePtsMs(frame, stream) > 0;
}

bool HasOnlyZeroOrMissingAudioFrameTimestamp(const AVFrame* frame) {
    if (frame == nullptr) {
        return false;
    }
    const bool best_effort_zero_or_missing =
        frame->best_effort_timestamp == AV_NOPTS_VALUE || frame->best_effort_timestamp == 0;
    const bool pts_zero_or_missing = frame->pts == AV_NOPTS_VALUE || frame->pts == 0;
    const bool dts_zero_or_missing = frame->pkt_dts == AV_NOPTS_VALUE || frame->pkt_dts == 0;
    return best_effort_zero_or_missing && pts_zero_or_missing && dts_zero_or_missing;
}

int64_t ResolveFrameDurationMs(const AVStream* stream) {
    if (stream == nullptr) {
        return 40;
    }
    AVRational frame_rate = stream->avg_frame_rate;
    if (frame_rate.num <= 0 || frame_rate.den <= 0) {
        frame_rate = stream->r_frame_rate;
    }
    if (frame_rate.num <= 0 || frame_rate.den <= 0) {
        return 40;
    }
    return std::max<int64_t>(1, av_rescale_q(1, av_inv_q(frame_rate), AVRational{1, 1000}));
}

int64_t DurationMsForAudioSamples(int64_t sample_count, int sample_rate) {
    if (sample_count <= 0 || sample_rate <= 0) {
        return 0;
    }
    return sample_count * 1000 / sample_rate;
}

AVRational ResolveAudioTimeBase(const AVCodecContext* codec_ctx, const AVStream* stream) {
    if (stream != nullptr && stream->time_base.den != 0) {
        return stream->time_base;
    }
    if (codec_ctx != nullptr && codec_ctx->time_base.den != 0) {
        return codec_ctx->time_base;
    }
    return AVRational{1, codec_ctx != nullptr && codec_ctx->sample_rate > 0 ? codec_ctx->sample_rate : 1000};
}

bool IsValidAudioSampleRate(int sample_rate) {
    return sample_rate >= 8000 && sample_rate <= 384000;
}

bool ShouldEmitPeriodicLog(std::atomic<int64_t>& last_log_ms, int64_t interval_ms, int64_t now_ms) {
    int64_t expected = last_log_ms.load();
    while (now_ms - expected >= interval_ms) {
        if (last_log_ms.compare_exchange_weak(expected, now_ms)) {
            return true;
        }
    }
    return false;
}

double ClampPlaybackRate(double rate) {
    return std::clamp(rate, kVideoRateControlMinPlaybackRate, kVideoRateControlMaxPlaybackRate);
}

int64_t ClampFrameDurationMs(int64_t duration_ms) {
    return std::clamp(duration_ms, kVideoPtsMinFrameDurationMs, kVideoPtsMaxFrameDurationMs);
}

size_t ComputeAdaptiveRenderQueueCapacity(const MediaSource& source, int64_t frame_duration_ms) {
    const int64_t clamped_frame_duration_ms = ClampFrameDurationMs(frame_duration_ms);
    const int64_t scheduling_headroom_ms = std::clamp(
        clamped_frame_duration_ms * 4,
        kRenderQueueMinSchedulingHeadroomMs,
        kRenderQueueMaxSchedulingHeadroomMs);
    int64_t target_buffer_ms = std::clamp(
        std::max<int64_t>(kRenderQueueDefaultTargetBufferMs, source.options.jitter_target_ms_initial),
        kRenderQueueMinTargetBufferMs,
        kRenderQueueMaxTargetBufferMs);
    if (source.options.latency_cap_ms > 0) {
        const int64_t latency_budget_ms = std::max<int64_t>(
            kRenderQueueMinTargetBufferMs,
            source.options.latency_cap_ms / 2);
        target_buffer_ms = std::min(target_buffer_ms, latency_budget_ms);
    }
    const int64_t queue_budget_ms = std::clamp(
        target_buffer_ms + scheduling_headroom_ms,
        kRenderQueueMinTargetBufferMs,
        kRenderQueueMaxTargetBufferMs + scheduling_headroom_ms / 2);
    const size_t requested_floor = static_cast<size_t>(std::max(3, source.options.video_render_queue_limit));
    const size_t computed_capacity = static_cast<size_t>(
        std::max<int64_t>(
            requested_floor,
            (queue_budget_ms + clamped_frame_duration_ms - 1) / clamped_frame_duration_ms +
                kRenderQueueExtraFrameSlots));
    return std::min<size_t>(std::max<size_t>(requested_floor, computed_capacity), 36);
}

int ResolveAudioSampleRate(const AVFrame* frame, const AVCodecContext* codec_ctx, const AVFormatContext* fmt_ctx, int stream_index) {
    if (frame != nullptr && IsValidAudioSampleRate(frame->sample_rate)) {
        return frame->sample_rate;
    }
    if (codec_ctx != nullptr && IsValidAudioSampleRate(codec_ctx->sample_rate)) {
        return codec_ctx->sample_rate;
    }
    if (fmt_ctx != nullptr && stream_index >= 0 && stream_index < static_cast<int>(fmt_ctx->nb_streams) &&
        fmt_ctx->streams[stream_index] != nullptr && fmt_ctx->streams[stream_index]->codecpar != nullptr &&
        IsValidAudioSampleRate(fmt_ctx->streams[stream_index]->codecpar->sample_rate)) {
        return fmt_ctx->streams[stream_index]->codecpar->sample_rate;
    }
    return kFallbackAudioSampleRate;
}

} // namespace

class PlayerEngine::PacketQueue {
public:
    struct TrimResult {
        uint64_t dropped_packets{0};
        int64_t dropped_duration_ms{0};
        bool kept_keyframe_boundary{false};
    };

    PacketQueue() = default;

    void Configure(size_t capacity, bool drop_oldest_on_full) {
        std::lock_guard<std::mutex> lock(mutex_);
        max_packets_ = capacity > 0 ? capacity : 1;
        drop_oldest_on_full_ = drop_oldest_on_full;
        aborted_ = false;
    }

    bool Push(const AVPacket& packet, int64_t packet_pts_ms, uint64_t& dropped_packets) {
        return PushWithTiming(packet, NowMs(), packet_pts_ms, dropped_packets);
    }

    bool PushWithTiming(const AVPacket& packet, int64_t arrival_ms, int64_t packet_pts_ms, uint64_t& dropped_packets) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (aborted_) {
            return false;
        }

        while (queue_.size() >= max_packets_) {
            if (!drop_oldest_on_full_) {
                cv_.wait(lock, [this]() { return aborted_ || queue_.size() < max_packets_; });
                if (aborted_) {
                    return false;
                }
                continue;
            }
            queue_.pop_front();
            ++dropped_packets;
        }

        PacketHolder holder;
        if (av_packet_ref(&holder.packet, &packet) < 0) {
            return false;
        }
        holder.arrival_ms = arrival_ms;
        holder.pts_ms = packet_pts_ms;

        queue_.push_back(std::move(holder));
        cv_.notify_one();
        return true;
    }

    bool Pop(AVPacket& packet) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this]() { return aborted_ || !queue_.empty(); });
        if (queue_.empty()) {
            return false;
        }

        PacketHolder holder = std::move(queue_.front());
        queue_.pop_front();
        cv_.notify_one();
        av_packet_move_ref(&packet, &holder.packet);
        return true;
    }

    bool PopWithMinDuration(AVPacket& packet, int64_t min_duration_ms, bool use_pts_duration = false) {
        std::unique_lock<std::mutex> lock(mutex_);
        const int64_t wait_begin_ms = SteadyNowMs();
        const int64_t max_wait_ms = std::max<int64_t>(80, min_duration_ms);
        while (true) {
            if (aborted_ && queue_.empty()) {
                break;
            }
            if (!queue_.empty()) {
                if (min_duration_ms <= 0) {
                    break;
                }
                if (SteadyNowMs() - wait_begin_ms >= max_wait_ms) {
                    break;
                }
                if (queue_.size() >= 2) {
                    const int64_t first = use_pts_duration ? queue_.front().pts_ms : queue_.front().arrival_ms;
                    const int64_t last = use_pts_duration ? queue_.back().pts_ms : queue_.back().arrival_ms;
                    if (first < 0 || last < first || (last - first) >= min_duration_ms) {
                        break;
                    }
                }
            }
            cv_.wait_for(lock, std::chrono::milliseconds(10));
        }

        if (queue_.empty()) {
            return false;
        }

        PacketHolder holder = std::move(queue_.front());
        queue_.pop_front();
        cv_.notify_one();
        av_packet_move_ref(&packet, &holder.packet);
        return true;
    }

    void Flush() {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
        cv_.notify_all();
    }

    void Abort() {
        std::lock_guard<std::mutex> lock(mutex_);
        aborted_ = true;
        cv_.notify_all();
    }

    void NotifyAll() {
        std::lock_guard<std::mutex> lock(mutex_);
        cv_.notify_all();
    }

    void ResetAbort() {
        std::lock_guard<std::mutex> lock(mutex_);
        aborted_ = false;
    }

    size_t Size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    int64_t FrontPtsMs() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty() ? 0 : queue_.front().pts_ms;
    }

    int64_t BufferedDurationMs() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() < 2) {
            return 0;
        }
        const bool use_pts_duration = queue_.front().pts_ms >= 0 &&
            queue_.back().pts_ms >= queue_.front().pts_ms;
        const int64_t first = use_pts_duration ? queue_.front().pts_ms : queue_.front().arrival_ms;
        const int64_t last = use_pts_duration ? queue_.back().pts_ms : queue_.back().arrival_ms;
        if (first < 0 || last < first) {
            return 0;
        }
        return last - first;
    }

    uint64_t DropOldestUntilBelowDurationMs(int64_t max_duration_ms) {
        std::lock_guard<std::mutex> lock(mutex_);
        uint64_t dropped = 0;
        while (queue_.size() > 1) {
            const bool use_pts_duration = queue_.front().pts_ms >= 0 &&
                queue_.back().pts_ms >= queue_.front().pts_ms;
            const int64_t first = use_pts_duration ? queue_.front().pts_ms : queue_.front().arrival_ms;
            const int64_t last = use_pts_duration ? queue_.back().pts_ms : queue_.back().arrival_ms;
            if (first < 0 || last < first || (last - first) <= max_duration_ms) {
                break;
            }
            queue_.pop_front();
            ++dropped;
        }
        return dropped;
    }

    TrimResult TrimForContinuity(size_t safe_limit, size_t critical_limit, bool preserve_keyframe_boundary) {
        std::lock_guard<std::mutex> lock(mutex_);
        TrimResult result;
        if (queue_.size() <= safe_limit) {
            return result;
        }

        while (queue_.size() > safe_limit) {
            if (queue_.size() <= 1) {
                break;
            }
            const bool front_is_keyframe = (queue_.front().packet.flags & AV_PKT_FLAG_KEY) != 0;
            if (preserve_keyframe_boundary && front_is_keyframe && queue_.size() <= critical_limit) {
                result.kept_keyframe_boundary = true;
                break;
            }
            const int64_t dropped_pts_ms = queue_.front().pts_ms;
            queue_.pop_front();
            ++result.dropped_packets;
            if (!queue_.empty()) {
                const int64_t next_pts_ms = queue_.front().pts_ms;
                if (dropped_pts_ms >= 0 && next_pts_ms > dropped_pts_ms) {
                    result.dropped_duration_ms += (next_pts_ms - dropped_pts_ms);
                }
            }
        }

        while (queue_.size() > critical_limit && queue_.size() > 1) {
            const int64_t dropped_pts_ms = queue_.front().pts_ms;
            queue_.pop_front();
            ++result.dropped_packets;
            if (!queue_.empty()) {
                const int64_t next_pts_ms = queue_.front().pts_ms;
                if (dropped_pts_ms >= 0 && next_pts_ms > dropped_pts_ms) {
                    result.dropped_duration_ms += (next_pts_ms - dropped_pts_ms);
                }
            }
        }

        if (result.dropped_packets > 0) {
            cv_.notify_all();
        }
        return result;
    }

    TrimResult DropOldestUntilBelowDurationMs(
        int64_t max_duration_ms,
        bool preserve_keyframe_boundary,
        size_t min_packets_to_keep) {
        std::lock_guard<std::mutex> lock(mutex_);
        TrimResult result;
        const size_t min_keep = std::max<size_t>(1, min_packets_to_keep);
        while (queue_.size() > min_keep) {
            const bool use_pts_duration = queue_.front().pts_ms >= 0 &&
                queue_.back().pts_ms >= queue_.front().pts_ms;
            const int64_t first = use_pts_duration ? queue_.front().pts_ms : queue_.front().arrival_ms;
            const int64_t last = use_pts_duration ? queue_.back().pts_ms : queue_.back().arrival_ms;
            if (first < 0 || last < first) {
                break;
            }
            const int64_t buffered_duration_ms = last - first;
            if (buffered_duration_ms <= max_duration_ms) {
                break;
            }
            const bool front_is_keyframe = (queue_.front().packet.flags & AV_PKT_FLAG_KEY) != 0;
            const int64_t keyframe_preserve_slack_ms = std::max<int64_t>(120, max_duration_ms / 4);
            const bool allow_keyframe_preserve =
                buffered_duration_ms <= max_duration_ms + keyframe_preserve_slack_ms;
            if (preserve_keyframe_boundary && front_is_keyframe && queue_.size() > min_keep + 1 &&
                allow_keyframe_preserve) {
                result.kept_keyframe_boundary = true;
                break;
            }
            const int64_t dropped_pts_ms = queue_.front().pts_ms;
            queue_.pop_front();
            ++result.dropped_packets;
            if (!queue_.empty()) {
                const int64_t next_pts_ms = queue_.front().pts_ms;
                if (dropped_pts_ms >= 0 && next_pts_ms > dropped_pts_ms) {
                    result.dropped_duration_ms += (next_pts_ms - dropped_pts_ms);
                }
            }
        }
        if (result.dropped_packets > 0) {
            cv_.notify_all();
        }
        return result;
    }

private:
    struct PacketHolder {
        PacketHolder() = default;

        ~PacketHolder() {
            av_packet_unref(&packet);
        }

        PacketHolder(const PacketHolder&) = delete;
        PacketHolder& operator=(const PacketHolder&) = delete;

        PacketHolder(PacketHolder&& other) noexcept
            : arrival_ms(other.arrival_ms),
              pts_ms(other.pts_ms) {
            av_packet_move_ref(&packet, &other.packet);
        }

        PacketHolder& operator=(PacketHolder&& other) noexcept {
            if (this != &other) {
                av_packet_unref(&packet);
                av_packet_move_ref(&packet, &other.packet);
                arrival_ms = other.arrival_ms;
                pts_ms = other.pts_ms;
            }
            return *this;
        }

        AVPacket packet{};
        int64_t arrival_ms{0};
        int64_t pts_ms{0};
    };

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<PacketHolder> queue_;
    size_t max_packets_{256};
    bool drop_oldest_on_full_{true};
    bool aborted_{false};
};

class PlayerEngine::VideoFrameQueue {
public:
    void Configure(size_t capacity, bool drop_oldest_on_full) {
        std::lock_guard<std::mutex> lock(mutex_);
        max_frames_ = capacity > 0 ? capacity : 1;
        drop_oldest_on_full_ = drop_oldest_on_full;
        aborted_ = false;
    }

    bool Push(VideoFrame&& frame, uint64_t& dropped_frames) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (aborted_) {
            return false;
        }

        while (queue_.size() >= max_frames_) {
            if (!drop_oldest_on_full_) {
                cv_.wait(lock, [this]() { return aborted_ || queue_.size() < max_frames_; });
                if (aborted_) {
                    return false;
                }
                continue;
            }
            queue_.pop_front();
            ++dropped_frames;
        }

        queue_.push_back(std::move(frame));
        cv_.notify_one();
        return true;
    }

    bool Pop(VideoFrame& frame) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this]() { return aborted_ || !queue_.empty(); });
        if (queue_.empty()) {
            return false;
        }

        frame = std::move(queue_.front());
        queue_.pop_front();
        cv_.notify_one();
        return true;
    }

    void Flush() {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
        cv_.notify_all();
    }

    void Abort() {
        std::lock_guard<std::mutex> lock(mutex_);
        aborted_ = true;
        cv_.notify_all();
    }

    void ResetAbort() {
        std::lock_guard<std::mutex> lock(mutex_);
        aborted_ = false;
    }

    size_t Size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    int64_t BufferedDurationMs() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() < 2) {
            return 0;
        }
        const int64_t first = queue_.front().pts_ms;
        const int64_t last = queue_.back().pts_ms;
        if (first <= 0 || last < first) {
            return 0;
        }
        return last - first;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<VideoFrame> queue_;
    size_t max_frames_{3};
    bool drop_oldest_on_full_{true};
    bool aborted_{false};
};

PlayerEngine::PlayerEngine()
    : video_packet_queue_(std::make_unique<PacketQueue>()),
      audio_packet_queue_(std::make_unique<PacketQueue>()),
      video_frame_queue_(std::make_unique<VideoFrameQueue>()) {}

PlayerEngine::~PlayerEngine() {
    Release();
}

bool PlayerEngine::Init(const PlayerInitConfig& config, const PlayerCallbacks& callbacks) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != PlayerState::Created && state_ != PlayerState::Stopped) {
        return false;
    }
    config_ = config;
    callbacks_ = callbacks;
    stats_ = {};
    stats_.start_time_ms = NowMs();
    ChangeState(PlayerState::Initialized);
    EDGELIVE_LOG_INFO(config_.log_tag, "Init success");
    return true;
}

bool PlayerEngine::Play(const PlayRequest& request) {
    const MediaSource source = NormalizeMediaSource(request);
    if (source.url.empty()) {
        EmitError(-1, "Play url is empty");
        return false;
    }

    Stop();

    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != PlayerState::Initialized && state_ != PlayerState::Stopped) {
        return false;
    }

    active_request_ = request;
    active_request_.source = source;
    playback_mode_ = ResolvePlaybackMode(source);
    stop_requested_ = false;
    demux_eof_ = false;
    first_video_rendered_ = false;
    stats_.start_time_ms = NowMs();
    ResetQueues(source);

    EDGELIVE_LOG_INFO(
        config_.log_tag,
        "Play request resolved: request_url=" + request.url +
            " normalized_url=" + source.url +
            " source_type=" + MediaSourceTypeToString(source.type) +
            " playback_mode=" + std::string(playback_mode_ == PlaybackMode::File ? "file" : "live") +
            " should_use_full_playback=" + std::string(ShouldUseFullPlayback(source) ? "true" : "false") +
            " should_output_audio=" + std::string(ShouldOutputAudio(source) ? "true" : "false") +
            " low_latency_enabled=" + std::string(ShouldEnableLowLatency(source) ? "true" : "false"));
    if (source.options.timeout_ms <= 0 || source.options.buffer_size <= 0 ||
        source.options.packet_queue_capacity <= 0 || source.options.video_decode_queue_limit <= 0 ||
        source.options.audio_decode_queue_limit <= 0 || source.options.video_render_queue_limit <= 0 ||
        source.options.av_sync_max_lead_ms < 0 || source.options.av_sync_max_lag_ms < 0 ||
        source.options.jitter_min_ms < 0 || source.options.jitter_max_ms < 0 ||
        source.options.jitter_target_ms_initial < 0 || source.options.latency_cap_ms < 0) {
        EDGELIVE_LOG_WARN(config_.log_tag, "Play request contains suspicious parameters: " + DescribeSource(source));
    }

    demux_thread_ = std::thread(&PlayerEngine::DemuxLoop, this);
    if (!source.url.empty()) {
        video_decode_thread_ = std::thread(&PlayerEngine::VideoDecodeLoop, this);
        video_render_thread_ = std::thread(&PlayerEngine::VideoRenderLoop, this);
        if (ShouldOutputAudio(source)) {
            audio_decode_thread_ = std::thread(&PlayerEngine::AudioDecodeLoop, this);
        }
    }

    stats_.play_count++;
    ChangeState(PlayerState::Playing);
    EDGELIVE_LOG_INFO(config_.log_tag, "Play started: " + DescribeSource(source));
    return true;
}

bool PlayerEngine::Stop() {
    stop_requested_ = true;
    demux_eof_ = true;
    startup_cv_.notify_all();
    if (video_packet_queue_) {
        video_packet_queue_->Abort();
    }
    if (video_frame_queue_) {
        video_frame_queue_->Abort();
    }
    if (audio_packet_queue_) {
        audio_packet_queue_->Abort();
    }

    if (demux_thread_.joinable()) {
        demux_thread_.join();
    }
    if (video_decode_thread_.joinable()) {
        video_decode_thread_.join();
    }
    if (video_render_thread_.joinable()) {
        video_render_thread_.join();
    }
    if (audio_decode_thread_.joinable()) {
        audio_decode_thread_.join();
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == PlayerState::Released) {
        return true;
    }

    CleanupRuntimeResources();
    if (state_ == PlayerState::Playing || state_ == PlayerState::Error) {
        stats_.stop_count++;
        ChangeState(PlayerState::Stopped);
    }
    EDGELIVE_LOG_INFO(config_.log_tag, "Stop success");
    return true;
}

PlayerEngine::PlaybackMode PlayerEngine::ResolvePlaybackMode(const MediaSource& source) const {
    return ShouldUseFullPlayback(source) ? PlaybackMode::File : PlaybackMode::Live;
}

bool PlayerEngine::ShouldOutputAudio(const MediaSource& source) const {
    // Realtime streams are forced to video-only: keep file/local playback audio intact.
    return ShouldUseFullPlayback(source) &&
        source.options.output_mode != PlaybackOutputMode::RealtimeVideoOnly;
}

bool PlayerEngine::Release() {
    Stop();
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == PlayerState::Released) {
        return true;
    }
    ChangeState(PlayerState::Released);
    EDGELIVE_LOG_INFO(config_.log_tag, "Release success");
    return true;
}

PlayerState PlayerEngine::GetState() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

PlayerStats PlayerEngine::GetStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

bool PlayerEngine::CleanupRuntimeResources() {
    if (video_packet_queue_) {
        video_packet_queue_->Flush();
    }
    if (video_frame_queue_) {
        video_frame_queue_->Flush();
    }
    if (audio_packet_queue_) {
        audio_packet_queue_->Flush();
    }
    if (sws_ctx_) {
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
    }
    if (swr_ctx_) {
        swr_free(&swr_ctx_);
    }
    if (video_codec_ctx_) {
        avcodec_free_context(&video_codec_ctx_);
    }
    if (audio_codec_ctx_) {
        avcodec_free_context(&audio_codec_ctx_);
    }
    if (fmt_ctx_) {
        avformat_close_input(&fmt_ctx_);
    }
    video_stream_index_ = -1;
    audio_stream_index_ = -1;
    stats_.video_queue_size = 0;
    stats_.audio_queue_size = 0;
    first_video_rendered_ = false;
    {
        std::lock_guard<std::mutex> sync_lock(av_sync_mutex_);
        av_sync_state_ = {};
    }
    {
        std::lock_guard<std::mutex> recovery_lock(video_recovery_mutex_);
        video_recovery_state_ = {};
    }
    {
        std::lock_guard<std::mutex> render_stat_lock(render_stat_mutex_);
        render_stat_window_ = {};
    }
    {
        std::lock_guard<std::mutex> rate_lock(video_rate_mutex_);
        video_rate_control_state_ = {};
    }
    return true;
}

void PlayerEngine::ResetQueues(const MediaSource& source) {
    const MediaSourceOptions& options = source.options;
    const bool full_playback_mode = ShouldUseFullPlayback(source);
    const bool low_latency_mode = ShouldEnableLowLatency(source);
    const bool output_audio = ShouldOutputAudio(source);
    const bool drop_oldest_video = options.drop_oldest_on_full && low_latency_mode;
    const bool drop_oldest_audio = false;
    const int64_t render_frame_duration_ms = 40;
    const size_t render_queue_capacity = full_playback_mode
        ? static_cast<size_t>(std::max(3, options.video_render_queue_limit))
        : (low_latency_mode
               ? ComputeAdaptiveRenderQueueCapacity(source, render_frame_duration_ms)
               : static_cast<size_t>(std::max(3, options.video_render_queue_limit)));
    const size_t queue_capacity = static_cast<size_t>(
        full_playback_mode
            ? std::max(options.packet_queue_capacity, kFullPlaybackMinQueueCapacity)
            : (low_latency_mode
                   ? std::min(std::max(options.packet_queue_capacity, 1), kLowLatencyQueueCapacity)
                   : std::max(options.packet_queue_capacity, kContinuityPlaybackMinQueueCapacity)));
    EDGELIVE_LOG_INFO(
        config_.log_tag,
        "Queue reset: requested_capacity=" + std::to_string(options.packet_queue_capacity) +
            " effective_capacity=" + std::to_string(queue_capacity) +
            " full_playback_mode=" + std::string(full_playback_mode ? "true" : "false") +
            " low_latency_mode=" + std::string(low_latency_mode ? "true" : "false") +
            " output_audio=" + std::string(output_audio ? "true" : "false") +
            " video_decode_queue_limit=" + std::to_string(std::max(1, options.video_decode_queue_limit)) +
            " audio_decode_queue_limit=" + std::to_string(std::max(1, options.audio_decode_queue_limit)) +
            " video_render_queue_limit=" + std::to_string(std::max(1, options.video_render_queue_limit)) +
            " effective_render_queue_capacity=" + std::to_string(render_queue_capacity) +
            " drop_oldest_video=" + std::string(drop_oldest_video ? "true" : "false") +
            " drop_oldest_audio=" + std::string(drop_oldest_audio ? "true" : "false"));
    if (video_packet_queue_) {
        video_packet_queue_->Flush();
        video_packet_queue_->Configure(queue_capacity, drop_oldest_video);
        video_packet_queue_->ResetAbort();
    }
    if (video_frame_queue_) {
        video_frame_queue_->Flush();
        video_frame_queue_->Configure(render_queue_capacity, options.drop_oldest_on_full && low_latency_mode);
        video_frame_queue_->ResetAbort();
    }
    if (audio_packet_queue_) {
        audio_packet_queue_->Flush();
        audio_packet_queue_->Configure(queue_capacity, drop_oldest_audio);
        audio_packet_queue_->ResetAbort();
        if (!output_audio) {
            audio_packet_queue_->Abort();
        }
    }
    stats_.demuxed_video_packets = 0;
    stats_.demuxed_audio_packets = 0;
    stats_.dropped_video_packets = 0;
    stats_.dropped_audio_packets = 0;
    stats_.video_queue_size = 0;
    stats_.audio_queue_size = 0;
    stats_.max_video_queue_size = 0;
    stats_.max_audio_queue_size = 0;
    stats_.dns_cost_ms = -1;
    stats_.connect_cost_ms = -1;
    stats_.handshake_cost_ms = -1;
    stats_.open_input_cost_ms = -1;
    stats_.stream_info_cost_ms = -1;
    stats_.first_read_cost_ms = -1;
    stats_.first_video_decode_cost_ms = -1;
    stats_.first_video_render_cost_ms = -1;
    stats_.first_audio_decode_cost_ms = -1;
    stats_.first_audio_play_cost_ms = -1;
    stats_.audio_pop_packet_count = 0;
    stats_.audio_send_packet_count = 0;
    stats_.audio_send_packet_fail_count = 0;
    stats_.audio_receive_frame_fail_count = 0;
    stats_.audio_decoded_frame_count = 0;
    stats_.audio_invalid_param_skip_count = 0;
    stats_.audio_invalid_sample_rate_skip_count = 0;
    stats_.audio_swr_convert_fail_count = 0;
    stats_.audio_last_swr_convert_result = 0;
    stats_.audio_started_after_first_video = false;
    stats_.av_offset_avg_ms = 0;
    stats_.av_offset_max_ms = 0;
    stats_.av_offset_min_ms = 0;
    stats_.av_offset_sample_count = 0;
    stats_.video_sync_drop_count = 0;
    stats_.last_av_offset_ms = 0;
    stats_.jitter_target_ms = std::clamp(
        source.options.jitter_target_ms_initial,
        source.options.jitter_min_ms,
        source.options.jitter_max_ms);
    stats_.jitter_current_ms = 0;
    stats_.jitter_interarrival_std_ms = 0.0;
    stats_.loss_rate = 0.0;
    stats_.reorder_rate = 0.0;
    stats_.jitter_mode_switch_count = 0;
    stats_.latency_cap_trigger_count = 0;
    stats_.latency_cap_dropped_video_packets = 0;
    stats_.latency_cap_dropped_audio_packets = 0;
    stats_.video_recovery_enter_count = 0;
    stats_.video_recovery_exit_count = 0;
    stats_.video_recovery_drop_count = 0;
    stats_.last_video_recovery_duration_ms = 0;
    stats_.video_decoded_frame_count = 0;
    stats_.video_rendered_frame_count = 0;
    stats_.audio_output_frame_count = 0;
    stats_.video_decode_avg_cost_us = 0;
    stats_.video_render_avg_cost_us = 0;
    stats_.audio_decode_avg_cost_us = 0;
    stats_.video_render_buffer_ms = 0;
    stats_.video_render_playback_rate = 1.0;
    stats_.video_render_pid_error_ms = 0;
    stats_.audio_latest_demux_pts_ms = 0;
    stats_.audio_latest_queue_head_pts_ms = 0;
    stats_.audio_latest_decode_pts_ms = 0;
    stats_.audio_latest_submit_pts_ms = 0;
    stats_.audio_decoder_pts_ms = 0;
    stats_.audio_computed_output_pts_ms = 0;
    stats_.audio_decode_to_submit_offset_ms = 0;
    stats_.audio_submit_to_video_gap_ms = 0;
    stats_.audio_demux_to_video_gap_ms = 0;
    stats_.audio_queue_duration_ms = 0;
    stats_.audio_trim_drop_count = 0;
    stats_.audio_trim_drop_duration_ms = 0;
    stats_.audio_latency_cap_drop_count = 0;
    stats_.audio_latency_cap_drop_duration_ms = 0;
    stats_.audio_pts_rebase_count = 0;
    stats_.audio_pts_rebase_last_reason = "none";
    stats_.video_packet_wait_avg_us = 0;
    stats_.audio_packet_wait_avg_us = 0;
    stats_.video_decode_fps = 0;
    stats_.video_render_fps = 0;
    stats_.audio_output_fps = 0;
    stats_.video_pts_backpressure_mode = "Normal";
    stats_.video_pts_invalid_count = 0;
    stats_.video_pts_rollback_count = 0;
    stats_.video_pts_jump_count = 0;
    stats_.video_pts_drift_correction_count = 0;
    stats_.video_pts_sanitized_count = 0;
    stats_.video_pts_keyframe_reanchor_count = 0;
    stats_.video_pts_last_raw_ms = 0;
    stats_.video_pts_last_effective_ms = 0;
    stats_.video_pts_last_drift_ms = 0;
    last_stats_emit_ms_ = 0;
    {
        std::lock_guard<std::mutex> sync_lock(av_sync_mutex_);
        av_sync_state_ = {};
    }
    {
        std::lock_guard<std::mutex> jitter_lock(jitter_mutex_);
        jitter_state_ = {};
        jitter_state_.target_buffer_ms = stats_.jitter_target_ms;
        jitter_state_.window_start_ms = NowMs();
    }
    {
        std::lock_guard<std::mutex> recovery_lock(video_recovery_mutex_);
        video_recovery_state_ = {};
    }
    ResetVideoPtsBackpressureState();
    {
        std::lock_guard<std::mutex> rate_lock(video_rate_mutex_);
        video_rate_control_state_ = {};
    }
    EDGELIVE_LOG_INFO(
        config_.log_tag,
        "Queue policy: full_playback=" + std::string(full_playback_mode ? "true" : "false") +
            " output_audio=" + std::string(output_audio ? "true" : "false") +
            " low_latency=" + std::string(low_latency_mode ? "true" : "false") +
            " drop_oldest_video=" + std::string(drop_oldest_video ? "true" : "false") +
            " drop_oldest_audio=" + std::string(drop_oldest_audio ? "true" : "false") +
            " queue_capacity=" + std::to_string(queue_capacity));
}

void PlayerEngine::DemuxLoop() {
    const MediaSource source = NormalizeMediaSource(active_request_);
    if (ResolvePlaybackMode(source) == PlaybackMode::File) {
        DemuxFileLoop(source);
        return;
    }
    DemuxLiveLoop(source);
}

void PlayerEngine::DemuxFileLoop(const MediaSource& source) {
    DemuxLoopImpl(source, false);
}

void PlayerEngine::DemuxLiveLoop(const MediaSource& source) {
    DemuxLoopImpl(source, true);
}

void PlayerEngine::DemuxLoopImpl(const MediaSource& source, bool live_mode) {
    const bool output_audio = ShouldOutputAudio(source);
    AVFormatContext* local_fmt_ctx = avformat_alloc_context();
    if (local_fmt_ctx == nullptr) {
        EmitError(-2, "Could not allocate format context");
        return;
    }

    local_fmt_ctx->interrupt_callback.opaque = this;
    local_fmt_ctx->interrupt_callback.callback = [](void* opaque) -> int {
        auto* engine = static_cast<PlayerEngine*>(opaque);
        return engine != nullptr && engine->stop_requested_.load() ? 1 : 0;
    };

    const int64_t play_start_ms = NowMs();
    int64_t dns_cost_ms = -1;
    int64_t connect_cost_ms = -1;
    if (live_mode) {
        ProbeNetworkTimings(source, source.options.timeout_ms, dns_cost_ms, connect_cost_ms);
    }
    if (live_mode && (dns_cost_ms >= 0 || connect_cost_ms >= 0)) {
        UpdateStats([&](PlayerStats& stats) {
            stats.dns_cost_ms = dns_cost_ms;
            stats.connect_cost_ms = connect_cost_ms;
        });
    }
    AVDictionary* open_options = BuildOpenOptions(source);
    EDGELIVE_LOG_INFO(config_.log_tag, "Opening source: " + DescribeSource(source));

    const int ret_open = avformat_open_input(&local_fmt_ctx, source.url.c_str(), nullptr, &open_options);
    av_dict_free(&open_options);
    if (ret_open < 0) {
        avformat_free_context(local_fmt_ctx);
        EmitError(-3, "Could not open input: " + FfmpegErrorToString(ret_open));
        return;
    }

    UpdateStats([&](PlayerStats& stats) {
        stats.open_input_cost_ms = NowMs() - play_start_ms;
        if (stats.dns_cost_ms >= 0 && stats.connect_cost_ms >= 0) {
            const int64_t remain = stats.open_input_cost_ms - stats.dns_cost_ms - stats.connect_cost_ms;
            stats.handshake_cost_ms = remain > 0 ? remain : 0;
        }
    });

    const int ret_info = avformat_find_stream_info(local_fmt_ctx, nullptr);
    if (ret_info < 0) {
        avformat_close_input(&local_fmt_ctx);
        EmitError(-4, "Could not find stream info: " + FfmpegErrorToString(ret_info));
        return;
    }

    UpdateStats([&](PlayerStats& stats) {
        stats.stream_info_cost_ms = NowMs() - play_start_ms;
    });

    const int local_video_stream_index =
        av_find_best_stream(local_fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    const int local_audio_stream_index = output_audio
        ? av_find_best_stream(local_fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0)
        : -1;
    if (local_video_stream_index < 0 && local_audio_stream_index < 0) {
        avformat_close_input(&local_fmt_ctx);
        EmitError(-5, "Could not find audio or video stream");
        return;
    }

    AVCodecContext* local_video_codec_ctx = nullptr;
    if (local_video_stream_index >= 0) {
        AVStream* video_stream = local_fmt_ctx->streams[local_video_stream_index];
        const AVCodec* video_codec = avcodec_find_decoder(video_stream->codecpar->codec_id);
        if (video_codec == nullptr) {
            avformat_close_input(&local_fmt_ctx);
            EmitError(-6, "Could not find video decoder");
            return;
        }
        local_video_codec_ctx = avcodec_alloc_context3(video_codec);
        if (local_video_codec_ctx == nullptr) {
            avformat_close_input(&local_fmt_ctx);
            EmitError(-7, "Could not allocate video decoder context");
            return;
        }
        if (avcodec_parameters_to_context(local_video_codec_ctx, video_stream->codecpar) < 0 ||
            avcodec_open2(local_video_codec_ctx, video_codec, nullptr) < 0) {
            avcodec_free_context(&local_video_codec_ctx);
            avformat_close_input(&local_fmt_ctx);
            EmitError(-8, "Could not open video decoder");
            return;
        }
    }

    AVCodecContext* local_audio_codec_ctx = nullptr;
    if (local_audio_stream_index >= 0) {
        AVStream* audio_stream = local_fmt_ctx->streams[local_audio_stream_index];
        const AVCodec* audio_codec = avcodec_find_decoder(audio_stream->codecpar->codec_id);
        if (audio_codec == nullptr) {
            if (local_video_codec_ctx != nullptr) {
                avcodec_free_context(&local_video_codec_ctx);
            }
            avformat_close_input(&local_fmt_ctx);
            EmitError(-10, "Could not find audio decoder");
            return;
        }
        local_audio_codec_ctx = avcodec_alloc_context3(audio_codec);
        if (local_audio_codec_ctx == nullptr) {
            if (local_video_codec_ctx != nullptr) {
                avcodec_free_context(&local_video_codec_ctx);
            }
            avformat_close_input(&local_fmt_ctx);
            EmitError(-11, "Could not allocate audio decoder context");
            return;
        }
        if (avcodec_parameters_to_context(local_audio_codec_ctx, audio_stream->codecpar) < 0 ||
            avcodec_open2(local_audio_codec_ctx, audio_codec, nullptr) < 0) {
            avcodec_free_context(&local_audio_codec_ctx);
            if (local_video_codec_ctx != nullptr) {
                avcodec_free_context(&local_video_codec_ctx);
            }
            avformat_close_input(&local_fmt_ctx);
            EmitError(-12, "Could not open audio decoder");
            return;
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        fmt_ctx_ = local_fmt_ctx;
        video_stream_index_ = local_video_stream_index;
        audio_stream_index_ = local_audio_stream_index;
        video_codec_ctx_ = local_video_codec_ctx;
        audio_codec_ctx_ = local_audio_codec_ctx;
        sws_ctx_ = nullptr;
        swr_ctx_ = nullptr;
    }

    EDGELIVE_LOG_INFO(
        config_.log_tag,
        "Demux ready: video_stream=" + std::to_string(local_video_stream_index) +
            " audio_stream=" + std::to_string(local_audio_stream_index));

    AVPacket* packet = av_packet_alloc();
    if (packet == nullptr) {
        EmitError(-14, "Could not allocate packet");
        return;
    }

    bool first_read_reported = false;
    int64_t last_jitter_eval_ms = 0;
    bool reached_eof = false;
    uint64_t audio_missing_ts_packet_count = 0;
    bool audio_missing_ts_packet_reported = false;
    constexpr uint64_t kAudioMissingTimestampReportThreshold = 20;
    while (!stop_requested_) {
        const int ret = av_read_frame(local_fmt_ctx, packet);
        if (ret == AVERROR_EOF) {
            EDGELIVE_LOG_INFO(config_.log_tag, "Demux reached EOF");
            reached_eof = true;
            break;
        }
        if (ret == AVERROR(EAGAIN)) {
            av_packet_unref(packet);
            continue;
        }
        if (ret < 0) {
            if (!stop_requested_) {
                EDGELIVE_LOG_WARN(config_.log_tag, "Demux read failed: " + FfmpegErrorToString(ret));
            }
            av_packet_unref(packet);
            break;
        }

        if (!first_read_reported) {
            first_read_reported = true;
            UpdateStats([&](PlayerStats& stats) {
                if (stats.first_read_cost_ms < 0) {
                    stats.first_read_cost_ms = NowMs() - stats.start_time_ms;
                }
            });
        }

        const int64_t arrival_ms = SteadyNowMs();
        if (packet->stream_index == local_video_stream_index && video_packet_queue_) {
            uint64_t dropped = 0;
            const int64_t packet_pts_ms = PtsToMs(
                packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts,
                local_fmt_ctx->streams[local_video_stream_index]->time_base);
            if (video_packet_queue_->PushWithTiming(*packet, arrival_ms, packet_pts_ms, dropped)) {
                const auto trim = video_packet_queue_->TrimForContinuity(
                    kMaxSafeDeepQueuePackets,
                    kCriticalQueuePackets,
                    true);
                OnVideoPacketObserved(
                    arrival_ms,
                    packet_pts_ms);
                int64_t demux_wall_delta_ms = -1;
                int64_t demux_pts_delta_ms = -1;
                uint64_t demux_packets_since_log = 0;
                {
                    std::lock_guard<std::mutex> diag_lock(video_diag_mutex_);
                    if (video_timing_diag_.last_demux_arrival_ms >= 0) {
                        demux_wall_delta_ms = arrival_ms - video_timing_diag_.last_demux_arrival_ms;
                    }
                    if (video_timing_diag_.last_demux_pts_ms >= 0 && packet_pts_ms >= 0) {
                        demux_pts_delta_ms = packet_pts_ms - video_timing_diag_.last_demux_pts_ms;
                    }
                    video_timing_diag_.last_demux_arrival_ms = arrival_ms;
                    video_timing_diag_.last_demux_pts_ms = packet_pts_ms;
                    video_timing_diag_.demux_packets_since_log++;
                    demux_packets_since_log = video_timing_diag_.demux_packets_since_log;
                }
                UpdateStats([&](PlayerStats& stats) {
                    stats.demuxed_video_packets++;
                    stats.dropped_video_packets += dropped + trim.dropped_packets;
                    stats.video_queue_size = video_packet_queue_->Size();
                    stats.max_video_queue_size = std::max(stats.max_video_queue_size, stats.video_queue_size);
                    stats.jitter_current_ms =
                        static_cast<int>(video_packet_queue_ ? video_packet_queue_->BufferedDurationMs() : 0);
                });
                if (ShouldEmitPeriodicLog(last_video_demux_diag_log_ms_, 500, arrival_ms)) {
                    EDGELIVE_LOG_INFO(
                        config_.log_tag,
                        "VIDEO-TIMING-DEMUX packets_since_log=" + std::to_string(demux_packets_since_log) +
                            " wall_delta_ms=" + std::to_string(demux_wall_delta_ms) +
                            " pts_delta_ms=" + std::to_string(demux_pts_delta_ms) +
                            " packet_pts_ms=" + std::to_string(packet_pts_ms) +
                            " pkt_pts=" + std::to_string(packet->pts) +
                            " pkt_dts=" + std::to_string(packet->dts) +
                            " key=" + std::string((packet->flags & AV_PKT_FLAG_KEY) != 0 ? "1" : "0") +
                            " size=" + std::to_string(packet->size) +
                            " video_q=" + std::to_string(video_packet_queue_ ? video_packet_queue_->Size() : 0) +
                            " video_q_buf_ms=" +
                            std::to_string(video_packet_queue_ ? video_packet_queue_->BufferedDurationMs() : 0) +
                            " dropped_on_push=" + std::to_string(dropped) +
                            " dropped_on_trim=" + std::to_string(trim.dropped_packets) +
                            " trim_duration_ms=" + std::to_string(trim.dropped_duration_ms));
                    std::lock_guard<std::mutex> diag_lock(video_diag_mutex_);
                    video_timing_diag_.demux_packets_since_log = 0;
                }
            }
        } else if (output_audio && packet->stream_index == local_audio_stream_index && audio_packet_queue_) {
            uint64_t dropped = 0;
            const int64_t packet_pts_ms = PtsToMs(
                packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts,
                local_fmt_ctx->streams[local_audio_stream_index]->time_base);
            if (audio_packet_queue_->PushWithTiming(*packet, arrival_ms, packet_pts_ms, dropped)) {
                if (HasMeaningfulPacketTimestamp(*packet)) {
                    audio_missing_ts_packet_count = 0;
                } else if (HasOnlyZeroOrMissingPacketTimestamp(*packet)) {
                    ++audio_missing_ts_packet_count;
                    if (!audio_missing_ts_packet_reported &&
                        audio_missing_ts_packet_count >= kAudioMissingTimestampReportThreshold) {
                        audio_missing_ts_packet_reported = true;
                        EDGELIVE_LOG_WARN(
                            config_.log_tag,
                            "Audio demux packets have no usable timestamp for " +
                                std::to_string(audio_missing_ts_packet_count) +
                                " consecutive packets. Publisher may be missing audio PTS/DTS: pts=" +
                                std::to_string(packet->pts) +
                                " dts=" + std::to_string(packet->dts));
                        UpdateStats([&](PlayerStats& stats) {
                            stats.audio_pts_rebase_last_reason = "audio_demux_timestamp_missing";
                        });
                    }
                } else {
                    audio_missing_ts_packet_count = 0;
                }
                UpdateStats([&](PlayerStats& stats) {
                    stats.demuxed_audio_packets++;
                    stats.dropped_audio_packets += dropped;
                    stats.audio_queue_size = audio_packet_queue_->Size();
                    stats.max_audio_queue_size = std::max(stats.max_audio_queue_size, stats.audio_queue_size);
                    stats.audio_latest_demux_pts_ms = packet_pts_ms;
                    stats.audio_queue_duration_ms =
                        audio_packet_queue_ ? audio_packet_queue_->BufferedDurationMs() : 0;
                    if (stats.sync_video_pts_ms > 0) {
                        stats.audio_demux_to_video_gap_ms = stats.sync_video_pts_ms - packet_pts_ms;
                    }
                });
            }
        }
        if (live_mode && source.options.adaptive_jitter_enabled &&
            (last_jitter_eval_ms <= 0 || arrival_ms - last_jitter_eval_ms >= kJitterUpdateIntervalMs)) {
            last_jitter_eval_ms = arrival_ms;
            MaybeUpdateJitterController(source, arrival_ms);
            ApplyLatencyCap(source);
        }

        av_packet_unref(packet);
    }

    demux_eof_ = true;
    if (video_packet_queue_) {
        video_packet_queue_->NotifyAll();
    }
    if (audio_packet_queue_) {
        audio_packet_queue_->NotifyAll();
    }
    if (video_packet_queue_) {
        video_packet_queue_->Abort();
    }
    if (audio_packet_queue_) {
        audio_packet_queue_->Abort();
    }
    if (reached_eof && local_video_stream_index < 0 && video_frame_queue_) {
        video_frame_queue_->Abort();
    }
    av_packet_free(&packet);
}

void PlayerEngine::VideoDecodeLoop() {
    const MediaSource source = NormalizeMediaSource(active_request_);
    if (ResolvePlaybackMode(source) == PlaybackMode::File) {
        VideoDecodeFileLoop(source);
        return;
    }
    VideoDecodeLiveLoop(source);
}

void PlayerEngine::VideoDecodeFileLoop(const MediaSource& source) {
    VideoDecodeLoopImpl(source, false);
}

void PlayerEngine::VideoDecodeLiveLoop(const MediaSource& source) {
    VideoDecodeLoopImpl(source, true);
}

void PlayerEngine::VideoDecodeLoopImpl(const MediaSource& source, bool live_mode) {
    AVFrame* frame = av_frame_alloc();
    AVFrame* bgra_frame = av_frame_alloc();
    if (frame == nullptr || bgra_frame == nullptr) {
        if (frame != nullptr) {
            av_frame_free(&frame);
        }
        if (bgra_frame != nullptr) {
            av_frame_free(&bgra_frame);
        }
        EmitError(-15, "Could not allocate video frame");
        if (video_frame_queue_) {
            video_frame_queue_->Abort();
        }
        return;
    }

    std::vector<std::shared_ptr<std::vector<uint8_t>>> frame_buffer_pool;
    SwsContext* sws_ctx = nullptr;
    int sws_width = 0;
    int sws_height = 0;
    int frame_buffer_size = 0;
    AVPixelFormat sws_input_format = AV_PIX_FMT_NONE;
    int64_t video_decode_total_cost_us = 0;
    uint64_t video_decode_cost_samples = 0;
    int64_t video_wait_total_us = 0;
    uint64_t video_wait_samples = 0;
    int64_t video_decode_fps_window_start_ms = SteadyNowMs();
    uint64_t video_decode_fps_window_frames = 0;

    while (!stop_requested_) {
        AVCodecContext* codec_ctx = nullptr;
        AVFormatContext* fmt_ctx = nullptr;
        int stream_index = -1;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            codec_ctx = video_codec_ctx_;
            fmt_ctx = fmt_ctx_;
            sws_ctx = sws_ctx_;
            stream_index = video_stream_index_;
        }

        if (codec_ctx == nullptr || fmt_ctx == nullptr || stream_index < 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        AVPacket packet{};
        int jitter_target_ms = live_mode ? GetCurrentJitterTargetMs(source) : 0;
        if (live_mode && jitter_target_ms > 0 && video_frame_queue_) {
            const size_t render_queue_size = video_frame_queue_->Size();
            const int64_t render_queue_buffer_ms = video_frame_queue_->BufferedDurationMs();
            if (render_queue_size == 0 || render_queue_buffer_ms <= 0) {
                jitter_target_ms = 0;
            } else if (render_queue_size == 1) {
                jitter_target_ms = std::min(jitter_target_ms, 40);
            }
        }
        const int64_t video_wait_begin_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
                                                .count();
        if (!video_packet_queue_ ||
            !video_packet_queue_->PopWithMinDuration(packet, jitter_target_ms, !live_mode)) {
            av_packet_unref(&packet);
            if (stop_requested_) {
                break;
            }
            if (demux_eof_) {
                break;
            }
            continue;
        }
        const int64_t video_wait_end_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
                                              .count();
        video_wait_total_us += (video_wait_end_us - video_wait_begin_us);
        video_wait_samples++;
        UpdateStats([&](PlayerStats& stats) {
            stats.video_packet_wait_avg_us = video_wait_samples > 0
                ? static_cast<int64_t>(video_wait_total_us / static_cast<int64_t>(video_wait_samples))
                : 0;
        });

        if (live_mode && ShouldEnableVideoRecovery(source)) {
            uint64_t dropped_packets = 0;
            const bool is_keyframe = (packet.flags & AV_PKT_FLAG_KEY) != 0;
            if (ShouldDropPacketUntilKeyframe(is_keyframe, dropped_packets)) {
                if (dropped_packets > 0) {
                    UpdateStats([&](PlayerStats& stats) {
                        stats.video_recovery_drop_count += dropped_packets;
                        stats.dropped_video_packets += dropped_packets;
                        stats.video_queue_size = video_packet_queue_ ? video_packet_queue_->Size() : 0;
                    });
                }
                av_packet_unref(&packet);
                continue;
            }
        }

        const int send_ret = avcodec_send_packet(codec_ctx, &packet);
        const bool should_receive_pending_frames = send_ret == 0 || send_ret == AVERROR(EAGAIN);
        if (send_ret < 0 && send_ret != AVERROR(EAGAIN)) {
                if (live_mode && ShouldEnableVideoRecovery(source)) {
                    EnterVideoRecovery("avcodec_send_packet failed: " + FfmpegErrorToString(send_ret));
                }
            av_packet_unref(&packet);
            continue;
        }
        av_packet_unref(&packet);
        if (!should_receive_pending_frames) {
            continue;
        }

        while (!stop_requested_) {
            const int64_t decode_begin_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                                                .count();
            av_frame_unref(frame);
            const int recv_ret = avcodec_receive_frame(codec_ctx, frame);
            if (recv_ret == AVERROR(EAGAIN) || recv_ret == AVERROR_EOF) {
                break;
            }
            if (recv_ret < 0) {
                if (live_mode && ShouldEnableVideoRecovery(source)) {
                    EnterVideoRecovery("avcodec_receive_frame failed: " + FfmpegErrorToString(recv_ret));
                }
                break;
            }
            const int64_t decode_end_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                                              .count();
            video_decode_total_cost_us += (decode_end_us - decode_begin_us);
            video_decode_cost_samples++;

            const bool frame_corrupt = (frame->flags & AV_FRAME_FLAG_CORRUPT) != 0;
            if (live_mode && frame_corrupt && ShouldEnableVideoRecovery(source)) {
                EnterVideoRecovery("decoded frame flagged corrupt");
                av_frame_unref(frame);
                continue;
            }
            if (live_mode && ShouldEnableVideoRecovery(source) && IsVideoRecoveryActive()) {
                ExitVideoRecovery();
            }

            const int width = frame->width > 0 ? frame->width : codec_ctx->width;
            const int height = frame->height > 0 ? frame->height : codec_ctx->height;
            if (width <= 0 || height <= 0) {
                av_frame_unref(frame);
                continue;
            }

            AVStream* video_stream = fmt_ctx->streams[stream_index];
            const int64_t resolved_frame_pts_ms = ResolveVideoFramePtsMs(frame, video_stream);
            const bool frame_is_key = (frame->flags & AV_FRAME_FLAG_KEY) != 0;
            const int64_t frame_duration_hint_ms = ResolveFrameDurationMs(video_stream);
            int64_t frame_pts_ms = resolved_frame_pts_ms;
            VideoPtsNormalizeResult pts_result{};
            if (ShouldEnableVideoPtsBackpressure(source)) {
                pts_result = NormalizeVideoPtsForBackpressure(
                    source,
                    resolved_frame_pts_ms,
                    frame_is_key,
                    frame_duration_hint_ms);
                frame_pts_ms = pts_result.effective_pts_ms;
                if (pts_result.request_keyframe_recovery && ShouldEnableVideoRecovery(source)) {
                    EnterVideoRecovery("video pts anomaly requested keyframe recovery");
                }
                UpdateStats([&](PlayerStats& stats) {
                    stats.video_pts_backpressure_mode = VideoPtsBackpressureModeToString(static_cast<int>(pts_result.mode));
                    stats.video_pts_last_raw_ms = pts_result.raw_pts_ms;
                    stats.video_pts_last_effective_ms = pts_result.effective_pts_ms;
                    stats.video_pts_last_drift_ms = pts_result.drift_ms;
                    if ((pts_result.flags & static_cast<uint32_t>(VideoPtsAnomalyFlags::Invalid)) != 0) {
                        stats.video_pts_invalid_count++;
                    }
                    if ((pts_result.flags & static_cast<uint32_t>(VideoPtsAnomalyFlags::Rollback)) != 0) {
                        stats.video_pts_rollback_count++;
                    }
                    if ((pts_result.flags & static_cast<uint32_t>(VideoPtsAnomalyFlags::Jump)) != 0) {
                        stats.video_pts_jump_count++;
                    }
                    if ((pts_result.flags & static_cast<uint32_t>(VideoPtsAnomalyFlags::Sanitized)) != 0) {
                        stats.video_pts_sanitized_count++;
                    }
                    if ((pts_result.flags & static_cast<uint32_t>(VideoPtsAnomalyFlags::KeyframeReanchor)) != 0) {
                        stats.video_pts_keyframe_reanchor_count++;
                    }
                });
            } else if (frame_pts_ms <= 0) {
                const int64_t estimated_step_ms = GetVideoPtsEstimatedFrameDurationMs();
                frame_pts_ms = estimated_step_ms;
            }
            const int64_t now_ms = SteadyNowMs();
            int64_t decode_wall_delta_ms = -1;
            int64_t decode_pts_delta_ms = -1;
            uint64_t decoded_frames_since_log = 0;
            int last_width = 0;
            int last_height = 0;
            int last_format = -1;
            {
                std::lock_guard<std::mutex> diag_lock(video_diag_mutex_);
                if (video_timing_diag_.last_decode_wall_ms >= 0) {
                    decode_wall_delta_ms = now_ms - video_timing_diag_.last_decode_wall_ms;
                }
                if (video_timing_diag_.last_decode_pts_ms >= 0) {
                    decode_pts_delta_ms = frame_pts_ms - video_timing_diag_.last_decode_pts_ms;
                }
                last_width = video_timing_diag_.last_width;
                last_height = video_timing_diag_.last_height;
                last_format = video_timing_diag_.last_format;
                video_timing_diag_.last_decode_wall_ms = now_ms;
                video_timing_diag_.last_decode_pts_ms = frame_pts_ms;
                video_timing_diag_.last_width = width;
                video_timing_diag_.last_height = height;
                video_timing_diag_.last_format = frame->format;
                video_timing_diag_.decoded_frames_since_log++;
                decoded_frames_since_log = video_timing_diag_.decoded_frames_since_log;
            }
            (void)decode_wall_delta_ms;
            (void)decode_pts_delta_ms;
            (void)decoded_frames_since_log;
            (void)last_width;
            (void)last_height;
            (void)last_format;

            const AVPixelFormat input_format = static_cast<AVPixelFormat>(frame->format);
            const bool use_gpu_format = IsGpuConvertibleVideoFormat(input_format);
            VideoPixelFormat output_format = VideoPixelFormat::Bgra;
            int output_strides[3]{0, 0, 0};
            int output_buffer_size = 0;
            if (use_gpu_format) {
                output_buffer_size = GpuConvertibleBufferSize(input_format, width, height);
                output_strides[0] = width;
                if (input_format == AV_PIX_FMT_NV12) {
                    output_format = VideoPixelFormat::Nv12;
                    output_strides[1] = width;
                } else {
                    output_format = VideoPixelFormat::Yuv420p;
                    output_strides[1] = (width + 1) / 2;
                    output_strides[2] = (width + 1) / 2;
                }
            }

            if (use_gpu_format && output_buffer_size <= 0) {
                av_frame_unref(frame);
                continue;
            }

            if (!use_gpu_format &&
                (sws_ctx == nullptr || width != sws_width || height != sws_height || input_format != sws_input_format)) {
                sws_ctx = sws_getCachedContext(
                    sws_ctx,
                    width,
                    height,
                    input_format,
                    width,
                    height,
                    AV_PIX_FMT_BGRA,
                    SWS_FAST_BILINEAR,
                    nullptr,
                    nullptr,
                    nullptr);
                if (sws_ctx == nullptr) {
                    EmitError(-9, "Could not create video converter");
                    if (video_frame_queue_) {
                        video_frame_queue_->Abort();
                    }
                    av_frame_free(&bgra_frame);
                    av_frame_free(&frame);
                    return;
                }

                sws_width = width;
                sws_height = height;
                sws_input_format = input_format;
                std::lock_guard<std::mutex> lock(mutex_);
                sws_ctx_ = sws_ctx;
            }

            if (!use_gpu_format) {
                output_buffer_size = av_image_get_buffer_size(AV_PIX_FMT_BGRA, width, height, 1);
                if (output_buffer_size <= 0) {
                    av_frame_unref(frame);
                    continue;
                }
            }

            if (output_buffer_size != frame_buffer_size) {
                frame_buffer_pool.clear();
                frame_buffer_size = output_buffer_size;
            }

            std::shared_ptr<std::vector<uint8_t>> frame_buffer;
            for (const auto& candidate : frame_buffer_pool) {
                if (candidate.use_count() == 1 && candidate->size() == static_cast<size_t>(output_buffer_size)) {
                    frame_buffer = candidate;
                    break;
                }
            }
            if (!frame_buffer) {
                frame_buffer = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(output_buffer_size));
                frame_buffer_pool.push_back(frame_buffer);
            }

            uint8_t* output_planes[3]{nullptr, nullptr, nullptr};
            if (use_gpu_format) {
                output_planes[0] = frame_buffer->data();
                CopyFramePlane(
                    output_planes[0],
                    output_strides[0],
                    frame->data[0],
                    frame->linesize[0],
                    width,
                    height);
                if (input_format == AV_PIX_FMT_NV12) {
                    output_planes[1] = output_planes[0] + output_strides[0] * height;
                    CopyFramePlane(
                        output_planes[1],
                        output_strides[1],
                        frame->data[1],
                        frame->linesize[1],
                        width,
                        (height + 1) / 2);
                } else {
                    const int chroma_width = (width + 1) / 2;
                    const int chroma_height = (height + 1) / 2;
                    output_planes[1] = output_planes[0] + output_strides[0] * height;
                    output_planes[2] = output_planes[1] + output_strides[1] * chroma_height;
                    CopyFramePlane(
                        output_planes[1],
                        output_strides[1],
                        frame->data[1],
                        frame->linesize[1],
                        chroma_width,
                        chroma_height);
                    CopyFramePlane(
                        output_planes[2],
                        output_strides[2],
                        frame->data[2],
                        frame->linesize[2],
                        chroma_width,
                        chroma_height);
                }
            } else {
                av_image_fill_arrays(
                    bgra_frame->data,
                    bgra_frame->linesize,
                    frame_buffer->data(),
                    AV_PIX_FMT_BGRA,
                    width,
                    height,
                    1);

                sws_scale(
                    sws_ctx,
                    frame->data,
                    frame->linesize,
                    0,
                    frame->height,
                    bgra_frame->data,
                    bgra_frame->linesize);
                output_strides[0] = bgra_frame->linesize[0];
                output_planes[0] = bgra_frame->data[0];
            }

            UpdateStats([&](PlayerStats& stats) {
                if (stats.first_video_decode_cost_ms < 0) {
                    stats.first_video_decode_cost_ms = NowMs() - stats.start_time_ms;
                }
                stats.video_queue_size = video_packet_queue_ ? video_packet_queue_->Size() : 0;
                stats.video_decoded_frame_count++;
                stats.video_decode_avg_cost_us = video_decode_cost_samples > 0
                    ? static_cast<int64_t>(video_decode_total_cost_us / static_cast<int64_t>(video_decode_cost_samples))
                    : 0;
            });
            video_decode_fps_window_frames++;
            const int64_t decode_now_ms = SteadyNowMs();
            if (decode_now_ms - video_decode_fps_window_start_ms >= 1000) {
                const int64_t window_ms = std::max<int64_t>(1, decode_now_ms - video_decode_fps_window_start_ms);
                const int fps = static_cast<int>(video_decode_fps_window_frames * 1000 / window_ms);
                UpdateStats([&](PlayerStats& stats) { stats.video_decode_fps = fps; });
                video_decode_fps_window_start_ms = decode_now_ms;
                video_decode_fps_window_frames = 0;
            }

            VideoFrame out;
            out.width = width;
            out.height = height;
            out.stride = output_strides[0];
            out.strides[0] = output_strides[0];
            out.strides[1] = output_strides[1];
            out.strides[2] = output_strides[2];
            out.pts_ms = frame_pts_ms;
            out.raw_pts_ms = resolved_frame_pts_ms;
            out.enqueue_steady_ms = SteadyNowMs();
            out.data = output_planes[0];
            out.planes[0] = output_planes[0];
            out.planes[1] = output_planes[1];
            out.planes[2] = output_planes[2];
            out.format = output_format;
            out.is_keyframe = frame_is_key;
            out.pts_reanchored = pts_result.request_render_clock_reset;
            out.buffer = frame_buffer;

            uint64_t dropped_frames = 0;
            if (video_frame_queue_ && video_frame_queue_->Push(std::move(out), dropped_frames) && dropped_frames > 0) {
                UpdateStats([&](PlayerStats& stats) {
                    stats.video_sync_drop_count += dropped_frames;
                });
            }

            av_frame_unref(frame);
        }
    }

    av_frame_free(&bgra_frame);
    av_frame_free(&frame);
    if (video_frame_queue_) {
        video_frame_queue_->Abort();
    }
}

void PlayerEngine::VideoRenderLoop() {
    const MediaSource source = NormalizeMediaSource(active_request_);
    if (ResolvePlaybackMode(source) == PlaybackMode::File) {
        VideoRenderFileLoop(source);
        return;
    }
    VideoRenderLiveLoop(source);
}

void PlayerEngine::VideoRenderFileLoop(const MediaSource& source) {
    VideoRenderLoopImpl(source, false);
}

void PlayerEngine::VideoRenderLiveLoop(const MediaSource& source) {
    VideoRenderLoopImpl(source, true);
}

void PlayerEngine::VideoRenderLoopImpl(const MediaSource& source, bool live_mode) {
    int64_t video_render_total_cost_us = 0;
    uint64_t video_render_cost_samples = 0;
    int64_t video_render_fps_window_start_ms = SteadyNowMs();
    uint64_t video_render_fps_window_frames = 0;
    while (!stop_requested_) {
        VideoFrame frame;
        if (!video_frame_queue_ || !video_frame_queue_->Pop(frame)) {
            if (stop_requested_ || demux_eof_) {
                break;
            }
            continue;
        }

        int audio_stream_index = -1;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            audio_stream_index = audio_stream_index_;
        }

        {
            std::lock_guard<std::mutex> sync_lock(av_sync_mutex_);
            if (frame.pts_ms > 0 && av_sync_state_.first_video_pts_ms == std::numeric_limits<int64_t>::min()) {
                av_sync_state_.first_video_pts_ms = frame.pts_ms;
                if (av_sync_state_.first_audio_pts_ms != std::numeric_limits<int64_t>::min()) {
                    av_sync_state_.av_offset_ms =
                        av_sync_state_.first_audio_pts_ms - av_sync_state_.first_video_pts_ms;
                    av_sync_state_.av_offset_ready = true;
                    EDGELIVE_LOG_INFO(
                        config_.log_tag,
                        "AV sync anchor ready(video-first): first_audio_pts_ms=" +
                            std::to_string(av_sync_state_.first_audio_pts_ms) +
                            " first_video_pts_ms=" + std::to_string(av_sync_state_.first_video_pts_ms) +
                            " av_offset_ms=" + std::to_string(av_sync_state_.av_offset_ms));
                }
            }
        }
        UpdateStats([&](PlayerStats& stats) {
            stats.sync_video_pts_ms = frame.pts_ms;
            stats.sync_audio_clock_ms = 0;
            stats.sync_raw_diff_ms = 0;
            stats.sync_final_delay_ms = 0;
            stats.sync_action = "RENDER_NOW";
        });

        {
            const int64_t audio_clock_ms = GetAudioMasterClockMs();
            const int64_t wall_now_ms = SteadyNowMs();
            int64_t current_av_diff_ms = 0;
            {
                std::lock_guard<std::mutex> stats_lock(mutex_);
                current_av_diff_ms = stats_.last_av_offset_ms;
            }
            std::lock_guard<std::mutex> render_stat_lock(render_stat_mutex_);
            if (!render_stat_window_.initialized) {
                render_stat_window_.initialized = true;
                render_stat_window_.wall_start_ms = wall_now_ms;
                render_stat_window_.video_pts_start_ms = frame.pts_ms;
                render_stat_window_.audio_clock_start_ms = audio_clock_ms;
                render_stat_window_.diff_sum_ms = current_av_diff_ms;
                render_stat_window_.diff_samples = 1;
            } else {
                render_stat_window_.diff_sum_ms += current_av_diff_ms;
                render_stat_window_.diff_samples++;
                const int64_t wall_elapsed_ms = std::max<int64_t>(0, wall_now_ms - render_stat_window_.wall_start_ms);
                if (wall_elapsed_ms >= 2000) {
                    const int64_t avg_diff_ms = render_stat_window_.diff_samples > 0
                        ? render_stat_window_.diff_sum_ms / static_cast<int64_t>(render_stat_window_.diff_samples)
                        : 0;
                    UpdateStats([&](PlayerStats& stats) {
                        stats.render_stat_wall_time_ms = wall_elapsed_ms;
                        stats.render_stat_video_pts_delta_ms = frame.pts_ms - render_stat_window_.video_pts_start_ms;
                        stats.render_stat_audio_clock_delta_ms =
                            audio_clock_ms - render_stat_window_.audio_clock_start_ms;
                        stats.render_stat_drops = render_stat_window_.drop_count;
                        stats.render_stat_avg_diff_ms = avg_diff_ms;
                    });
                    render_stat_window_.wall_start_ms = wall_now_ms;
                    render_stat_window_.video_pts_start_ms = frame.pts_ms;
                    render_stat_window_.audio_clock_start_ms = audio_clock_ms;
                    render_stat_window_.diff_sum_ms = 0;
                    render_stat_window_.diff_samples = 0;
                    render_stat_window_.drop_count = 0;
                }
            }
        }

        if (!live_mode && audio_stream_index >= 0) {
            while (!stop_requested_) {
                {
                    std::lock_guard<std::mutex> sync_lock(av_sync_mutex_);
                    if (av_sync_state_.audio_clock_started) {
                        break;
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }

        int64_t render_buffer_ms = 0;
        double playback_rate = 1.0;
        int64_t pid_error_ms = 0;
        int64_t wait_before_render_ms = 0;
        int64_t audio_clock_ms = 0;
        int64_t av_diff_ms = 0;
        VideoPtsBackpressureState::Mode pts_mode = VideoPtsBackpressureState::Mode::Normal;
        int64_t drift_ms = 0;
        RenderBackpressureAction action = RenderBackpressureAction::RenderNow;
        int64_t estimated_frame_step_ms = 1;
        bool lagging_realtime = false;
        int64_t effective_target_buffer_ms = 0;
        if (live_mode) {
            if (frame.pts_reanchored) {
                std::lock_guard<std::mutex> rate_lock(video_rate_mutex_);
                video_rate_control_state_.initialized = true;
                video_rate_control_state_.playback_pts_ms = frame.pts_ms;
                video_rate_control_state_.last_frame_pts_ms = frame.pts_ms;
                video_rate_control_state_.last_steady_ms = SteadyNowMs();
                video_rate_control_state_.playback_rate = 1.0;
                video_rate_control_state_.integral_error = 0.0;
                video_rate_control_state_.last_error_ms = 0.0;
            }
            render_buffer_ms = video_frame_queue_ ? video_frame_queue_->BufferedDurationMs() : 0;
            audio_clock_ms = GetAudioMasterClockMs();
            av_diff_ms = audio_clock_ms > 0 ? audio_clock_ms - frame.pts_ms : 0;
            {
                std::lock_guard<std::mutex> pts_lock(video_pts_backpressure_mutex_);
                pts_mode = video_pts_backpressure_state_.mode;
                if (audio_clock_ms > 0) {
                    const int64_t now_ms = SteadyNowMs();
                    if (video_pts_backpressure_state_.short_window_start_steady_ms <= 0) {
                        video_pts_backpressure_state_.short_window_start_steady_ms = now_ms;
                    }
                    if (video_pts_backpressure_state_.long_window_start_steady_ms <= 0) {
                        video_pts_backpressure_state_.long_window_start_steady_ms = now_ms;
                    }
                    video_pts_backpressure_state_.short_window_diff_sum_ms += av_diff_ms;
                    video_pts_backpressure_state_.long_window_diff_sum_ms += av_diff_ms;
                    video_pts_backpressure_state_.short_window_samples++;
                    video_pts_backpressure_state_.long_window_samples++;
                    if (now_ms - video_pts_backpressure_state_.short_window_start_steady_ms >= kVideoPtsShortDriftWindowMs &&
                        video_pts_backpressure_state_.short_window_samples > 0) {
                        const int64_t short_avg_ms =
                            video_pts_backpressure_state_.short_window_diff_sum_ms /
                            static_cast<int64_t>(video_pts_backpressure_state_.short_window_samples);
                        if (std::llabs(short_avg_ms) >= source.options.video_pts_drift_threshold_ms) {
                            video_pts_backpressure_state_.mode =
                                std::max(video_pts_backpressure_state_.mode, VideoPtsBackpressureState::Mode::SoftBackpressure);
                        }
                        video_pts_backpressure_state_.short_window_start_steady_ms = now_ms;
                        video_pts_backpressure_state_.short_window_diff_sum_ms = 0;
                        video_pts_backpressure_state_.short_window_samples = 0;
                    }
                    if (now_ms - video_pts_backpressure_state_.long_window_start_steady_ms >= kVideoPtsLongDriftWindowMs &&
                        video_pts_backpressure_state_.long_window_samples > 0) {
                        const int64_t long_avg_ms =
                            video_pts_backpressure_state_.long_window_diff_sum_ms /
                            static_cast<int64_t>(video_pts_backpressure_state_.long_window_samples);
                        if (std::llabs(long_avg_ms) >= source.options.video_pts_drift_threshold_ms) {
                            const int64_t correction_step_ms = std::clamp(
                                long_avg_ms / 6,
                                -kVideoPtsMaxDriftCorrectionStepMs,
                                kVideoPtsMaxDriftCorrectionStepMs);
                            video_pts_backpressure_state_.drift_offset_ms -= correction_step_ms;
                            drift_ms = video_pts_backpressure_state_.drift_offset_ms;
                            UpdateStats([&](PlayerStats& stats) {
                                stats.video_pts_drift_correction_count++;
                                stats.video_pts_last_drift_ms = drift_ms;
                            });
                        } else {
                            drift_ms = video_pts_backpressure_state_.drift_offset_ms;
                        }
                        video_pts_backpressure_state_.long_window_start_steady_ms = now_ms;
                        video_pts_backpressure_state_.long_window_diff_sum_ms = 0;
                        video_pts_backpressure_state_.long_window_samples = 0;
                    } else {
                        drift_ms = video_pts_backpressure_state_.drift_offset_ms;
                    }
                } else {
                    drift_ms = video_pts_backpressure_state_.drift_offset_ms;
                }
                pts_mode = video_pts_backpressure_state_.mode;
            }
            action = ShouldEnableVideoPtsBackpressure(source)
                ? DecideRenderBackpressureAction(pts_mode, render_buffer_ms, av_diff_ms, drift_ms, source)
                : RenderBackpressureAction::RenderNow;
            if (action == RenderBackpressureAction::FlushToKeyframe && ShouldEnableVideoRecovery(source)) {
                EnterVideoRecovery("render backpressure requested keyframe recovery");
                continue;
            }
            if (action == RenderBackpressureAction::DropLateFrame) {
                UpdateStats([&](PlayerStats& stats) {
                    stats.video_sync_drop_count++;
                });
                std::lock_guard<std::mutex> render_stat_lock(render_stat_mutex_);
                render_stat_window_.drop_count++;
                continue;
            }

            const bool freeze_pid =
                action == RenderBackpressureAction::FreezePid || action == RenderBackpressureAction::ClampPts;
            const int64_t target_buffer_ms = freeze_pid
                ? std::max<int64_t>(kVideoRateControlMinBufferMs, kVideoRateControlTargetBufferMs - 40)
                : kVideoRateControlTargetBufferMs;
            effective_target_buffer_ms = target_buffer_ms;
            if (render_buffer_ms >= kVideoRateControlMaxBufferMs) {
                effective_target_buffer_ms = kVideoRateControlMaxBufferMs;
            } else if (render_buffer_ms <= kVideoRateControlMinBufferMs) {
                effective_target_buffer_ms = kVideoRateControlMinBufferMs;
            }
            pid_error_ms = render_buffer_ms - effective_target_buffer_ms;

            {
                std::lock_guard<std::mutex> rate_lock(video_rate_mutex_);
                VideoRateControlState& rate_state = video_rate_control_state_;
                const int64_t now_ms = SteadyNowMs();
                estimated_frame_step_ms = std::max<int64_t>(1, GetVideoPtsEstimatedFrameDurationMs());
                lagging_realtime =
                    render_buffer_ms > effective_target_buffer_ms || av_diff_ms > estimated_frame_step_ms;
                if (!rate_state.initialized) {
                    rate_state.initialized = true;
                    rate_state.playback_pts_ms = frame.pts_ms;
                    rate_state.last_frame_pts_ms = frame.pts_ms;
                    rate_state.last_steady_ms = now_ms;
                    rate_state.playback_rate = 1.0;
                    rate_state.integral_error = 0.0;
                    rate_state.last_error_ms = static_cast<double>(pid_error_ms);
                } else {
                    const int64_t elapsed_ms = std::max<int64_t>(0, now_ms - rate_state.last_steady_ms);
                    rate_state.playback_pts_ms += static_cast<int64_t>(
                        static_cast<double>(elapsed_ms) * rate_state.playback_rate);
                    rate_state.last_steady_ms = now_ms;

                    const double clamped_error_ms = std::clamp(
                        static_cast<double>(pid_error_ms),
                        -1000.0,
                        1000.0);
                    if (!freeze_pid) {
                        rate_state.integral_error = std::clamp(
                            rate_state.integral_error + clamped_error_ms,
                            -5000.0,
                            5000.0);
                    }
                    const double derivative_error_ms = clamped_error_ms - rate_state.last_error_ms;
                    if (freeze_pid) {
                        rate_state.playback_rate = std::clamp(1.0, 0.98, 1.02);
                    } else {
                        const double next_rate =
                            1.0 + kVideoRateControlKp * clamped_error_ms +
                            kVideoRateControlKi * rate_state.integral_error +
                            kVideoRateControlKd * derivative_error_ms;
                        rate_state.playback_rate = ClampPlaybackRate(next_rate);
                        if (lagging_realtime) {
                            rate_state.playback_rate = std::max(rate_state.playback_rate, 1.20);
                        }
                    }
                    rate_state.last_error_ms = clamped_error_ms;
                }

                if (frame.pts_ms > rate_state.playback_pts_ms) {
                    const int64_t gap_ms = frame.pts_ms - rate_state.playback_pts_ms;
                    wait_before_render_ms = std::max<int64_t>(
                        0,
                        static_cast<int64_t>(std::llround(
                            static_cast<double>(gap_ms) / std::max(0.0001, rate_state.playback_rate))));
                } else {
                    wait_before_render_ms = 0;
                    if (frame.pts_ms > rate_state.last_frame_pts_ms) {
                        rate_state.playback_pts_ms = frame.pts_ms;
                    }
                }
                rate_state.last_frame_pts_ms = frame.pts_ms;
                playback_rate = rate_state.playback_rate;
            }
            if (action == RenderBackpressureAction::ClampPts) {
                wait_before_render_ms = std::min<int64_t>(wait_before_render_ms, estimated_frame_step_ms);
            } else if (action == RenderBackpressureAction::FreezePid) {
                wait_before_render_ms = std::min<int64_t>(wait_before_render_ms, kLiveRenderMaxSleepMs);
            } else if (action == RenderBackpressureAction::SleepShort ||
                       action == RenderBackpressureAction::SleepLong ||
                       action == RenderBackpressureAction::RenderNow) {
                wait_before_render_ms = std::min<int64_t>(wait_before_render_ms, kLiveRenderMaxSleepMs);
            }
            if (frame.pts_reanchored || lagging_realtime || render_buffer_ms >= effective_target_buffer_ms) {
                wait_before_render_ms = 0;
            }
            if (wait_before_render_ms > 1) {
                std::this_thread::sleep_for(std::chrono::milliseconds(wait_before_render_ms));
            }
        }

        UpdateStats([&](PlayerStats& stats) {
            stats.video_render_buffer_ms = render_buffer_ms;
            stats.video_render_playback_rate = playback_rate;
            stats.video_render_pid_error_ms = pid_error_ms;
        });

        const int64_t render_begin_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
                                            .count();
        if (callbacks_.on_video_frame) {
            callbacks_.on_video_frame(frame);
        }
        const int64_t render_end_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
                                          .count();
        video_render_total_cost_us += (render_end_us - render_begin_us);
        video_render_cost_samples++;
        const int64_t render_wall_ms = SteadyNowMs();
        const int64_t render_queue_residence_ms =
            frame.enqueue_steady_ms > 0 ? std::max<int64_t>(0, render_wall_ms - frame.enqueue_steady_ms) : -1;
        int64_t render_wall_delta_ms = -1;
        int64_t render_pts_delta_ms = -1;
        uint64_t rendered_frames_since_log = 0;
        {
            std::lock_guard<std::mutex> diag_lock(video_diag_mutex_);
            if (video_timing_diag_.last_render_wall_ms >= 0) {
                render_wall_delta_ms = render_wall_ms - video_timing_diag_.last_render_wall_ms;
            }
            if (video_timing_diag_.last_render_pts_ms >= 0) {
                render_pts_delta_ms = frame.pts_ms - video_timing_diag_.last_render_pts_ms;
            }
            video_timing_diag_.last_render_wall_ms = render_wall_ms;
            video_timing_diag_.last_render_pts_ms = frame.pts_ms;
            video_timing_diag_.rendered_frames_since_log++;
            rendered_frames_since_log = video_timing_diag_.rendered_frames_since_log;
        }

        UpdateStats([&](PlayerStats& stats) {
            if (stats.first_video_render_cost_ms < 0) {
                stats.first_video_render_cost_ms = NowMs() - stats.start_time_ms;
            }
            stats.video_rendered_frame_count++;
            stats.video_render_avg_cost_us = video_render_cost_samples > 0
                ? static_cast<int64_t>(video_render_total_cost_us / static_cast<int64_t>(video_render_cost_samples))
                : 0;
        });
        const char* action_name = "RenderNow";
        switch (action) {
        case RenderBackpressureAction::RenderNow:
            action_name = "RenderNow";
            break;
        case RenderBackpressureAction::SleepShort:
            action_name = "SleepShort";
            break;
        case RenderBackpressureAction::SleepLong:
            action_name = "SleepLong";
            break;
        case RenderBackpressureAction::ClampPts:
            action_name = "ClampPts";
            break;
        case RenderBackpressureAction::DropLateFrame:
            action_name = "DropLateFrame";
            break;
        case RenderBackpressureAction::FreezePid:
            action_name = "FreezePid";
            break;
        case RenderBackpressureAction::FlushToKeyframe:
            action_name = "FlushToKeyframe";
            break;
        }
        EDGELIVE_LOG_INFO(
            config_.log_tag,
            "VIDEO-RENDER-PTS raw_pts_ms=" + std::to_string(frame.raw_pts_ms) +
                " effective_pts_ms=" + std::to_string(frame.pts_ms) +
                " pts_adjust_ms=" + std::to_string(frame.pts_ms - frame.raw_pts_ms) +
                " audio_clock_ms=" + std::to_string(audio_clock_ms) +
                " av_diff_ms=" + std::to_string(av_diff_ms) +
                " render_buf_ms=" + std::to_string(render_buffer_ms) +
                " wait_before_render_ms=" + std::to_string(wait_before_render_ms) +
                " queue_residence_ms=" + std::to_string(render_queue_residence_ms) +
                " render_wall_delta_ms=" + std::to_string(render_wall_delta_ms) +
                " render_pts_delta_ms=" + std::to_string(render_pts_delta_ms) +
                " render_cost_us=" + std::to_string(render_end_us - render_begin_us) +
                " pts_mode=" + std::string(VideoPtsBackpressureModeToString(static_cast<int>(pts_mode))) +
                " action=" + std::string(action_name) +
                " drift_ms=" + std::to_string(drift_ms) +
                " pid_error_ms=" + std::to_string(pid_error_ms) +
                " playback_rate=" + std::to_string(playback_rate) +
                " render_q=" + std::to_string(video_frame_queue_ ? video_frame_queue_->Size() : 0) +
                " video_q=" + std::to_string(video_packet_queue_ ? video_packet_queue_->Size() : 0) +
                " rendered_frames_since_log=" + std::to_string(rendered_frames_since_log));
        {
            std::lock_guard<std::mutex> diag_lock(video_diag_mutex_);
            video_timing_diag_.rendered_frames_since_log = 0;
        }
        video_render_fps_window_frames++;
        const int64_t render_now_ms = SteadyNowMs();
        if (render_now_ms - video_render_fps_window_start_ms >= 1000) {
            const int64_t window_ms = std::max<int64_t>(1, render_now_ms - video_render_fps_window_start_ms);
            const int fps = static_cast<int>(video_render_fps_window_frames * 1000 / window_ms);
            UpdateStats([&](PlayerStats& stats) { stats.video_render_fps = fps; });
            video_render_fps_window_start_ms = render_now_ms;
            video_render_fps_window_frames = 0;
        }
        first_video_rendered_ = true;
        startup_cv_.notify_all();
    }

    if (!stop_requested_ && demux_eof_) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == PlayerState::Playing) {
            stats_.stop_count++;
            ChangeState(PlayerState::Stopped);
            EDGELIVE_LOG_INFO(config_.log_tag, "Playback completed (EOF)");
        }
    }
}

void PlayerEngine::AudioDecodeLoop() {
    const MediaSource source = NormalizeMediaSource(active_request_);
    if (ResolvePlaybackMode(source) == PlaybackMode::File) {
        AudioDecodeFileLoop(source);
        return;
    }
    AudioDecodeLiveLoop(source);
}

void PlayerEngine::AudioDecodeFileLoop(const MediaSource& source) {
    AudioDecodeLoopImpl(source, false);
}

void PlayerEngine::AudioDecodeLiveLoop(const MediaSource& source) {
    AudioDecodeLoopImpl(source, true);
}

void PlayerEngine::AudioDecodeLoopImpl(const MediaSource& source, bool live_mode) {
    const bool hold_audio_for_startup = live_mode && ShouldHoldAudioForFastStartup();
    uint64_t audio_send_fail_count = 0;
    uint64_t audio_send_count = 0;
    uint64_t audio_decode_fail_count = 0;
    uint64_t audio_param_skip_count = 0;
    uint64_t audio_invalid_sample_rate_skip_count = 0;
    uint64_t audio_swr_convert_fail_count = 0;
    int64_t audio_decode_total_cost_us = 0;
    uint64_t audio_decode_cost_samples = 0;
    int64_t audio_wait_total_us = 0;
    uint64_t audio_wait_samples = 0;
    int64_t audio_output_fps_window_start_ms = SteadyNowMs();
    uint64_t audio_output_fps_window_frames = 0;
    int64_t local_audio_start_steady_ms = 0;
    int64_t local_audio_submitted_duration_ms = 0;
    int64_t audio_pts_origin_ms = 0;
    int audio_pts_sample_rate = 0;
    int64_t audio_output_sample_count = 0;
    bool audio_pts_origin_valid = false;
    bool audio_flushed_on_eof = false;
    uint64_t audio_missing_ts_frame_count = 0;
    bool audio_missing_ts_frame_reported = false;
    constexpr int64_t kAudioPtsDriftLogThresholdMs = 200;
    constexpr uint64_t kAudioMissingTimestampReportThreshold = 20;
    const auto pace_local_audio = [&](int converted_samples, int sample_rate) {
        if (converted_samples <= 0 || sample_rate <= 0) {
            return;
        }
        const int64_t duration_ms = av_rescale_q(
            converted_samples,
            AVRational{1, sample_rate},
            AVRational{1, 1000});
        if (local_audio_start_steady_ms <= 0) {
            local_audio_start_steady_ms = SteadyNowMs();
        }
        const int64_t target_ms = local_audio_start_steady_ms + local_audio_submitted_duration_ms;
        if (!live_mode) {
            const int64_t now_ms = SteadyNowMs();
            if (target_ms > now_ms) {
                std::this_thread::sleep_for(std::chrono::milliseconds(target_ms - now_ms));
            }
        } else {
            const int64_t lead_window_ms =
                std::min<int64_t>(kLiveAudioLeadWindowMs, std::max<int64_t>(120, GetCurrentAudioJitterTargetMs(source)));
            while (!stop_requested_) {
                const int64_t now_ms = SteadyNowMs();
                const int64_t elapsed_ms = std::max<int64_t>(0, now_ms - local_audio_start_steady_ms);
                const int64_t pending_audio_ms =
                    std::max<int64_t>(0, local_audio_submitted_duration_ms - elapsed_ms);
                const int64_t projected_pending_ms = pending_audio_ms + duration_ms;
                if (projected_pending_ms <= lead_window_ms) {
                    break;
                }
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(std::min<int64_t>(5, projected_pending_ms - lead_window_ms)));
            }
        }
        local_audio_submitted_duration_ms += duration_ms;
        {
            std::lock_guard<std::mutex> sync_lock(av_sync_mutex_);
            av_sync_state_.submitted_audio_duration_ms = local_audio_submitted_duration_ms;
        }
    };
    const auto compute_audio_output_pts_ms = [&](int64_t decoder_pts_ms, int sample_rate) -> int64_t {
        std::string rebase_reason;
        if (!audio_pts_origin_valid) {
            rebase_reason = "initial_origin";
        } else if (audio_pts_sample_rate != sample_rate) {
            rebase_reason = "sample_rate_changed";
        }
        if (!audio_pts_origin_valid || audio_pts_sample_rate != sample_rate) {
            audio_pts_origin_ms = std::max<int64_t>(0, decoder_pts_ms);
            audio_pts_sample_rate = sample_rate;
            audio_output_sample_count = 0;
            audio_pts_origin_valid = true;
            UpdateStats([&](PlayerStats& stats) {
                stats.audio_pts_rebase_count++;
                stats.audio_pts_rebase_last_reason = rebase_reason;
            });
        }
        const int64_t computed_pts_ms = audio_pts_origin_ms +
            av_rescale_q(audio_output_sample_count, AVRational{1, sample_rate}, AVRational{1, 1000});
        const int64_t drift_ms = std::llabs(computed_pts_ms - std::max<int64_t>(0, decoder_pts_ms));
        if (drift_ms >= kAudioPtsDriftLogThresholdMs) {
            UpdateStats([&](PlayerStats& stats) {
                stats.audio_pts_rebase_last_reason = "drift_detected";
            });
            const int64_t now_ms = SteadyNowMs();
            if (ShouldEmitPeriodicLog(last_audio_drift_log_ms_, 1000, now_ms)) {
                int64_t submitted_audio_duration_ms = 0;
                int64_t anchor_audio_pts_ms = 0;
                int64_t anchor_monotonic_ms = 0;
                int64_t pending_audio_ms = 0;
                {
                    std::lock_guard<std::mutex> sync_lock(av_sync_mutex_);
                    submitted_audio_duration_ms = av_sync_state_.submitted_audio_duration_ms;
                    anchor_audio_pts_ms = av_sync_state_.anchor_audio_pts_ms;
                    anchor_monotonic_ms = av_sync_state_.anchor_monotonic_ms;
                    if (anchor_monotonic_ms > 0) {
                        const int64_t elapsed_ms =
                            std::max<int64_t>(0, now_ms - anchor_monotonic_ms);
                        pending_audio_ms =
                            std::max<int64_t>(0, submitted_audio_duration_ms - elapsed_ms);
                    }
                }
                EDGELIVE_LOG_WARN(
                    config_.log_tag,
                    "AUDIO-DRIFT decoder_pts=" + std::to_string(decoder_pts_ms) +
                        " computed_pts=" + std::to_string(computed_pts_ms) +
                        " drift_ms=" + std::to_string(drift_ms) +
                        " origin_pts=" + std::to_string(audio_pts_origin_ms) +
                        " sample_rate=" + std::to_string(sample_rate) +
                        " output_samples=" + std::to_string(audio_output_sample_count) +
                        " submitted_dur=" + std::to_string(submitted_audio_duration_ms) +
                        " anchor_pts=" + std::to_string(anchor_audio_pts_ms) +
                        " anchor_monotonic=" + std::to_string(anchor_monotonic_ms) +
                        " pending_audio=" + std::to_string(pending_audio_ms));
            }
        }
        return computed_pts_ms;
    };
    const auto submit_audio_frame = [&](const uint8_t* data,
                                        size_t data_size,
                                        int sample_rate,
                                        int channels,
                                        int bytes_per_sample,
                                        int converted_samples,
                                        int64_t decoder_pts_ms,
                                        int64_t fallback_pts_ms,
                                        bool flush_path) {
        if (data == nullptr || data_size == 0 || converted_samples <= 0 || sample_rate <= 0) {
            return;
        }

        const int64_t resolved_decoder_pts_ms = decoder_pts_ms > 0 ? decoder_pts_ms : std::max<int64_t>(0, fallback_pts_ms);

        AudioFrame out;
        out.sample_rate = sample_rate;
        out.channels = channels;
        out.bytes_per_sample = bytes_per_sample;
        out.pts_ms = compute_audio_output_pts_ms(resolved_decoder_pts_ms, sample_rate);
        out.data_size = data_size;
        out.data = data;
        {
            std::lock_guard<std::mutex> sync_lock(av_sync_mutex_);
            if (av_sync_state_.first_audio_pts_ms == std::numeric_limits<int64_t>::min()) {
                av_sync_state_.first_audio_pts_ms = out.pts_ms;
                if (av_sync_state_.first_video_pts_ms != std::numeric_limits<int64_t>::min()) {
                    av_sync_state_.av_offset_ms =
                        av_sync_state_.first_audio_pts_ms - av_sync_state_.first_video_pts_ms;
                    av_sync_state_.av_offset_ready = true;
                    EDGELIVE_LOG_INFO(
                        config_.log_tag,
                        std::string(flush_path ? "AV sync anchor ready(flush): " : "AV sync anchor ready: ") +
                            "first_audio_pts_ms=" + std::to_string(av_sync_state_.first_audio_pts_ms) +
                            " first_video_pts_ms=" + std::to_string(av_sync_state_.first_video_pts_ms) +
                            " av_offset_ms=" + std::to_string(av_sync_state_.av_offset_ms));
                }
            }
            if (!av_sync_state_.audio_clock_started) {
                av_sync_state_.audio_clock_started = true;
                av_sync_state_.anchor_audio_pts_ms = out.pts_ms;
                av_sync_state_.anchor_monotonic_ms = SteadyNowMs();
            }
            av_sync_state_.last_audio_pts_ms = out.pts_ms;
        }

        pace_local_audio(converted_samples, sample_rate);
        if (callbacks_.on_audio_frame) {
            callbacks_.on_audio_frame(out);
        }
        audio_output_sample_count += converted_samples;

        UpdateStats([&](PlayerStats& stats) {
            if (stats.first_audio_play_cost_ms < 0) {
                stats.first_audio_play_cost_ms = NowMs() - stats.start_time_ms;
            }
            if (!flush_path && first_video_rendered_.load()) {
                stats.audio_started_after_first_video = true;
            }
            if (!flush_path) {
                stats.audio_output_frame_count++;
            }
            stats.audio_latest_submit_pts_ms = out.pts_ms;
            stats.audio_decoder_pts_ms = resolved_decoder_pts_ms;
            stats.audio_computed_output_pts_ms = out.pts_ms;
            stats.audio_decode_to_submit_offset_ms = out.pts_ms - resolved_decoder_pts_ms;
            if (stats.sync_video_pts_ms > 0) {
                stats.audio_submit_to_video_gap_ms = stats.sync_video_pts_ms - out.pts_ms;
            }
        });
        if (!flush_path) {
            audio_output_fps_window_frames++;
        }
    };
    while (!stop_requested_) {
        AVCodecContext* codec_ctx = nullptr;
        AVFormatContext* fmt_ctx = nullptr;
        SwrContext* swr_ctx = nullptr;
        int stream_index = -1;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            codec_ctx = audio_codec_ctx_;
            fmt_ctx = fmt_ctx_;
            swr_ctx = swr_ctx_;
            stream_index = audio_stream_index_;
        }

        if (codec_ctx == nullptr || fmt_ctx == nullptr || stream_index < 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        AVPacket packet{};
        const int jitter_target_ms = live_mode ? GetCurrentAudioJitterTargetMs(source) : 0;
        const int64_t audio_wait_begin_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
                                                .count();
        if (!audio_packet_queue_ ||
            !audio_packet_queue_->PopWithMinDuration(packet, jitter_target_ms, !live_mode)) {
            av_packet_unref(&packet);
            if (stop_requested_) {
                break;
            }
            if (demux_eof_) {
                if (audio_flushed_on_eof) {
                    break;
                }
                audio_flushed_on_eof = true;
                avcodec_send_packet(codec_ctx, nullptr);
                AVFrame* flush_frame = av_frame_alloc();
                if (flush_frame == nullptr) {
                    EmitError(-16, "Could not allocate audio frame");
                    return;
                }
                while (!stop_requested_) {
                    const int flush_ret = avcodec_receive_frame(codec_ctx, flush_frame);
                    if (flush_ret == AVERROR(EAGAIN) || flush_ret == AVERROR_EOF) {
                        break;
                    }
                    if (flush_ret < 0) {
                        ++audio_decode_fail_count;
                        UpdateStats([&](PlayerStats& stats) {
                            stats.audio_receive_frame_fail_count = audio_decode_fail_count;
                        });
                        break;
                    }

                    UpdateStats([&](PlayerStats& stats) {
                        stats.audio_decoded_frame_count++;
                        if (stats.first_audio_decode_cost_ms < 0) {
                            stats.first_audio_decode_cost_ms = NowMs() - stats.start_time_ms;
                        }
                        stats.audio_latest_decode_pts_ms = PtsToMs(
                            flush_frame->best_effort_timestamp,
                            ResolveAudioTimeBase(codec_ctx, fmt_ctx->streams[stream_index]));
                    });

                        if (hold_audio_for_startup && !first_video_rendered_.load()) {
                            std::unique_lock<std::mutex> startup_lock(startup_mutex_);
                            startup_cv_.wait_for(
                                startup_lock,
                            std::chrono::milliseconds(kFastStartupAudioWaitMs),
                            [this]() { return stop_requested_.load() || first_video_rendered_.load(); });
                        if (stop_requested_.load()) {
                            av_frame_unref(flush_frame);
                            break;
                        }
                        if (!first_video_rendered_.load()) {
                            av_frame_unref(flush_frame);
                            continue;
                        }
                    }

                    if (swr_ctx == nullptr) {
                        int in_rate = ResolveAudioSampleRate(flush_frame, codec_ctx, fmt_ctx, stream_index);
                        AVChannelLayout in_layout{};
                        bool in_layout_initialized = false;
                        if (flush_frame->ch_layout.nb_channels > 0) {
                            if (av_channel_layout_copy(&in_layout, &flush_frame->ch_layout) >= 0) {
                                in_layout_initialized = true;
                            }
                        }
                        if (!in_layout_initialized && codec_ctx->ch_layout.nb_channels > 0) {
                            if (av_channel_layout_copy(&in_layout, &codec_ctx->ch_layout) >= 0) {
                                in_layout_initialized = true;
                            }
                        }
                        if (!in_layout_initialized) {
                            int fallback_channels = 0;
#if LIBAVCODEC_VERSION_MAJOR < 59
                            fallback_channels = flush_frame->channels > 0
                                ? flush_frame->channels
                                : (codec_ctx->channels > 0 ? codec_ctx->channels : 0);
#else
                            fallback_channels = flush_frame->ch_layout.nb_channels > 0
                                ? flush_frame->ch_layout.nb_channels
                                : codec_ctx->ch_layout.nb_channels;
#endif
                            if (fallback_channels > 0) {
                                av_channel_layout_default(&in_layout, fallback_channels);
                                in_layout_initialized = true;
                            }
                        }
                        const AVSampleFormat in_fmt = flush_frame->format != AV_SAMPLE_FMT_NONE
                            ? static_cast<AVSampleFormat>(flush_frame->format)
                            : codec_ctx->sample_fmt;
                        if (!in_layout_initialized || in_layout.nb_channels <= 0 || in_fmt == AV_SAMPLE_FMT_NONE) {
                            av_channel_layout_uninit(&in_layout);
                            av_frame_unref(flush_frame);
                            continue;
                        }

                        AVChannelLayout out_ch_layout{};
                        av_channel_layout_default(&out_ch_layout, in_layout.nb_channels);
                        const int swr_ret = swr_alloc_set_opts2(
                            &swr_ctx,
                            &out_ch_layout,
                            kOutputSampleFormat,
                            in_rate,
                            &in_layout,
                            in_fmt,
                            in_rate,
                            0,
                            nullptr);
                        av_channel_layout_uninit(&in_layout);
                        av_channel_layout_uninit(&out_ch_layout);
                        if (swr_ret < 0 || swr_ctx == nullptr || swr_init(swr_ctx) < 0) {
                            if (swr_ctx != nullptr) {
                                swr_free(&swr_ctx);
                            }
                            EmitError(-13, "Could not create audio resampler");
                            av_frame_unref(flush_frame);
                            av_frame_free(&flush_frame);
                            return;
                        }

                        std::lock_guard<std::mutex> lock(mutex_);
                        swr_ctx_ = swr_ctx;
                    }

                    const int out_channels = flush_frame->ch_layout.nb_channels > 0
                        ? flush_frame->ch_layout.nb_channels
                        : (codec_ctx->ch_layout.nb_channels > 0 ? codec_ctx->ch_layout.nb_channels : 2);
                    const int out_bytes_per_sample = av_get_bytes_per_sample(kOutputSampleFormat);
                    const int sample_rate = ResolveAudioSampleRate(flush_frame, codec_ctx, fmt_ctx, stream_index);
                    const int out_samples = av_rescale_rnd(
                        swr_get_delay(swr_ctx, sample_rate) + flush_frame->nb_samples,
                        sample_rate,
                        sample_rate,
                        AV_ROUND_UP);
                    std::vector<uint8_t> audio_buffer(static_cast<size_t>(out_samples * out_channels * out_bytes_per_sample));
                    uint8_t* output_data[] = {audio_buffer.data(), nullptr};
                    const int converted_samples = swr_convert(
                        swr_ctx,
                        output_data,
                        out_samples,
                        const_cast<const uint8_t**>(flush_frame->extended_data),
                        flush_frame->nb_samples);
                    if (converted_samples > 0) {
                        const int64_t decoder_pts_ms = ResolveAudioFramePtsMs(
                            flush_frame,
                            fmt_ctx->streams[stream_index]);
                        if (HasMeaningfulAudioFrameTimestamp(flush_frame, fmt_ctx->streams[stream_index])) {
                            audio_missing_ts_frame_count = 0;
                        } else if (HasOnlyZeroOrMissingAudioFrameTimestamp(flush_frame)) {
                            ++audio_missing_ts_frame_count;
                            if (!audio_missing_ts_frame_reported &&
                                audio_missing_ts_frame_count >= kAudioMissingTimestampReportThreshold) {
                                audio_missing_ts_frame_reported = true;
                                EDGELIVE_LOG_WARN(
                                    config_.log_tag,
                                    "Decoded audio frames have no usable timestamp for " +
                                        std::to_string(audio_missing_ts_frame_count) +
                                        " consecutive frames. Publisher may be missing audio timestamps: best_effort=" +
                                        std::to_string(flush_frame->best_effort_timestamp) +
                                        " pts=" + std::to_string(flush_frame->pts) +
                                        " pkt_dts=" + std::to_string(flush_frame->pkt_dts));
                                UpdateStats([&](PlayerStats& stats) {
                                    stats.audio_pts_rebase_last_reason = "audio_frame_timestamp_missing";
                                });
                            }
                        } else {
                            audio_missing_ts_frame_count = 0;
                        }
                        const int64_t fallback_pts_ms =
                            stats_.audio_latest_submit_pts_ms +
                            DurationMsForAudioSamples(static_cast<int64_t>(converted_samples), sample_rate);
                        submit_audio_frame(
                            audio_buffer.data(),
                            static_cast<size_t>(converted_samples * out_channels * out_bytes_per_sample),
                            sample_rate,
                            out_channels,
                            out_bytes_per_sample,
                            converted_samples,
                            decoder_pts_ms,
                            fallback_pts_ms,
                            true);
                    }
                    av_frame_unref(flush_frame);
                }
                av_frame_free(&flush_frame);
                continue;
            }
            continue;
        }
        const int64_t audio_wait_end_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
                                              .count();
        audio_wait_total_us += (audio_wait_end_us - audio_wait_begin_us);
        audio_wait_samples++;
        UpdateStats([&](PlayerStats& stats) {
            stats.audio_packet_wait_avg_us = audio_wait_samples > 0
                ? static_cast<int64_t>(audio_wait_total_us / static_cast<int64_t>(audio_wait_samples))
                : 0;
        });

        UpdateStats([&](PlayerStats& stats) {
            stats.audio_pop_packet_count++;
            stats.audio_queue_size = audio_packet_queue_ ? audio_packet_queue_->Size() : 0;
            stats.audio_queue_duration_ms =
                audio_packet_queue_ ? audio_packet_queue_->BufferedDurationMs() : 0;
            stats.audio_latest_queue_head_pts_ms =
                audio_packet_queue_ ? audio_packet_queue_->FrontPtsMs() : 0;
        });

        const int send_ret = avcodec_send_packet(codec_ctx, &packet);
        if (send_ret < 0) {
            ++audio_send_fail_count;
            UpdateStats([&](PlayerStats& stats) {
                stats.audio_send_packet_fail_count = audio_send_fail_count;
            });
            if (audio_send_fail_count == 1 || (audio_send_fail_count % 100) == 0) {
                EDGELIVE_LOG_WARN(
                    config_.log_tag,
                    "Audio send packet failed(count=" + std::to_string(audio_send_fail_count) +
                        "): " + FfmpegErrorToString(send_ret));
            }
            av_packet_unref(&packet);
            continue;
        }
        ++audio_send_count;
        UpdateStats([&](PlayerStats& stats) {
            stats.audio_send_packet_count = audio_send_count;
        });
        av_packet_unref(&packet);

        AVFrame* frame = av_frame_alloc();
        if (frame == nullptr) {
            EmitError(-16, "Could not allocate audio frame");
            return;
        }

        while (!stop_requested_) {
            const int64_t audio_decode_begin_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                                                      .count();
            const int recv_ret = avcodec_receive_frame(codec_ctx, frame);
            if (recv_ret == AVERROR(EAGAIN) || recv_ret == AVERROR_EOF) {
                break;
            }
            if (recv_ret < 0) {
                ++audio_decode_fail_count;
                UpdateStats([&](PlayerStats& stats) {
                    stats.audio_receive_frame_fail_count = audio_decode_fail_count;
                });
                if (audio_decode_fail_count == 1 || (audio_decode_fail_count % 100) == 0) {
                    EDGELIVE_LOG_WARN(
                        config_.log_tag,
                        "Audio receive frame failed(count=" + std::to_string(audio_decode_fail_count) +
                            "): " + FfmpegErrorToString(recv_ret));
                }
                break;
            }
            const int64_t audio_decode_end_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                                                    .count();
            audio_decode_total_cost_us += (audio_decode_end_us - audio_decode_begin_us);
            audio_decode_cost_samples++;
            if (hold_audio_for_startup && !first_video_rendered_.load()) {
                std::unique_lock<std::mutex> startup_lock(startup_mutex_);
                startup_cv_.wait_for(
                    startup_lock,
                    std::chrono::milliseconds(kFastStartupAudioWaitMs),
                    [this]() { return stop_requested_.load() || first_video_rendered_.load(); });
                if (stop_requested_.load()) {
                    av_frame_unref(frame);
                    break;
                }
                if (!first_video_rendered_.load()) {
                    av_frame_unref(frame);
                    continue;
                }
            }

            if (swr_ctx == nullptr) {
                int in_rate = ResolveAudioSampleRate(frame, codec_ctx, fmt_ctx, stream_index);
                AVChannelLayout in_layout{};
                bool in_layout_initialized = false;
                if (frame->ch_layout.nb_channels > 0) {
                    if (av_channel_layout_copy(&in_layout, &frame->ch_layout) >= 0) {
                        in_layout_initialized = true;
                    }
                }
                if (!in_layout_initialized && codec_ctx->ch_layout.nb_channels > 0) {
                    if (av_channel_layout_copy(&in_layout, &codec_ctx->ch_layout) >= 0) {
                        in_layout_initialized = true;
                    }
                }
                if (!in_layout_initialized) {
                    int fallback_channels = 0;
#if LIBAVCODEC_VERSION_MAJOR < 59
                    fallback_channels = frame->channels > 0
                        ? frame->channels
                        : (codec_ctx->channels > 0 ? codec_ctx->channels : 0);
#else
                    fallback_channels = frame->ch_layout.nb_channels > 0
                        ? frame->ch_layout.nb_channels
                        : codec_ctx->ch_layout.nb_channels;
#endif
                    if (fallback_channels > 0) {
                        av_channel_layout_default(&in_layout, fallback_channels);
                        in_layout_initialized = true;
                    }
                }
                if (in_layout_initialized && av_channel_layout_check(&in_layout) != 1) {
                    const int fallback_channels = in_layout.nb_channels;
                    av_channel_layout_uninit(&in_layout);
                    in_layout_initialized = false;
                    if (fallback_channels > 0) {
                        av_channel_layout_default(&in_layout, fallback_channels);
                        in_layout_initialized = true;
                    }
                }
                const AVSampleFormat in_fmt =
                    frame->format != AV_SAMPLE_FMT_NONE ? static_cast<AVSampleFormat>(frame->format) : codec_ctx->sample_fmt;
                if (!in_layout_initialized || in_layout.nb_channels <= 0 || in_fmt == AV_SAMPLE_FMT_NONE) {
                    ++audio_param_skip_count;
                    UpdateStats([&](PlayerStats& stats) {
                        stats.audio_invalid_param_skip_count = audio_param_skip_count;
                    });
                    if (audio_param_skip_count == 1 || (audio_param_skip_count % 100) == 0) {
                        EDGELIVE_LOG_WARN(
                            config_.log_tag,
                            "Audio frame skipped due to invalid params(count=" +
                                std::to_string(audio_param_skip_count) +
                                "): sample_rate=" + std::to_string(in_rate) +
                                " channels=" + std::to_string(in_layout.nb_channels) +
                                " sample_fmt=" + std::to_string(static_cast<int>(in_fmt)));
                    }
                    av_channel_layout_uninit(&in_layout);
                    av_frame_unref(frame);
                    continue;
                }

                AVChannelLayout out_ch_layout{};
                av_channel_layout_default(&out_ch_layout, in_layout.nb_channels);
                const int swr_ret = swr_alloc_set_opts2(
                    &swr_ctx,
                    &out_ch_layout,
                    kOutputSampleFormat,
                    in_rate,
                    &in_layout,
                    in_fmt,
                    in_rate,
                    0,
                    nullptr);
                av_channel_layout_uninit(&in_layout);
                av_channel_layout_uninit(&out_ch_layout);
                if (swr_ret < 0 || swr_ctx == nullptr || swr_init(swr_ctx) < 0) {
                    if (swr_ctx != nullptr) {
                        swr_free(&swr_ctx);
                    }
                    EmitError(-13, "Could not create audio resampler");
                    av_frame_unref(frame);
                    av_frame_free(&frame);
                    return;
                }

                std::lock_guard<std::mutex> lock(mutex_);
                swr_ctx_ = swr_ctx;
            }

            UpdateStats([&](PlayerStats& stats) {
                stats.audio_decoded_frame_count++;
                if (stats.first_audio_decode_cost_ms < 0) {
                    stats.first_audio_decode_cost_ms = NowMs() - stats.start_time_ms;
                }
                stats.audio_queue_size = audio_packet_queue_ ? audio_packet_queue_->Size() : 0;
                stats.audio_queue_duration_ms =
                    audio_packet_queue_ ? audio_packet_queue_->BufferedDurationMs() : 0;
                stats.audio_latest_decode_pts_ms =
                    PtsToMs(frame->best_effort_timestamp, ResolveAudioTimeBase(codec_ctx, fmt_ctx->streams[stream_index]));
                stats.audio_decode_avg_cost_us = audio_decode_cost_samples > 0
                    ? static_cast<int64_t>(audio_decode_total_cost_us / static_cast<int64_t>(audio_decode_cost_samples))
                    : 0;
            });

            const int out_channels = frame->ch_layout.nb_channels > 0
                ? frame->ch_layout.nb_channels
                : (codec_ctx->ch_layout.nb_channels > 0 ? codec_ctx->ch_layout.nb_channels : 2);
            const int out_bytes_per_sample = av_get_bytes_per_sample(kOutputSampleFormat);
            int sample_rate = ResolveAudioSampleRate(frame, codec_ctx, fmt_ctx, stream_index);
            if (!IsValidAudioSampleRate(sample_rate)) {
                ++audio_invalid_sample_rate_skip_count;
                UpdateStats([&](PlayerStats& stats) {
                    stats.audio_invalid_sample_rate_skip_count = audio_invalid_sample_rate_skip_count;
                });
                av_frame_unref(frame);
                continue;
            }

            const int out_samples = av_rescale_rnd(
                swr_get_delay(swr_ctx, sample_rate) + frame->nb_samples,
                sample_rate,
                sample_rate,
                AV_ROUND_UP);

            std::vector<uint8_t> audio_buffer(static_cast<size_t>(out_samples * out_channels * out_bytes_per_sample));
            uint8_t* output_data[] = {audio_buffer.data(), nullptr};
            const int converted_samples = swr_convert(
                swr_ctx,
                output_data,
                out_samples,
                const_cast<const uint8_t**>(frame->extended_data),
                frame->nb_samples);
            if (converted_samples <= 0) {
                ++audio_swr_convert_fail_count;
                UpdateStats([&](PlayerStats& stats) {
                    stats.audio_swr_convert_fail_count = audio_swr_convert_fail_count;
                    stats.audio_last_swr_convert_result = converted_samples;
                });
                av_frame_unref(frame);
                continue;
            }
            UpdateStats([&](PlayerStats& stats) {
                stats.audio_last_swr_convert_result = converted_samples;
            });

            const int64_t decoder_pts_ms = ResolveAudioFramePtsMs(frame, fmt_ctx->streams[stream_index]);
            if (HasMeaningfulAudioFrameTimestamp(frame, fmt_ctx->streams[stream_index])) {
                audio_missing_ts_frame_count = 0;
            } else if (HasOnlyZeroOrMissingAudioFrameTimestamp(frame)) {
                ++audio_missing_ts_frame_count;
                if (!audio_missing_ts_frame_reported &&
                    audio_missing_ts_frame_count >= kAudioMissingTimestampReportThreshold) {
                    audio_missing_ts_frame_reported = true;
                    EDGELIVE_LOG_WARN(
                        config_.log_tag,
                        "Decoded audio frames have no usable timestamp for " +
                            std::to_string(audio_missing_ts_frame_count) +
                            " consecutive frames. Publisher may be missing audio timestamps: best_effort=" +
                            std::to_string(frame->best_effort_timestamp) +
                            " pts=" + std::to_string(frame->pts) +
                            " pkt_dts=" + std::to_string(frame->pkt_dts));
                    UpdateStats([&](PlayerStats& stats) {
                        stats.audio_pts_rebase_last_reason = "audio_frame_timestamp_missing";
                    });
                }
            } else {
                audio_missing_ts_frame_count = 0;
            }
            const int64_t fallback_pts_ms =
                stats_.audio_latest_submit_pts_ms +
                DurationMsForAudioSamples(static_cast<int64_t>(converted_samples), sample_rate);
            submit_audio_frame(
                audio_buffer.data(),
                static_cast<size_t>(converted_samples * out_channels * out_bytes_per_sample),
                sample_rate,
                out_channels,
                out_bytes_per_sample,
                converted_samples,
                decoder_pts_ms,
                fallback_pts_ms,
                false);
            const int64_t audio_now_ms = SteadyNowMs();
            if (audio_now_ms - audio_output_fps_window_start_ms >= 1000) {
                const int64_t window_ms = std::max<int64_t>(1, audio_now_ms - audio_output_fps_window_start_ms);
                const int fps = static_cast<int>(audio_output_fps_window_frames * 1000 / window_ms);
                UpdateStats([&](PlayerStats& stats) { stats.audio_output_fps = fps; });
                audio_output_fps_window_start_ms = audio_now_ms;
                audio_output_fps_window_frames = 0;
            }

            av_frame_unref(frame);
        }

        av_frame_free(&frame);
    }

    if (!stop_requested_ && demux_eof_) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == PlayerState::Playing) {
            stats_.stop_count++;
            ChangeState(PlayerState::Stopped);
            EDGELIVE_LOG_INFO(config_.log_tag, "Playback completed (EOF)");
        }
    }
}

void PlayerEngine::ChangeState(PlayerState state) {
    state_ = state;
    stats_.state_change_count++;
    if (callbacks_.on_state_changed) {
        callbacks_.on_state_changed(state_);
    }
    if (callbacks_.on_stats) {
        callbacks_.on_stats(stats_);
    }
}

void PlayerEngine::EmitError(int code, const std::string& message) {
    PlayerCallbacks callbacks_copy;
    PlayerStats stats_copy;
    PlayerState state_copy = PlayerState::Error;
    std::string log_tag;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.error_count++;
        state_ = PlayerState::Error;
        stats_.state_change_count++;
        callbacks_copy = callbacks_;
        stats_copy = stats_;
        state_copy = state_;
        log_tag = config_.log_tag;
    }

    EDGELIVE_LOG_ERROR(log_tag, message);
    if (callbacks_copy.on_state_changed) {
        callbacks_copy.on_state_changed(state_copy);
    }
    if (callbacks_copy.on_stats) {
        callbacks_copy.on_stats(stats_copy);
    }
    if (callbacks_copy.on_error) {
        callbacks_copy.on_error(code, message);
    }
}

void PlayerEngine::UpdateStats(const std::function<void(PlayerStats&)>& updater) {
    PlayerStats stats_copy;
    std::function<void(const PlayerStats&)> stats_cb;
    bool should_emit = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        updater(stats_);
        const int64_t now_ms = NowMs();
        const bool force_emit =
            stats_.state_change_count == 0 || stats_.error_count > 0 || stats_.first_video_render_cost_ms >= 0;
        if (force_emit || now_ms - last_stats_emit_ms_ >= 200) {
            last_stats_emit_ms_ = now_ms;
            stats_copy = stats_;
            stats_cb = callbacks_.on_stats;
            should_emit = true;
        }
    }
    if (should_emit && stats_cb) {
        stats_cb(stats_copy);
    }
}

bool PlayerEngine::ShouldEnableLowLatency(const MediaSource& source) const {
    return source.options.low_latency_mode && source.options.output_mode != PlaybackOutputMode::LocalPlayback &&
        !ShouldUseFullPlayback(source);
}

bool PlayerEngine::ShouldEnableVideoPtsBackpressure(const MediaSource& source) const {
    return source.options.video_pts_backpressure_enabled &&
        ResolvePlaybackMode(source) == PlaybackMode::Live;
}

const char* PlayerEngine::VideoPtsBackpressureModeToString(int mode) const {
    switch (static_cast<VideoPtsBackpressureState::Mode>(mode)) {
    case VideoPtsBackpressureState::Mode::Normal:
        return "Normal";
    case VideoPtsBackpressureState::Mode::SanitizeOnly:
        return "SanitizeOnly";
    case VideoPtsBackpressureState::Mode::SoftBackpressure:
        return "SoftBackpressure";
    case VideoPtsBackpressureState::Mode::HardBackpressure:
        return "HardBackpressure";
    case VideoPtsBackpressureState::Mode::WaitKeyframeRecovery:
        return "WaitKeyframeRecovery";
    }
    return "Unknown";
}

void PlayerEngine::ResetVideoPtsBackpressureState() {
    std::lock_guard<std::mutex> pts_lock(video_pts_backpressure_mutex_);
    video_pts_backpressure_state_ = {};
    video_pts_backpressure_state_.estimated_frame_duration_ms = 40;
    video_pts_backpressure_state_.mode = VideoPtsBackpressureState::Mode::Normal;
    video_pts_backpressure_state_.last_mode_change_steady_ms = SteadyNowMs();
}

int64_t PlayerEngine::GetVideoPtsEstimatedFrameDurationMs() const {
    std::lock_guard<std::mutex> pts_lock(video_pts_backpressure_mutex_);
    return video_pts_backpressure_state_.estimated_frame_duration_ms;
}

bool PlayerEngine::ShouldEnableVideoRecovery(const MediaSource& source) const {
    if (!source.options.video_recovery_enabled) {
        return false;
    }
    if (source.options.video_recovery_live_only) {
        return source.type == MediaSourceType::Rtsp || source.type == MediaSourceType::Rtmp;
    }
    return true;
}

bool PlayerEngine::ShouldHoldAudioForFastStartup() const {
    const MediaSource source = NormalizeMediaSource(active_request_);
    return ShouldOutputAudio(source) && source.options.fast_startup && source.options.defer_audio_until_first_video &&
        source.type != MediaSourceType::File;
}

int64_t PlayerEngine::GetAudioMasterClockMs() const {
    if (callbacks_.get_audio_clock_ms) {
        const int64_t external_clock_ms = callbacks_.get_audio_clock_ms();
        if (external_clock_ms > 0) {
            const int64_t now_ms = SteadyNowMs();
            if (ShouldEmitPeriodicLog(last_audio_clock_log_ms_, 1000, now_ms)) {
                int64_t submitted_audio_duration_ms = 0;
                int64_t anchor_audio_pts_ms = 0;
                int64_t anchor_monotonic_ms = 0;
                int64_t pending_audio_ms = 0;
                {
                    std::lock_guard<std::mutex> sync_lock(av_sync_mutex_);
                    submitted_audio_duration_ms = av_sync_state_.submitted_audio_duration_ms;
                    anchor_audio_pts_ms = av_sync_state_.anchor_audio_pts_ms;
                    anchor_monotonic_ms = av_sync_state_.anchor_monotonic_ms;
                    if (anchor_monotonic_ms > 0) {
                        const int64_t elapsed_ms =
                            std::max<int64_t>(0, now_ms - anchor_monotonic_ms);
                        pending_audio_ms =
                            std::max<int64_t>(0, submitted_audio_duration_ms - elapsed_ms);
                    }
                }
                EDGELIVE_LOG_INFO(
                    config_.log_tag,
                    "Audio clock(external): clock_ms=" + std::to_string(external_clock_ms) +
                        " anchor_audio_pts_ms=" + std::to_string(anchor_audio_pts_ms) +
                        " anchor_monotonic_ms=" + std::to_string(anchor_monotonic_ms) +
                        " submitted_audio_duration_ms=" + std::to_string(submitted_audio_duration_ms) +
                        " pending_audio_ms=" + std::to_string(pending_audio_ms));
            }
            return external_clock_ms;
        }
    }
    std::lock_guard<std::mutex> sync_lock(av_sync_mutex_);
    if (!av_sync_state_.audio_clock_started) {
        return 0;
    }
    const int64_t elapsed_ms = std::max<int64_t>(0, SteadyNowMs() - av_sync_state_.anchor_monotonic_ms);
    const int64_t pending_audio_ms =
        std::max<int64_t>(0, av_sync_state_.submitted_audio_duration_ms - elapsed_ms);
    const int64_t fallback_clock_ms = av_sync_state_.anchor_audio_pts_ms + elapsed_ms - pending_audio_ms;
    const int64_t now_ms = SteadyNowMs();
    if (ShouldEmitPeriodicLog(last_audio_clock_log_ms_, 1000, now_ms)) {
        EDGELIVE_LOG_INFO(
            config_.log_tag,
            "Audio clock(fallback): clock_ms=" + std::to_string(fallback_clock_ms) +
                " anchor_audio_pts_ms=" + std::to_string(av_sync_state_.anchor_audio_pts_ms) +
                " elapsed_ms=" + std::to_string(elapsed_ms) +
                " pending_audio_ms=" + std::to_string(pending_audio_ms));
    }
    return fallback_clock_ms;
}

int64_t PlayerEngine::ComputeAudioPlaybackDelayMs(int64_t audio_pts_ms) const {
    std::lock_guard<std::mutex> sync_lock(av_sync_mutex_);
    if (!av_sync_state_.audio_clock_started || av_sync_state_.anchor_monotonic_ms <= 0) {
        return 0;
    }
    const int64_t elapsed_ms = std::max<int64_t>(0, SteadyNowMs() - av_sync_state_.anchor_monotonic_ms);
    const int64_t pending_audio_ms =
        std::max<int64_t>(0, av_sync_state_.submitted_audio_duration_ms - elapsed_ms);
    const int64_t target_elapsed_ms =
        std::max<int64_t>(0, audio_pts_ms - av_sync_state_.anchor_audio_pts_ms + pending_audio_ms);
    if (target_elapsed_ms <= elapsed_ms) {
        return 0;
    }
    return target_elapsed_ms - elapsed_ms;
}

void PlayerEngine::OnVideoPacketObserved(int64_t arrival_ms, int64_t pts_ms) {
    std::lock_guard<std::mutex> lock(jitter_mutex_);
    if (jitter_state_.last_arrival_ms > 0 && arrival_ms >= jitter_state_.last_arrival_ms) {
        jitter_state_.interarrival_ms.push_back(arrival_ms - jitter_state_.last_arrival_ms);
    }
    if (jitter_state_.last_pts_ms > 0) {
        const int64_t delta = pts_ms - jitter_state_.last_pts_ms;
        if (delta < 0) {
            jitter_state_.reorder_count++;
        } else if (delta > 200) {
            jitter_state_.loss_events++;
        }
        jitter_state_.pts_delta_ms.push_back(delta);
    }
    jitter_state_.last_arrival_ms = arrival_ms;
    jitter_state_.last_pts_ms = pts_ms;
    jitter_state_.packet_count++;
}

void PlayerEngine::MaybeUpdateJitterController(const MediaSource& source, int64_t now_ms) {
    std::lock_guard<std::mutex> lock(jitter_mutex_);
    if (!source.options.adaptive_jitter_enabled) {
        return;
    }
    const int window_ms = std::max(200, source.options.jitter_eval_window_ms);
    if (jitter_state_.window_start_ms <= 0) {
        jitter_state_.window_start_ms = now_ms;
    }
    if (now_ms - jitter_state_.window_start_ms < window_ms) {
        return;
    }
    const double n = static_cast<double>(jitter_state_.interarrival_ms.size());
    double mean = 0.0;
    double stddev = 0.0;
    if (n > 0.0) {
        for (const int64_t v : jitter_state_.interarrival_ms) {
            mean += static_cast<double>(v);
        }
        mean /= n;
        double var = 0.0;
        for (const int64_t v : jitter_state_.interarrival_ms) {
            const double d = static_cast<double>(v) - mean;
            var += d * d;
        }
        var /= n;
        stddev = std::sqrt(std::max(0.0, var));
    }
    const double loss_rate = jitter_state_.packet_count == 0
        ? 0.0
        : static_cast<double>(jitter_state_.loss_events) / static_cast<double>(jitter_state_.packet_count);
    const double reorder_rate = jitter_state_.packet_count == 0
        ? 0.0
        : static_cast<double>(jitter_state_.reorder_count) / static_cast<double>(jitter_state_.packet_count);

    const bool poor_network = (stddev >= 10.0) || (loss_rate >= 0.02) || (reorder_rate >= 0.015);
    const bool good_network = (stddev <= 3.0) && (loss_rate <= 0.003) && (reorder_rate <= 0.002);
    const bool excellent_network = (stddev <= 1.5) && (loss_rate <= 0.001) && (reorder_rate <= 0.0005);

    auto next_mode = jitter_state_.mode;
    if (poor_network) {
        jitter_state_.anti_jitter_hits++;
        jitter_state_.stable_hits = 0;
        if (jitter_state_.anti_jitter_hits >= 2) {
            next_mode = JitterControlState::Mode::AntiJitter;
        }
    } else if (good_network) {
        jitter_state_.stable_hits++;
        jitter_state_.anti_jitter_hits = 0;
        if (jitter_state_.stable_hits >= 3) {
            next_mode = JitterControlState::Mode::Stable;
        }
    }
    if (next_mode != jitter_state_.mode) {
        jitter_state_.mode = next_mode;
        UpdateStats([&](PlayerStats& stats) { stats.jitter_mode_switch_count++; });
    }

    const int min_ms = std::max(20, source.options.jitter_min_ms);
    const int max_ms = std::max(min_ms, source.options.jitter_max_ms);
    const int current_buffer_ms = static_cast<int>(video_packet_queue_ ? video_packet_queue_->BufferedDurationMs() : 0);
    const int stable_base_ms = excellent_network ? min_ms : std::max(min_ms, source.options.jitter_target_ms_initial / 2);
    const int anti_jitter_base_ms = std::max(stable_base_ms + 40, std::min(max_ms, 90));
    const int base = jitter_state_.mode == JitterControlState::Mode::AntiJitter ? anti_jitter_base_ms : stable_base_ms;
    const int dynamic = static_cast<int>(stddev * 2.0 + loss_rate * 700.0 + reorder_rate * 500.0);
    int next_target = std::clamp(base + dynamic, min_ms, max_ms);
    if (excellent_network) {
        next_target = std::max(min_ms, next_target - 20);
    }
    if (current_buffer_ms > next_target + 40 && jitter_state_.mode == JitterControlState::Mode::Stable) {
        next_target = std::max(min_ms, next_target - 20);
    }
    if (current_buffer_ms > source.options.latency_cap_ms / 2 && jitter_state_.mode == JitterControlState::Mode::Stable) {
        next_target = std::max(min_ms, next_target - 20);
    }
    if (poor_network && current_buffer_ms < next_target / 2) {
        next_target = std::min(max_ms, next_target + 20);
    }
    jitter_state_.target_buffer_ms = next_target;

    UpdateStats([&](PlayerStats& stats) {
        stats.jitter_interarrival_std_ms = stddev;
        stats.loss_rate = loss_rate;
        stats.reorder_rate = reorder_rate;
        stats.jitter_target_ms = jitter_state_.target_buffer_ms;
        stats.jitter_current_ms = current_buffer_ms;
    });

    jitter_state_.interarrival_ms.clear();
    jitter_state_.pts_delta_ms.clear();
    jitter_state_.packet_count = 0;
    jitter_state_.reorder_count = 0;
    jitter_state_.loss_events = 0;
    jitter_state_.window_start_ms = now_ms;
}

void PlayerEngine::ApplyLatencyCap(const MediaSource& source) {
    const int cap_ms = source.options.latency_cap_ms;
    if (cap_ms <= 0) {
        return;
    }

    uint64_t dropped_video = 0;
    PacketQueue::TrimResult dropped_audio{};
    if (video_packet_queue_) {
        const auto trim = video_packet_queue_->DropOldestUntilBelowDurationMs(cap_ms, true, 1);
        dropped_video = trim.dropped_packets;
    }
    if (dropped_video == 0) {
        return;
    }

    UpdateStats([&](PlayerStats& stats) {
        stats.latency_cap_trigger_count++;
        stats.latency_cap_dropped_video_packets += dropped_video;
        stats.dropped_video_packets += dropped_video;
        stats.video_queue_size = video_packet_queue_ ? video_packet_queue_->Size() : 0;
        stats.audio_queue_size = audio_packet_queue_ ? audio_packet_queue_->Size() : 0;
        stats.audio_queue_duration_ms =
            audio_packet_queue_ ? audio_packet_queue_->BufferedDurationMs() : 0;
        stats.audio_latest_queue_head_pts_ms =
            audio_packet_queue_ ? audio_packet_queue_->FrontPtsMs() : 0;
        stats.jitter_current_ms = static_cast<int>(video_packet_queue_ ? video_packet_queue_->BufferedDurationMs() : 0);
    });
}

int PlayerEngine::GetCurrentJitterTargetMs(const MediaSource& source) const {
    if (!source.options.adaptive_jitter_enabled) {
        return 0;
    }
    if (!ShouldEnableLowLatency(source)) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(jitter_mutex_);
    const int min_ms = std::max(0, source.options.jitter_min_ms);
    const int max_ms = std::max(min_ms, source.options.jitter_max_ms);
    const int target = jitter_state_.target_buffer_ms > 0
        ? jitter_state_.target_buffer_ms
        : source.options.jitter_target_ms_initial;
    return std::clamp(target, min_ms, max_ms);
}

int PlayerEngine::GetCurrentAudioJitterTargetMs(const MediaSource& source) const {
    if (!source.options.adaptive_jitter_enabled) {
        return 0;
    }
    if (!ShouldEnableLowLatency(source)) {
        return 0;
    }

    // Keep audio buffering steadier and smaller than video so weak-network
    // video smoothing does not starve the pull-based audio sink.
    constexpr int kAudioJitterFloorMs = 120;
    constexpr int kAudioJitterCeilMs = 220;
    const int initial_hint_ms = std::max(kAudioJitterFloorMs, source.options.jitter_target_ms_initial / 2);
    const int capped_initial_ms = std::min(initial_hint_ms, kAudioJitterCeilMs);
    const int video_target_ms = GetCurrentJitterTargetMs(source);
    if (video_target_ms <= 0) {
        return capped_initial_ms;
    }
    return std::clamp(std::min(video_target_ms, capped_initial_ms), kAudioJitterFloorMs, kAudioJitterCeilMs);
}

void PlayerEngine::EnterVideoRecovery(const std::string& reason) {
    bool should_log = false;
    bool should_flush = false;
    std::string log_reason = reason;
    {
        std::lock_guard<std::mutex> recovery_lock(video_recovery_mutex_);
        video_recovery_state_.last_reason = reason;
        if (!video_recovery_state_.active) {
            video_recovery_state_.active = true;
            video_recovery_state_.waiting_for_keyframe = true;
            video_recovery_state_.enter_steady_ms = SteadyNowMs();
            video_recovery_state_.dropped_frames = 0;
            video_recovery_state_.dropped_packets = 0;
            video_recovery_state_.decoder_flushed = false;
            video_recovery_state_.catchup_active = false;
            video_recovery_state_.catchup_begin_steady_ms = 0;
            should_log = true;
        }
        if (reason.find("decoded frame flagged corrupt") != std::string::npos) {
            video_recovery_state_.consecutive_corrupt_frames++;
        } else {
            video_recovery_state_.consecutive_decode_failures++;
        }
        const uint32_t failure_hits = std::max(
            video_recovery_state_.consecutive_decode_failures,
            video_recovery_state_.consecutive_corrupt_frames);
        if (!video_recovery_state_.decoder_flushed && failure_hits >= kVideoRecoveryFailureThreshold) {
            video_recovery_state_.decoder_flushed = true;
            should_flush = true;
        }
        log_reason += " decode_failures=" + std::to_string(video_recovery_state_.consecutive_decode_failures) +
            " corrupt_frames=" + std::to_string(video_recovery_state_.consecutive_corrupt_frames);
    }
    if (should_flush) {
        AVCodecContext* codec_ctx = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            codec_ctx = video_codec_ctx_;
        }
        if (codec_ctx != nullptr) {
            avcodec_flush_buffers(codec_ctx);
            EDGELIVE_LOG_WARN(config_.log_tag, "Video decoder flushed for recovery");
        }
    }
    if (!should_log) {
        return;
    }
    UpdateStats([&](PlayerStats& stats) {
        stats.video_recovery_enter_count++;
    });
    EDGELIVE_LOG_WARN(config_.log_tag, "Video recovery enter: " + log_reason);
}

void PlayerEngine::ExitVideoRecovery() {
    int64_t duration_ms = 0;
    bool should_log = false;
    uint64_t dropped_packets = 0;
    {
        std::lock_guard<std::mutex> recovery_lock(video_recovery_mutex_);
        if (!video_recovery_state_.active) {
            return;
        }
        const int64_t now_ms = SteadyNowMs();
        duration_ms = video_recovery_state_.enter_steady_ms > 0
            ? std::max<int64_t>(0, now_ms - video_recovery_state_.enter_steady_ms)
            : 0;
        dropped_packets = video_recovery_state_.dropped_packets;
        video_recovery_state_.active = false;
        video_recovery_state_.waiting_for_keyframe = false;
        video_recovery_state_.decoder_flushed = false;
        video_recovery_state_.enter_steady_ms = 0;
        video_recovery_state_.dropped_frames = 0;
        video_recovery_state_.dropped_packets = 0;
        video_recovery_state_.consecutive_decode_failures = 0;
        video_recovery_state_.consecutive_corrupt_frames = 0;
        video_recovery_state_.catchup_active = false;
        video_recovery_state_.catchup_begin_steady_ms = 0;
        video_recovery_state_.last_reason.clear();
        should_log = true;
    }
    if (!should_log) {
        return;
    }
    UpdateStats([&](PlayerStats& stats) {
        stats.video_recovery_exit_count++;
        stats.last_video_recovery_duration_ms = duration_ms;
    });
    EDGELIVE_LOG_INFO(
        config_.log_tag,
        "Video recovery exit: duration_ms=" + std::to_string(duration_ms) +
            " dropped_packets=" + std::to_string(dropped_packets));
}

bool PlayerEngine::IsVideoRecoveryActive() const {
    std::lock_guard<std::mutex> recovery_lock(video_recovery_mutex_);
    return video_recovery_state_.active;
}

bool PlayerEngine::ShouldDropPacketUntilKeyframe(bool is_keyframe, uint64_t& dropped_packets) {
    dropped_packets = 0;
    std::lock_guard<std::mutex> recovery_lock(video_recovery_mutex_);
    const bool waiting_for_recovery = video_recovery_state_.active && video_recovery_state_.waiting_for_keyframe;
    const bool waiting_for_catchup = video_recovery_state_.catchup_active && video_recovery_state_.waiting_for_keyframe;
    if (!waiting_for_recovery && !waiting_for_catchup) {
        return false;
    }
    if (is_keyframe) {
        video_recovery_state_.waiting_for_keyframe = false;
        if (video_recovery_state_.catchup_active) {
            video_recovery_state_.catchup_active = false;
            video_recovery_state_.catchup_begin_steady_ms = 0;
            EDGELIVE_LOG_INFO(config_.log_tag, "Video catchup got keyframe");
        } else {
            EDGELIVE_LOG_INFO(
                config_.log_tag,
                "Video recovery got keyframe after dropping packets=" +
                    std::to_string(video_recovery_state_.dropped_packets));
        }
        return false;
    }
    video_recovery_state_.dropped_packets++;
    video_recovery_state_.dropped_frames++;
    dropped_packets = 1;
    return true;
}

PlayerEngine::VideoPtsNormalizeResult PlayerEngine::NormalizeVideoPtsForBackpressure(
    const MediaSource& source,
    int64_t raw_pts_ms,
    bool is_keyframe,
    int64_t frame_duration_hint_ms) {
    std::lock_guard<std::mutex> pts_lock(video_pts_backpressure_mutex_);
    VideoPtsNormalizeResult result;
    VideoPtsBackpressureState& state = video_pts_backpressure_state_;
    result.raw_pts_ms = raw_pts_ms;
    state.estimated_frame_duration_ms = ClampFrameDurationMs(frame_duration_hint_ms);
    const int64_t estimated_step_ms = state.estimated_frame_duration_ms;
    const int64_t now_ms = SteadyNowMs();
    const int64_t last_effective_pts_ms = state.last_effective_pts_ms;
    const int64_t steady_delta_ms = state.last_raw_steady_ms > 0
        ? std::max<int64_t>(0, now_ms - state.last_raw_steady_ms)
        : 0;
    const int64_t jump_threshold_ms = std::max<int64_t>(
        source.options.video_pts_jump_threshold_ms,
        estimated_step_ms * kVideoPtsLargeJumpClampMultiplier);
    const int64_t rollback_threshold_ms = std::max<int64_t>(
        source.options.video_pts_rollback_threshold_ms,
        estimated_step_ms * 2);
    int64_t effective_pts_ms = raw_pts_ms;

    if (raw_pts_ms <= 0) {
        result.flags |= static_cast<uint32_t>(VideoPtsAnomalyFlags::Invalid);
        result.flags |= static_cast<uint32_t>(VideoPtsAnomalyFlags::Sanitized);
        state.invalid_streak++;
        effective_pts_ms = last_effective_pts_ms > 0 ? last_effective_pts_ms + estimated_step_ms : estimated_step_ms;
    } else {
        state.invalid_streak = 0;
    }

    if (raw_pts_ms > 0 && state.last_raw_pts_ms > 0) {
        const int64_t raw_delta_ms = raw_pts_ms - state.last_raw_pts_ms;
        if (raw_delta_ms < 0) {
            result.flags |= static_cast<uint32_t>(VideoPtsAnomalyFlags::Rollback);
            if (std::llabs(raw_delta_ms) <= estimated_step_ms * 2) {
                result.flags |= static_cast<uint32_t>(VideoPtsAnomalyFlags::Sanitized);
                effective_pts_ms = std::max(last_effective_pts_ms + 1, effective_pts_ms);
            } else {
                state.rollback_streak++;
                effective_pts_ms = std::max(last_effective_pts_ms + 1, last_effective_pts_ms + estimated_step_ms);
            }
        } else {
            state.rollback_streak = 0;
            if (raw_delta_ms > jump_threshold_ms) {
                const int64_t raw_lead_ms = last_effective_pts_ms > 0 ? raw_pts_ms - last_effective_pts_ms : 0;
                const bool should_reanchor_timeline = last_effective_pts_ms > 0 &&
                    raw_lead_ms >= std::max<int64_t>(
                        kVideoPtsTimelineReanchorThresholdMs,
                        jump_threshold_ms * kVideoPtsLargeJumpClampMultiplier) &&
                    (is_keyframe || state.jump_streak > 0 || state.drift_offset_ms > 0 ||
                     steady_delta_ms >= estimated_step_ms * 4);
                if (should_reanchor_timeline &&
                    std::llabs(raw_pts_ms - (last_effective_pts_ms + state.drift_offset_ms)) >=
                        kVideoPtsKeyframeReanchorThresholdMs) {
                    effective_pts_ms = raw_pts_ms;
                    state.drift_offset_ms = 0;
                    state.jump_streak = 0;
                    result.request_render_clock_reset = true;
                    result.flags |= static_cast<uint32_t>(
                        is_keyframe ? VideoPtsAnomalyFlags::KeyframeReanchor
                                    : VideoPtsAnomalyFlags::TimelineReanchor);
                } else {
                    result.flags |= static_cast<uint32_t>(VideoPtsAnomalyFlags::Jump);
                    state.jump_streak++;
                    result.flags |= static_cast<uint32_t>(VideoPtsAnomalyFlags::Sanitized);
                    effective_pts_ms = last_effective_pts_ms > 0
                        ? last_effective_pts_ms + jump_threshold_ms
                        : raw_pts_ms;
                    if (raw_pts_ms > effective_pts_ms) {
                        state.drift_offset_ms += std::min<int64_t>(
                            raw_pts_ms - effective_pts_ms,
                            source.options.video_pts_drift_threshold_ms);
                    }
                }
            } else {
                state.jump_streak = 0;
                effective_pts_ms = raw_pts_ms;
            }
        }
    } else if (raw_pts_ms > 0) {
        effective_pts_ms = raw_pts_ms;
        state.rollback_streak = 0;
        state.jump_streak = 0;
    }

    if (effective_pts_ms <= 0) {
        effective_pts_ms = last_effective_pts_ms > 0 ? last_effective_pts_ms + estimated_step_ms : estimated_step_ms;
        result.flags |= static_cast<uint32_t>(VideoPtsAnomalyFlags::Sanitized);
    }
    if (last_effective_pts_ms > 0 && effective_pts_ms <= last_effective_pts_ms) {
        effective_pts_ms = last_effective_pts_ms + 1;
        result.flags |= static_cast<uint32_t>(VideoPtsAnomalyFlags::Sanitized);
    }

    VideoPtsBackpressureState::Mode next_mode = VideoPtsBackpressureState::Mode::Normal;
    if (state.invalid_streak >= std::max(1, source.options.video_pts_invalid_max_streak)) {
        next_mode = VideoPtsBackpressureState::Mode::SanitizeOnly;
    }
    if (HasVideoPtsFlag(static_cast<VideoPtsAnomalyFlags>(result.flags), VideoPtsAnomalyFlags::Rollback)) {
        next_mode = state.rollback_streak >= 2
            ? VideoPtsBackpressureState::Mode::WaitKeyframeRecovery
            : std::max(next_mode, VideoPtsBackpressureState::Mode::SoftBackpressure);
    }
    if (HasVideoPtsFlag(static_cast<VideoPtsAnomalyFlags>(result.flags), VideoPtsAnomalyFlags::Jump)) {
        next_mode = state.jump_streak >= 2
            ? VideoPtsBackpressureState::Mode::HardBackpressure
            : std::max(next_mode, VideoPtsBackpressureState::Mode::SoftBackpressure);
    }
    if (source.options.video_pts_hard_recover_to_keyframe &&
        (state.rollback_streak >= 3 || state.jump_streak >= 3)) {
        next_mode = VideoPtsBackpressureState::Mode::WaitKeyframeRecovery;
        result.request_keyframe_recovery = true;
    }

    if (next_mode != state.mode) {
        state.mode = next_mode;
        state.last_mode_change_steady_ms = now_ms;
    }

    state.last_raw_pts_ms = raw_pts_ms;
    state.last_raw_steady_ms = now_ms;
    state.last_effective_pts_ms = effective_pts_ms;
    if (raw_pts_ms > 0) {
        state.last_good_pts_ms = raw_pts_ms;
    }
    result.effective_pts_ms = effective_pts_ms;
    result.mode = state.mode;
    result.drift_ms = state.drift_offset_ms;
    return result;
}

PlayerEngine::RenderBackpressureAction PlayerEngine::DecideRenderBackpressureAction(
    VideoPtsBackpressureState::Mode mode,
    int64_t render_buffer_ms,
    int64_t av_diff_ms,
    int64_t drift_ms,
    const MediaSource& source) const {
    const int64_t drift_threshold_ms = std::max(80, source.options.video_pts_drift_threshold_ms);
    if (mode == VideoPtsBackpressureState::Mode::WaitKeyframeRecovery) {
        return RenderBackpressureAction::FlushToKeyframe;
    }
    if (mode == VideoPtsBackpressureState::Mode::HardBackpressure) {
        if (render_buffer_ms >= kVideoPtsHardBackpressureBufferMs || av_diff_ms <= -kVideoPtsLateDropThresholdMs) {
            return RenderBackpressureAction::DropLateFrame;
        }
        return RenderBackpressureAction::FreezePid;
    }
    if (mode == VideoPtsBackpressureState::Mode::SoftBackpressure) {
        if (render_buffer_ms >= kVideoPtsSoftBackpressureBufferMs || av_diff_ms <= -120) {
            return RenderBackpressureAction::DropLateFrame;
        }
        return RenderBackpressureAction::SleepShort;
    }
    if (mode == VideoPtsBackpressureState::Mode::SanitizeOnly) {
        if (std::llabs(drift_ms) >= drift_threshold_ms) {
            return RenderBackpressureAction::ClampPts;
        }
        return RenderBackpressureAction::FreezePid;
    }
    return RenderBackpressureAction::RenderNow;
}

std::unique_ptr<IPlayer> CreatePlayer() {
    return std::make_unique<PlayerEngine>();
}

} // namespace edgelive
