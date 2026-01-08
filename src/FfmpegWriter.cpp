#include "FfmpegWriter.h"
#include "Logging.h"
#include <QDir>
#include <QFileInfo>
#include <QDebug>
#include <QVector>
#include <QString>
#include <QSet>
#include <cstring>
#include <cmath>
#include <libavutil/rational.h>

FfmpegWriter::FfmpegWriter()
    : m_fmtCtx(nullptr), m_videoStream(nullptr), m_videoCodecCtx(nullptr),
      m_sws(nullptr), m_convertedFrame(nullptr),
      m_startMs(0), m_segmentIndex(1), m_inputWidth(0), m_inputHeight(0), m_inputFormat(AV_PIX_FMT_NONE),
      m_audioSampleRate(48000), m_currentAudioChannels(0), m_headerWritten(false),
      m_videoFramesWritten(0), m_syncTimestamp(0), m_firstVideoPts(0), m_lastVideoPts(AV_NOPTS_VALUE), m_firstAudioPts(0),
      m_firstVideoFrameWritten(false), m_firstAudioFrameWritten(false), m_waitingForAudio(false),
      m_videoFramesBeforeHeaderWrite(0)
{
    avformat_network_init();
}

FfmpegWriter::~FfmpegWriter()
{
    stop();
    avformat_network_deinit();
}

QString FfmpegWriter::nextFileName()
{
    QDateTime now = QDateTime::currentDateTime();
    QString ts = now.toString("yyyyMMdd_HHmmss");
    QString ext = m_cfg.fileExtension.toLower();
    if (m_cfg.segmented)
    {
        return QString("%1/%2_%3_part%4.%5").arg(m_cfg.outputFolder, m_cfg.sourceLabel, ts, QString::number(m_segmentIndex).rightJustified(2, '0'), ext);
    }
    return QString("%1/%2_%3.%4").arg(m_cfg.outputFolder, m_cfg.sourceLabel, ts, ext);
}

bool FfmpegWriter::openContext(const QString &path)
{
    // Validate path
    if (path.isEmpty())
    {
        Logger::instance().log("Error: Empty file path provided to openContext");
        return false;
    }
    
    // Ensure parent directory exists
    QFileInfo fileInfo(path);
    QDir parentDir = fileInfo.absoluteDir();
    if (!parentDir.exists())
    {
        if (!parentDir.mkpath("."))
        {
            Logger::instance().log(QString("Error: Failed to create directory: %1").arg(parentDir.absolutePath()));
            return false;
        }
    }
    
    // Determine container format from file extension
    QString ext = m_cfg.fileExtension.toLower();
    const char *format = "mp4"; // default
    if (ext == "mov")
        format = "mov";
    else if (ext == "mkv")
        format = "matroska";
    else if (ext == "mp4")
        format = "mp4";
    
    avformat_alloc_output_context2(&m_fmtCtx, nullptr, format, path.toUtf8().constData());
    if (!m_fmtCtx)
    {
        Logger::instance().log(QString("Failed to alloc output context for format: %1").arg(format));
        return false;
    }

    m_videoStream = nullptr;
    m_videoCodecCtx = nullptr;
    
    if (!(m_fmtCtx->oformat->flags & AVFMT_NOFILE))
    {
        if (avio_open(&m_fmtCtx->pb, path.toUtf8().constData(), AVIO_FLAG_WRITE) < 0)
        {
            Logger::instance().log("Failed to open output file");
            return false;
        }
    }

    m_headerWritten = false;
    m_videoFramesWritten = 0;
    m_firstVideoPts = 0;
    m_lastVideoPts = AV_NOPTS_VALUE;
    m_firstAudioPts = 0;
    m_firstVideoFrameWritten = false;
    m_firstAudioFrameWritten = false;
    m_startMs = QDateTime::currentMSecsSinceEpoch();
    return true;
}

AVRational FfmpegWriter::videoTimeBase() const
{
    if (m_videoCodecCtx)
        return m_videoCodecCtx->time_base;
    return AVRational{1, 1};
}

bool FfmpegWriter::start(const RecordingConfig &cfg)
{
    QMutexLocker locker(&m_mutex);
    m_cfg = cfg;
    m_segmentIndex = 1;
    QDir().mkpath(cfg.outputFolder);
    const QString nextFile = nextFileName();
    if (!openContext(nextFile))
    {
        m_currentFile.clear();
        return false;
    }
    m_currentFile = nextFile;
    return true;
}

void FfmpegWriter::closeContext()
{
    if (m_fmtCtx)
    {
        auto flushEncoder = [&](AVCodecContext *ctx, AVStream *stream) {
            if (!ctx || !stream)
                return;
            if (avcodec_send_frame(ctx, nullptr) < 0)
                return;
            AVPacket *pkt = av_packet_alloc();
            if (!pkt)
                return;
            while (avcodec_receive_packet(ctx, pkt) == 0)
            {
                pkt->stream_index = stream->index;
                av_packet_rescale_ts(pkt, ctx->time_base, stream->time_base);
                av_interleaved_write_frame(m_fmtCtx, pkt);
                av_packet_unref(pkt);
            }
            av_packet_free(&pkt);
        };

        if (m_videoCodecCtx && m_videoStream)
        {
            flushEncoder(m_videoCodecCtx, m_videoStream);
        }
        
        // Flush all audio streams - encode remaining buffered samples
        for (auto &audioInfo : m_audioStreams)
        {
            if (audioInfo.codecCtx && audioInfo.stream && !audioInfo.sampleBuffer.isEmpty())
            {
                int frameSize = audioInfo.codecCtx->frame_size;
                if (frameSize <= 0)
                    frameSize = 1024;
                
                int numChannels = audioInfo.codecCtx->ch_layout.nb_channels;
                int remainingSamples = audioInfo.sampleBuffer.size() / numChannels;
                
                if (remainingSamples > 0 && audioInfo.frame)
                {
                    int samplesToSend = remainingSamples < frameSize ? remainingSamples : frameSize;
                    
                    for (int c = 0; c < numChannels; ++c)
                    {
                        float *encoderChannel = (float *)audioInfo.frame->data[c];
                        for (int s = 0; s < samplesToSend; ++s)
                        {
                            encoderChannel[s] = audioInfo.sampleBuffer[s * numChannels + c];
                        }
                        for (int s = samplesToSend; s < frameSize; ++s)
                        {
                            encoderChannel[s] = 0.0f;
                        }
                    }
                    
                    audioInfo.frame->pts = audioInfo.pts;
                    audioInfo.frame->nb_samples = samplesToSend;
                    
                    avcodec_send_frame(audioInfo.codecCtx, audioInfo.frame);
                    audioInfo.sampleBuffer.clear();
                }
            }
        }
        
        // Flush all audio encoders
        for (auto &audioInfo : m_audioStreams)
        {
            if (audioInfo.codecCtx && audioInfo.stream)
            {
                flushEncoder(audioInfo.codecCtx, audioInfo.stream);
            }
        }
        
        av_write_trailer(m_fmtCtx);
        if (!(m_fmtCtx->oformat->flags & AVFMT_NOFILE))
        {
            avio_closep(&m_fmtCtx->pb);
        }
        avformat_free_context(m_fmtCtx);
    }
    m_fmtCtx = nullptr;
    if (m_videoCodecCtx)
    {
        avcodec_free_context(&m_videoCodecCtx);
        m_videoCodecCtx = nullptr;
    }
    m_videoStream = nullptr;
    for (auto &audioInfo : m_audioStreams)
    {
        if (audioInfo.swr)
            swr_free(&audioInfo.swr);
        if (audioInfo.frame)
        {
            av_channel_layout_uninit(&audioInfo.frame->ch_layout);
            av_frame_free(&audioInfo.frame);
        }
        if (audioInfo.codecCtx)
        {
            av_channel_layout_uninit(&audioInfo.codecCtx->ch_layout);
            avcodec_free_context(&audioInfo.codecCtx);
        }
        audioInfo.sampleBuffer.clear();
    }
    m_audioStreams.clear();
    if (m_sws)
    {
        sws_freeContext(m_sws);
        m_sws = nullptr;
    }
    if (m_convertedFrame)
    {
        av_frame_free(&m_convertedFrame);
    }
}

void FfmpegWriter::stop()
{
    QMutexLocker locker(&m_mutex);
    closeContext();
    m_currentFile.clear();
    m_currentAudioChannels = 0;
    m_headerWritten = false;
    m_videoFramesWritten = 0;
    m_segmentIndex = 1;
    m_syncTimestamp = 0;
    m_firstVideoPts = 0;
    m_lastVideoPts = AV_NOPTS_VALUE;
    m_firstAudioPts = 0;
    m_firstVideoFrameWritten = false;
    m_firstAudioFrameWritten = false;
    m_ndiColorFormat = 0;
    m_ndiPictureAspectRatio = 0.0f;
    m_ndiMetadata = QString();
}

