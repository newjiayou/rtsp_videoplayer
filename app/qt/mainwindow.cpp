#include "mainwindow.h"
#include "ui_mainwindow.h"

#include <QFileDialog>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QDateTime>
#include <QCheckBox>
#include <QMessageBox>
#include <QMetaObject>
#include <QPixmap>
#include <QThread>
#include <QDebug>
#include <QStringList>

namespace {

edgelive::MediaSourceType DetectMediaSourceType(const QString& value) {
    if (value.startsWith("rtsp://", Qt::CaseInsensitive)) {
        return edgelive::MediaSourceType::Rtsp;
    }
    if (value.startsWith("rtmp://", Qt::CaseInsensitive)) {
        return edgelive::MediaSourceType::Rtmp;
    }
    return edgelive::MediaSourceType::File;
}

constexpr int kLiveAudioBufferTargetMs = 120;
constexpr int kStrongNetworkJitterMinMs = 80;
constexpr int kStrongNetworkJitterInitialMs = 120;
constexpr int kStrongNetworkJitterMaxMs = 220;
constexpr qint64 kStatsUiUpdateIntervalMs = 1000;

QString FormatMs(int64_t value) {
    return QString::number(value) + "ms";
}

QString FormatUs(int64_t value) {
    return QString::number(value) + "us";
}

QString FormatPercent(double value) {
    return QString::number(value * 100.0, 'f', 2) + "%";
}

QString FormatCount(size_t value) {
    return QString::number(static_cast<qulonglong>(value));
}

QString FormatNA() {
    return QStringLiteral("N/A");
}

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent),
      ui_(new Ui::MainWindow),
      player_(edgelive::CreatePlayer()),
      alsa_audio_output_(std::make_unique<AlsaAudioOutput>()) {
    ui_->setupUi(this);
    resize(1440, 960);
    setMinimumSize(1280, 860);
    ui_->statusbar->setStyleSheet("QStatusBar { min-height: 220px; }");
    stats_label_ = new QLabel(this);
    stats_label_->setWordWrap(true);
    stats_label_->setTextFormat(Qt::PlainText);
    stats_label_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    stats_label_->setStyleSheet("QLabel { padding: 8px 12px; font-family: monospace; font-size: 13px; }");
    ui_->statusbar->addPermanentWidget(stats_label_, 1);
    ui_->btnPause->setIcon(QIcon(":/src/true.png"));
    ui_->lineEditVideoPath->setReadOnly(false);
    ui_->lineEditVideoPath->setPlaceholderText("Input local path / rtsp:// / rtmp://");

    connect(this, &MainWindow::frameReady, this, [this]() {
        DrainLatestFrame();
    });

    edgelive::PlayerInitConfig config;
    edgelive::PlayerCallbacks callbacks;
    callbacks.on_video_frame = [this](const edgelive::VideoFrame& frame) {
        if (!frame.buffer || frame.width <= 0 || frame.height <= 0 || frame.stride <= 0) {
            return;
        }
        bool should_dispatch = false;
        {
            std::lock_guard<std::mutex> lock(frame_mutex_);
            latest_video_frame_ = frame;
            latest_frame_dirty_ = true;
            if (!frame_dispatch_pending_) {
                frame_dispatch_pending_ = true;
                should_dispatch = true;
            }
        }
        if (should_dispatch) {
            emit frameReady();
        }
    };
    callbacks.on_audio_frame = [this](const edgelive::AudioFrame& frame) {
        const uint64_t generation = playback_generation_.load();
        if (frame.data == nullptr || frame.data_size == 0) {
            return;
        }
        QByteArray bytes(reinterpret_cast<const char*>(frame.data), static_cast<qsizetype>(frame.data_size));
        const int sample_rate = frame.sample_rate;
        const int channels = frame.channels;
        const int bytes_per_sample = frame.bytes_per_sample;
        {
            std::lock_guard<std::mutex> lock(audio_mutex_);
            if (!playing_ || generation != playback_generation_.load()) {
                return;
            }
            pending_audio_sample_rate_ = sample_rate;
            pending_audio_channels_ = channels;
            pending_audio_bytes_per_sample_ = bytes_per_sample;
        }
        if (!EnsureAudioOutputReady(sample_rate, channels, bytes_per_sample)) {
            static qint64 last_audio_init_failure_log_ms = 0;
            const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
            if (now_ms - last_audio_init_failure_log_ms >= 1000) {
                last_audio_init_failure_log_ms = now_ms;
                qWarning() << "[EdgeLivePlayer] audio frame dropped because audio output is unavailable:"
                           << "sr/ch/bps=" << sample_rate << channels << bytes_per_sample;
            }
            return;
        }
        QueueAudioFrame(sample_rate, channels, bytes_per_sample, frame.pts_ms, std::move(bytes));
    };
    callbacks.get_audio_clock_ms = [this]() {
        return GetAudioPlaybackClockMs();
    };
    callbacks.on_error = [this](int, const std::string& message) {
        const QString text = QString::fromStdString(message);
        QMetaObject::invokeMethod(this, [this, text]() {
            QMessageBox::warning(this, "Player Error", text);
            playing_ = false;
            ui_->btnPause->setIcon(QIcon(":/src/true.png"));
        });
    };
    callbacks.on_stats = [this](const edgelive::PlayerStats& stats) {
        static qint64 last_stats_ui_update_ms = 0;
        const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
        if (now_ms - last_stats_ui_update_ms < kStatsUiUpdateIntervalMs) {
            return;
        }
        last_stats_ui_update_ms = now_ms;
        QMetaObject::invokeMethod(this, [this, stats]() {
            const AlsaAudioOutput::Diagnostics audio_diag =
                alsa_audio_output_ ? alsa_audio_output_->GetDiagnostics() : AlsaAudioOutput::Diagnostics{};
            QStringList lines;
            lines
                << "Push Sender"
                << QString("  send_bitrate=%1  actual_send_rate=%2  rtt=%3  loss=%4  retrans=%5  cc_bandwidth=%6")
                       .arg(FormatNA(), FormatNA(), FormatNA(), FormatNA(), FormatNA(), FormatNA())
                << QString("  encoder_keyframe_size=%1  encoder_cost=%2  bitrate_spike=%3")
                       .arg(FormatNA(), FormatNA(), FormatNA())
                << "Play Receiver"
                << QString("  receive_bitrate=%1  jitter_buffer=%2  decode_queue=%3  network_callback=%4")
                       .arg(FormatNA())
                       .arg(FormatMs(stats.jitter_current_ms))
                       .arg(FormatCount(stats.video_queue_size) + "/" + FormatCount(stats.audio_queue_size))
                       .arg(QString::fromStdString(stats.sync_action))
                << QString("  jitter_target=%1  jitter_std=%2ms  packet_loss=%3  packet_reorder=%4")
                       .arg(FormatMs(stats.jitter_target_ms))
                       .arg(QString::number(stats.jitter_interarrival_std_ms, 'f', 2))
                       .arg(FormatPercent(stats.loss_rate))
                       .arg(FormatPercent(stats.reorder_rate))
                << "Video / Render"
                << QString("  decoded=%1  rendered=%2  decode_fps=%3  render_fps=%4  decode_cost=%5  render_cost=%6")
                       .arg(QString::number(stats.video_decoded_frame_count))
                       .arg(QString::number(stats.video_rendered_frame_count))
                       .arg(QString::number(stats.video_decode_fps))
                       .arg(QString::number(stats.video_render_fps))
                       .arg(FormatUs(stats.video_decode_avg_cost_us))
                       .arg(FormatUs(stats.video_render_avg_cost_us))
                << QString("  render_buffer=%1  pid_error=%2  playback_rate=%3  backpressure=%4")
                       .arg(FormatMs(stats.video_render_buffer_ms))
                       .arg(FormatMs(stats.video_render_pid_error_ms))
                       .arg(QString::number(stats.video_render_playback_rate, 'f', 3))
                       .arg(QString::fromStdString(stats.video_pts_backpressure_mode))
                << QString("  sync_video_pts=%1  sync_audio_clock=%2  av_diff_avg=%3  av_diff_last=%4  drop=%5")
                       .arg(FormatMs(stats.sync_video_pts_ms))
                       .arg(FormatMs(stats.sync_audio_clock_ms))
                       .arg(FormatMs(stats.av_offset_avg_ms))
                       .arg(FormatMs(stats.last_av_offset_ms))
                       .arg(QString::number(stats.video_sync_drop_count))
                << "Audio Delays"
                << QString("  demux_pts=%1  queue_head_pts=%2  decode_pts=%3  submit_pts=%4  output_pts=%5")
                       .arg(FormatMs(stats.audio_latest_demux_pts_ms))
                       .arg(FormatMs(stats.audio_latest_queue_head_pts_ms))
                       .arg(FormatMs(stats.audio_latest_decode_pts_ms))
                       .arg(FormatMs(stats.audio_latest_submit_pts_ms))
                       .arg(FormatMs(stats.audio_computed_output_pts_ms))
                << QString("  demux_to_video=%1  decode_to_submit=%2  submit_to_video=%3  queue_duration=%4")
                       .arg(FormatMs(stats.audio_demux_to_video_gap_ms))
                       .arg(FormatMs(stats.audio_decode_to_submit_offset_ms))
                       .arg(FormatMs(stats.audio_submit_to_video_gap_ms))
                       .arg(FormatMs(stats.audio_queue_duration_ms))
                << QString("  audio_clock=%1  queued_audio=%2  device_delay=%3  path_lag=%4  underrun=%5")
                       .arg(FormatMs(audio_diag.clock_ms))
                       .arg(FormatMs(audio_diag.queued_audio_ms))
                       .arg(FormatMs(audio_diag.device_delay_ms))
                       .arg(FormatMs(audio_diag.audio_path_lag_ms))
                       .arg(QString::number(audio_diag.underrun_count))
                << QString("  output_fps=%1  decode_cost=%2  wait_avg=%3  clock_provider=%4")
                       .arg(QString::number(stats.audio_output_fps))
                       .arg(FormatUs(stats.audio_decode_avg_cost_us))
                       .arg(FormatUs(stats.audio_packet_wait_avg_us))
                       .arg(QString::fromStdString(stats.audio_clock_provider));
            const QString status_text = lines.join('\n');
            if (stats_label_ != nullptr) {
                stats_label_->setText(status_text);
            }
        });
    };
    player_->Init(config, callbacks);
}

