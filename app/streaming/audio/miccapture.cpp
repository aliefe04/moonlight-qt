#include "miccapture.h"
#include "settings/streamingpreferences.h"

// -----------------------------------------------------------------------
// MicCaptureThread
// -----------------------------------------------------------------------

MicCaptureThread::MicCaptureThread(uint8_t audioInputId, const QString &deviceName, QObject *parent)
    : QThread(parent)
    , m_AudioInputId(audioInputId)
    , m_DeviceName(deviceName)
{}

MicCaptureThread::~MicCaptureThread()
{
    stopCapture();
    wait();
}

void MicCaptureThread::stopCapture()
{
    m_Active = false;
    requestInterruption();
}

bool MicCaptureThread::initOpus()
{
    int err = OPUS_OK;
    m_OpusEncoder = opus_encoder_create(MIC_SAMPLE_RATE, MIC_CHANNELS,
                                        OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK || !m_OpusEncoder) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed to create Opus encoder: %s", opus_strerror(err));
        return false;
    }
    opus_encoder_ctl(m_OpusEncoder, OPUS_SET_BITRATE(MIC_BITRATE));
    opus_encoder_ctl(m_OpusEncoder, OPUS_SET_INBAND_FEC(1));   // packet-loss resilience
    opus_encoder_ctl(m_OpusEncoder, OPUS_SET_DTX(0));          // continuous transmission
    return true;
}

void MicCaptureThread::cleanupOpus()
{
    if (m_OpusEncoder) {
        opus_encoder_destroy(m_OpusEncoder);
        m_OpusEncoder = nullptr;
    }
}

bool MicCaptureThread::initSdlAudio()
{
    SDL_AudioSpec want{}, have{};
    want.freq     = MIC_SAMPLE_RATE;
    want.format   = AUDIO_F32SYS;   // 32-bit float, native endian
    want.channels = static_cast<Uint8>(MIC_CHANNELS);
    want.samples  = static_cast<Uint16>(MIC_SAMPLES_PER_FRAME);
    want.callback = nullptr;         // queue-based capture

    // Use the user-selected device or nullptr for the system default
    const char *devName = m_DeviceName.isEmpty()
                          ? nullptr
                          : m_DeviceName.toUtf8().constData();
    m_AudioDevice = SDL_OpenAudioDevice(devName, 1, &want, &have, 0);
    if (m_AudioDevice == 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed to open microphone: %s", SDL_GetError());
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Microphone opened: %d Hz, %d ch, %d samples/frame",
                have.freq, have.channels, have.samples);

    SDL_PauseAudioDevice(m_AudioDevice, 0); // start recording
    return true;
}

void MicCaptureThread::cleanupSdlAudio()
{
    if (m_AudioDevice != 0) {
        SDL_PauseAudioDevice(m_AudioDevice, 1);
        SDL_CloseAudioDevice(m_AudioDevice);
        m_AudioDevice = 0;
    }
}

void MicCaptureThread::processFrame(const float *samples, int frameCount)
{
    if (!m_OpusEncoder || !m_Active) return;

    // Encode float PCM → Opus.  4000 bytes is well above any 20ms Opus frame.
    unsigned char opusBuf[4000];
    int opusLen = opus_encode_float(m_OpusEncoder, samples, frameCount,
                                    opusBuf, static_cast<opus_int32>(sizeof(opusBuf)));
    if (opusLen < 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Opus encode error: %s", opus_strerror(opusLen));
        return;
    }

    int rc = LiSendMicDataEvent(m_AudioInputId, m_FrameIndex,
                                reinterpret_cast<const char *>(opusBuf), opusLen);
    if (rc != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "LiSendMicDataEvent failed: %d", rc);
    }

    ++m_FrameIndex; // wraps naturally at uint16_t overflow
}

