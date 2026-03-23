#include "miccapture.h"
#include "settings/streamingpreferences.h"

#include <SDL.h>

MicCapture* MicCapture::s_Instance = nullptr;

MicCapture::MicCapture(QObject* parent)
    : QObject(parent),
      m_CaptureThread(nullptr),
      m_Enabled(false)
{
}

MicCapture::~MicCapture()
{
    stop();
}

MicCapture* MicCapture::get()
{
    if (!s_Instance) {
        s_Instance = new MicCapture();
    }
    return s_Instance;
}

bool MicCapture::start()
{
    if (m_CaptureThread && m_CaptureThread->isActive()) {
        return true;  // Already running
    }

    // Check if host supports mic passthrough
    if (!LiIsMicPassthroughSupported()) {
        emit captureError(tr("Host does not support microphone passthrough"));
        return false;
    }

    m_CaptureThread = new MicCaptureThread(this);
    connect(m_CaptureThread, &MicCaptureThread::captureStarted, this, &MicCapture::captureStarted);
    connect(m_CaptureThread, &MicCaptureThread::captureStopped, this, &MicCapture::captureStopped);
    connect(m_CaptureThread, &MicCaptureThread::captureError, this, &MicCapture::captureError);

    if (!m_CaptureThread->startCapture()) {
        delete m_CaptureThread;
        m_CaptureThread = nullptr;
        return false;
    }

    m_Enabled = true;
    return true;
}

void MicCapture::stop()
{
    if (m_CaptureThread) {
        m_CaptureThread->stopCapture();
        m_CaptureThread->wait();
        delete m_CaptureThread;
        m_CaptureThread = nullptr;
    }
    m_Enabled = false;
    emit captureStopped();
}

bool MicCapture::isActive() const
{
    return m_CaptureThread && m_CaptureThread->isActive();
}

bool MicCapture::isSupported() const
{
    return LiIsMicPassthroughSupported();
}

// MicCaptureThread implementation

MicCaptureThread::MicCaptureThread(QObject* parent)
    : QThread(parent),
      m_AudioDevice(0),
      m_OpusEncoder(nullptr),
      m_FrameIndex(0),
      m_AudioInputId(0),
      m_Active(false),
      m_BufferPos(0)
{
}

MicCaptureThread::~MicCaptureThread()
{
    stopCapture();
}

bool MicCaptureThread::initOpusEncoder()
{
    int error;
    m_OpusEncoder = opus_encoder_create(MIC_SAMPLE_RATE, MIC_CHANNELS,
                                          OPUS_APPLICATION_AUDIO, &error);
    if (error != OPUS_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed to create Opus encoder: %s", opus_strerror(error));
        return false;
    }

    // Set bitrate
    opus_encoder_ctl(m_OpusEncoder, OPUS_SET_BITRATE(MIC_BITRATE));

    // Enable FEC for better quality on packet loss
    opus_encoder_ctl(m_OpusEncoder, OPUS_SET_INBAND_FEC(1));

    return true;
}

void MicCaptureThread::cleanupOpusEncoder()
{
    if (m_OpusEncoder) {
        opus_encoder_destroy(m_OpusEncoder);
        m_OpusEncoder = nullptr;
    }
}

// SDL audio callback - called from SDL audio thread
static void SDLCALL micAudioCallback(void* userdata, Uint8* stream, int len)
{
    MicCaptureThread* capture = static_cast<MicCaptureThread*>(userdata);
    // This is called by SDL when audio is available
    // We'll use a different approach - queue audio instead of callback
}