void MainWindow::DrainLatestFrame() {
    edgelive::VideoFrame frame;
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        if (!latest_frame_dirty_ || !latest_video_frame_.buffer || latest_video_frame_.buffer->empty() ||
            latest_video_frame_.width <= 0 || latest_video_frame_.height <= 0 || latest_video_frame_.stride <= 0) {
            frame_dispatch_pending_ = false;
            return;
        }
        frame = latest_video_frame_;
        latest_frame_dirty_ = false;
    }
    ui_->screenwidget->setFrame(std::move(frame));

    bool need_reschedule = false;
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        need_reschedule = latest_frame_dirty_;
        if (!need_reschedule) {
            frame_dispatch_pending_ = false;
        }
    }
    if (need_reschedule) {
        QMetaObject::invokeMethod(this, [this]() { DrainLatestFrame(); }, Qt::QueuedConnection);
    }
}

MainWindow::~MainWindow() {
    if (alsa_audio_output_) {
        alsa_audio_output_->Stop();
    }
    if (player_) {
        player_->Stop();
        player_->Release();
    }
    delete ui_;
}

void MainWindow::StartPlay(const QString& path) {
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty()) {
        QMessageBox::warning(this, "Play Failed", "Please input a media file path or stream URL.");
        return;
    }

    if (alsa_audio_output_) {
        alsa_audio_output_->Stop();
    }
    {
        std::lock_guard<std::mutex> lock(audio_mutex_);
        pending_audio_sample_rate_ = 0;
        pending_audio_channels_ = 0;
        pending_audio_bytes_per_sample_ = 0;
        audio_output_initialized_ = false;
        active_audio_sample_rate_ = 0;
        active_audio_channels_ = 0;
        active_audio_bytes_per_sample_ = 0;
        latest_audio_pkt_pts_ms_ = 0;
        latest_audio_pkt_end_pts_ms_ = 0;
    }
    ResetAudioPlaybackClock();
    playback_generation_++;

    edgelive::PlayRequest req;
    req.url = trimmed.toStdString();
    req.source.url = req.url;
    req.source.type = DetectMediaSourceType(trimmed);
    req.source.options.transport = "tcp";
    req.source.options.timeout_ms = 5000;
    req.source.options.buffer_size = 1024 * 1024;
    const bool is_rtsp = req.source.type == edgelive::MediaSourceType::Rtsp;
    local_playback_mode_ = req.source.type == edgelive::MediaSourceType::File;
    const bool realtime_audio_enabled = !local_playback_mode_ && IsRealtimeAudioEnabled();
    req.source.options.output_mode = local_playback_mode_
        ? edgelive::PlaybackOutputMode::LocalPlayback
        : (realtime_audio_enabled ? edgelive::PlaybackOutputMode::RealtimeVideoAudio
                                  : edgelive::PlaybackOutputMode::RealtimeVideoOnly);
    req.source.options.low_latency_mode = (req.source.type != edgelive::MediaSourceType::File);
    req.source.options.packet_queue_capacity = req.source.options.low_latency_mode ? 512 : 256;
    req.source.options.video_decode_queue_limit = is_rtsp ? 24 : 8;
    req.source.options.audio_decode_queue_limit = is_rtsp ? 48 : 16;
    req.source.options.video_render_queue_limit = is_rtsp ? 8 : 3;
    req.source.options.fast_startup = realtime_audio_enabled && req.source.options.low_latency_mode && is_rtsp;
    req.source.options.defer_audio_until_first_video =
        realtime_audio_enabled && req.source.options.low_latency_mode && is_rtsp;
    if (req.source.options.low_latency_mode) {
        req.source.options.jitter_min_ms = kStrongNetworkJitterMinMs;
        req.source.options.jitter_target_ms_initial = kStrongNetworkJitterInitialMs;
        req.source.options.jitter_max_ms = kStrongNetworkJitterMaxMs;
        req.source.options.jitter_eval_window_ms = 500;
        req.source.options.av_sync_max_lead_ms = 80;
        req.source.options.av_sync_max_lag_ms = 180;
        req.source.options.latency_cap_ms = 400;
    }
    qInfo() << "[EdgeLivePlayer] play request params:"
            << "url=" << trimmed
            << "source_type=" << static_cast<int>(req.source.type)
            << "local_playback_mode=" << local_playback_mode_
            << "realtime_audio_enabled=" << realtime_audio_enabled
            << "output_mode=" << static_cast<int>(req.source.options.output_mode)
            << "transport=" << QString::fromStdString(req.source.options.transport)
            << "timeout_ms=" << req.source.options.timeout_ms
            << "buffer_size=" << req.source.options.buffer_size
            << "packet_queue_capacity=" << req.source.options.packet_queue_capacity
            << "low_latency_mode=" << req.source.options.low_latency_mode
            << "video_decode_queue_limit=" << req.source.options.video_decode_queue_limit
            << "audio_decode_queue_limit=" << req.source.options.audio_decode_queue_limit
            << "video_render_queue_limit=" << req.source.options.video_render_queue_limit
            << "fast_startup=" << req.source.options.fast_startup
            << "defer_audio_until_first_video=" << req.source.options.defer_audio_until_first_video
            << "jitter_min_ms=" << req.source.options.jitter_min_ms
            << "jitter_target_ms_initial=" << req.source.options.jitter_target_ms_initial
            << "jitter_max_ms=" << req.source.options.jitter_max_ms
            << "jitter_eval_window_ms=" << req.source.options.jitter_eval_window_ms
            << "av_sync_dead_zone_ms=" << req.source.options.av_sync_dead_zone_ms
            << "av_sync_max_lead_ms=" << req.source.options.av_sync_max_lead_ms
            << "av_sync_max_lag_ms=" << req.source.options.av_sync_max_lag_ms
            << "latency_cap_ms=" << req.source.options.latency_cap_ms;
    if (req.source.options.timeout_ms <= 0 || req.source.options.buffer_size <= 0 ||
        req.source.options.packet_queue_capacity <= 0 || req.source.options.video_decode_queue_limit <= 0 ||
        req.source.options.audio_decode_queue_limit <= 0 || req.source.options.video_render_queue_limit <= 0) {
        qWarning() << "[EdgeLivePlayer] suspicious play request params detected";
    }

    if (!player_->Play(req)) {
        QMessageBox::warning(this, "Play Failed", "Failed to start playback.");
        return;
    }
    selected_path_ = trimmed;
    ui_->lineEditVideoPath->setText(trimmed);
    playing_ = true;
    ui_->btnPause->setIcon(QIcon(":/src/R.jpg"));
}