bool FfmpegWriter::prepareVideoStream(int width, int height)
{
    if (!m_fmtCtx)
        return false;
    
    QMutexLocker locker(&m_mutex);
    
    if (m_headerWritten)
    {
        Logger::instance().log("Cannot prepare video stream after header is written");
        return false;
    }
    
    if (m_videoCodecCtx && m_videoStream)
        return true; // Already prepared
    
    // Select codec based on configuration (hardware detection is now automatic from codec name)
    CodecInfo codecInfo = selectCodec(m_cfg.videoCodec, false);
    bool isHEVC = (codecInfo.id == AV_CODEC_ID_H265);
    
    // Find encoder by name (codecInfo.name is already set from selectCodec)
    const AVCodec *videoCodec = avcodec_find_encoder_by_name(codecInfo.name);
    if (!videoCodec)
    {
        // Fallback to codec ID lookup
        videoCodec = avcodec_find_encoder(codecInfo.id);
    }
    
    if (!videoCodec)
    {
        Logger::instance().log(QString("ERROR: Video codec not found: %1 (ID: %2)").arg(codecInfo.name).arg(codecInfo.id));
        return false;
    }
    
    // Create stream
    m_videoStream = avformat_new_stream(m_fmtCtx, videoCodec);
    if (!m_videoStream)
    {
        Logger::instance().log("Failed to create video stream");
        return false;
    }
    
    // Allocate codec context
    m_videoCodecCtx = avcodec_alloc_context3(videoCodec);
    m_videoCodecCtx->codec_id = codecInfo.id;
    m_videoCodecCtx->width = width > 0 ? width : m_cfg.width;
    m_videoCodecCtx->height = height > 0 ? height : m_cfg.height;
    m_videoCodecCtx->pix_fmt = selectPixelFormat(codecInfo.isHardware, isHEVC, m_cfg.outputPixFmt);
    m_videoCodecCtx->time_base = {m_cfg.fpsDen, m_cfg.fpsNum};
    m_videoCodecCtx->framerate = {m_cfg.fpsNum, m_cfg.fpsDen};
    m_videoCodecCtx->gop_size = m_cfg.fps;
    m_videoCodecCtx->max_b_frames = 0;
    
    if (m_cfg.qualityMode == "bitrate")
    {
        m_videoCodecCtx->bit_rate = m_cfg.videoBitrate;
    }
    
    if (m_fmtCtx->oformat->flags & AVFMT_GLOBALHEADER)
        m_videoCodecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    
    // Setup codec options
    AVDictionary *videoOpts = nullptr;
    setupCodecOptions(&videoOpts, m_cfg.videoCodec, codecInfo.isHardware, isHEVC, m_videoCodecCtx->pix_fmt);
    
    // Open codec
    int ret = avcodec_open2(m_videoCodecCtx, videoCodec, &videoOpts);
    if (ret < 0)
    {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        Logger::instance().log(QString("ERROR: Failed to open video codec: %1").arg(errbuf));
        av_dict_free(&videoOpts);
        return false;
    }
    av_dict_free(&videoOpts);
    
    // Verify codec matches expectations
    if (!verifyCodec(videoCodec, m_videoCodecCtx, m_videoStream, codecInfo))
    {
        avcodec_free_context(&m_videoCodecCtx);
        return false;
    }
    
    // Copy parameters to stream
    if (avcodec_parameters_from_context(m_videoStream->codecpar, m_videoCodecCtx) < 0)
    {
        Logger::instance().log("Failed to copy video params");
        return false;
    }
    
    // Ensure stream parameters match
    m_videoStream->codecpar->codec_id = codecInfo.id;
    
    // Set codec tag for HEVC in .mov and .mp4 files (hvc1)
    if (codecInfo.id == AV_CODEC_ID_H265)
    {
        QString ext = m_cfg.fileExtension.toLower();
        if (ext == "mov" || ext == "mp4")
        {
            m_videoStream->codecpar->codec_tag = MKTAG('h', 'v', 'c', '1');
        }
    }
    
    m_videoStream->time_base = m_videoCodecCtx->time_base;
    m_videoStream->avg_frame_rate = {m_cfg.fpsNum, m_cfg.fpsDen};
    m_videoStream->r_frame_rate = {m_cfg.fpsNum, m_cfg.fpsDen};
    
    // Apply color metadata from NDI if available
    applyColorMetadata();
    
    Logger::instance().log(QString("Video stream prepared: codec=%1, hardware=%2, codec_tag=%3")
                          .arg(codecInfo.name)
                          .arg(codecInfo.isHardware ? "yes" : "no")
                          .arg(codecInfo.id == AV_CODEC_ID_H265 ? "hvc1" : "none"));
    
    return true;
}

bool FfmpegWriter::writeVideoFrame(AVFrame *frame)
{
    QMutexLocker locker(&m_mutex);
    if (!m_fmtCtx)
        return false;
    
    // Create video stream lazily on first video frame if not already prepared
    if (!m_videoCodecCtx || !m_videoStream)
    {
        if (m_headerWritten)
        {
            return false; // Can't create video stream after header written
        }
        
        // Use frame dimensions to prepare stream
        if (!prepareVideoStream(frame->width, frame->height))
        {
            return false;
        }
    }
    
    frame->width = m_videoCodecCtx->width;
    frame->height = m_videoCodecCtx->height;

    if (!m_sws || m_inputWidth != frame->width || m_inputHeight != frame->height || m_inputFormat != frame->format)
    {
        m_sws = sws_getCachedContext(m_sws, frame->width, frame->height, (AVPixelFormat)frame->format,
                                     m_videoCodecCtx->width, m_videoCodecCtx->height, m_videoCodecCtx->pix_fmt,
                                     SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!m_sws)
            return false;
        m_inputWidth = frame->width;
        m_inputHeight = frame->height;
        m_inputFormat = (AVPixelFormat)frame->format;
    }

    if (!ensureConvertedFrame())
        return false;
    if (av_frame_make_writable(m_convertedFrame) < 0)
        return false;
    if (sws_scale(m_sws, frame->data, frame->linesize, 0, frame->height, m_convertedFrame->data, m_convertedFrame->linesize) <= 0)
    {
        return false;
    }

    // Use PTS directly from NDI timestamp (already calculated relative to sync point)
    // Ensure PTS is strictly increasing to prevent frame drops
    if (!m_firstVideoFrameWritten)
    {
        m_firstVideoPts = frame->pts;
        m_lastVideoPts = frame->pts;
        m_convertedFrame->pts = frame->pts;
        m_firstVideoFrameWritten = true;
        Logger::instance().verbose(QString("First video frame PTS: %1").arg(frame->pts));
    }
    else
    {
        // Ensure PTS is strictly increasing (at least 1 frame apart)
        // This prevents duplicate PTS values that cause frames to be dropped
        if (frame->pts <= m_lastVideoPts)
        {
            // PTS didn't advance - use incremental PTS instead
            m_convertedFrame->pts = m_lastVideoPts + 1;
            Logger::instance().verbose(QString("PTS didn't advance (frame PTS: %1, last: %2), using incremental: %3")
                                      .arg(frame->pts).arg(m_lastVideoPts).arg(m_convertedFrame->pts));
            m_lastVideoPts = m_convertedFrame->pts;
        }
        else
        {
            // PTS advanced correctly
            m_convertedFrame->pts = frame->pts;
            m_lastVideoPts = frame->pts;
        }
    }

    // Verify codec matches what was requested
    CodecInfo expectedCodecInfo = selectCodec(m_cfg.videoCodec, false);
    if (m_videoCodecCtx->codec && m_videoCodecCtx->codec->name)
    {
        const char *actualName = m_videoCodecCtx->codec->name;
        QString actualCodecName = QString::fromUtf8(actualName).toLower();
        QString expectedCodecName = QString::fromUtf8(expectedCodecInfo.name).toLower();
        
        // Check if the actual codec matches what we requested
        if (actualCodecName != expectedCodecName)
        {
            Logger::instance().log(QString("WARNING: Codec mismatch - requested %1, got %2").arg(expectedCodecInfo.name).arg(actualName));
            // Don't fail - FFmpeg may have selected a compatible codec
        }
    }
    
    int ret = avcodec_send_frame(m_videoCodecCtx, m_convertedFrame);
    if (ret < 0)
    {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        Logger::instance().log(QString("ERROR: Failed to send frame to codec %1: %2 (error code: %3)").arg(m_videoCodecCtx->codec->name ? m_videoCodecCtx->codec->name : "unknown").arg(errbuf).arg(ret));
        return false;
    }
    
    AVPacket *pkt = av_packet_alloc();
    if (!pkt)
        return false;
        
    while ((ret = avcodec_receive_packet(m_videoCodecCtx, pkt)) == 0)
    {
        // Verify first packet is correct codec (if HEVC was requested)
        QString normalizedCodec = m_cfg.videoCodec.trimmed().toLower();
        bool expectHEVC = (normalizedCodec == "h265" || normalizedCodec == "hevc");
        
        if (m_videoFramesWritten == 0 && expectHEVC)
        {
            if (!checkPacketCodec(pkt, true))
            {
                av_packet_free(&pkt);
                return false;
            }
        }
        
        // Write header on first packet
        // If audio streams exist, include them. If not, write video-only header.
        // Note: We can't add audio streams after header is written (MP4 limitation)
        // Write header on first packet
        if (!m_headerWritten)
        {
            // Ensure codec ID and tag are correct before writing header
            if (expectHEVC)
            {
                m_videoStream->codecpar->codec_id = AV_CODEC_ID_H265;
                m_videoCodecCtx->codec_id = AV_CODEC_ID_H265;
                
                // Set hvc1 tag for .mov and .mp4 files
                QString ext = m_cfg.fileExtension.toLower();
                if (ext == "mov" || ext == "mp4")
                {
                    m_videoStream->codecpar->codec_tag = MKTAG('h', 'v', 'c', '1');
                }
            }
            
            if (avformat_write_header(m_fmtCtx, nullptr) < 0)
            {
                Logger::instance().log("Failed to write header");
                av_packet_free(&pkt);
                return false;
            }
            
            m_headerWritten = true;
            m_waitingForAudio = false;
        }
        
        // Ensure codec ID and tag are correct before writing packet
        if (expectHEVC)
        {
            m_videoStream->codecpar->codec_id = AV_CODEC_ID_H265;
            m_videoCodecCtx->codec_id = AV_CODEC_ID_H265;
            
            QString ext = m_cfg.fileExtension.toLower();
            if (ext == "mov" || ext == "mp4")
            {
                m_videoStream->codecpar->codec_tag = MKTAG('h', 'v', 'c', '1');
            }
        }
        
        if (pkt->pts == AV_NOPTS_VALUE)
        {
            pkt->pts = m_convertedFrame->pts;
        }
        if (pkt->dts == AV_NOPTS_VALUE)
        {
            pkt->dts = pkt->pts;
        }
        
        pkt->stream_index = m_videoStream->index;
        av_packet_rescale_ts(pkt, m_videoCodecCtx->time_base, m_videoStream->time_base);
        
        // Calculate proper duration based on frame rate
        // Duration in stream time_base units = (1 frame) / (frames per second)
        // Since stream time_base = codec time_base = {fpsDen, fpsNum}, duration = 1
        if (pkt->duration <= 0)
        {
            pkt->duration = av_rescale_q(1, m_videoCodecCtx->time_base, m_videoStream->time_base);
            if (pkt->duration <= 0)
                pkt->duration = 1;
        }
        
        // Verify packet codec (for HEVC, check NAL units) - only on first packet to avoid performance issues
        // If it fails, log but don't reject (to avoid dropping frames)
        if (expectHEVC && m_videoFramesWritten == 0 && !checkPacketCodec(pkt, true))
        {
            Logger::instance().log("WARNING: First packet failed NAL unit check, but continuing");
            // Don't fail - let FFmpeg handle it
        }
        
        if (av_interleaved_write_frame(m_fmtCtx, pkt) < 0)
        {
            Logger::instance().log("ERROR: Failed to write video packet");
            av_packet_free(&pkt);
            return false;
        }
        av_packet_unref(pkt);
        m_videoFramesWritten++;
    }
    
    if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
    {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        Logger::instance().log(QString("ERROR: Failed to receive packet from codec: %1 (error code: %2)").arg(errbuf).arg(ret));
        av_packet_free(&pkt);
        return false;
    }
    
    av_packet_free(&pkt);
    return true;
}

