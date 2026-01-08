#include "SourceRecorder.h"
#include "Logging.h"
#include <QImage>
#include <QByteArray>
#include <QThread>
#include <QMutexLocker>
#include <QCoreApplication>
#include <algorithm>
#include <cmath>
extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/rational.h>
#include <Processing.NDI.Lib.h>
}

SourceRecorder::SourceRecorder(QObject *parent)
    : QObject(parent), m_running(false), m_previewOnly(false), m_recordingStarted(false), m_recv(nullptr), 
      m_reusableVideoFrame(nullptr), m_videoInfoLogged(false), m_audioInfoLogged(false), m_metadataSet(false),
      m_syncEstablished(false), m_hasSeenVideo(false), m_hasSeenAudio(false)
{
    m_status = "Idle";
}

SourceRecorder::~SourceRecorder()
{
    // Ensure thread is stopped before destruction
    m_running = false;
    m_previewOnly = false;
    
    // Disconnect signals to prevent any callbacks during destruction
    disconnect(&m_videoThread, nullptr, this, nullptr);
    
    // Ensure we're in the main thread before cleaning up
    QThread *mainThread = QCoreApplication::instance() ? QCoreApplication::instance()->thread() : nullptr;
    if (mainThread && thread() != mainThread && thread() == &m_videoThread)
    {
        // We're in the worker thread - this shouldn't happen, but handle it
        moveToThread(mainThread);
        QThread::msleep(10); // Give Qt time to process the move
    }
    
    // Stop and wait for thread to finish
    if (m_videoThread.isRunning())
    {
        m_videoThread.quit();
        // Wait up to 5 seconds for thread to finish
        // The thread should exit quickly since m_running is now false
        if (!m_videoThread.wait(5000))
        {
            // Thread didn't finish gracefully, terminate it (last resort)
            Logger::instance().log("Warning: Video thread did not stop gracefully, terminating");
            m_videoThread.terminate();
            m_videoThread.wait(1000); // Wait a bit more for termination
        }
    }
    
    // Clean up NDI receiver
    if (m_recv)
    {
        NDIlib_recv_destroy((NDIlib_recv_instance_t)m_recv);
        m_recv = nullptr;
    }
    
    // Free reusable AVFrame
    if (m_reusableVideoFrame)
    {
        av_frame_free(&m_reusableVideoFrame);
    }
    
    // Stop writer
    m_writer.stop();
}

void SourceRecorder::applySettings(const SourceSettings &settings)
{
    bool ndiSourceChanged = false;
    {
        QMutexLocker locker(&m_mutex);
        ndiSourceChanged = (m_settings.ndiSource != settings.ndiSource);
        m_settings = settings;
        if (m_settings.label.isEmpty())
            m_settings.label = m_settings.ndiSource;
    }
    emit settingsChanged();
    
    // If NDI source changed and we're not recording, restart preview
    if (ndiSourceChanged && !m_running && !settings.ndiSource.isEmpty())
    {
        // Refresh sources before starting preview
        NdiManager ndi;
        ndi.refreshSources();
        QThread::msleep(200);
        startPreview();
    }
    else if (ndiSourceChanged && m_previewOnly && !settings.ndiSource.isEmpty())
    {
        // Restart preview with new source
        // Refresh sources before restarting
        NdiManager ndi;
        ndi.refreshSources();
        QThread::msleep(200);
        stop();
        startPreview();
    }
}

void SourceRecorder::start()
{
    // If already running in preview mode, stop it first
    if (m_running && m_previewOnly)
    {
        stop();
    }
    
    if (m_running)
        return;

    if (m_settings.ndiSource.isEmpty() || m_settings.outputFolder.isEmpty())
    {
        m_status = "Missing settings";
        emit errorOccurred("Configure NDI source and output folder before starting.");
        return;
    }

    // Validate that the configured NDI source is still available
    NdiManager ndi;
    if (!ndi.availableSources().contains(m_settings.ndiSource))
    {
        m_status = "Source unavailable";
        emit errorOccurred("NDI source not found: " + m_settings.ndiSource);
        return;
    }

    m_running = true;
    m_previewOnly = false;
    m_recordingStarted = false;
    m_videoPts = 0;
    m_expectedFrameTicks10ns = 0;
    m_expectedPtsStep = 1;
    m_sourceFpsNum = 60;
    m_sourceFpsDen = 1;
    m_bufferingTimer.invalidate();
    m_hasSeenVideo = false;
    m_hasSeenAudio = false;
    m_previewThrottle.invalidate();
    m_status = "Connecting";
    {
        QMutexLocker locker(&m_mutex);
        m_preview = QImage();
    }
    emit previewUpdated();

    // Wait for thread to finish if it's still running
    if (m_videoThread.isRunning())
    {
        m_videoThread.quit();
        m_videoThread.wait();
    }
    
    // Disconnect any existing connection to avoid duplicates
    disconnect(&m_videoThread, &QThread::started, this, &SourceRecorder::videoThreadFunc);
    connect(&m_videoThread, &QThread::started, this, &SourceRecorder::videoThreadFunc);
    
    // Only move to thread if we're currently in the main thread
    if (thread() == QThread::currentThread())
    {
        moveToThread(&m_videoThread);
        m_videoThread.start();
    }
    else
    {
        // If object is in wrong thread, we can't move it from here
        // Start the thread and it will call videoThreadFunc automatically
        // The move should have already happened, but if not, we'll handle it
        m_videoThread.start();
    }
}

