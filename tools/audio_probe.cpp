#include "sdk/player.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace {

std::atomic<bool> g_stop{false};

void SignalHandler(int) {
    g_stop.store(true);
}

edgelive::MediaSourceType DetectMediaSourceType(const std::string& value) {
    if (value.rfind("rtsp://", 0) == 0) {
        return edgelive::MediaSourceType::Rtsp;
    }
    if (value.rfind("rtmp://", 0) == 0) {
        return edgelive::MediaSourceType::Rtmp;
    }
    return edgelive::MediaSourceType::File;
}

edgelive::PlayRequest BuildRequest(const std::string& input) {
    edgelive::PlayRequest request;
    request.url = input;
    request.source.url = input;
    request.source.type = DetectMediaSourceType(input);
    request.source.options.transport = "tcp";
    request.source.options.timeout_ms = 5000;
    request.source.options.buffer_size = 1024 * 1024;

    const bool is_live = request.source.type != edgelive::MediaSourceType::File;
    const bool is_rtsp = request.source.type == edgelive::MediaSourceType::Rtsp;
    request.source.options.low_latency_mode = is_live;
    request.source.options.packet_queue_capacity = is_live ? 32 : 256;
    request.source.options.video_decode_queue_limit = is_rtsp ? 24 : 8;
    request.source.options.audio_decode_queue_limit = is_rtsp ? 48 : 16;
    request.source.options.video_render_queue_limit = is_rtsp ? 12 : 3;
    request.source.options.fast_startup = is_rtsp;
    request.source.options.defer_audio_until_first_video = is_rtsp;
    return request;
}

void PrintUsage(const char* argv0) {
    std::cerr << "Usage: " << argv0 << " <media-path-or-url> [duration_sec]\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        PrintUsage(argv[0]);
        return 1;
    }

    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);

    const std::string input = argv[1];
    const int duration_sec = argc >= 3 ? std::max(0, std::atoi(argv[2])) : 10;

    auto player = edgelive::CreatePlayer();
    if (!player) {
        std::cerr << "failed to create player\n";
        return 2;
    }

    std::mutex stats_mutex;
    edgelive::PlayerStats latest_stats;
    std::atomic<uint64_t> video_frames{0};
    std::atomic<uint64_t> audio_frames{0};
    std::atomic<int64_t> last_audio_pts_ms{0};

    edgelive::PlayerInitConfig config;
    config.log_tag = "audio_probe";

    edgelive::PlayerCallbacks callbacks;
    callbacks.on_video_frame = [&](const edgelive::VideoFrame& frame) {
        const uint64_t count = ++video_frames;
        if (count <= 3 || count % 60 == 0) {
            std::cout << "[video] count=" << count
                      << " pts_ms=" << frame.pts_ms
                      << " size=" << frame.width << "x" << frame.height
                      << " stride=" << frame.stride << '\n';
        }
    };
    callbacks.on_audio_frame = [&](const edgelive::AudioFrame& frame) {
        const uint64_t count = ++audio_frames;
        last_audio_pts_ms.store(frame.pts_ms);
        if (count <= 3 || count % 120 == 0) {
            std::cout << "[audio] count=" << count
                      << " pts_ms=" << frame.pts_ms
                      << " sr=" << frame.sample_rate
                      << " ch=" << frame.channels
                      << " bytes=" << frame.data_size << '\n';
        }
    };
    callbacks.get_audio_clock_ms = [&]() {
        return last_audio_pts_ms.load();
    };
    callbacks.on_state_changed = [&](edgelive::PlayerState state) {
        std::cout << "[state] " << static_cast<int>(state) << '\n';
        if (state == edgelive::PlayerState::Stopped || state == edgelive::PlayerState::Released ||
            state == edgelive::PlayerState::Error) {
            g_stop.store(true);
        }
    };
    callbacks.on_error = [&](int code, const std::string& message) {
        std::cerr << "[error] code=" << code << " message=" << message << '\n';
        g_stop.store(true);
    };
    callbacks.on_stats = [&](const edgelive::PlayerStats& stats) {
        std::lock_guard<std::mutex> lock(stats_mutex);
        latest_stats = stats;
    };

    if (!player->Init(config, callbacks)) {
        std::cerr << "Init failed\n";
        return 3;
    }

    const auto request = BuildRequest(input);
    if (!player->Play(request)) {
        std::cerr << "Play failed for " << input << '\n';
        player->Release();
        return 4;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(duration_sec);
    while (!g_stop.load()) {
        if (duration_sec > 0 && std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    player->Stop();
    player->Release();

    {
        std::lock_guard<std::mutex> lock(stats_mutex);
        std::cout << std::fixed << std::setprecision(1)
                  << "[summary] video_frames=" << video_frames.load()
                  << " audio_frames=" << audio_frames.load()
                  << " ttff_video_ms=" << latest_stats.first_video_render_cost_ms
                  << " ttff_audio_ms=" << latest_stats.first_audio_play_cost_ms
                  << " demux_v=" << latest_stats.demuxed_video_packets
                  << " demux_a=" << latest_stats.demuxed_audio_packets
                  << " drop_v=" << latest_stats.dropped_video_packets
                  << " drop_a=" << latest_stats.dropped_audio_packets
                  << " jitter_cur_ms=" << latest_stats.jitter_current_ms
                  << " av_offset_last_ms=" << latest_stats.last_av_offset_ms
                  << '\n';
    }

    return 0;
}