bool FfmpegWriter::writeAudioFrameWithTimestamp(const float *audioData, int numSamples, int sampleRate, int numChannels, int64_t ndiTimestamp)
{
    if (!m_fmtCtx || !audioData || numSamples <= 0 || numChannels <= 0)
        return false;

    QMutexLocker locker(&m_mutex);
    
    // Calculate audio PTS directly from NDI timestamp relative to sync point
    // NDI timestamps are in 100ns units (10,000,000 per second)
    // Audio PTS is in samples (time_base = {1, codecSampleRate})
    // IMPORTANT: Use codec sample rate for PTS calculation, not incoming sample rate
    // This ensures PTS matches the encoded stream's sample rate
    int64_t audioPts = 0;
    int codecSampleRate = sampleRate; // Default to incoming rate
    if (!m_audioStreams.isEmpty() && m_audioStreams[0].codecCtx)
    {
        codecSampleRate = m_audioStreams[0].codecCtx->sample_rate;
    }
    
    if (m_syncTimestamp > 0)
    {
        // Handle both cases: timestamp before and after sync point
        int64_t timeSinceSync = ndiTimestamp - m_syncTimestamp;
        // Convert from 100ns units to audio samples using codec sample rate
        // samples = (time_ns * codecSampleRate) / 10000000
        audioPts = (timeSinceSync * codecSampleRate) / 10000000LL;
        // Ensure PTS is non-negative (skip samples before sync point)
        if (audioPts < 0)
            audioPts = 0;
    }
    else
    {
        // No sync timestamp set yet, use incremental PTS
        if (!m_audioStreams.isEmpty())
        {
            int numCh = m_audioStreams[0].codecCtx ? m_audioStreams[0].codecCtx->ch_layout.nb_channels : 1;
            int buffered = m_audioStreams[0].sampleBuffer.size() / numCh;
            audioPts = m_audioStreams[0].pts + buffered;
        }
    }
    
    // Log sample rate mismatch if detected
    if (sampleRate != codecSampleRate)
    {
        Logger::instance().verbose(QString("Audio sample rate mismatch: incoming=%1 Hz, codec=%2 Hz. PTS calculated using codec rate.")
                                   .arg(sampleRate).arg(codecSampleRate));
    }
    
    return writeAudioFrameInternal(audioData, numSamples, sampleRate, numChannels, audioPts);
}

bool FfmpegWriter::writeAudioFrame(const float *audioData, int numSamples, int sampleRate, int numChannels)
{
    if (!m_fmtCtx || !audioData || numSamples <= 0 || numChannels <= 0)
        return false;
    
    QMutexLocker locker(&m_mutex);
    // For non-timestamped audio, use incremental PTS
    int64_t audioPts = 0;
    if (!m_audioStreams.isEmpty())
    {
        int numCh = m_audioStreams[0].codecCtx ? m_audioStreams[0].codecCtx->ch_layout.nb_channels : 1;
        int buffered = m_audioStreams[0].sampleBuffer.size() / numCh;
        audioPts = m_audioStreams[0].pts + buffered;
    }
    return writeAudioFrameInternal(audioData, numSamples, sampleRate, numChannels, audioPts);
}