void SourceRecorder::startPreview()
{
    if (m_running)
        return;

    if (m_settings.ndiSource.isEmpty())
    {
        m_status = "No NDI source";
        return;
    }

    // Refresh NDI sources before attempting connection to get the latest list
    NdiManager ndi;
    ndi.refreshSources();
    // Give NDI a moment to discover sources
    QThread::msleep(200);
    QStringList available = ndi.availableSources();
    if (!available.contains(m_settings.ndiSource))
    {
        // Source not in list, but try anyway - it might appear soon or might be connectable
        // We'll set status appropriately when connection is attempted
        Logger::instance().log("NDI source not in available list, attempting connection anyway: " + m_settings.ndiSource);
    }

    m_running = true;
    m_previewOnly = true;
    m_recordingStarted = false;
    // Reset metadata and logging flags when starting preview
    m_metadataSet = false;
    m_videoInfoLogged = false;
    m_audioInfoLogged = false;
    m_syncEstablished = false;
    m_previewThrottle.invalidate();
    m_status = "Connecting";
    {
        QMutexLocker locker(&m_mutex);
        m_preview = QImage();
    }
    emit previewUpdated();

    // Wait for thread to finish if it's still running
    if (m_videoThread.isRunning())
    {
        m_videoThread.quit();
        m_videoThread.wait();
    }
    
    // Disconnect any existing connection to avoid duplicates
    disconnect(&m_videoThread, &QThread::started, this, &SourceRecorder::videoThreadFunc);
    connect(&m_videoThread, &QThread::started, this, &SourceRecorder::videoThreadFunc);
    
    // Move to worker thread - must be called from object's current thread
    // If we're being called from wrong thread, queue the operation
    if (thread() == QThread::currentThread())
    {
        moveToThread(&m_videoThread);
        m_videoThread.start();
    }
    else
    {
        // This shouldn't happen in normal flow, but handle it gracefully
        // Move will happen automatically when thread starts
        m_videoThread.start();
    }
}

void SourceRecorder::stop()
{
    bool wasPreviewOnly = m_previewOnly;
    const QString recordedFile = wasPreviewOnly ? QString() : m_writer.currentFile();

    m_running = false;
    m_previewOnly = false;
    m_recordingStarted = false;
    m_syncEstablished = false;
    // Clear buffered frames
    m_bufferedVideoFrames.clear();
    m_bufferedAudioFrames.clear();
    m_bufferingTimer.invalidate();
    m_hasSeenVideo = false;
    m_hasSeenAudio = false;
    
    // Disconnect signal before stopping thread
    disconnect(&m_videoThread, &QThread::started, this, &SourceRecorder::videoThreadFunc);
    
    // Stop thread if running
    if (m_videoThread.isRunning())
    {
        m_videoThread.quit();
        // Wait up to 3 seconds for thread to finish gracefully
        if (!m_videoThread.wait(3000))
        {
            Logger::instance().log("Warning: Video thread did not stop within timeout");
        }
    }
    
    // Move object back to main thread only if we're currently in the worker thread
    // This must be done from the worker thread, so we check thread() first
    QThread *mainThread = QCoreApplication::instance()->thread();
    if (thread() != mainThread && thread() == &m_videoThread)
    {
        // We're in the worker thread - move back to main thread
        moveToThread(mainThread);
        // Give Qt time to process the move
        QThread::msleep(10);
    }
    
    if (m_recv)
    {
        NDIlib_recv_destroy((NDIlib_recv_instance_t)m_recv);
        m_recv = nullptr;
    }
    m_writer.stop();

    if (!wasPreviewOnly)
        emit recordingStopped();
    
    // If we were recording (not just previewing) and have an NDI source configured,
    // automatically restart preview mode
    if (!wasPreviewOnly && !m_settings.ndiSource.isEmpty())
    {
        // Refresh NDI sources before starting preview
        NdiManager ndi;
        ndi.refreshSources();
        QThread::msleep(200);
        
        // Start preview automatically
        startPreview();
    }
    else
    {
        m_status = "Idle";
        {
            QMutexLocker locker(&m_mutex);
            m_preview = QImage();
        }
        emit previewUpdated();
    }
}

qint64 SourceRecorder::elapsedMs() const
{
    if (!m_running || !m_recordingStarted)
        return 0;
    QMutexLocker stateLocker(&m_stateMutex);
    return m_timer.elapsed();
}

QImage SourceRecorder::lastFrame() const
{
    QMutexLocker locker(&m_mutex);
    return m_preview;
}

void SourceRecorder::reconnect()
{
    if (m_recv)
    {
        NDIlib_recv_destroy((NDIlib_recv_instance_t)m_recv);
        m_recv = nullptr;
    }

    QByteArray ndiNameUtf8 = m_settings.ndiSource.toUtf8();
    NDIlib_source_t source = {};
    source.p_ndi_name = ndiNameUtf8.constData();

    NDIlib_recv_create_v3_t recvCreate = {};
    recvCreate.source_to_connect_to = source;
    recvCreate.color_format = NDIlib_recv_color_format_RGBX_RGBA;
    recvCreate.bandwidth = NDIlib_recv_bandwidth_highest;
    recvCreate.allow_video_fields = false;

    m_recv = (void*)NDIlib_recv_create_v3(&recvCreate);
    if (!m_recv)
    {
        Logger::instance().log("Failed to create NDI receiver for " + m_settings.ndiSource);
        if (m_previewOnly)
        {
            // In preview mode, set status but keep trying (connection might work later)
            QMutexLocker locker(&m_mutex);
            m_status = "Connection failed";
        }
        else
        {
            // In recording mode, this is an error
            emit errorOccurred("NDI receiver failed for " + m_settings.ndiSource);
            m_running = false;
            {
                QMutexLocker locker(&m_mutex);
                m_status = "Error";
            }
        }
    }
    else
    {
        QMutexLocker locker(&m_mutex);
        if (m_status == "Connection failed" || m_status == "Error")
        {
            m_status = m_previewOnly ? "Connecting" : "Connecting";
        }
    }
}