bool MainWindow::IsRealtimeAudioEnabled() const {
    const auto* checkbox = findChild<QCheckBox*>("checkBoxRealtimeAudio");
    return checkbox != nullptr && checkbox->isChecked();
}

bool MainWindow::EnsureAudioOutput(int sample_rate, int channels, int bytes_per_sample) {
    if (sample_rate < 8000 || sample_rate > 384000 || channels <= 0 || bytes_per_sample != 2) {
        qWarning() << "[EdgeLivePlayer] invalid audio format skipped:"
                   << "sr/ch/bps=" << sample_rate << channels << bytes_per_sample;
        return false;
    }

    if (alsa_audio_output_) {
        if (!alsa_audio_output_->Initialize(sample_rate, channels, bytes_per_sample, local_playback_mode_)) {
            qWarning() << "[EdgeLivePlayer] failed to initialize ALSA audio output";
            return false;
        }
        alsa_audio_output_->Start();
        qInfo() << "[EdgeLivePlayer] audio output ready:"
                << "sr/ch/bps=" << sample_rate << channels << bytes_per_sample
                << "local_playback=" << local_playback_mode_;
    }
    return alsa_audio_output_ != nullptr;
}

bool MainWindow::EnsureAudioOutputReady(int sample_rate, int channels, int bytes_per_sample) {
    {
        std::lock_guard<std::mutex> lock(audio_mutex_);
        if (audio_output_initialized_ &&
            active_audio_sample_rate_ == sample_rate &&
            active_audio_channels_ == channels &&
            active_audio_bytes_per_sample_ == bytes_per_sample) {
            return true;
        }
    }

    bool initialized = false;
    if (QThread::currentThread() == thread()) {
        initialized = EnsureAudioOutput(sample_rate, channels, bytes_per_sample);
    } else {
        QMetaObject::invokeMethod(
            this,
            [this, sample_rate, channels, bytes_per_sample, &initialized]() {
                initialized = EnsureAudioOutput(sample_rate, channels, bytes_per_sample);
            },
            Qt::BlockingQueuedConnection);
    }

    std::lock_guard<std::mutex> lock(audio_mutex_);
    if (!initialized) {
        audio_output_initialized_ = false;
        active_audio_sample_rate_ = 0;
        active_audio_channels_ = 0;
        active_audio_bytes_per_sample_ = 0;
        return false;
    }

    const bool first_init = !audio_output_initialized_;
    const bool format_changed =
        active_audio_sample_rate_ != sample_rate ||
        active_audio_channels_ != channels ||
        active_audio_bytes_per_sample_ != bytes_per_sample;
    audio_output_initialized_ = true;
    active_audio_sample_rate_ = sample_rate;
    active_audio_channels_ = channels;
    active_audio_bytes_per_sample_ = bytes_per_sample;
    if (first_init || format_changed) {
        qInfo() << "[EdgeLivePlayer] audio output format configured:"
                << "sr/ch/bps=" << sample_rate << channels << bytes_per_sample
                << "first_init=" << first_init
                << "format_changed=" << format_changed;
    }
    return true;
}