bool FfmpegWriter::writeAudioFrameInternal(const float *audioData, int numSamples, int sampleRate, int numChannels, int64_t firstSamplePts)
{
    if (!m_fmtCtx || !audioData || numSamples <= 0 || numChannels <= 0)
        return false;
    
    // Note: Mutex is already locked by writeAudioFrameWithTimestamp or writeAudioFrame
    
    // Initialize audio streams on first frame
    // Check if we need to recreate streams due to channel count or sample rate mismatch
    bool needRecreate = m_audioStreams.isEmpty() || 
                       m_currentAudioChannels != numChannels ||
                       (m_audioSampleRate > 0 && m_audioSampleRate != sampleRate);
    
    if (needRecreate)
    {
        // If header is already written and we're trying to add audio, we can't do it
        // This happens when audio arrives after video-only recording has started
        if (m_headerWritten && m_audioStreams.isEmpty())
        {
            // Header already written without audio - can't add audio now (MP4 limitation)
            Logger::instance().log("Warning: Audio arrived after header written - cannot add audio stream. Recording will be video-only.");
            return false;
        }
        
        // If we're waiting for audio and header isn't written yet, create audio streams now
        if (m_waitingForAudio && !m_headerWritten)
        {
            // Good timing - audio arrived before header was written
            // Create audio streams, then header will be written on next video frame
        }
        
        if (m_headerWritten && !m_audioStreams.isEmpty())
        {
            if (m_currentAudioChannels != numChannels)
            {
                Logger::instance().log("Cannot change audio channel count after header is written");
                return false;
            }
            if (m_audioSampleRate > 0 && m_audioSampleRate != sampleRate)
            {
                Logger::instance().log(QString("WARNING: Audio sample rate mismatch - codec=%1 Hz, incoming=%2 Hz. Using codec rate.")
                                      .arg(m_audioSampleRate).arg(sampleRate));
                // Don't fail, but use the codec's sample rate for resampling
            }
        }
        
        // Close existing streams if channel count or sample rate changed
        if (!m_audioStreams.isEmpty())
        {
            for (auto &audioInfo : m_audioStreams)
            {
                if (audioInfo.swr)
                    swr_free(&audioInfo.swr);
                if (audioInfo.frame)
                {
                    av_channel_layout_uninit(&audioInfo.frame->ch_layout);
                    av_frame_free(&audioInfo.frame);
                }
                if (audioInfo.codecCtx)
                {
                    av_channel_layout_uninit(&audioInfo.codecCtx->ch_layout);
                    avcodec_free_context(&audioInfo.codecCtx);
                }
            }
            m_audioStreams.clear();
        }

        m_currentAudioChannels = numChannels;
        m_audioSampleRate = sampleRate;

        const AVCodec *audioCodec = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (!audioCodec)
        {
            Logger::instance().log("AAC encoder not found");
            return false;
        }

        if (numChannels == 1)
        {
            AudioStreamInfo info;
            info.channelIndex = 0;
            info.isStereo = false;
            if (createAudioStream(audioCodec, sampleRate, 1, 192000, info))
                m_audioStreams.append(info);
        }
        else if (numChannels == 2)
        {
            AudioStreamInfo info;
            info.channelIndex = 0;
            info.isStereo = true;
            if (createAudioStream(audioCodec, sampleRate, 2, 320000, info))
                m_audioStreams.append(info);
        }
        else
        {
            for (int i = 0; i < numChannels && i < 16; ++i)
            {
                AudioStreamInfo info;
                info.channelIndex = i;
                info.isStereo = false;
                if (createAudioStream(audioCodec, sampleRate, 1, 192000, info))
                    m_audioStreams.append(info);
            }
        }

        if (m_audioStreams.isEmpty())
        {
            Logger::instance().log("Failed to create any audio streams");
            return false;
        }
        
        // If header wasn't written yet and we just created audio streams, 
        // the next video frame will write the header with both streams
        if (m_waitingForAudio && !m_headerWritten)
        {
            Logger::instance().verbose("Audio streams created before header write - both streams will be included");
        }
    }

    // Write audio data to streams
    bool success = true;
    for (auto &audioInfo : m_audioStreams)
    {
        if (audioInfo.isStereo)
        {
            // Stereo: pass interleaved data directly [L0, R0, L1, R1, ...]
            success = writeAudioToStream(audioInfo, audioData, numSamples, firstSamplePts) && success;
        }
        else
        {
            // Mono: extract single channel from interleaved multi-channel data
            // NDI interleaved format: [Ch0_S0, Ch1_S0, Ch2_S0, Ch3_S0, Ch0_S1, Ch1_S1, Ch2_S1, Ch3_S1, ...]
            // For 4 channels: buffer indices are [0,1,2,3, 4,5,6,7, 8,9,10,11, ...]
            // For channel 0: samples at 0, 4, 8, 12, ... (i*numChannels + 0)
            // For channel 1: samples at 1, 5, 9, 13, ... (i*numChannels + 1)
            // For channel 2: samples at 2, 6, 10, 14, ... (i*numChannels + 2)
            // For channel 3: samples at 3, 7, 11, 15, ... (i*numChannels + 3)
            QVector<float> monoData(numSamples);
            float *dst = monoData.data();
            int channelIdx = audioInfo.channelIndex;
            
            // Validate inputs
            if (channelIdx < 0 || channelIdx >= numChannels)
            {
                Logger::instance().log(QString("ERROR: Invalid channel index %1 for %2 channels").arg(channelIdx).arg(numChannels));
                return false;
            }
            
            // Extract samples for this channel from interleaved buffer
            // Interleaved format: [Ch0_S0, Ch1_S0, Ch2_S0, Ch3_S0, Ch0_S1, Ch1_S1, ...]
            // Formula: srcIdx = sampleIndex * numChannels + channelIndex
            for (int i = 0; i < numSamples; ++i)
            {
                int srcIdx = i * numChannels + channelIdx;
                if (srcIdx >= numSamples * numChannels)
                {
                    Logger::instance().log(QString("ERROR: Source index %1 out of bounds for %2 samples, %3 channels")
                                          .arg(srcIdx).arg(numSamples).arg(numChannels));
                    return false;
                }
                dst[i] = audioData[srcIdx];
            }
            
            // Pass extracted mono data to resampler
            success = writeAudioToStream(audioInfo, monoData.constData(), numSamples, firstSamplePts) && success;
        }
    }
    return success;
}

bool FfmpegWriter::prepareAudioStreams(int sampleRate, int numChannels)
{
    if (!m_fmtCtx || numChannels <= 0)
        return false;
    
    QMutexLocker locker(&m_mutex);
    
    if (m_headerWritten)
    {
        Logger::instance().log("Cannot prepare audio streams after header is written");
        return false;
    }
    
    if (!m_audioStreams.isEmpty() && m_currentAudioChannels == numChannels)
        return true;
    
    if (!m_audioStreams.isEmpty())
    {
        for (auto &audioInfo : m_audioStreams)
        {
            if (audioInfo.swr)
                swr_free(&audioInfo.swr);
            if (audioInfo.frame)
            {
                av_channel_layout_uninit(&audioInfo.frame->ch_layout);
                av_frame_free(&audioInfo.frame);
            }
            if (audioInfo.codecCtx)
            {
                av_channel_layout_uninit(&audioInfo.codecCtx->ch_layout);
                avcodec_free_context(&audioInfo.codecCtx);
            }
        }
        m_audioStreams.clear();
    }
    
    const AVCodec *audioCodec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!audioCodec)
    {
        Logger::instance().log("AAC encoder not found");
        return false;
    }
    
    if (numChannels == 1)
    {
        AudioStreamInfo info;
        if (!createAudioStream(audioCodec, sampleRate, 1, 192000, info))
            return false;
        info.channelIndex = 0;
        info.isStereo = false;
        m_audioStreams.append(info);
    }
    else if (numChannels == 2)
    {
        AudioStreamInfo info;
        if (!createAudioStream(audioCodec, sampleRate, 2, 320000, info))
            return false;
        info.channelIndex = 0;
        info.isStereo = true;
        m_audioStreams.append(info);
    }
    else
    {
        for (int i = 0; i < numChannels; ++i)
        {
            AudioStreamInfo info;
            if (!createAudioStream(audioCodec, sampleRate, 1, 192000, info))
                return false;
            info.channelIndex = i;
            info.isStereo = false;
            m_audioStreams.append(info);
        }
    }
    
    m_currentAudioChannels = numChannels;
    m_audioSampleRate = sampleRate;
    
    Logger::instance().verbose(QString("Audio streams prepared: sampleRate=%1 Hz, channels=%2")
                              .arg(sampleRate).arg(numChannels));
    
    return true;
}

bool FfmpegWriter::createAudioStream(const AVCodec *codec, int sampleRate, int channels, int bitrate, AudioStreamInfo &info)
{
    info.stream = avformat_new_stream(m_fmtCtx, codec);
    if (!info.stream)
    {
        Logger::instance().log("Failed to create audio stream");
        return false;
    }

    info.codecCtx = avcodec_alloc_context3(codec);
    info.codecCtx->codec_id = AV_CODEC_ID_AAC;
    info.codecCtx->codec_type = AVMEDIA_TYPE_AUDIO;
    info.codecCtx->sample_fmt = AV_SAMPLE_FMT_FLTP;
    info.codecCtx->sample_rate = sampleRate;
    info.codecCtx->bit_rate = bitrate;
    info.codecCtx->time_base = {1, sampleRate};

    av_channel_layout_uninit(&info.codecCtx->ch_layout);
    if (channels == 2)
        av_channel_layout_from_mask(&info.codecCtx->ch_layout, AV_CH_LAYOUT_STEREO);
    else
        av_channel_layout_from_mask(&info.codecCtx->ch_layout, AV_CH_LAYOUT_MONO);

    if (m_fmtCtx->oformat->flags & AVFMT_GLOBALHEADER)
        info.codecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if (avcodec_open2(info.codecCtx, codec, nullptr) < 0)
    {
        Logger::instance().log(QString("Failed to open audio codec for %1 channel stream").arg(channels));
        av_channel_layout_uninit(&info.codecCtx->ch_layout);
        avcodec_free_context(&info.codecCtx);
        return false;
    }

    if (avcodec_parameters_from_context(info.stream->codecpar, info.codecCtx) < 0)
    {
        Logger::instance().log("Failed to copy audio params");
        av_channel_layout_uninit(&info.codecCtx->ch_layout);
        avcodec_free_context(&info.codecCtx);
        return false;
    }

    info.stream->time_base = info.codecCtx->time_base;
    info.pts = 0;
    info.lastDts = AV_NOPTS_VALUE;
    info.samplesWritten = 0;
    
    Logger::instance().verbose(QString("Created audio stream: codec_sample_rate=%1 Hz, stream_time_base=%2/%3, channels=%4")
                              .arg(info.codecCtx->sample_rate)
                              .arg(info.stream->time_base.num)
                              .arg(info.stream->time_base.den)
                              .arg(channels));
    
    return true;
}