void SourceRecorder::videoThreadFunc()
{
    reconnect();
    
    // Emit status update after connection attempt
    emit previewUpdated();

    // If receiver creation failed and we're not in preview mode, exit
    if (!m_recv && !m_previewOnly)
    {
        {
            QMutexLocker locker(&m_mutex);
            m_status = "Connection failed";
        }
        emit previewUpdated();
        return;
    }

    // If receiver creation failed in preview mode, keep retrying
    if (!m_recv && m_previewOnly)
    {
        // Retry connection periodically
        int retryCount = 0;
        while (m_running && !m_recv && retryCount < 100)  // Retry for up to 50 seconds
        {
            QThread::msleep(500);
            reconnect();
            retryCount++;
        }
        
        if (!m_recv)
        {
            QMutexLocker locker(&m_mutex);
            m_status = "Source unavailable";
            emit previewUpdated();  // Update UI even on failure
            return;
        }
    }

    NDIlib_video_frame_v2_t videoFrame;
    NDIlib_audio_frame_v3_t audioFrame;
    int timeoutStreak = 0;

    while (m_running)
    {
        if (!m_recv)
        {
            QThread::msleep(100);
            continue;
        }
        
switch (NDIlib_recv_capture_v3((NDIlib_recv_instance_t)m_recv, &videoFrame, &audioFrame, nullptr, 100))
        {
        case NDIlib_frame_type_video:
        {
            // Validate frame dimensions before processing
            if (videoFrame.xres <= 0 || videoFrame.yres <= 0 || !videoFrame.p_data)
            {
                Logger::instance().log(QString("Invalid video frame dimensions: %1x%2").arg(videoFrame.xres).arg(videoFrame.yres));
                NDIlib_recv_free_video_v2((NDIlib_recv_instance_t)m_recv, &videoFrame);
                break;
            }
            
            // Update preview (always update preview, even while buffering)
            const bool shouldUpdatePreview = !m_previewThrottle.isValid() || m_previewThrottle.elapsed() >= 200;
            if (shouldUpdatePreview)
            {
                QImage img((uchar *)videoFrame.p_data, videoFrame.xres, videoFrame.yres, QImage::Format_RGBA8888);
                {
                    QMutexLocker locker(&m_mutex);
                    m_preview = img.copy();
                    if (m_previewOnly)
                        m_status = "Previewing";
                    else
                        m_status = m_syncEstablished ? "Recording" : "Buffering";
                }
                emit previewUpdated();
                m_previewThrottle.restart();
            }
            else if (!m_previewOnly)
            {
                QMutexLocker locker(&m_mutex);
                m_status = m_syncEstablished ? "Recording" : "Buffering";
            }

            timeoutStreak = 0;

            // In preview-only mode, skip all recording logic
            if (m_previewOnly)
            {
                NDIlib_recv_free_video_v2((NDIlib_recv_instance_t)m_recv, &videoFrame);
                break;
            }

            // Buffer video frame until we have audio to establish sync
            if (!m_syncEstablished)
            {
                // Start buffering timer on first frame
                if (!m_bufferingTimer.isValid())
                {
                    m_bufferingTimer.start();
                }
                
                // Log video stream information on first frame
                
                if (!m_videoInfoLogged && m_bufferedVideoFrames.isEmpty())
                {
                    float fps = (videoFrame.frame_rate_D > 0) ? 
                                (float)videoFrame.frame_rate_N / (float)videoFrame.frame_rate_D : 0.0f;
                    QString aspectRatioStr = (videoFrame.picture_aspect_ratio > 0.0f) ?
                                            QString::number(videoFrame.picture_aspect_ratio, 'f', 2) : "unknown";
                    QString metadataStr = videoFrame.p_metadata ? QString::fromUtf8(videoFrame.p_metadata) : "none";
                    
                    Logger::instance().verbose(QString("NDI Video Stream Detected: %1x%2 @ %3 fps, aspect ratio: %4, bit depth: 8-bit (RGBA)")
                                          .arg(videoFrame.xres)
                                          .arg(videoFrame.yres)
                                          .arg(fps, 0, 'f', 2)
                                          .arg(aspectRatioStr));
                    
                    if (videoFrame.p_metadata && strlen(videoFrame.p_metadata) > 0)
                    {
                        Logger::instance().verbose(QString("NDI Video Metadata: %1").arg(metadataStr.left(200))); // Limit to first 200 chars
                    }
                    
                    m_videoInfoLogged = true;
                }
                
                m_hasSeenVideo = true;
                
                BufferedVideoFrame buffered;
                int frameSize = videoFrame.xres * videoFrame.yres * 4; // RGBA
                buffered.data = QByteArray((const char *)videoFrame.p_data, frameSize);
                buffered.width = videoFrame.xres;
                buffered.height = videoFrame.yres;
                buffered.timestamp = videoFrame.timestamp; // NDI timestamp in 100ns units
                buffered.fpsNum = videoFrame.frame_rate_N;
                buffered.fpsDen = videoFrame.frame_rate_D;
                
                // Limit buffer size to prevent memory issues (max 2 seconds at 60fps = 120 frames)
                if (m_bufferedVideoFrames.size() < 120)
                {
                    m_bufferedVideoFrames.append(buffered);
                    Logger::instance().verbose(QString("Buffering video frame (timestamp=%1). Total buffered: %2 video, %3 audio")
                                              .arg(videoFrame.timestamp)
                                              .arg(m_bufferedVideoFrames.size())
                                              .arg(m_bufferedAudioFrames.size()));
                }
                
                // Check if we can establish sync
                // Require at least 1 second of buffering and both audio/video if both are present
                checkAndStartRecording();
            }
            else
            {
                // Sync established, process frames normally
                if (!m_recordingStarted)
                {
                    m_timer.restart();
                    m_recordingStarted = true;
                }

                // Extract and set metadata from NDI frame (only on first frame to avoid overhead)
                
                if (!m_metadataSet)
                {
                    // NDI video frame v2 structure fields:
                    // - picture_aspect_ratio (float)
                    // - p_metadata (const char*)
                    // Note: color_format is set when creating the receiver (RGBX_RGBA in our case)
                    m_writer.setVideoMetadata(
                        NDIlib_recv_color_format_RGBX_RGBA, // Use the color format we requested
                        videoFrame.picture_aspect_ratio,
                        videoFrame.p_metadata
                    );
                    m_metadataSet = true;
                }
                
                // Write video frame
                // Optimize: reuse AVFrame to avoid per-frame allocation
                if (!m_reusableVideoFrame)
                {
                    m_reusableVideoFrame = av_frame_alloc();
                    if (!m_reusableVideoFrame)
                    {
                        Logger::instance().log("Failed to allocate reusable AVFrame");
                        NDIlib_recv_free_video_v2((NDIlib_recv_instance_t)m_recv, &videoFrame);
                        break;
                    }
                }
                AVFrame *frame = m_reusableVideoFrame;
                frame->format = AV_PIX_FMT_RGBA;
                frame->width = videoFrame.xres;
                frame->height = videoFrame.yres;
                av_image_fill_arrays(frame->data, frame->linesize, videoFrame.p_data, AV_PIX_FMT_RGBA, videoFrame.xres, videoFrame.yres, 1);
                
                // Calculate PTS from NDI timestamp relative to sync point
                // This ensures proper sync with audio
                // NDI timestamps are in 100ns units, video time_base is {fpsDen, fpsNum}
                // PTS = (timeSinceSync * fpsNum) / (fpsDen * 10000000)
                int64_t timeSinceSync = videoFrame.timestamp - m_syncTimestamp;
                if (timeSinceSync < 0)
                    timeSinceSync = 0; // Skip frames before sync point
                // Protect against division by zero
                int64_t ptsFromTimestamp = 0;
                if (m_sourceFpsDen > 0)
                {
                    ptsFromTimestamp = (timeSinceSync * m_sourceFpsNum) / (m_sourceFpsDen * 10000000LL);
                }
                else
                {
                    Logger::instance().log("Invalid FPS denominator, using default PTS calculation");
                    ptsFromTimestamp = timeSinceSync / 10000000LL; // Fallback calculation
                }
                
                // Ensure PTS is strictly increasing (at least 1 frame apart)
                // This prevents duplicate PTS values that cause DTS errors
                if (ptsFromTimestamp <= m_videoPts)
                {
                    ptsFromTimestamp = m_videoPts + 1;
                }
                frame->pts = ptsFromTimestamp;
                m_videoPts = ptsFromTimestamp;
                
                // Write video frame
                if (!m_writer.writeVideoFrame(frame))
                {
                    Logger::instance().log("Failed to write video frame");
                }
                // Note: Don't free frame here - it's reused

                if (m_writer.needsRollover())
                {
                    m_writer.rollover();
                    m_videoPts = 0;
                    m_expectedPtsStep = 1;
                }
            }

            NDIlib_recv_free_video_v2((NDIlib_recv_instance_t)m_recv, &videoFrame);
            break;
        }
        case NDIlib_frame_type_audio:
        {
            // Validate audio frame parameters before processing
            if (audioFrame.no_samples <= 0 || audioFrame.no_channels <= 0 || audioFrame.sample_rate <= 0 || !audioFrame.p_data)
            {
                Logger::instance().log(QString("Invalid audio frame parameters: %1 samples, %2 channels, %3 Hz")
                                      .arg(audioFrame.no_samples).arg(audioFrame.no_channels).arg(audioFrame.sample_rate));
                NDIlib_recv_free_audio_v3((NDIlib_recv_instance_t)m_recv, &audioFrame);
                break;
            }
            
            // In preview-only mode, skip audio processing
            if (m_previewOnly)
            {
                NDIlib_recv_free_audio_v3((NDIlib_recv_instance_t)m_recv, &audioFrame);
                break;
            }

            // Buffer audio frame until we have video to establish sync
            if (!m_syncEstablished)
            {
                // Start buffering timer on first frame
                if (!m_bufferingTimer.isValid())
                {
                    m_bufferingTimer.start();
                }
                
                // Log audio stream information on first frame
                
                if (!m_audioInfoLogged && m_bufferedAudioFrames.isEmpty())
                {
                    QString channelLayout = (audioFrame.no_channels == 1) ? "Mono" :
                                           (audioFrame.no_channels == 2) ? "Stereo" :
                                           QString("%1 channels").arg(audioFrame.no_channels);
                    
                    Logger::instance().verbose(QString("NDI Audio Stream Detected: %1 @ %2 Hz, %3 samples per frame, bit depth: 32-bit float")
                                          .arg(channelLayout)
                                          .arg(audioFrame.sample_rate)
                                          .arg(audioFrame.no_samples));
                    
                    m_audioInfoLogged = true;
                }
                
                m_hasSeenAudio = true;
                
                // NDI v3 audio frames are PLANAR (FLTP format) by default
                // Convert to interleaved for buffering
                bool isPlanar = (audioFrame.FourCC == NDIlib_FourCC_audio_type_FLTP);
                int channelStride = audioFrame.channel_stride_in_bytes / sizeof(float);
                
                BufferedAudioFrame buffered;
                buffered.numSamples = audioFrame.no_samples;
                buffered.sampleRate = audioFrame.sample_rate;
                buffered.numChannels = audioFrame.no_channels;
                buffered.timestamp = audioFrame.timestamp; // NDI timestamp in 100ns units
                
                if (isPlanar)
                {
                    // Convert planar to interleaved for buffering
                    QVector<float> interleavedData(audioFrame.no_samples * audioFrame.no_channels);
                    const float *p_data = (const float *)audioFrame.p_data;
                    
                    for (int s = 0; s < audioFrame.no_samples; ++s)
                    {
                        for (int c = 0; c < audioFrame.no_channels; ++c)
                        {
                            int srcIdx = c * channelStride + s;
                            int dstIdx = s * audioFrame.no_channels + c;
                            interleavedData[dstIdx] = p_data[srcIdx];
                        }
                    }
                    buffered.data = QByteArray((const char *)interleavedData.constData(), 
                                             interleavedData.size() * sizeof(float));
                }
                else
                {
                    // Already interleaved (shouldn't happen with NDI v3)
                    int audioDataSize = audioFrame.no_samples * audioFrame.no_channels * sizeof(float);
                    buffered.data = QByteArray((const char *)audioFrame.p_data, audioDataSize);
                }
                
                // Limit buffer size (max ~2 seconds at 48kHz = ~96k samples per channel)
                if (m_bufferedAudioFrames.size() < 100)
                {
                    m_bufferedAudioFrames.append(buffered);
                    Logger::instance().verbose(QString("Buffering audio frame (timestamp=%1, %2 samples). Total buffered: %3 video, %4 audio")
                                              .arg(audioFrame.timestamp)
                                              .arg(audioFrame.no_samples)
                                              .arg(m_bufferedVideoFrames.size())
                                              .arg(m_bufferedAudioFrames.size()));
                }
                
                // Check if we can establish sync
                // Require at least 1 second of buffering and both audio/video if both are present
                checkAndStartRecording();
            }
            else
            {
                // Sync established, process frames normally
                if (m_running && !m_writer.currentFile().isEmpty())
                {
                    // NDI v3 audio frames are PLANAR (FLTP format) by default, not interleaved!
                    // Each channel is stored separately with channel_stride_in_bytes stride
                    // We need to convert planar to interleaved for FFmpeg
                    int numSamples = audioFrame.no_samples;
                    int sampleRate = audioFrame.sample_rate;
                    int numChannels = audioFrame.no_channels;
                    
                    if (numSamples > 0 && sampleRate > 0 && numChannels > 0)
                    {
                        // NDI v3 audio frames are planar (FLTP format) by default
                        // Convert planar to interleaved for FFmpeg processing
                        QVector<float> interleavedData;
                        const float *audioDataPtr = nullptr;
                        
                        bool isPlanar = (audioFrame.FourCC == NDIlib_FourCC_audio_type_FLTP);
                        if (isPlanar)
                        {
                            // Planar format: each channel stored separately with stride
                            // Channel 0: p_data[0] to p_data[channelStride-1]
                            // Channel 1: p_data[channelStride] to p_data[2*channelStride-1]
                            int channelStride = audioFrame.channel_stride_in_bytes / sizeof(float);
                            interleavedData.resize(numSamples * numChannels);
                            const float *p_data = (const float *)audioFrame.p_data;
                            
                            for (int s = 0; s < numSamples; ++s)
                            {
                                for (int c = 0; c < numChannels; ++c)
                                {
                                    int srcIdx = c * channelStride + s;
                                    int dstIdx = s * numChannels + c;
                                    interleavedData[dstIdx] = p_data[srcIdx];
                                }
                            }
                            audioDataPtr = interleavedData.constData();
                        }
                        else
                        {
                            // Already interleaved (shouldn't happen with NDI v3, but handle it)
                            audioDataPtr = (const float *)audioFrame.p_data;
                        }
                        
                        // If audio streams don't exist yet, try to create them
                        // This handles the case where audio arrives after video-only recording started
                        // Note: This will fail if header is already written (MP4 limitation)
                        if (!m_writer.writeAudioFrameWithTimestamp(audioDataPtr, numSamples, sampleRate, numChannels, audioFrame.timestamp))
                        {
                            // Audio write failed - might be because header was already written
                            // Try to prepare audio streams first if they don't exist
                            // This should have been done during sync establishment, but handle late audio
                            Logger::instance().verbose(QString("Audio write failed - may be late arrival after header written. Attempting to prepare audio streams..."));
                            if (m_writer.prepareAudioStreams(sampleRate, numChannels))
                            {
                                // Retry writing audio (need to convert again if planar)
                                if (isPlanar && interleavedData.isEmpty())
                                {
                                    // Re-convert if needed
                                    int channelStride = audioFrame.channel_stride_in_bytes / sizeof(float);
                                    interleavedData.resize(numSamples * numChannels);
                                    const float *p_data = (const float *)audioFrame.p_data;
                                    for (int s = 0; s < numSamples; ++s)
                                    {
                                        for (int c = 0; c < numChannels; ++c)
                                        {
                                            int srcIdx = c * channelStride + s;
                                            int dstIdx = s * numChannels + c;
                                            interleavedData[dstIdx] = p_data[srcIdx];
                                        }
                                    }
                                    audioDataPtr = interleavedData.constData();
                                }
                                if (!m_writer.writeAudioFrameWithTimestamp(audioDataPtr, numSamples, sampleRate, numChannels, audioFrame.timestamp))
                                {
                                    Logger::instance().log(QString("Failed to write audio frame after preparing streams: %1 samples, %2 Hz, %3 channels")
                                                          .arg(numSamples).arg(sampleRate).arg(numChannels));
                                }
                            }
                            else
                            {
                                Logger::instance().log(QString("Failed to prepare audio streams (header may already be written): %1 samples, %2 Hz, %3 channels")
                                                      .arg(numSamples).arg(sampleRate).arg(numChannels));
                            }
                        }
                    }
                }
            }
            NDIlib_recv_free_audio_v3((NDIlib_recv_instance_t)m_recv, &audioFrame);
            break;
        }
        case NDIlib_frame_type_none:
            timeoutStreak++;
            if (!m_previewOnly)
            {
                Logger::instance().log("NDI timeout for " + m_settings.label);
                if (!m_recordingStarted && timeoutStreak >= 10)
                {
                    QMutexLocker locker(&m_mutex);
                    m_status = "No signal";
                    emit errorOccurred("No signal received from " + m_settings.label);
                }
            }
            else if (m_previewOnly)
            {
                // In preview mode, update status if we haven't received frames for a while
                if (timeoutStreak >= 10)
                {
                    QMutexLocker locker(&m_mutex);
                    m_status = "No signal";
                }
                else if (timeoutStreak == 1)
                {
                    QMutexLocker locker(&m_mutex);
                    m_status = "Waiting for source...";
                }
            }
            break;
        default:
            break;
        }
    }
    
    // Clean up receiver before exiting
    if (m_recv)
    {
        NDIlib_recv_destroy((NDIlib_recv_instance_t)m_recv);
        m_recv = nullptr;
    }
    
    // Before thread exits, move object back to main thread
    QThread *mainThread = QCoreApplication::instance()->thread();
    if (thread() == &m_videoThread && mainThread)
    {
        moveToThread(mainThread);
        // Emit final status update after moving back to main thread
        {
            QMutexLocker locker(&m_mutex);
            if (m_previewOnly && m_status == "Previewing")
                m_status = "Idle";
        }
        emit previewUpdated();
    }
}

