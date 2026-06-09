// FFmpeg-backed realtime pusher

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

namespace {

void PrintUsage(const char* argv0)
{
    std::cerr
        << "Usage:\n"
        << "  " << argv0 << " <input-file> <output-url> [--copy] [--re]\n"
        << "Example:\n"
        << "  " << argv0 << " test.mp4 rtsp://127.0.0.1:8554/live --copy --re\n";
}

std::string ShellQuote(const std::string& value)
{
    std::string out = "'";
    for (char ch : value) {
        if (ch == '\'') {
            out += "'\\''";
        } else {
            out += ch;
        }
    }
    out += "'";
    return out;
}

std::string BuildPushCommand(const std::string& input, const std::string& output, bool use_copy, bool use_re)
{
    const bool is_rtmp = output.rfind("rtmp://", 0) == 0;
    const bool is_rtsp = output.rfind("rtsp://", 0) == 0;
    std::ostringstream cmd;
    cmd << "ffmpeg -hide_banner -loglevel info -stream_loop -1 ";
    if (use_re) {
        cmd << "-re ";
    }
    cmd << "-i " << ShellQuote(input) << ' ';
    if (use_copy) {
        cmd << "-c copy ";
    } else {
        cmd << "-c:v libx264 -preset veryfast -tune zerolatency "
            << "-b:v 4M -maxrate 4M -bufsize 8M "
            << "-pix_fmt yuv420p "
            << "-c:a aac -b:a 128k ";
    }
    if (is_rtsp) {
        cmd << "-rtsp_transport tcp -muxdelay 0.1 ";
    }
    cmd << "-f " << (is_rtmp ? "flv " : "rtsp ") << ShellQuote(output);
    return cmd.str();
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        PrintUsage(argv[0]);
        return 1;
    }

    const std::string input = argv[1];
    const std::string output = argv[2];
    bool use_copy = false;
    bool use_re = false;
    for (int i = 3; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--copy") {
            use_copy = true;
        } else if (arg == "--re") {
            use_re = true;
        } else {
            std::cerr << "unknown arg: " << arg << '\n';
            PrintUsage(argv[0]);
            return 1;
        }
    }

    const std::string cmd = BuildPushCommand(input, output, use_copy, use_re);
    std::cout << "[rtsp_h264_file] command: " << cmd << '\n';

    while (true) {
        const int rc = std::system(cmd.c_str());
        std::cerr << "[rtsp_h264_file] ffmpeg exited with code " << rc << ", restarting in 1s\n";
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    return 0;
}