void FfmpegWriter::setSyncTimestamp(int64_t ndiTimestamp)
{
    QMutexLocker locker(&m_mutex);
    m_syncTimestamp = ndiTimestamp;
    // Reset PTS tracking when sync timestamp is set
    m_firstVideoPts = 0;
    m_lastVideoPts = AV_NOPTS_VALUE;
    m_firstAudioPts = 0;
    m_firstVideoFrameWritten = false;
    m_firstAudioFrameWritten = false;
    for (auto &audioInfo : m_audioStreams)
    {
        audioInfo.pts = 0;
        audioInfo.samplesWritten = 0;
        audioInfo.lastDts = AV_NOPTS_VALUE;
        audioInfo.sampleBuffer.clear();
    }
}

bool FfmpegWriter::writeAudioToStream(AudioStreamInfo &info, const float *channelData, int numSamples, int64_t firstSamplePts)
{
    if (!info.codecCtx || !info.stream || !channelData || numSamples <= 0)
        return false;
    
    // Note: Mutex is already held by writeAudioFrameInternal

    // Initialize or reinitialize resampler if sample rate changed
    // Use the actual input sample rate (m_audioSampleRate) for input, codec sample rate for output
    int inputSampleRate = m_audioSampleRate > 0 ? m_audioSampleRate : info.codecCtx->sample_rate;
    int outputSampleRate = info.codecCtx->sample_rate;
    
    bool needReinitResampler = false;
    if (!info.swr)
    {
        needReinitResampler = true;
    }
    else
    {
        // Check if sample rate changed - need to reinitialize resampler
        // We can't directly query swr for its input rate, so we'll reinit if m_audioSampleRate changed
        // This is a bit inefficient but ensures correctness
        needReinitResampler = (inputSampleRate != outputSampleRate);
    }
    
    if (needReinitResampler)
    {
        if (info.swr)
        {
            swr_free(&info.swr);
            info.swr = nullptr;
        }
        
        AVChannelLayout inLayout = {}, outLayout = {};
        av_channel_layout_uninit(&inLayout);
        av_channel_layout_uninit(&outLayout);

        int inChannels = info.isStereo ? 2 : 1;
        int outChannels = info.codecCtx->ch_layout.nb_channels;

        if (inChannels == 2)
            av_channel_layout_from_mask(&inLayout, AV_CH_LAYOUT_STEREO);
        else
            av_channel_layout_from_mask(&inLayout, AV_CH_LAYOUT_MONO);

        if (outChannels == 2)
            av_channel_layout_from_mask(&outLayout, AV_CH_LAYOUT_STEREO);
        else
            av_channel_layout_from_mask(&outLayout, AV_CH_LAYOUT_MONO);

        Logger::instance().verbose(QString("Initializing audio resampler: input=%1 Hz, output=%2 Hz, channels=%3")
                                  .arg(inputSampleRate).arg(outputSampleRate).arg(inChannels));
        
        swr_alloc_set_opts2(&info.swr,
                           &outLayout, AV_SAMPLE_FMT_FLTP, outputSampleRate,
                           &inLayout, AV_SAMPLE_FMT_FLT, inputSampleRate,
                           0, nullptr);
        if (swr_init(info.swr) < 0)
        {
            Logger::instance().log(QString("Failed to initialize audio resampler: input=%1 Hz, output=%2 Hz")
                                  .arg(inputSampleRate).arg(outputSampleRate));
            av_channel_layout_uninit(&inLayout);
            av_channel_layout_uninit(&outLayout);
            swr_free(&info.swr);
            return false;
        }
        av_channel_layout_uninit(&inLayout);
        av_channel_layout_uninit(&outLayout);
    }

    int frameSize = info.codecCtx->frame_size;
    if (frameSize <= 0)
        frameSize = 1024;

    if (!info.frame || info.frame->nb_samples != frameSize)
    {
        if (info.frame)
        {
            av_channel_layout_uninit(&info.frame->ch_layout);
            av_frame_free(&info.frame);
        }
        info.frame = av_frame_alloc();
        if (!info.frame)
            return false;
        info.frame->format = AV_SAMPLE_FMT_FLTP;
        info.frame->sample_rate = info.codecCtx->sample_rate;
        av_channel_layout_uninit(&info.frame->ch_layout);
        if (info.codecCtx->ch_layout.nb_channels == 2)
            av_channel_layout_from_mask(&info.frame->ch_layout, AV_CH_LAYOUT_STEREO);
        else
            av_channel_layout_from_mask(&info.frame->ch_layout, AV_CH_LAYOUT_MONO);
        info.frame->nb_samples = frameSize;
        if (av_frame_get_buffer(info.frame, 0) < 0)
        {
            av_channel_layout_uninit(&info.frame->ch_layout);
            av_frame_free(&info.frame);
            return false;
        }
    }

    // Setup input data for resampler
    // For AV_SAMPLE_FMT_FLT (interleaved float), all channels are interleaved in one buffer
    // NDI provides interleaved data: [Ch0_S0, Ch1_S0, Ch2_S0, Ch3_S0, Ch0_S1, Ch1_S1, ...] for multi-channel
    // For mono (extracted from interleaved), channelData is sequential [S0, S1, S2, ...]
    // Use swr_convert_frame with AVFrame for proper stride handling
    AVFrame *inFrame = av_frame_alloc();
    if (!inFrame)
        return false;
    
    int inChannels = info.isStereo ? 2 : 1;
    inFrame->format = AV_SAMPLE_FMT_FLT;
    inFrame->sample_rate = inputSampleRate;
    inFrame->nb_samples = numSamples;
    
    // Set channel layout for input
    av_channel_layout_uninit(&inFrame->ch_layout);
    if (inChannels == 2)
        av_channel_layout_from_mask(&inFrame->ch_layout, AV_CH_LAYOUT_STEREO);
    else
        av_channel_layout_from_mask(&inFrame->ch_layout, AV_CH_LAYOUT_MONO);
    
    // Allocate buffer and copy data
    if (av_frame_get_buffer(inFrame, 0) < 0)
    {
        av_channel_layout_uninit(&inFrame->ch_layout);
        av_frame_free(&inFrame);
        return false;
    }
    
    // Copy input data to frame
    // For interleaved format (stereo), all channels are in one buffer: [L0, R0, L1, R1, ...]
    // For mono (extracted from interleaved), channelData is sequential: [S0, S1, S2, ...]
    // AVFrame with AV_SAMPLE_FMT_FLT interleaved expects data in interleaved format
    // For mono, interleaved and sequential are the same, so copy numSamples * sizeof(float) bytes
    // For stereo, copy numSamples * 2 * sizeof(float) bytes (interleaved)
    // IMPORTANT: channelData for mono is already extracted (sequential), so copy exactly numSamples floats
    // For stereo, channelData is interleaved, so copy numSamples * 2 floats
    int copySize = numSamples * inChannels * sizeof(float);
    memcpy(inFrame->data[0], channelData, copySize);
    
    // Calculate output samples based on actual input/output sample rate ratio
    int maxOutputSamples = av_rescale_rnd(numSamples, outputSampleRate, inputSampleRate, AV_ROUND_UP);
    maxOutputSamples += 256;
    
    AVFrame *tempFrame = av_frame_alloc();
    if (!tempFrame)
    {
        av_channel_layout_uninit(&inFrame->ch_layout);
        av_frame_free(&inFrame);
        return false;
    }
    tempFrame->format = AV_SAMPLE_FMT_FLTP;
    tempFrame->sample_rate = info.codecCtx->sample_rate;
    av_channel_layout_copy(&tempFrame->ch_layout, &info.codecCtx->ch_layout);
    tempFrame->nb_samples = maxOutputSamples;
    if (av_frame_get_buffer(tempFrame, 0) < 0)
    {
        av_channel_layout_uninit(&tempFrame->ch_layout);
        av_frame_free(&tempFrame);
        av_channel_layout_uninit(&inFrame->ch_layout);
        av_frame_free(&inFrame);
        return false;
    }
    
    // Convert from interleaved input (AV_SAMPLE_FMT_FLT) to planar output (AV_SAMPLE_FMT_FLTP)
    int outputSamples = 0;
    
    // If sample rates match, do direct format conversion (interleaved -> planar) without resampling
    // This avoids potential issues with the resampler when no rate conversion is needed
    if (inputSampleRate == outputSampleRate)
    {
        // Direct conversion: just deinterleave from FLT to FLTP
        // For mono: FLT is sequential [S0, S1, S2, ...], FLTP is also sequential [S0, S1, S2, ...]
        // For stereo: FLT is interleaved [L0, R0, L1, R1, ...], FLTP is planar [L0, L1, ...] [R0, R1, ...]
        const float *inputData = (const float *)inFrame->data[0];
        float *outputData[2] = {(float *)tempFrame->data[0], nullptr};
        if (inChannels == 2)
            outputData[1] = (float *)tempFrame->data[1];
        
        if (inChannels == 1)
        {
            // Mono: just copy sequential data
            memcpy(outputData[0], inputData, numSamples * sizeof(float));
            outputSamples = numSamples;
        }
        else
        {
            // Stereo: deinterleave [L0, R0, L1, R1, ...] -> [L0, L1, ...] [R0, R1, ...]
            for (int i = 0; i < numSamples; ++i)
            {
                outputData[0][i] = inputData[i * 2 + 0];     // Left channel
                outputData[1][i] = inputData[i * 2 + 1];     // Right channel
            }
            outputSamples = numSamples;
        }
        tempFrame->nb_samples = outputSamples;
        
        // Clean up input frame
        av_channel_layout_uninit(&inFrame->ch_layout);
        av_frame_free(&inFrame);
    }
    else
    {
        // Use resampler for rate conversion (when rates differ)
        // swr_convert_frame handles stride calculation automatically
        // Note: swr_convert_frame returns 0 on success, <0 on error (not number of samples)
        if (!info.swr)
        {
            Logger::instance().log(QString("ERROR: Resampler not initialized but rates differ: input=%1 Hz, output=%2 Hz")
                                  .arg(inputSampleRate).arg(outputSampleRate));
            av_channel_layout_uninit(&inFrame->ch_layout);
            av_frame_free(&inFrame);
            av_frame_free(&tempFrame);
            return false;
        }
        int ret = swr_convert_frame(info.swr, tempFrame, inFrame);
        
        // Clean up input frame (no longer needed)
        av_channel_layout_uninit(&inFrame->ch_layout);
        av_frame_free(&inFrame);
        
        if (ret < 0)
        {
            Logger::instance().log(QString("Audio resampling failed: input=%1 samples @ %2 Hz, output=%3 Hz, channels=%4, error=%5")
                                  .arg(numSamples).arg(inputSampleRate).arg(outputSampleRate).arg(inChannels).arg(ret));
            av_frame_free(&tempFrame);
            return false;
        }
        
        // swr_convert_frame returns 0 on success
        // The actual number of output samples is in tempFrame->nb_samples
        outputSamples = tempFrame->nb_samples;
        
        // Log resampling details for debugging
        if (inputSampleRate != outputSampleRate)
        {
            Logger::instance().verbose(QString("Audio resampled: %1 input samples -> %2 output samples (input=%3 Hz, output=%4 Hz)")
                                      .arg(numSamples).arg(outputSamples).arg(inputSampleRate).arg(outputSampleRate));
        }
        
        // If no samples were output, try flushing the resampler
        if (outputSamples == 0)
        {
            ret = swr_convert(info.swr, tempFrame->data, maxOutputSamples, nullptr, 0);
            if (ret <= 0)
            {
                av_frame_free(&tempFrame);
                return true; // No more data to output
            }
            outputSamples = ret;
            tempFrame->nb_samples = ret;
        }
    }
    
    int numChannels = info.codecCtx->ch_layout.nb_channels;
    int currentBufferSize = info.sampleBuffer.size();
    int bufferedSamples = currentBufferSize / numChannels;
    
    // Use PTS directly from NDI timestamp (already normalized relative to sync point)
    // First frame sets the base PTS
    if (!m_firstAudioFrameWritten)
    {
        // Store the first PTS for reference, but use it directly
        m_firstAudioPts = firstSamplePts;
        info.pts = firstSamplePts;
        m_firstAudioFrameWritten = true;
    }
    else
    {
        // Check if incoming PTS matches expected position
        int64_t expectedPts = info.pts + bufferedSamples;
        int64_t ptsDiff = firstSamplePts - expectedPts;
        
        // Allow small tolerance (within 10 samples ~0.2ms at 48kHz) for rounding/clock drift
        if (ptsDiff > 10 || ptsDiff < -10)
        {
            // Significant gap or overlap - adjust to maintain sync
            // Use the incoming PTS as the source of truth
            info.pts = firstSamplePts - bufferedSamples;
        }
    }
    
    // Add new samples to buffer
    // Use outputSamples (from tempFrame->nb_samples) instead of ret (which is just error code)
    info.sampleBuffer.resize(currentBufferSize + outputSamples * numChannels);
    float *bufferDst = info.sampleBuffer.data() + currentBufferSize;
    for (int s = 0; s < outputSamples; ++s)
    {
        for (int c = 0; c < numChannels; ++c)
        {
            float *channelDataPtr = (float *)tempFrame->data[c];
            bufferDst[s * numChannels + c] = channelDataPtr[s];
        }
    }
    av_frame_free(&tempFrame);
    
    bool success = true;
    // Process complete frames immediately to minimize latency
    while (info.sampleBuffer.size() >= frameSize * numChannels)
    {
        for (int c = 0; c < numChannels; ++c)
        {
            float *encoderChannel = (float *)info.frame->data[c];
            for (int s = 0; s < frameSize; ++s)
            {
                encoderChannel[s] = info.sampleBuffer[s * numChannels + c];
            }
        }
        
        info.sampleBuffer.remove(0, frameSize * numChannels);
        
        // Set PTS for this frame - it's the PTS of the first sample in the frame
        info.frame->pts = info.pts;
        // Increment PTS for next frame
        info.pts += frameSize;
        info.frame->nb_samples = frameSize;
        
        if (avcodec_send_frame(info.codecCtx, info.frame) < 0)
        {
            Logger::instance().log("Failed to send audio frame to encoder");
            success = false;
            break;
        }
        
        AVPacket *pkt = av_packet_alloc();
        if (!pkt)
        {
            success = false;
            break;
        }
        while (avcodec_receive_packet(info.codecCtx, pkt) == 0)
        {
            // Write header on first packet (for audio-only recordings, this is the first packet)
            if (!m_headerWritten)
            {
                // For audio-only, we need to write header here since there's no video
                if (avformat_write_header(m_fmtCtx, nullptr) < 0)
                {
                    Logger::instance().log("Failed to write header before audio packet");
                    av_packet_free(&pkt);
                    success = false;
                    break;
                }
                m_headerWritten = true;
                Logger::instance().verbose("Writing header with audio stream(s)");
            }
            
            pkt->stream_index = info.stream->index;
            av_packet_rescale_ts(pkt, info.codecCtx->time_base, info.stream->time_base);
            
            // Set DTS - must be strictly increasing
            if (pkt->dts == AV_NOPTS_VALUE)
            {
                pkt->dts = pkt->pts;
            }
            // Ensure DTS >= PTS (for audio, they should be equal)
            if (pkt->dts < pkt->pts)
            {
                pkt->dts = pkt->pts;
            }
            // CRITICAL: Ensure DTS is strictly increasing
            if (info.lastDts != AV_NOPTS_VALUE)
            {
                if (pkt->dts <= info.lastDts)
                {
                    // Increment DTS to ensure strict monotonic increase
                    pkt->dts = info.lastDts + 1;
                    // Adjust PTS to maintain DTS >= PTS
                    if (pkt->pts < pkt->dts)
                    {
                        pkt->pts = pkt->dts;
                    }
                }
            }
            info.lastDts = pkt->dts;
            
            if (av_interleaved_write_frame(m_fmtCtx, pkt) < 0)
            {
                av_packet_free(&pkt);
                success = false;
                break;
            }
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt);
        
        if (!success)
            break;
    }
    
    return success;
}