void SourceRecorder::checkAndStartRecording()
{
    // Don't start a new recording if one is already in progress
    if (m_syncEstablished || m_recordingStarted)
        return;
    
    // Require at least 1 second of buffering to ensure we know what streams are available
    if (!m_bufferingTimer.isValid() || m_bufferingTimer.elapsed() < 1000)
    {
        return; // Not enough time buffered yet - need 1 second to detect streams
    }
    
    bool hasVideo = !m_bufferedVideoFrames.isEmpty();
    bool hasAudio = !m_bufferedAudioFrames.isEmpty();
    
    // After 1 second, check what streams we've seen
    // If we've seen both audio and video, require both to be present
    if (m_hasSeenVideo && m_hasSeenAudio)
    {
        if (!hasVideo || !hasAudio)
        {
            return; // Waiting for both streams to have buffered frames
        }
        
        // Check if we have frames that are reasonably close in time (within 200ms)
        // This ensures proper sync before starting recording
        bool hasCloseFrames = false;
        for (const auto &videoFrame : m_bufferedVideoFrames) {
            for (const auto &audioFrame : m_bufferedAudioFrames) {
                int64_t diff = std::abs(videoFrame.timestamp - audioFrame.timestamp);
                if (diff < 2000000LL) { // 200ms in 100ns units
                    hasCloseFrames = true;
                    break;
                }
            }
            if (hasCloseFrames) break;
        }
        
        if (!hasCloseFrames) {
            // Frames are too far apart - keep buffering to find better sync
            // But if we have enough frames, start anyway (streams may have started at different times)
            if (m_bufferedVideoFrames.size() < 5 || m_bufferedAudioFrames.size() < 5)
            {
                return; // Need more frames to establish sync
            }
        }
    }
    // If we've only seen one type after 1 second, assume that's all we're getting
    else if (m_hasSeenAudio && !m_hasSeenVideo)
    {
        // Audio-only: need at least 2 frames
        if (m_bufferedAudioFrames.size() < 2)
        {
            return;
        }
    }
    else if (m_hasSeenVideo && !m_hasSeenAudio)
    {
        // Video-only: need at least 2 frames
        if (m_bufferedVideoFrames.size() < 2)
        {
            return;
        }
    }
    else
    {
        // Haven't seen anything yet after 1 second - keep waiting
        return;
    }
    
    // All conditions met: 1 second elapsed, streams detected, start recording
    Logger::instance().verbose(QString("Starting recording after 1 second buffering: video=%1, audio=%2")
                              .arg(hasVideo ? "yes" : "no")
                              .arg(hasAudio ? "yes" : "no"));
    
    // Log stream summary before establishing sync
    if (hasVideo && !m_bufferedVideoFrames.isEmpty())
    {
        BufferedVideoFrame &vf = m_bufferedVideoFrames.first();
        float fps = (vf.fpsDen > 0) ? (float)vf.fpsNum / (float)vf.fpsDen : 0.0f;
        Logger::instance().verbose(QString("NDI Stream Summary - Video: %1x%2 @ %3 fps").arg(vf.width).arg(vf.height).arg(fps, 0, 'f', 2));
    }
    if (hasAudio && !m_bufferedAudioFrames.isEmpty())
    {
        BufferedAudioFrame &af = m_bufferedAudioFrames.first();
        QString channelLayout = (af.numChannels == 1) ? "Mono" :
                               (af.numChannels == 2) ? "Stereo" :
                               QString("%1 channels").arg(af.numChannels);
        Logger::instance().verbose(QString("NDI Stream Summary - Audio: %1 @ %2 Hz, %3 samples per frame").arg(channelLayout).arg(af.sampleRate).arg(af.numSamples));
    }
    
    establishSyncAndStartRecording();
}

