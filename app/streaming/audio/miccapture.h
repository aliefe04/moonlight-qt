#pragma once

#include <QObject>
#include <QThread>
#include <QMutex>
#include <QStringList>

#include <Limelight.h>
#if __has_include(<opus/opus.h>)
#include <opus/opus.h>
#else
#include <opus.h>
#endif

#include "SDL_compat.h"

// -----------------------------------------------------------------------
// Audio constants — must match Sunshine's mic_stream.h SAMPLES_PER_FRAME
// -----------------------------------------------------------------------
constexpr int MIC_SAMPLE_RATE        = 44100;  // Match Steam Streaming Microphone
constexpr int MIC_FRAME_DURATION_MS  = 20;
constexpr int MIC_SAMPLES_PER_FRAME  = MIC_SAMPLE_RATE * MIC_FRAME_DURATION_MS / 1000; // 882
constexpr int MIC_CHANNELS           = 2;     // stereo
constexpr int MIC_BITRATE            = 64000; // bps
constexpr int MIC_FEC_PERCENTAGE     = 20;    // 20% FEC for packet loss resilience

// -----------------------------------------------------------------------
// MicCaptureThread — SDL audio capture + Opus encode + LiSendMic* calls
// -----------------------------------------------------------------------
class MicCaptureThread : public QThread
{
    Q_OBJECT

public:
    explicit MicCaptureThread(uint8_t audioInputId = 0,
                              const QString &deviceName = {},
                              QObject *parent = nullptr);
    ~MicCaptureThread() override;

    void stopCapture();
    bool isActive() const { return m_Active; }

signals:
    void captureStarted();
    void captureStopped();
    void captureError(const QString &message);

protected:
    void run() override;

private:
    bool initOpus();
    void cleanupOpus();
    bool initSdlAudio();
    void cleanupSdlAudio();
    void processFrame(const float *samples, int frameCount);

    SDL_AudioDeviceID m_AudioDevice = 0;
    OpusEncoder      *m_OpusEncoder = nullptr;
    uint16_t          m_FrameIndex  = 0;
    uint8_t           m_AudioInputId;
    QString           m_DeviceName;   ///< SDL device name; empty = default
    bool              m_Active      = false;
};

// -----------------------------------------------------------------------
// MicCapture — singleton façade, owned by Session
// -----------------------------------------------------------------------
class MicCapture : public QObject
{
    Q_OBJECT

public:
    static MicCapture *get();

    /** Start capture (noop if host doesn't support mic passthrough). */
    bool start(uint8_t audioInputId = 0);

    /** Stop capture and wait for thread to finish. */
    void stop();

    bool isActive() const;
    bool isSupported() const;

    /**
     * @brief Return a list of available microphone device names via SDL.
     * The first entry is always "Default" (nullptr device).
     */
    Q_INVOKABLE QStringList availableDevices() const;

    /**
     * @brief Check if microphone permission has been granted.
     * @return true if permission is granted, false otherwise.
     */
    Q_INVOKABLE bool hasPermission() const;

    /**
     * @brief Request microphone permission from the user.
     * 
     * On macOS, this triggers the system permission dialog.
     * The permissionResult signal is emitted when the user responds.
     * On other platforms, immediately emits permissionResult(true).
     */
    Q_INVOKABLE void requestPermission();

    /**
     * @brief Toggle microphone capture on/off.
     * @return true if mic is now active, false if now stopped.
     */
    Q_INVOKABLE bool toggle();

signals:
    void captureStarted();
    void captureStopped();
    void captureError(const QString &message);
    
    /**
     * @brief Emitted after requestPermission() completes.
     * @param granted true if user granted permission, false if denied.
     */
    void permissionResult(bool granted);

private:
    explicit MicCapture(QObject *parent = nullptr);
    ~MicCapture() override;

    static MicCapture *s_Instance;
    MicCaptureThread  *m_Thread = nullptr;
};