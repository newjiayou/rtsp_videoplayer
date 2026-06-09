#pragma once

#include "sdk/player.h"

#include "alsa_audio_output.h"

#include <QLabel>
#include <QMainWindow>
#include <QByteArray>
#include <QString>
#include <atomic>
#include <mutex>
#include <vector>

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

signals:
    void frameReady();

private slots:
    void on_btnSelectFile_clicked();
    void on_btnPause_clicked();

private:
    void StartPlay(const QString& path);
    bool EnsureAudioOutput(int sample_rate, int channels, int bytes_per_sample);
    bool EnsureAudioOutputReady(int sample_rate, int channels, int bytes_per_sample);
    void QueueAudioFrame(int sample_rate, int channels, int bytes_per_sample, int64_t pts_ms, QByteArray bytes);
    void DrainLatestFrame();
    int64_t GetAudioPlaybackClockMs();
    void ResetAudioPlaybackClock();
    bool IsRealtimeAudioEnabled() const;

private:
    Ui::MainWindow* ui_;
    QLabel* stats_label_{nullptr};
    std::unique_ptr<edgelive::IPlayer> player_;
    bool playing_{false};
    bool local_playback_mode_{false};
    std::atomic<uint64_t> playback_generation_{0};
    QString selected_path_;
    std::unique_ptr<AlsaAudioOutput> alsa_audio_output_;
    std::mutex audio_mutex_;
    int pending_audio_sample_rate_{0};
    int pending_audio_channels_{0};
    int pending_audio_bytes_per_sample_{0};
    bool audio_output_initialized_{false};
    int active_audio_sample_rate_{0};
    int active_audio_channels_{0};
    int active_audio_bytes_per_sample_{0};
    int64_t latest_audio_pkt_pts_ms_{0};
    int64_t latest_audio_pkt_end_pts_ms_{0};
    qint64 last_audio_trace_log_ms_{0};
    std::mutex frame_mutex_;
    edgelive::VideoFrame latest_video_frame_;
    bool latest_frame_dirty_{false};
    bool frame_dispatch_pending_{false};
};