void SourceRecorder::establishSyncAndStartRecording()
{
    // Don't start a new recording if one is already in progress
    if (m_syncEstablished || m_recordingStarted)
        return;
    
    // Must have at least one type of frame
    if (m_bufferedVideoFrames.isEmpty() && m_bufferedAudioFrames.isEmpty())
        return;

    bool hasVideo = !m_bufferedVideoFrames.isEmpty();
    bool hasAudio = !m_bufferedAudioFrames.isEmpty();
    
    Logger::instance().verbose(QString("Establishing sync: %1 video frames, %2 audio frames buffered (video=%3, audio=%4)")
                              .arg(m_bufferedVideoFrames.size())
                              .arg(m_bufferedAudioFrames.size())
                              .arg(hasVideo ? "yes" : "no")
                              .arg(hasAudio ? "yes" : "no"));

    // Find the sync point
    // For proper sync, we need to find frames from both streams that are closest in time
    // After buffering for 1 second, the earliest frames might be far apart
    int64_t earliestVideoTs = hasVideo ? m_bufferedVideoFrames.first().timestamp : 0;
    int64_t earliestAudioTs = hasAudio ? m_bufferedAudioFrames.first().timestamp : 0;
    int64_t latestVideoTs = hasVideo ? m_bufferedVideoFrames.last().timestamp : 0;
    int64_t latestAudioTs = hasAudio ? m_bufferedAudioFrames.last().timestamp : 0;
    
    if (hasVideo && hasAudio) {
        // Both present: find frames from both streams that are closest in time
        // This ensures proper sync even if one stream started earlier
        int64_t bestSyncTs = 0;
        int64_t minDiff = LLONG_MAX;
        const BufferedVideoFrame *bestVideoFrame = nullptr;
        const BufferedAudioFrame *bestAudioFrame = nullptr;
        
        // Try to find frames from both streams that are closest in time
        for (const auto &videoFrame : m_bufferedVideoFrames) {
            for (const auto &audioFrame : m_bufferedAudioFrames) {
                int64_t diff = std::abs(videoFrame.timestamp - audioFrame.timestamp);
                if (diff < minDiff) {
                    minDiff = diff;
                    bestVideoFrame = &videoFrame;
                    bestAudioFrame = &audioFrame;
                    // Use the later of the two timestamps as sync point
                    // This ensures we don't skip frames from the earlier stream
                    bestSyncTs = std::max(videoFrame.timestamp, audioFrame.timestamp);
                }
            }
        }
        
        // Use the best sync point we found
        m_syncTimestamp = bestSyncTs;
        
        // Log the sync quality
        if (bestVideoFrame && bestAudioFrame) {
            int64_t syncDiff = std::abs(bestVideoFrame->timestamp - bestAudioFrame->timestamp);
            Logger::instance().verbose(QString("Best sync match: video ts=%1, audio ts=%2, diff=%3 ms")
                                      .arg(bestVideoFrame->timestamp)
                                      .arg(bestAudioFrame->timestamp)
                                      .arg(syncDiff / 10000));
        }
    } else if (hasVideo) {
        // Video-only: use first video timestamp
        m_syncTimestamp = earliestVideoTs;
    } else {
        // Audio-only: use first audio timestamp
        m_syncTimestamp = earliestAudioTs;
    }
    
    if (hasVideo && hasAudio) {
        Logger::instance().verbose(QString("Sync timestamps: Video earliest=%1, latest=%2, Audio earliest=%3, latest=%4, Sync=%5 (video offset=%6 ms, audio offset=%7 ms, frame diff=%8 ms)")
                                  .arg(earliestVideoTs)
                                  .arg(latestVideoTs)
                                  .arg(earliestAudioTs)
                                  .arg(latestAudioTs)
                                  .arg(m_syncTimestamp)
                                  .arg((m_syncTimestamp - earliestVideoTs) / 10000)
                                  .arg((m_syncTimestamp - earliestAudioTs) / 10000)
                                  .arg((latestVideoTs - latestAudioTs) / 10000));
    } else if (hasVideo) {
        Logger::instance().verbose(QString("Sync timestamp (video-only): %1").arg(m_syncTimestamp));
    } else {
        Logger::instance().verbose(QString("Sync timestamp (audio-only): %1").arg(m_syncTimestamp));
    }

    // Initialize writer
    RecordingConfig cfg;
    cfg.outputFolder = m_settings.outputFolder;
    cfg.sourceLabel = m_settings.label;
    cfg.segmented = m_settings.segmented;
    cfg.segmentMinutes = m_settings.segmentMinutes;
    cfg.fileExtension = m_settings.fileExtension;
    cfg.videoCodec = m_settings.videoCodec;
    cfg.useHardwareEncoder = m_settings.useHardwareEncoder;
    
    // CRITICAL: Log the exact values being passed to FfmpegWriter
    Logger::instance().log(QString("SourceRecorder: Starting recording with videoCodec='%1', useHardwareEncoder=%2, qualityMode='%3', fileExtension='%4'")
                          .arg(cfg.videoCodec)
                          .arg(cfg.useHardwareEncoder ? "true" : "false")
                          .arg(m_settings.qualityMode)
                          .arg(cfg.fileExtension));
    cfg.qualityMode = m_settings.qualityMode;
    cfg.crfValue = m_settings.crfValue;
    cfg.videoBitrate = m_settings.videoBitrate;
    cfg.hardwareQuality = m_settings.hardwareQuality;
    
    const int defaultFps = 60;
    auto validatedFrameRate = [&](int num, int den) {
        struct {
            int fps{};
            int num{};
            int den{};
        } result;

        if (num > 0 && den > 0) {
            const double fpsValue = static_cast<double>(num) / den;
            if (fpsValue >= 1.0 && fpsValue <= 240.0) {
                result.fps = (std::max)(1, static_cast<int>(fpsValue + 0.5));
                result.num = num;
                result.den = den;
                return result;
            }
        }
        result.fps = defaultFps;
        result.num = defaultFps;
        result.den = 1;
        return result;
    };

    if (hasVideo) {
        // Initialize with video frame parameters
        const auto &firstVideo = m_bufferedVideoFrames.first();
        cfg.width = firstVideo.width;
        cfg.height = firstVideo.height;
        const auto fpsInfo = validatedFrameRate(firstVideo.fpsNum, firstVideo.fpsDen);
        cfg.fps = fpsInfo.fps;
        cfg.fpsNum = fpsInfo.num;
        cfg.fpsDen = fpsInfo.den;
        m_sourceFpsNum = fpsInfo.num;
        m_sourceFpsDen = fpsInfo.den;
        m_expectedFrameTicks10ns = (static_cast<qint64>(10000000) * fpsInfo.den) / fpsInfo.num;
        cfg.inputPixFmt = AV_PIX_FMT_RGBA;
        cfg.outputPixFmt = AV_PIX_FMT_YUV420P;
    } else {
        // Audio-only: use default video parameters (won't be used, but required by config)
        cfg.width = 1920;
        cfg.height = 1080;
        cfg.fps = defaultFps;
        cfg.fpsNum = defaultFps;
        cfg.fpsDen = 1;
        m_sourceFpsNum = defaultFps;
        m_sourceFpsDen = 1;
        m_expectedFrameTicks10ns = (static_cast<qint64>(10000000) * 1) / defaultFps;
        cfg.inputPixFmt = AV_PIX_FMT_RGBA;
        cfg.outputPixFmt = AV_PIX_FMT_YUV420P;
    }

    if (!m_writer.start(cfg)) {
        m_status = "Error";
        emit errorOccurred("Failed to start writer for " + m_settings.label);
        m_running = false;
        return;
    }

    // Set sync timestamp in writer so it can calculate PTS from NDI timestamps
    m_writer.setSyncTimestamp(m_syncTimestamp);

    m_videoPts = 0;
    m_expectedPtsStep = 1;
    m_syncEstablished = true;

    Logger::instance().verbose(QString("Sync established at timestamp %1. Flushing buffered frames.")
                              .arg(m_syncTimestamp));

    // CRITICAL: Create streams BEFORE writing header
    // This ensures both streams exist before any packets are written
    if (hasVideo)
    {
        const auto &firstVideo = m_bufferedVideoFrames.first();
        if (!m_writer.prepareVideoStream(firstVideo.width, firstVideo.height))
        {
            Logger::instance().log("Failed to prepare video stream before sync flush");
            return;
        }
    }
    
    if (hasAudio)
    {
        const auto &firstAudio = m_bufferedAudioFrames.first();
        if (!m_writer.prepareAudioStreams(firstAudio.sampleRate, firstAudio.numChannels))
        {
            Logger::instance().log("Failed to prepare audio streams before sync flush");
            return;
        }
    }
    
    // Calculate PTS offsets relative to sync timestamp
    // Video PTS: frames since sync timestamp
    // Audio PTS: samples since sync timestamp

    // Flush buffered video frames - calculate PTS from timestamps
    // Ensure PTS values are strictly increasing to avoid DTS errors
    int videoFrameIndex = 0;
    if (hasVideo) {
        // Reset video PTS to start from 0 when flushing buffered frames
        m_videoPts = -1;
        
        for (const auto &buffered : m_bufferedVideoFrames) {
            // Skip frames before sync point
            if (buffered.timestamp < m_syncTimestamp)
                continue;

            AVFrame *frame = av_frame_alloc();
            frame->format = AV_PIX_FMT_RGBA;
            frame->width = buffered.width;
            frame->height = buffered.height;
            av_image_fill_arrays(frame->data, frame->linesize, (const uint8_t *)buffered.data.constData(),
                               AV_PIX_FMT_RGBA, buffered.width, buffered.height, 1);
            
            // Calculate PTS from timestamp relative to sync point
            int64_t timeSinceSync = buffered.timestamp - m_syncTimestamp;
            // Protect against division by zero
            int64_t ptsFromTimestamp = 0;
            if (m_sourceFpsDen > 0)
            {
                ptsFromTimestamp = (timeSinceSync * m_sourceFpsNum) / (m_sourceFpsDen * 10000000LL);
            }
            else
            {
                Logger::instance().log("Invalid FPS denominator in flush, using default PTS calculation");
                ptsFromTimestamp = timeSinceSync / 10000000LL; // Fallback calculation
            }
            
            // Ensure PTS is strictly increasing (at least 1 frame apart)
            // This prevents duplicate PTS values that cause DTS errors
            if (ptsFromTimestamp <= m_videoPts)
            {
                ptsFromTimestamp = m_videoPts + 1;
            }
            frame->pts = ptsFromTimestamp;
            m_videoPts = ptsFromTimestamp;
            
            m_writer.writeVideoFrame(frame);
            av_frame_free(&frame);
            videoFrameIndex++;
        }
    }

    // Flush buffered audio frames - calculate PTS from timestamps
    if (hasAudio) {
        Logger::instance().verbose(QString("Flushing %1 buffered audio frames").arg(m_bufferedAudioFrames.size()));
        for (const auto &buffered : m_bufferedAudioFrames) {
            // Skip frames before sync point
            if (buffered.timestamp < m_syncTimestamp) {
                // Check if we need to use partial frame
                int64_t timeDiff = m_syncTimestamp - buffered.timestamp;
                // Protect against division by zero and validate sample rate
                int64_t samplesToSkip = 0;
                if (buffered.sampleRate > 0)
                {
                    samplesToSkip = (timeDiff * buffered.sampleRate) / 10000000LL;
                }
                else
                {
                    Logger::instance().log(QString("Invalid sample rate in buffered audio frame: %1").arg(buffered.sampleRate));
                    continue; // Skip this frame
                }
                if (samplesToSkip >= buffered.numSamples)
                    continue; // Skip entire frame
                
                // Use partial frame starting from sync point
                const float *audioData = (const float *)buffered.data.constData();
                int remainingSamples = buffered.numSamples - samplesToSkip;
                const float *syncStartData = audioData + (samplesToSkip * buffered.numChannels);
                
                Logger::instance().verbose(QString("Writing partial audio frame: %1 samples from timestamp %2").arg(remainingSamples).arg(m_syncTimestamp));
                if (!m_writer.writeAudioFrameWithTimestamp(syncStartData, remainingSamples, buffered.sampleRate, buffered.numChannels, m_syncTimestamp))
                {
                    Logger::instance().log("Failed to write partial audio frame during flush");
                }
                continue;
            }

            // Write complete frame with timestamp
            const float *audioData = (const float *)buffered.data.constData();
            if (!m_writer.writeAudioFrameWithTimestamp(audioData, buffered.numSamples, buffered.sampleRate, buffered.numChannels, buffered.timestamp))
            {
                Logger::instance().log("Failed to write audio frame during flush");
            }
        }
        Logger::instance().verbose("Finished flushing buffered audio frames");
    }

    // Clear buffers
    m_bufferedVideoFrames.clear();
    m_bufferedAudioFrames.clear();
    m_bufferingTimer.invalidate();
    m_hasSeenVideo = false;
    m_hasSeenAudio = false;

    m_recordingStarted = true;
    m_timer.restart();
    emit recordingStarted(m_writer.currentFile());
    
    if (hasVideo && hasAudio) {
        Logger::instance().verbose(QString("Buffered frames flushed: %1 video frames, audio from sync point")
                                  .arg(videoFrameIndex));
    } else if (hasVideo) {
        Logger::instance().verbose(QString("Buffered frames flushed: %1 video frames (video-only)")
                                  .arg(videoFrameIndex));
    } else {
        Logger::instance().verbose(QString("Buffered frames flushed: audio-only recording started"));
    }
}

