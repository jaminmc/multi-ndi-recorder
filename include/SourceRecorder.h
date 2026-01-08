#pragma once
#include <QObject>
#include <QImage>
#include <QThread>
#include <QAtomicInteger>
#include <QElapsedTimer>
#include <QMutex>
#include <QVector>
#include <QByteArray>
#include "FfmpegWriter.h"
#include "NdiManager.h"

// Use void* to avoid MOC processing NDI headers
// Cast to NDIlib_recv_instance* in .cpp file

struct BufferedVideoFrame
{
    QByteArray data;
    int width;
    int height;
    int64_t timestamp; // NDI timestamp (100ns units)
    int fpsNum;
    int fpsDen;
};

struct BufferedAudioFrame
{
    QByteArray data;
    int numSamples;
    int sampleRate;
    int numChannels;
    int64_t timestamp; // NDI timestamp (100ns units)
};

struct SourceSettings
{
    QString ndiSource;
    QString outputFolder;
    QString label;
    bool segmented = false;
    int segmentMinutes = 20;
    
    // Video encoding settings
    QString fileExtension = "mp4";  // "mp4", "mov", "mkv"
    QString videoCodec = "h264";    // "h264", "h265"
    bool useHardwareEncoder = false; // Use hardware encoder (h264_videotoolbox, hevc_videotoolbox on Mac)
    QString qualityMode = "crf";    // "crf" or "bitrate"
    int crfValue = 23;              // CRF value (0-51, lower is better quality)
    int videoBitrate = 12000000;    // Video bitrate in bits per second
    int hardwareQuality = 55;        // Hardware encoder quality (VideoToolbox CQ: 1-100 higher=better, default 55; others: uses CRF)
};

class SourceRecorder : public QObject
{
    Q_OBJECT
public:
    explicit SourceRecorder(QObject *parent = nullptr);
    ~SourceRecorder();

    void applySettings(const SourceSettings &settings);
    SourceSettings settings() const { return m_settings; }

    void start();
    void startPreview();  // Start preview-only mode (no recording)
    void stop();

    QImage lastFrame() const;
    QString status() const;
    qint64 elapsedMs() const;
    QString currentFile() const { return m_writer.currentFile(); }

signals:
    void previewUpdated();
    void errorOccurred(const QString &err);
    void recordingStarted(const QString &file);
    void recordingStopped();
    void settingsChanged();

private:
    void videoThreadFunc();
    void reconnect();
    void establishSyncAndStartRecording();
    void checkAndStartRecording(); // Check if we have enough buffered data to start recording

    mutable QMutex m_mutex;
    mutable QMutex m_stateMutex;
    SourceSettings m_settings;
    FfmpegWriter m_writer;
    QThread m_videoThread;
    QAtomicInteger<bool> m_running;
    QAtomicInteger<bool> m_previewOnly;  // True if only previewing (not recording)
    QAtomicInteger<bool> m_recordingStarted;
    qint64 m_videoPts = 0;
    qint64 m_expectedFrameTicks10ns = 0;
    qint64 m_expectedPtsStep = 1;
    int m_sourceFpsNum = 60;
    int m_sourceFpsDen = 1;
    QImage m_preview;
    QString m_status;
    QElapsedTimer m_timer;
    QElapsedTimer m_previewThrottle;
    void *m_recv; // NDIlib_recv_instance* - cast in .cpp to avoid MOC issues
    
    // Optimize: reusable AVFrame to avoid per-frame allocation
    struct AVFrame *m_reusableVideoFrame;
    
    // Optimize: moved from static variables for better thread safety
    bool m_videoInfoLogged;
    bool m_audioInfoLogged;
    bool m_metadataSet;
    
    // Sync buffering
    bool m_syncEstablished;
    QVector<BufferedVideoFrame> m_bufferedVideoFrames;
    QVector<BufferedAudioFrame> m_bufferedAudioFrames;
    int64_t m_syncTimestamp; // Common timestamp when sync is established
    QElapsedTimer m_bufferingTimer; // Timer to track buffering duration
    bool m_hasSeenVideo; // Track if we've seen any video frames
    bool m_hasSeenAudio; // Track if we've seen any audio frames
};