void MainWindow::QueueAudioFrame(int sample_rate, int channels, int bytes_per_sample, int64_t pts_ms, QByteArray bytes) {
    if (bytes.isEmpty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(audio_mutex_);
    latest_audio_pkt_pts_ms_ = pts_ms;
    latest_audio_pkt_end_pts_ms_ = pts_ms +
        (sample_rate > 0 && channels > 0 && bytes_per_sample > 0
                ? static_cast<int64_t>(bytes.size()) * 1000 / static_cast<int64_t>(sample_rate * channels * bytes_per_sample)
                : 0);
    if (alsa_audio_output_) {
        alsa_audio_output_->QueueAudioFrame(pts_ms, std::move(bytes));
    }
}

int64_t MainWindow::GetAudioPlaybackClockMs() {
    return alsa_audio_output_ != nullptr ? alsa_audio_output_->GetAudioClockMs() : 0;
}

void MainWindow::ResetAudioPlaybackClock() {
    std::lock_guard<std::mutex> lock(audio_mutex_);
    latest_audio_pkt_pts_ms_ = 0;
    latest_audio_pkt_end_pts_ms_ = 0;
    last_audio_trace_log_ms_ = 0;
    audio_output_initialized_ = false;
    active_audio_sample_rate_ = 0;
    active_audio_channels_ = 0;
    active_audio_bytes_per_sample_ = 0;
    if (alsa_audio_output_ != nullptr) {
        alsa_audio_output_->Reset();
    }
}

void MainWindow::on_btnSelectFile_clicked() {
    const QString filePath = QFileDialog::getOpenFileName(
        this,
        "Select Media File",
        QString(),
        "Media Files (*.mp4 *.mkv *.avi *.mov);;All Files (*.*)");

    if (filePath.isEmpty()) {
        return;
    }

    player_->Stop();
    StartPlay(filePath);
}

void MainWindow::on_btnPause_clicked() {
    if (!playing_) {
        const QString target = ui_->lineEditVideoPath->text().trimmed().isEmpty()
            ? selected_path_
            : ui_->lineEditVideoPath->text().trimmed();
        if (target.isEmpty()) {
            return;
        }
        StartPlay(target);
        return;
    }

    if (alsa_audio_output_) {
        alsa_audio_output_->Stop();
    }
    {
        std::lock_guard<std::mutex> lock(audio_mutex_);
        pending_audio_sample_rate_ = 0;
        pending_audio_channels_ = 0;
        pending_audio_bytes_per_sample_ = 0;
        audio_output_initialized_ = false;
        active_audio_sample_rate_ = 0;
        active_audio_channels_ = 0;
        active_audio_bytes_per_sample_ = 0;
        latest_audio_pkt_pts_ms_ = 0;
        latest_audio_pkt_end_pts_ms_ = 0;
    }
    ResetAudioPlaybackClock();
    playing_ = false;
    playback_generation_++;
    player_->Stop();
    ui_->btnPause->setIcon(QIcon(":/src/true.png"));
}