bool MicCaptureThread::initSdlAudio()
{
    SDL_AudioSpec want, have;

    SDL_zero(want);
    want.freq = MIC_SAMPLE_RATE;
    want.format = AUDIO_F32SYS;
    want.channels = MIC_CHANNELS;
    want.samples = MIC_SAMPLES_PER_FRAME;
    want.callback = nullptr;  // Use queue-based capture

    // Open capture device
    m_AudioDevice = SDL_OpenAudioDevice(nullptr, 1, &want, &have, 0);
    if (m_AudioDevice == 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed to open microphone device: %s", SDL_GetError());
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Microphone opened: %d Hz, %d channels, %d samples",
                have.freq, have.channels, have.samples);

    // Start recording
    SDL_PauseAudioDevice(m_AudioDevice, 0);

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

void MicCaptureThread::processAudioFrame(const float* samples, int frameCount)
{
    if (!m_OpusEncoder || !m_Active) {
        return;
    }

    // Encode to Opus
    unsigned char opusBuffer[1024];  // Should be enough for 20ms mono at 64kbps
    int opusSize = opus_encode_float(m_OpusEncoder, samples, frameCount,
                                      opusBuffer, sizeof(opusBuffer));

    if (opusSize < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Opus encoding failed: %s", opus_strerror(opusSize));
        return;
    }

    // Send to host via control stream
    int result = LiSendMicDataEvent(m_AudioInputId, m_FrameIndex,
                                     reinterpret_cast<const char*>(opusBuffer), opusSize);

    if (result != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to send mic data: %d", result);
    }

    // Increment frame index (wraps at 65535)
    m_FrameIndex++;
}

void MicCaptureThread::run()
{
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Microphone capture thread started");

    // Initialize SDL audio subsystem if not already initialized
    if (!SDL_WasInit(SDL_INIT_AUDIO)) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            emit captureError(tr("Failed to initialize SDL audio: %1").arg(SDL_GetError()));
            return;
        }
    }

    // Initialize Opus encoder
    if (!initOpusEncoder()) {
        emit captureError(tr("Failed to initialize Opus encoder"));
        return;
    }

    // Initialize SDL audio capture
    if (!initSdlAudio()) {
        cleanupOpusEncoder();
        emit captureError(tr("Failed to open microphone device"));
        return;
    }

    // Send mic start event to host
    int result = LiSendMicStartEvent(m_AudioInputId, LI_MIC_CODEC_OPUS,
                                      MIC_CHANNELS, MIC_SAMPLE_RATE, MIC_BITRATE);
    if (result != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed to send mic start event: %d", result);
        cleanupSdlAudio();
        cleanupOpusEncoder();
        emit captureError(tr("Failed to start microphone stream on host"));
        return;
    }

    m_Active = true;
    m_FrameIndex = 0;
    m_BufferPos = 0;
    emit captureStarted();

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Microphone capture started successfully");

    // Main capture loop
    while (m_Active && !isInterruptionRequested()) {
        // Check how much audio is available
        Uint32 queuedBytes = SDL_GetQueuedAudioSize(m_AudioDevice);

        // Calculate how many samples are available
        int samplesAvailable = queuedBytes / (sizeof(float) * MIC_CHANNELS);

        // Wait until we have at least one full frame
        if (samplesAvailable < MIC_SAMPLES_PER_FRAME) {
            // Wait a bit for more audio
            msleep(5);
            continue;
        }

        // Read one frame of audio
        float frameBuffer[MIC_SAMPLES_PER_FRAME * MIC_CHANNELS];
        Uint32 bytesRead = SDL_DequeueAudio(m_AudioDevice,
                                             frameBuffer,
                                             MIC_SAMPLES_PER_FRAME * sizeof(float) * MIC_CHANNELS);

        if (bytesRead > 0) {
            int samplesRead = bytesRead / (sizeof(float) * MIC_CHANNELS);
            processAudioFrame(frameBuffer, samplesRead);
        }
    }

    // Send mic stop event to host
    LiSendMicStopEvent(m_AudioInputId);

    // Cleanup
    cleanupSdlAudio();
    cleanupOpusEncoder();

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Microphone capture thread stopped");
    emit captureStopped();
}

bool MicCaptureThread::startCapture()
{
    if (m_Active) {
        return true;
    }

    start();  // Start QThread
    return true;
}

void MicCaptureThread::stopCapture()
{
    if (!m_Active) {
        return;
    }

    m_Active = false;
    requestInterruption();
    wait();  // Wait for thread to finish
}