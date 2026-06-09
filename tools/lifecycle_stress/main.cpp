#include "sdk/player.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace {

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
    std::cerr << "Usage: " << argv0 << " <media-path-or-url> [cycles] [play_ms]\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        PrintUsage(argv[0]);
        return 1;
    }

    const std::string input = argv[1];
    const int cycles = argc >= 3 ? std::max(1, std::atoi(argv[2])) : 100;
    const int play_ms = argc >= 4 ? std::max(50, std::atoi(argv[3])) : 300;
    const auto request = BuildRequest(input);

    int success_count = 0;
    for (int i = 0; i < cycles; ++i) {
        std::atomic<bool> saw_playing{false};
        std::atomic<bool> saw_error{false};

        auto player = edgelive::CreatePlayer();
        if (!player) {
            std::cerr << "[cycle " << (i + 1) << "] create player failed\n";
            break;
        }

        edgelive::PlayerInitConfig config;
        config.log_tag = "lifecycle_stress";

        edgelive::PlayerCallbacks callbacks;
        callbacks.on_video_frame = [&](const edgelive::VideoFrame&) {};
        callbacks.on_audio_frame = [&](const edgelive::AudioFrame&) {};
        callbacks.get_audio_clock_ms = []() { return int64_t{0}; };
        callbacks.on_state_changed = [&](edgelive::PlayerState state) {
            if (state == edgelive::PlayerState::Playing) {
                saw_playing.store(true);
            }
            if (state == edgelive::PlayerState::Error) {
                saw_error.store(true);
            }
        };
        callbacks.on_error = [&](int code, const std::string& message) {
            saw_error.store(true);
            std::cerr << "[cycle " << (i + 1) << "] error code=" << code << " msg=" << message << '\n';
        };
        callbacks.on_stats = [&](const edgelive::PlayerStats&) {};

        if (!player->Init(config, callbacks)) {
            std::cerr << "[cycle " << (i + 1) << "] Init failed\n";
            player->Release();
            break;
        }

        if (!player->Play(request)) {
            std::cerr << "[cycle " << (i + 1) << "] Play failed\n";
            player->Release();
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(play_ms));

        const bool stop_ok = player->Stop();
        const bool release_ok = player->Release();
        if (!stop_ok || !release_ok || saw_error.load()) {
            std::cerr << "[cycle " << (i + 1) << "] stop/release failed"
                      << " stop_ok=" << stop_ok
                      << " release_ok=" << release_ok
                      << " saw_playing=" << saw_playing.load()
                      << '\n';
            break;
        }

        ++success_count;
        std::cout << "[cycle " << (i + 1) << "/" << cycles << "] ok"
                  << " played=" << (saw_playing.load() ? "true" : "false")
                  << '\n';
    }

    std::cout << "[summary] success=" << success_count << "/" << cycles << '\n';
    return success_count == cycles ? 0 : 2;
}
