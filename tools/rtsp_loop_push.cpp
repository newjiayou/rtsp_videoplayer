#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

enum class PushMode {
    Paced,
    Realtime,
    SimulatedNetwork,
};

struct NetworkSimulationOptions {
    int startup_delay_ms{0};
    int jitter_ms{0};
    int burst_ms{0};
    int stall_every_ms{0};
    int stall_duration_ms{0};
    int max_bitrate_kbps{0};
    int pacing_chunk_bytes{1200};
};

struct ParsedArgs {
    std::string input;
    std::string output;
    bool use_copy{false};
    bool use_re_compat{false};
    PushMode mode{PushMode::Paced};
    NetworkSimulationOptions network;
};

std::atomic<bool> g_stop_requested{false};

void HandleSignal(int) {
    g_stop_requested = true;
}

void PrintUsage(const char* argv0) {
    std::cerr
        << "Usage: " << argv0 << " <input-file> <output-url> [options]\n"
        << "Options:\n"
        << "  --copy\n"
        << "  --re\n"
        << "  --mode <paced|realtime|simulated-network>\n"
        << "  --startup-delay-ms <int>\n"
        << "  --jitter-ms <int>\n"
        << "  --burst-ms <int>\n"
        << "  --stall-every-ms <int>\n"
        << "  --stall-duration-ms <int>\n"
        << "  --max-bitrate-kbps <int>\n"
        << "Examples:\n"
        << "  " << argv0 << " demo.mp4 rtsp://127.0.0.1:8554/live/test --copy\n"
        << "  " << argv0 << " demo.mp4 rtsp://127.0.0.1:8554/live/test --mode paced --max-bitrate-kbps 4500\n"
        << "  " << argv0 << " demo.mp4 rtsp://127.0.0.1:8554/live/test --mode simulated-network "
        << "--jitter-ms 50 --burst-ms 150 --stall-every-ms 5000 --stall-duration-ms 300\n";
}

std::string ShellQuote(const std::string& value) {
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

bool StartsWith(const std::string& value, const std::string& prefix) {
    return value.rfind(prefix, 0) == 0;
}

std::string ModeToString(PushMode mode) {
    if (mode == PushMode::Paced) {
        return "paced";
    }
    return mode == PushMode::SimulatedNetwork ? "simulated-network" : "realtime";
}

int ParseIntArg(const std::string& flag, const std::string& value) {
    try {
        size_t pos = 0;
        const int parsed = std::stoi(value, &pos);
        if (pos != value.size()) {
            throw std::invalid_argument("trailing data");
        }
        if (parsed < 0) {
            throw std::out_of_range("negative");
        }
        return parsed;
    } catch (const std::exception&) {
        throw std::runtime_error("invalid value for " + flag + ": " + value);
    }
}

ParsedArgs ParseArgs(int argc, char** argv) {
    if (argc < 3) {
        PrintUsage(argv[0]);
        throw std::runtime_error("missing required args");
    }

    ParsedArgs args;
    args.input = argv[1];
    args.output = argv[2];

    for (int i = 3; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--copy") {
            args.use_copy = true;
            continue;
        }
        if (arg == "--re") {
            args.use_re_compat = true;
            continue;
        }
        const auto require_value = [&](const std::string& flag) -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error("missing value for " + flag);
            }
            return argv[++i];
        };
        if (arg == "--mode") {
            const std::string value = require_value(arg);
            if (value == "paced") {
                args.mode = PushMode::Paced;
            } else if (value == "realtime") {
                args.mode = PushMode::Realtime;
            } else if (value == "simulated-network") {
                args.mode = PushMode::SimulatedNetwork;
            } else {
                throw std::runtime_error("invalid value for --mode: " + value);
            }
            continue;
        }
        if (arg == "--startup-delay-ms") {
            args.network.startup_delay_ms = ParseIntArg(arg, require_value(arg));
            continue;
        }
        if (arg == "--jitter-ms") {
            args.network.jitter_ms = ParseIntArg(arg, require_value(arg));
            continue;
        }
        if (arg == "--burst-ms") {
            args.network.burst_ms = ParseIntArg(arg, require_value(arg));
            continue;
        }
        if (arg == "--stall-every-ms") {
            args.network.stall_every_ms = ParseIntArg(arg, require_value(arg));
            continue;
        }
        if (arg == "--stall-duration-ms") {
            args.network.stall_duration_ms = ParseIntArg(arg, require_value(arg));
            continue;
        }
        if (arg == "--max-bitrate-kbps") {
            args.network.max_bitrate_kbps = ParseIntArg(arg, require_value(arg));
            continue;
        }

        throw std::runtime_error("unknown arg: " + arg);
    }

    return args;
}