bool FfmpegWriter::ensureConvertedFrame()
{
    if (m_convertedFrame &&
        (m_convertedFrame->width != m_videoCodecCtx->width || m_convertedFrame->height != m_videoCodecCtx->height ||
         m_convertedFrame->format != m_videoCodecCtx->pix_fmt))
    {
        av_frame_free(&m_convertedFrame);
    }

    if (!m_convertedFrame)
    {
        m_convertedFrame = av_frame_alloc();
        if (!m_convertedFrame)
            return false;
        m_convertedFrame->format = m_videoCodecCtx->pix_fmt;
        m_convertedFrame->width = m_videoCodecCtx->width;
        m_convertedFrame->height = m_videoCodecCtx->height;
        if (av_frame_get_buffer(m_convertedFrame, 32) < 0)
        {
            av_frame_free(&m_convertedFrame);
            return false;
        }
    }
    return true;
}

bool FfmpegWriter::needsRollover()
{
    if (!m_cfg.segmented)
        return false;
    qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - m_startMs;
    return elapsed >= (qint64)m_cfg.segmentMinutes * 60 * 1000;
}

void FfmpegWriter::rollover()
{
    QMutexLocker locker(&m_mutex);
    closeContext();
    ++m_segmentIndex;
    m_currentAudioChannels = 0;
    m_headerWritten = false;
    m_videoFramesWritten = 0;
    m_firstVideoPts = 0;
    m_lastVideoPts = AV_NOPTS_VALUE;
    m_firstAudioPts = 0;
    m_firstVideoFrameWritten = false;
    m_firstAudioFrameWritten = false;
    
    m_currentFile = nextFileName();
    openContext(m_currentFile);
}

