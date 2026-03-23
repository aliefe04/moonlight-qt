#pragma once

#include <QObject>
#include <QThread>
#include <QMutex>

#include <Limelight.h>
#include <opus.h>
#include "SDL_compat.h"

// Microphone capture constants
constexpr int MIC_SAMPLE_RATE = 48000;
constexpr int MIC_FRAME_DURATION_MS = 20;
constexpr int MIC_SAMPLES_PER_FRAME = MIC_SAMPLE_RATE * MIC_FRAME_DURATION_MS / 1000;  // 960 samples
constexpr int MIC_CHANNELS = 1;  // Mono
constexpr int MIC_BITRATE = 64000;

class MicCaptureThread : public QThread
{
    Q_OBJECT

public:
    explicit MicCaptureThread(QObject* parent = nullptr);
    virtual ~MicCaptureThread();

    bool startCapture();
    void stopCapture();

    bool isActive() const { return m_Active; }

signals:
    void captureStarted();
    void captureStopped();
    void captureError(const QString& error);

protected:
    void run() override;

private:
    bool initOpusEncoder();
    void cleanupOpusEncoder();
    bool initSdlAudio();
    void cleanupSdlAudio();
    void processAudioFrame(const float* samples, int frameCount);

    SDL_AudioDeviceID m_AudioDevice;
    OpusEncoder* m_OpusEncoder;
    uint16_t m_FrameIndex;
    uint8_t m_AudioInputId;
    bool m_Active;
    QMutex m_Mutex;

    // Audio buffer for capture
    float m_AudioBuffer[MIC_SAMPLES_PER_FRAME * MIC_CHANNELS];
    int m_BufferPos;
};

class MicCapture : public QObject
{
    Q_OBJECT

public:
    static MicCapture* get();

    Q_INVOKABLE bool start();
    Q_INVOKABLE void stop();
    Q_INVOKABLE bool isActive() const;
    Q_INVOKABLE bool isSupported() const;

signals:
    void captureStarted();
    void captureStopped();
    void captureError(const QString& error);

private:
    explicit MicCapture(QObject* parent = nullptr);
    ~MicCapture();

    static MicCapture* s_Instance;

    MicCaptureThread* m_CaptureThread;
    bool m_Enabled;
};