std::string BuildPushCommand(
    const std::string& input,
    const std::string& output,
    bool use_copy,
    bool force_realtime) {
    const bool is_rtmp = StartsWith(output, "rtmp://");
    const bool is_rtsp = StartsWith(output, "rtsp://");

    std::ostringstream cmd;
    cmd << "ffmpeg -hide_banner -loglevel info "
        << "-fflags +genpts+nobuffer -flags low_delay -avioflags direct -flush_packets 1 "
        << "-stream_loop -1 ";
    if (force_realtime) {
        cmd << "-re ";
    }
    cmd << "-i " << ShellQuote(input) << ' ';
    if (use_copy) {
        cmd << "-c copy ";
    } else {
        cmd << "-vf fps=60 "
            << "-fps_mode cfr "
            << "-c:v libx264 -preset veryfast -tune zerolatency "
            << "-g 60 -keyint_min 60 -sc_threshold 0 "
            << "-r 60 "
            << "-b:v 4M -maxrate 4M -bufsize 1M "
            << "-x264-params nal-hrd=cbr:force-cfr=1 "
            << "-pix_fmt yuv420p "
            << "-c:a aac -b:a 128k ";
    }
    if (is_rtsp) {
        cmd << "-rtsp_transport tcp -muxdelay 0 -muxpreload 0 ";
    }
    cmd << "-f " << (is_rtmp ? "flv " : "rtsp ") << ShellQuote(output);
    return cmd.str();
}

int ResolveDefaultPacingBitrateKbps(const ParsedArgs& args) {
    if (args.network.max_bitrate_kbps > 0) {
        return args.network.max_bitrate_kbps;
    }
    return args.use_copy ? 12000 : 4500;
}