// Static function to query available H.264 and HEVC encoders
QVector<QString> FfmpegWriter::getAvailableEncoders()
{
    QVector<QString> encoders;
    
    void *iter = nullptr;
    const AVCodec *codec = nullptr;
    
    // Iterate through all codecs
    while ((codec = av_codec_iterate(&iter)) != nullptr)
    {
        // Only include encoders (not decoders)
        if (!av_codec_is_encoder(codec))
            continue;
        
        QString name = QString::fromUtf8(codec->name);
        
        // Check if it's an H.264 or HEVC encoder
        bool isH264 = (codec->id == AV_CODEC_ID_H264);
        bool isHEVC = (codec->id == AV_CODEC_ID_H265);
        
        if (isH264 || isHEVC)
        {
            encoders.append(name);
        }
    }
    
    return encoders;
}

// Helper function: Select codec based on configuration
// Codec name is provided directly from settings (e.g., "hevc_videotoolbox", "libx264")
FfmpegWriter::CodecInfo FfmpegWriter::selectCodec(const QString &codecName, bool useHardware)
{
    CodecInfo info = {AV_CODEC_ID_H264, "libx264", false};
    
    QString normalized = codecName.trimmed().toLower();
    
    // Try to find the codec by name first (handles specific encoder names like "hevc_videotoolbox")
    const AVCodec *codec = avcodec_find_encoder_by_name(codecName.toUtf8().constData());
    if (codec)
    {
        // Found the codec - use its name (which is a static string from FFmpeg)
        info.id = codec->id;
        info.name = codec->name;
        // Detect if it's a hardware encoder by checking common hardware encoder suffixes
        QString codecNameLower = QString::fromUtf8(codec->name).toLower();
        info.isHardware = codecNameLower.contains("_nvenc") ||
                         codecNameLower.contains("_amf") ||
                         codecNameLower.contains("_qsv") ||
                         codecNameLower.contains("_vaapi") ||
                         codecNameLower.contains("_videotoolbox") ||
                         codecNameLower.contains("_mf") ||
                         codecNameLower.contains("_mediacodec");
        return info;
    }
    
    // Fallback: determine codec ID from name pattern and use defaults
    if (normalized.contains("h265") || normalized.contains("hevc"))
    {
        info.id = AV_CODEC_ID_H265;
        info.name = "libx265";
        info.isHardware = false;
    }
    else if (normalized.contains("h264"))
    {
        info.id = AV_CODEC_ID_H264;
        info.name = "libx264";
        info.isHardware = false;
    }
    
    return info;
}

// Set video metadata from NDI frames
void FfmpegWriter::setVideoMetadata(int colorFormat, float pictureAspectRatio, const char *metadata)
{
    QMutexLocker locker(&m_mutex);
    m_ndiColorFormat = colorFormat;
    m_ndiPictureAspectRatio = pictureAspectRatio;
    if (metadata)
    {
        m_ndiMetadata = QString::fromUtf8(metadata);
    }
    else
    {
        m_ndiMetadata = QString();
    }
}

// Apply color metadata from NDI to FFmpeg codec context and stream
void FfmpegWriter::applyColorMetadata()
{
    if (!m_videoCodecCtx || !m_videoStream)
        return;
    
    // Map NDI color format to FFmpeg color space/primaries/transfer
    // NDI color formats: NDIlib_recv_color_format_BGRX_BGRA, NDIlib_recv_color_format_UYVY_BGRA, 
    // NDIlib_recv_color_format_RGBX_RGBA, NDIlib_recv_color_format_UYVY_RGBA, NDIlib_recv_color_format_fastest
    // Default to BT.709 for most NDI sources
    AVColorSpace colorSpace = AVCOL_SPC_BT709;
    AVColorPrimaries colorPrimaries = AVCOL_PRI_BT709;
    AVColorTransferCharacteristic colorTrc = AVCOL_TRC_BT709;
    AVColorRange colorRange = AVCOL_RANGE_MPEG; // Limited range (16-235)
    
    // NDI typically uses BT.709 for HD content
    // If we have metadata, we could parse it for color info, but for now use defaults
    // Most NDI sources are HD (720p/1080p) and use BT.709
    
    // Apply to codec context
    m_videoCodecCtx->colorspace = colorSpace;
    m_videoCodecCtx->color_primaries = colorPrimaries;
    m_videoCodecCtx->color_trc = colorTrc;
    m_videoCodecCtx->color_range = colorRange;
    
    // Apply to stream codecpar
    m_videoStream->codecpar->color_space = colorSpace;
    m_videoStream->codecpar->color_primaries = colorPrimaries;
    m_videoStream->codecpar->color_trc = colorTrc;
    m_videoStream->codecpar->color_range = colorRange;
    
    // Set picture aspect ratio if available from NDI
    if (m_ndiPictureAspectRatio > 0.0f)
    {
        // Calculate display aspect ratio
        // NDI provides picture_aspect_ratio (width/height)
        // FFmpeg uses AVRational for sample aspect ratio
        // For most cases, if picture_aspect_ratio != (width/height), we need to set sample_aspect_ratio
        float pixelAspectRatio = (float)m_videoCodecCtx->width / (float)m_videoCodecCtx->height;
        if (std::abs(m_ndiPictureAspectRatio - pixelAspectRatio) > 0.01f)
        {
            // Calculate sample aspect ratio: SAR = (DAR * height) / width
            // DAR = m_ndiPictureAspectRatio, PAR = width/height
            // SAR = DAR / PAR = (m_ndiPictureAspectRatio) / (width/height)
            float sar = m_ndiPictureAspectRatio / pixelAspectRatio;
            // Convert to rational (simplify to common ratios)
            AVRational sarRational = av_d2q(sar, 1000);
            m_videoCodecCtx->sample_aspect_ratio = sarRational;
            m_videoStream->codecpar->sample_aspect_ratio = sarRational;
        }
    }
    
    // Add NDI metadata to stream metadata if available
    if (!m_ndiMetadata.isEmpty())
    {
        // Parse XML metadata and add relevant fields to stream metadata
        // For now, add the raw metadata as a comment
        av_dict_set(&m_videoStream->metadata, "ndi_metadata", m_ndiMetadata.toUtf8().constData(), 0);
        
        // Try to extract common metadata fields from XML
        // Look for common NDI metadata tags
        QString xml = m_ndiMetadata.toLower();
        if (xml.contains("<ndi_color_info>"))
        {
            // Extract color info if present
            // This would require XML parsing, but for now just mark that color info exists
            av_dict_set(&m_videoStream->metadata, "has_ndi_color_info", "1", 0);
        }
    }
    
    Logger::instance().log(QString("Applied color metadata: colorspace=BT.709, range=%1")
                          .arg(colorRange == AVCOL_RANGE_MPEG ? "limited" : "full"));
}

// Helper function: Select appropriate pixel format
AVPixelFormat FfmpegWriter::selectPixelFormat(bool isHardware, bool isHEVC, AVPixelFormat requested)
{
    if (!isHardware)
        return requested;
    
    // For VideoToolbox hardware encoders
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(requested);
    bool is10Bit = desc && desc->comp[0].depth >= 10;
    
    if (isHEVC && is10Bit)
        return AV_PIX_FMT_P010LE; // HEVC 10-bit
    else
        return AV_PIX_FMT_NV12; // 8-bit (H.264 or HEVC)
}

