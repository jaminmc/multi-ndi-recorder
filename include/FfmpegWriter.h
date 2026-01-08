#pragma once
#include <QString>
#include <QMutex>
#include <QDateTime>
#include <QVector>
#include <functional>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/macros.h>
#include <libavutil/pixdesc.h>
}

struct AudioStreamInfo
{
    AVStream *stream = nullptr;
    AVCodecContext *codecCtx = nullptr;
    SwrContext *swr = nullptr;
    AVFrame *frame = nullptr;
    int64_t pts = 0;
    int64_t lastDts = AV_NOPTS_VALUE; // Track last DTS written to ensure monotonic increase
    int64_t samplesWritten = 0; // Total samples written to encoder (for timestamp-based PTS calculation)
    int channelIndex = 0; // Which channel from input (0-based)
    bool isStereo = false;
    QVector<float> sampleBuffer; // Buffer to accumulate samples to match encoder frame_size
};

struct RecordingConfig
{
    QString outputFolder;
    QString sourceLabel;
    bool segmented = false;
    int segmentMinutes = 20;
    int width = 1920;
    int height = 1080;
    int fps = 30;
    int fpsNum = 30;
    int fpsDen = 1;
    AVPixelFormat inputPixFmt = AV_PIX_FMT_RGBA;
    AVPixelFormat outputPixFmt = AV_PIX_FMT_YUV420P;
    
    // Video encoding settings
    QString fileExtension = "mp4";
    QString videoCodec = "h264";
    bool useHardwareEncoder = false;
    QString qualityMode = "crf";
    int crfValue = 23;
    int videoBitrate = 12000000;
    int hardwareQuality = 55; // Hardware encoder quality (VideoToolbox: 1-100 higher=better, default 55; others: uses CRF value 0-51 lower=better)
};

class FfmpegWriter
{
public:
    FfmpegWriter();
    ~FfmpegWriter();

    bool start(const RecordingConfig &cfg);
    void stop();
    bool writeVideoFrame(AVFrame *frame);
    bool writeAudioFrame(const float *audioData, int numSamples, int sampleRate, int numChannels);
    bool writeAudioFrameWithTimestamp(const float *audioData, int numSamples, int sampleRate, int numChannels, int64_t ndiTimestamp);
    bool prepareAudioStreams(int sampleRate, int numChannels);
    bool prepareVideoStream(int width, int height);
    void setVideoMetadata(int colorFormat, float pictureAspectRatio, const char *metadata);
    void applyColorMetadata();
    bool needsRollover();
    void rollover();
    void setSyncTimestamp(int64_t ndiTimestamp);

    QString currentFile() const { return m_currentFile; }
    AVRational videoTimeBase() const;
    
    // Static function to query available H.264 and HEVC encoders
    static QVector<QString> getAvailableEncoders();

private:
    bool openContext(const QString &path);
    void closeContext();
    QString nextFileName();
    bool ensureConvertedFrame();
    bool createAudioStream(const AVCodec *codec, int sampleRate, int channels, int bitrate, AudioStreamInfo &info);
    bool writeAudioToStream(AudioStreamInfo &info, const float *channelData, int numSamples, int64_t firstSamplePts);
    bool writeAudioFrameInternal(const float *audioData, int numSamples, int sampleRate, int numChannels, int64_t firstSamplePts);
    
    // Helper functions for video encoding
    struct CodecInfo {
        AVCodecID id;
        const char *name;
        bool isHardware;
    };
    CodecInfo selectCodec(const QString &codecStr, bool useHardware = false); // useHardware parameter kept for compatibility but not used
    AVPixelFormat selectPixelFormat(bool isHardware, bool isHEVC, AVPixelFormat requested);
    void setupCodecOptions(AVDictionary **opts, const QString &codecName, bool isHardware, bool isHEVC, AVPixelFormat pixFmt);
    bool verifyCodec(const AVCodec *codec, AVCodecContext *ctx, AVStream *stream, const CodecInfo &expected);
    bool checkPacketCodec(AVPacket *pkt, bool expectHEVC);

    RecordingConfig m_cfg;
    AVFormatContext *m_fmtCtx;
    AVStream *m_videoStream;
    AVCodecContext *m_videoCodecCtx;
    QVector<AudioStreamInfo> m_audioStreams;
    SwsContext *m_sws;
    AVFrame *m_convertedFrame;
    qint64 m_startMs;
    
    // Video metadata from NDI
    int m_ndiColorFormat;
    float m_ndiPictureAspectRatio;
    QString m_ndiMetadata;
    QString m_currentFile;
    QMutex m_mutex;
    int m_segmentIndex;
    int m_inputWidth;
    int m_inputHeight;
    AVPixelFormat m_inputFormat;
    int m_audioSampleRate;
    int m_currentAudioChannels;
    bool m_headerWritten;
    int m_videoFramesWritten;
    int64_t m_syncTimestamp; // NDI sync timestamp (100ns units) - set by SourceRecorder
    int64_t m_firstVideoPts; // First video PTS written (for normalization to start at 0)
    int64_t m_lastVideoPts; // Last video PTS written (to ensure monotonic increase)
    int64_t m_firstAudioPts; // First audio PTS written (for normalization to start at 0)
    bool m_firstVideoFrameWritten; // Track if first video frame has been written
    bool m_firstAudioFrameWritten; // Track if first audio frame has been written
    bool m_waitingForAudio; // True if we're delaying header write to wait for audio
    int m_videoFramesBeforeHeaderWrite; // Count video frames before writing header (to allow audio to arrive)
};