std::string BuildProxyCommand(
    const std::string& upstream_host,
    int upstream_port,
    int listen_port,
    const ParsedArgs& args) {
    std::ostringstream cmd;
    cmd << "python3 -u -c " << ShellQuote(
        "import random\n"
        "import socket\n"
        "import sys\n"
        "import threading\n"
        "import time\n"
        "\n"
        "up_host = sys.argv[1]\n"
        "up_port = int(sys.argv[2])\n"
        "listen_port = int(sys.argv[3])\n"
        "startup_delay_ms = int(sys.argv[4])\n"
        "jitter_ms = int(sys.argv[5])\n"
        "burst_ms = int(sys.argv[6])\n"
        "stall_every_ms = int(sys.argv[7])\n"
        "stall_duration_ms = int(sys.argv[8])\n"
        "max_bitrate_kbps = int(sys.argv[9])\n"
        "pacing_chunk_bytes = max(256, int(sys.argv[10]))\n"
        "\n"
        "server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)\n"
        "server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)\n"
        "server.settimeout(3.0)\n"
        "server.bind(('127.0.0.1', listen_port))\n"
        "server.listen(1)\n"
        "print(f'[rtsp_loop_push] proxy-listen 127.0.0.1:{listen_port}', flush=True)\n"
        "try:\n"
        "    client, _ = server.accept()\n"
        "except socket.timeout:\n"
        "    print('[rtsp_loop_push] proxy accept timeout', flush=True)\n"
        "    server.close()\n"
        "    raise SystemExit(2)\n"
        "upstream = socket.create_connection((up_host, up_port))\n"
        "client.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)\n"
        "upstream.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)\n"
        "stop = False\n"
        "rng = random.Random()\n"
        "start = time.monotonic()\n"
        "next_stall = (stall_every_ms / 1000.0) if stall_every_ms > 0 else None\n"
        "budget_bytes = 0.0\n"
        "budget_last = time.monotonic()\n"
        "\n"
        "def apply_shape(size):\n"
        "    global next_stall, budget_bytes, budget_last\n"
        "    now = time.monotonic()\n"
        "    if startup_delay_ms > 0 and now - start < startup_delay_ms / 1000.0:\n"
        "        time.sleep((startup_delay_ms / 1000.0) - (now - start))\n"
        "        now = time.monotonic()\n"
        "    if next_stall is not None:\n"
        "        elapsed = now - start\n"
        "        if elapsed >= next_stall:\n"
        "            time.sleep(stall_duration_ms / 1000.0)\n"
        "            now = time.monotonic()\n"
        "            next_stall += stall_every_ms / 1000.0\n"
        "    if jitter_ms > 0:\n"
        "        time.sleep(rng.uniform(0.0, jitter_ms / 1000.0))\n"
        "        now = time.monotonic()\n"
        "    if burst_ms > 0:\n"
        "        cycle_ms = max(burst_ms, 1)\n"
        "        cycle_pos = int((now - start) * 1000.0) % (cycle_ms * 2)\n"
        "        if cycle_pos >= cycle_ms:\n"
        "            time.sleep(burst_ms / 1000.0)\n"
        "            now = time.monotonic()\n"
        "    if max_bitrate_kbps > 0:\n"
        "        bytes_per_sec = max_bitrate_kbps * 1000.0 / 8.0\n"
        "        elapsed = now - budget_last\n"
        "        budget_last = now\n"
        "        budget_bytes = min(bytes_per_sec * 2.0, budget_bytes + elapsed * bytes_per_sec)\n"
        "        if budget_bytes < size:\n"
        "            need = (size - budget_bytes) / bytes_per_sec\n"
        "            time.sleep(max(0.0, need))\n"
        "            budget_last = time.monotonic()\n"
        "            budget_bytes = 0.0\n"
        "        else:\n"
        "            budget_bytes -= size\n"
        "\n"
        "def relay(src, dst, shape):\n"
        "    global stop\n"
        "    try:\n"
        "        while not stop:\n"
        "            data = src.recv(16384)\n"
        "            if not data:\n"
        "                break\n"
        "            if shape:\n"
        "                for offset in range(0, len(data), pacing_chunk_bytes):\n"
        "                    chunk = data[offset:offset + pacing_chunk_bytes]\n"
        "                    apply_shape(len(chunk))\n"
        "                    dst.sendall(chunk)\n"
        "            else:\n"
        "                dst.sendall(data)\n"
        "    except Exception:\n"
        "        pass\n"
        "    stop = True\n"
        "    try:\n"
        "        dst.shutdown(socket.SHUT_RDWR)\n"
        "    except Exception:\n"
        "        pass\n"
        "    try:\n"
        "        src.shutdown(socket.SHUT_RDWR)\n"
        "    except Exception:\n"
        "        pass\n"
        "\n"
        "threads = [\n"
        "    threading.Thread(target=relay, args=(client, upstream, True), daemon=True),\n"
        "    threading.Thread(target=relay, args=(upstream, client, False), daemon=True),\n"
        "]\n"
        "for thread in threads:\n"
        "    thread.start()\n"
        "for thread in threads:\n"
        "    thread.join()\n"
        "client.close()\n"
        "upstream.close()\n"
        "server.close()\n")
        << ' '
        << ShellQuote(upstream_host) << ' '
        << upstream_port << ' '
        << listen_port << ' '
        << args.network.startup_delay_ms << ' '
        << args.network.jitter_ms << ' '
        << args.network.burst_ms << ' '
        << args.network.stall_every_ms << ' '
        << args.network.stall_duration_ms << ' '
        << args.network.max_bitrate_kbps << ' '
        << args.network.pacing_chunk_bytes;
    return cmd.str();
}

bool ParseTcpEndpoint(const std::string& url, std::string& host, int& port) {
    const auto scheme_pos = url.find("://");
    if (scheme_pos == std::string::npos) {
        return false;
    }

    const size_t authority_begin = scheme_pos + 3;
    const size_t authority_end = url.find('/', authority_begin);
    const std::string authority = url.substr(authority_begin, authority_end - authority_begin);
    if (authority.empty()) {
        return false;
    }

    const size_t at_pos = authority.rfind('@');
    const std::string host_port = at_pos == std::string::npos ? authority : authority.substr(at_pos + 1);
    if (host_port.empty()) {
        return false;
    }

    host.clear();
    port = 0;
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
        if (StartsWith(url, "rtsp://")) {
            port = 554;
        } else if (StartsWith(url, "rtmp://")) {
            port = 1935;
        }
    }
    return port > 0;
}

int CreateListeningSocket() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 1) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int ReserveLoopbackPort() {
    const int fd = CreateListeningSocket();
    if (fd < 0) {
        throw std::runtime_error("failed to reserve loopback port");
    }

    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        close(fd);
        throw std::runtime_error("failed to inspect reserved loopback port");
    }
    const int port = ntohs(addr.sin_port);
    close(fd);
    return port;
}