// Helper function: Setup codec options
void FfmpegWriter::setupCodecOptions(AVDictionary **opts, const QString &codecName, bool isHardware, bool isHEVC, AVPixelFormat pixFmt)
{
    if (isHardware)
    {
        QString normalizedCodec = codecName.trimmed().toLower();
        
        // Detect hardware encoder type
        bool isVideoToolbox = normalizedCodec.contains("videotoolbox");
        bool isNVENC = normalizedCodec.contains("nvenc");
        bool isQSV = normalizedCodec.contains("qsv");
        bool isAMF = normalizedCodec.contains("amf");
        bool isVAAPI = normalizedCodec.contains("vaapi");
        
        // Common options for hardware encoders
        if (isHEVC)
        {
            // Determine HEVC profile based on bit depth
            const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(pixFmt);
            bool is10Bit = desc && desc->comp[0].depth >= 10;
            
            if (isVideoToolbox)
            {
                av_dict_set(opts, "profile", is10Bit ? "main10" : "main", 0);
                av_dict_set(opts, "level", "4.1", 0);
                av_dict_set(opts, "codec", "hevc", 0);
            }
            else if (isNVENC || isQSV || isAMF || isVAAPI)
            {
                // Set profile for other hardware encoders if supported
                if (is10Bit)
                {
                    av_dict_set(opts, "profile", "main10", 0);
                }
            }
        }
        
        // Quality settings - encoder-specific
        if (m_cfg.qualityMode == "crf")
        {
            if (isVideoToolbox)
            {
                // Apple VideoToolbox: -q:v 1-100 (higher = better quality)
                // For VideoToolbox, crfValue contains the CQ value (1-100)
                int qValue = m_cfg.crfValue;
                if (qValue < 1) qValue = 1;
                if (qValue > 100) qValue = 100;
                av_dict_set(opts, "q", QString::number(qValue).toUtf8().constData(), 0);
            }
            else
            {
                // Other encoders: CRF is 0-51, lower is better
                int qualityValue = m_cfg.crfValue;
                
                if (isNVENC)
            {
                // NVIDIA NVENC: Constant Quality (CQ) mode -rc vbr -cq <0-51> (lower = better)
                av_dict_set(opts, "rc", "vbr", 0);
                av_dict_set(opts, "cq", QString::number(qualityValue).toUtf8().constData(), 0);
                av_dict_set(opts, "qmin", QString::number(qualityValue).toUtf8().constData(), 0);
                av_dict_set(opts, "qmax", QString::number(qualityValue).toUtf8().constData(), 0);
                av_dict_set(opts, "b", "0", 0); // Variable bitrate
                }
                else if (isQSV)
            {
                // Intel Quick Sync: Intelligent Constant Quality (ICQ) -global_quality <1-51> (lower = better)
                int qsvQuality = qualityValue;
                if (qsvQuality < 1) qsvQuality = 1;
                if (qsvQuality > 51) qsvQuality = 51;
                av_dict_set(opts, "global_quality", QString::number(qsvQuality).toUtf8().constData(), 0);
                av_dict_set(opts, "preset", "slow", 0);
                av_dict_set(opts, "look_ahead", "1", 0); // Enable look-ahead for better quality
                }
                else if (isAMF)
            {
                // AMD AMF: Constant QP (CQP) -rc cqp -qp_i/-qp_p/-qp_b <0-51> (lower = better)
                av_dict_set(opts, "rc", "cqp", 0);
                QString qpStr = QString::number(qualityValue);
                av_dict_set(opts, "qp_i", qpStr.toUtf8().constData(), 0);
                av_dict_set(opts, "qp_p", qpStr.toUtf8().constData(), 0);
                if (!isHEVC) // H.264 also has B-frames
                {
                    av_dict_set(opts, "qp_b", qpStr.toUtf8().constData(), 0);
                }
                }
                else if (isVAAPI)
            {
                // VAAPI: Constant QP -qp <0-52> (lower = better)
                int vaapiQp = qualityValue;
                if (vaapiQp > 52) vaapiQp = 52;
                av_dict_set(opts, "qp", QString::number(vaapiQp).toUtf8().constData(), 0);
                }
            }
        }
        else
        {
            // Bitrate mode
            if (m_cfg.qualityMode == "bitrate")
            {
                av_dict_set(opts, "b", QString::number(m_cfg.videoBitrate).toUtf8().constData(), 0);
                
                // For VideoToolbox, also set quality when using bitrate mode (default to 55)
                if (isVideoToolbox)
                {
                    int qValue = 55; // Default VideoToolbox quality
                    av_dict_set(opts, "q", QString::number(qValue).toUtf8().constData(), 0);
                }
            }
        }
        
        // Encoder-specific options
        if (isVideoToolbox)
        {
            av_dict_set(opts, "realtime", "1", 0);
            av_dict_set(opts, "allow_sw", "0", 0); // Prevent software fallback
        }
        else if (isNVENC)
        {
            av_dict_set(opts, "preset", "p1", 0); // Fastest preset for low latency
            av_dict_set(opts, "tune", "ll", 0); // Low latency
        }
        else if (isQSV)
        {
            av_dict_set(opts, "async_depth", "1", 0); // Low latency
        }
    }
    else
    {
        // Software encoder options
        av_dict_set(opts, "preset", "ultrafast", 0);
        av_dict_set(opts, "tune", "zerolatency", 0);
        
        if (m_cfg.qualityMode == "crf")
        {
            av_dict_set(opts, "crf", QString::number(m_cfg.crfValue).toUtf8().constData(), 0);
        }
        else
        {
            av_dict_set(opts, "b", QString::number(m_cfg.videoBitrate).toUtf8().constData(), 0);
        }
    }
}

// Helper function: Verify codec matches expectations
bool FfmpegWriter::verifyCodec(const AVCodec *codec, AVCodecContext *ctx, AVStream *stream, const CodecInfo &expected)
{
    if (!codec || !ctx || !stream)
        return false;
    
    // Verify codec ID matches
    if (codec->id != expected.id || ctx->codec_id != expected.id)
    {
        Logger::instance().log(QString("ERROR: Codec ID mismatch. Expected %1, got codec->id=%2, ctx->id=%3")
                              .arg(expected.id).arg(codec->id).arg(ctx->codec_id));
        return false;
    }
    
    // Verify codec name matches (log warning if different, but don't fail)
    if (codec->name)
    {
        QString actualName = QString::fromUtf8(codec->name).toLower();
        QString expectedName = QString::fromUtf8(expected.name).toLower();
        if (actualName != expectedName)
        {
            Logger::instance().log(QString("WARNING: Codec name mismatch - expected %1, got %2").arg(expected.name).arg(codec->name));
        }
    }
    
    // Force stream codecpar to match
    stream->codecpar->codec_id = expected.id;
    if (expected.id == AV_CODEC_ID_H265)
    {
        // Set hvc1 tag for .mov and .mp4 files
        QString ext = m_cfg.fileExtension.toLower();
        if (ext == "mov" || ext == "mp4")
        {
            stream->codecpar->codec_tag = MKTAG('h', 'v', 'c', '1');
        }
    }
    
    return true;
}

// Helper function: Check packet for correct codec (simplified NAL unit check)
bool FfmpegWriter::checkPacketCodec(AVPacket *pkt, bool expectHEVC)
{
    if (!expectHEVC || !pkt || pkt->size < 5)
        return true; // Only check HEVC packets
    
    // Check NAL units to verify they're HEVC, not H.264
    // H.264: NAL type in bits 0-4 (lower 5 bits)
    // HEVC: NAL type in bits 1-6 (middle 6 bits)
    for (int i = 0; i < qMin(pkt->size - 4, 200); i++)
    {
        // Check for 4-byte start code: 0x00 0x00 0x00 0x01
        if (i + 3 < pkt->size && 
            pkt->data[i] == 0x00 && pkt->data[i+1] == 0x00 && 
            pkt->data[i+2] == 0x00 && pkt->data[i+3] == 0x01)
        {
            if (i + 4 < pkt->size)
            {
                uint8_t nalHeader = pkt->data[i + 4];
                // Check H.264 NAL type (bits 0-4)
                uint8_t h264Type = nalHeader & 0x1F;
                // Check HEVC NAL type (bits 1-6)
                uint8_t hevcType = (nalHeader >> 1) & 0x3F;
                
                // If it matches H.264 types (1-5 for video, 7-8 for parameter sets), it's H.264
                if (h264Type >= 1 && h264Type <= 8)
                {
                    Logger::instance().log(QString("ERROR: Found H.264 NAL unit (H.264 type=%1, HEVC type=%2) in HEVC packet").arg(h264Type).arg(hevcType));
                    return false;
                }
                
                // If it matches HEVC types, it's HEVC
                if (hevcType >= 19 && hevcType <= 21 || hevcType >= 32 && hevcType <= 34 || hevcType == 39 || hevcType == 40)
                {
                    return true; // Found valid HEVC NAL unit
                }
            }
            i += 3; // Skip past this start code
        }
        // Check for 3-byte start code: 0x00 0x00 0x01
        else if (i + 2 < pkt->size && 
                 pkt->data[i] == 0x00 && pkt->data[i+1] == 0x00 && pkt->data[i+2] == 0x01)
        {
            // Make sure it's not part of a 4-byte start code
            if (i == 0 || pkt->data[i-1] != 0x00)
            {
                if (i + 3 < pkt->size)
                {
                    uint8_t nalHeader = pkt->data[i + 3];
                    uint8_t h264Type = nalHeader & 0x1F;
                    uint8_t hevcType = (nalHeader >> 1) & 0x3F;
                    
                    if (h264Type >= 1 && h264Type <= 8)
                    {
                        Logger::instance().log(QString("ERROR: Found H.264 NAL unit (H.264 type=%1, HEVC type=%2) in HEVC packet").arg(h264Type).arg(hevcType));
                        return false;
                    }
                    
                    if (hevcType >= 19 && hevcType <= 21 || hevcType >= 32 && hevcType <= 34 || hevcType == 39 || hevcType == 40)
                    {
                        return true; // Found valid HEVC NAL unit
                    }
                }
            }
            i += 2; // Skip past this start code
        }
    }
    
    return true; // If we can't determine, assume it's OK (let FFmpeg handle it)
}