void MicCaptureThread::run()
{
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Mic capture thread starting");

    // Ensure SDL audio subsystem is up
    if (!SDL_WasInit(SDL_INIT_AUDIO)) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            emit captureError(tr("Failed to initialise SDL audio: %1").arg(SDL_GetError()));
            return;
        }
    }

    if (!initOpus()) {
        emit captureError(tr("Failed to initialise Opus encoder"));
        return;
    }

    if (!initSdlAudio()) {
        cleanupOpus();
        emit captureError(tr("Failed to open microphone device"));
        return;
    }

    // Tell Sunshine we are starting a mic stream
    int rc = LiSendMicStartEvent(m_AudioInputId,
                                 LI_MIC_CODEC_OPUS,
                                 static_cast<uint8_t>(MIC_CHANNELS),
                                 static_cast<uint32_t>(MIC_SAMPLE_RATE),
                                 static_cast<uint32_t>(MIC_BITRATE));
    if (rc != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "LiSendMicStartEvent failed: %d", rc);
        cleanupSdlAudio();
        cleanupOpus();
        emit captureError(tr("Failed to start microphone stream on host"));
        return;
    }

    m_Active      = true;
    m_FrameIndex  = 0;
    emit captureStarted();

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Mic capture running");

    // Capture loop: dequeue exactly one frame worth of float samples at a time
    const Uint32 frameSizeBytes = static_cast<Uint32>(MIC_SAMPLES_PER_FRAME * sizeof(float));
    float frameBuf[MIC_SAMPLES_PER_FRAME];

    while (m_Active && !isInterruptionRequested()) {
        if (SDL_GetQueuedAudioSize(m_AudioDevice) >= frameSizeBytes) {
            Uint32 read = SDL_DequeueAudio(m_AudioDevice, frameBuf, frameSizeBytes);
            if (read == frameSizeBytes) {
                processFrame(frameBuf, MIC_SAMPLES_PER_FRAME);
            }
        } else {
            // Wait for more audio (half a frame ≈ 10 ms)
            msleep(10);
        }
    }

    // Notify Sunshine we are done
    LiSendMicStopEvent(m_AudioInputId);

    cleanupSdlAudio();
    cleanupOpus();

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Mic capture thread stopped");
    emit captureStopped();
}

// -----------------------------------------------------------------------
// MicCapture singleton
// -----------------------------------------------------------------------

QStringList MicCapture::availableDevices() const
{
    QStringList devices;
    devices.append(tr("Default"));  // index 0 = nullptr device name

    // SDL must be initialised to enumerate devices
    bool initedHere = false;
    if (!SDL_WasInit(SDL_INIT_AUDIO)) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
            initedHere = true;
        } else {
            return devices;
        }
    }

    int count = SDL_GetNumAudioDevices(1 /* capture */);
    for (int i = 0; i < count; ++i) {
        const char *name = SDL_GetAudioDeviceName(i, 1);
        if (name) {
            devices.append(QString::fromUtf8(name));
        }
    }

    if (initedHere) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }

    return devices;
}

MicCapture *MicCapture::s_Instance = nullptr;

MicCapture::MicCapture(QObject *parent) : QObject(parent) {}

MicCapture::~MicCapture()
{
    stop();
}

MicCapture *MicCapture::get()
{
    if (!s_Instance) {
        s_Instance = new MicCapture();
    }
    return s_Instance;
}

bool MicCapture::start(uint8_t audioInputId)
{
    if (m_Thread && m_Thread->isActive()) {
        return true; // already running
    }

    if (!LiIsMicPassthroughSupported()) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Mic passthrough not supported by host — skipping");
        return false;
    }

    if (m_Thread) {
        m_Thread->wait();
        delete m_Thread;
        m_Thread = nullptr;
    }

    // Use the device name the user selected (empty = system default)
    QString deviceName;
    auto *prefs = StreamingPreferences::get();
    if (prefs) {
        deviceName = prefs->micDeviceName;
    }

    m_Thread = new MicCaptureThread(audioInputId, deviceName, this);
    connect(m_Thread, &MicCaptureThread::captureStarted, this, &MicCapture::captureStarted);
    connect(m_Thread, &MicCaptureThread::captureStopped, this, &MicCapture::captureStopped);
    connect(m_Thread, &MicCaptureThread::captureError,   this, &MicCapture::captureError);
    connect(m_Thread, &MicCaptureThread::finished,       m_Thread, &QObject::deleteLater);
    m_Thread->start();
    return true;
}

void MicCapture::stop()
{
    if (m_Thread) {
        m_Thread->stopCapture();
        m_Thread->wait(3000); // up to 3 s
        if (m_Thread->isRunning()) {
            m_Thread->terminate();
        }
        m_Thread = nullptr;
    }
}

bool MicCapture::isActive() const
{
    return m_Thread && m_Thread->isActive();
}

bool MicCapture::isSupported() const
{
    return LiIsMicPassthroughSupported();
}