std::string BuildProxyOutputUrl(const std::string& original_url, int local_port) {
    const auto scheme_pos = original_url.find("://");
    if (scheme_pos == std::string::npos) {
        throw std::runtime_error("invalid output url: " + original_url);
    }

    const size_t authority_begin = scheme_pos + 3;
    const size_t authority_end = original_url.find('/', authority_begin);
    const std::string suffix = authority_end == std::string::npos ? "" : original_url.substr(authority_end);
    return original_url.substr(0, scheme_pos + 3) + std::string("127.0.0.1:") + std::to_string(local_port) + suffix;
}

class Subprocess {
public:
    explicit Subprocess(std::string command)
        : command_(std::move(command)) {
    }

    int Run() const {
        return std::system(command_.c_str());
    }

    const std::string& command() const {
        return command_;
    }

private:
    std::string command_;
};

} // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    ParsedArgs args;
    try {
        args = ParseArgs(argc, argv);
    } catch (const std::exception& ex) {
        std::cerr << "[rtsp_loop_push] " << ex.what() << '\n';
        return 1;
    }

    if (args.mode == PushMode::Paced) {
        args.network.max_bitrate_kbps = ResolveDefaultPacingBitrateKbps(args);
    }

    const bool use_proxy = args.mode == PushMode::Paced ||
        (args.mode == PushMode::SimulatedNetwork &&
         (args.network.startup_delay_ms > 0 ||
          args.network.jitter_ms > 0 ||
          args.network.burst_ms > 0 ||
          args.network.stall_every_ms > 0 ||
          args.network.stall_duration_ms > 0 ||
          args.network.max_bitrate_kbps > 0));

    std::string effective_output = args.output;
    std::string proxy_command;
    if (use_proxy) {
        std::string upstream_host;
        int upstream_port = 0;
        if (!ParseTcpEndpoint(args.output, upstream_host, upstream_port)) {
            std::cerr << "[rtsp_loop_push] simulated-network mode requires a tcp-style rtsp/rtmp output url\n";
            return 1;
        }
        const int proxy_port = ReserveLoopbackPort();
        effective_output = BuildProxyOutputUrl(args.output, proxy_port);
        proxy_command = BuildProxyCommand(upstream_host, upstream_port, proxy_port, args);
    }

    const std::string ffmpeg_command = BuildPushCommand(args.input, effective_output, args.use_copy, true);

    std::cout << "[rtsp_loop_push] mode=" << ModeToString(args.mode)
              << " compat_re=" << (args.use_re_compat ? "true" : "false")
              << " proxy=" << (use_proxy ? "enabled" : "disabled") << '\n';
    if (args.mode == PushMode::SimulatedNetwork || args.mode == PushMode::Paced) {
        std::cout << "[rtsp_loop_push] simulated-network startup_delay_ms=" << args.network.startup_delay_ms
                  << " jitter_ms=" << args.network.jitter_ms
                  << " burst_ms=" << args.network.burst_ms
                  << " stall_every_ms=" << args.network.stall_every_ms
                  << " stall_duration_ms=" << args.network.stall_duration_ms
                  << " max_bitrate_kbps=" << args.network.max_bitrate_kbps
                  << " pacing_chunk_bytes=" << args.network.pacing_chunk_bytes << '\n';
    }
    if (use_proxy) {
        std::cout << "[rtsp_loop_push] proxy command: " << proxy_command << '\n';
        std::cout << "[rtsp_loop_push] ffmpeg target rewritten to: " << effective_output << '\n';
    }
    std::cout << "[rtsp_loop_push] ffmpeg command: " << ffmpeg_command << '\n';

    while (!g_stop_requested) {
        std::thread proxy_thread;
        std::atomic<int> proxy_exit_code{-1};
        if (use_proxy) {
            proxy_thread = std::thread([&]() {
                proxy_exit_code = std::system(proxy_command.c_str());
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        const int ffmpeg_rc = std::system(ffmpeg_command.c_str());
        std::cerr << "[rtsp_loop_push] ffmpeg exited with code " << ffmpeg_rc << '\n';

        if (proxy_thread.joinable()) {
            proxy_thread.join();
            std::cerr << "[rtsp_loop_push] proxy exited with code " << proxy_exit_code.load() << '\n';
        }

        if (g_stop_requested) {
            break;
        }

        std::cerr << "[rtsp_loop_push] restarting in 1s\n";
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    return 0;
}
