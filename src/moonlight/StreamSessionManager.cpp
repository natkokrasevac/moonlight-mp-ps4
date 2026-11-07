// Copyright (C) 2026 fourletterworkshop.com
// This file is part of Moonlight DX for PlayStation 4.
// Moonlight DX for PlayStation 4 is free software: you can redistribute it
// and/or modify it under the terms of the GNU General Public License as
// published by the Free Software Foundation, either version 3 of the License,
// or (at your option) any later version.
// Moonlight DX for PlayStation 4 is distributed in the hope that it will be
// useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
// Public License for more details.
// You should have received a copy of the GNU General Public License along with
// Moonlight DX for PlayStation 4. If not, see <https://www.gnu.org/licenses/>.
//

// === StreamSessionManager.cpp ===============================================
//
// OpenOrbis Moonlight client — Stream Session / moonlight-common-c
// Connection Integration
//
// Implements the StreamSessionManager concrete stream connection subsystem:
//   - One lifecycle worker thread per active connection.
//   - LiStartConnection() / LiStopConnection() / LiInterruptConnection().
//   - Connection listener / video / audio callback adaptation.
//   - Bounded connection-event queue.
//   - Deterministic stop, interruption, failure, and shutdown.
//
// The dependency stack (moonlight-common-c, ENet, nanors) is used UNPATCHED.
// Video callbacks forward encoded decode units to the VideoDecoder, and audio
// callbacks enqueue encoded Opus packets for the AudioDecodeOutput.
//
// moonlight-common-c C headers are confined to this translation unit.
// No Moonlight C types escape into public headers.
//
// OpenOrbis adaptation: uses POSIX pthread (pthread_create/join/mutex/cond)
// matching the proven OpenOrbis threading path used by ApplicationCatalog /
// ApplicationSessionManager / DiscoveryManager. The SCE reference's
// ScePthread* types are replaced with their POSIX equivalents. IPv4 validation
// uses the native sceNetInetPton path (orbis/Net.h) matching DiscoveryManager.
//
// OpenOrbis SDK includes are confined to this translation unit.
// ===============================================================================

#include "StreamSessionManager.h"
#include "VideoDecoder.h"
#include "platform/ps4/InputManager.h"

// --- moonlight-common-c (C) header — confined to this translation unit --------
#include "Limelight.h"

// --- Native PS4 network API (proven OpenOrbis path) --------------------------
#include <orbis/Net.h>         // sceNetInetPton, OrbisNetInAddr, ORBIS_NET_AF_INET

// --- POSIX threading headers (OpenOrbis libc) --------------------------------
#include <pthread.h>           // pthread_create, pthread_join, pthread_mutex_*, pthread_cond_*

#include <orbis/libkernel.h>   // sceKernelGetProcessTime, sceKernelUsleep

#include <cstdio>              // printf, fflush, snprintf
#include <cstring>             // memset, memcpy
#include <cstdarg>             // va_list, va_start, va_end
#include <deque>
#include <atomic>
#include <utility>             // std::move

// ---------------------------------------------------------------------------
// Diagnostics helper (minimal; consistent with other modules)
// ---------------------------------------------------------------------------

static void ssmDiagPrint(const char* msg) {
    printf("%s\n", msg);
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// Module constants
// ---------------------------------------------------------------------------

namespace {

/// Connection event queue capacity.
constexpr size_t kEventQueueCapacity = 16;

/// Audio queue depth (packets).
constexpr size_t kAudioQueueCapacity = 30;

/// Maximum encoded audio packet size (bytes).
/// Matches MAX_PACKET_SIZE in moonlight-common-c AudioStream.c.
constexpr size_t kMaxAudioPacketSize = 1400;

/// Remote input AES key/IV length (bytes).
constexpr size_t kRemoteInputKeyLen = 16;

} // namespace

// ---------------------------------------------------------------------------
// Private implementation state
// ---------------------------------------------------------------------------

namespace ml {

/// Private implementation of StreamSessionManager.
struct StreamSessionManager::Impl {
    // --- Lifecycle ----------------------------------------------------------
    std::atomic<bool> stopRequested{false};
    std::atomic<bool> running{false};
    bool threadCreated = false;
    pthread_t workerThread;

    // --- Session state ------------------------------------------------------
    std::atomic<StreamSessionState> state{StreamSessionState::Idle};
    std::atomic<bool> stopCalled{false};
    std::atomic<bool> workerActive{false};
    std::atomic<bool> terminationRequested{false};  ///< Remote termination received.
    std::atomic<bool> streamEverStarted{false};     ///< True once start() has been called.
    /// Session generation counter. Incremented each time start() begins a new
    /// stream session. Used to make audio-consumer waiting/termination
    /// semantics session-specific rather than process-lifetime/"ever started"
    /// based. A newly started AudioDecodeOutput worker captures this (and the
    /// current state) at start to determine which session it will consume.
    std::atomic<uint32_t> sessionGeneration{0};
    int terminationErrorCode = 0;                   ///< Guarded by wakeMutex.

    // --- Active session descriptor (moved in on start) ----------------------
    StreamSessionDescriptor activeSession;

    // --- Negotiated stream metadata -----------------------------------------
    int negotiatedVideoFormat = 0;
    int videoWidth = 0;
    int videoHeight = 0;
    int videoRedrawRate = 0;
    AudioStreamConfiguration audioConfig;

    // --- Temporary first-media diagnostics ----------------------------------
    std::atomic<bool> firstVideoLogged{false};
    std::atomic<bool> firstAudioLogged{false};

    // --- Stream statistics --------------------------------------------------
    /// Cumulative encoded-video bytes received for the current session.
    /// Incremented in the video submit callback (counts the assembled AU
    /// bytes). Reset when a new stream session starts. Read by MoonlightApp
    /// once per second to compute the measured encoded-video bitrate.
    std::atomic<uint64_t> statsRxBytes{0};

    // --- VideoDecoder dependency (non-owning) -------------------------------
    // Set via setVideoDecoder() before start(). The decoder must outlive the
    // active stream session. Video callbacks forward to it.
    VideoDecoder* videoDecoder = nullptr;

    // --- InputManager dependency (non-owning) -------------------------------
    // Set via setInputManager() before start(). Receives rumble and LED callbacks.
    ps4::InputManager* inputManager = nullptr;

    // --- Connection event queue ---------------------------------------------
    pthread_mutex_t eventMutex;
    bool eventMutexInit = false;
    std::deque<StreamSessionUpdate> eventQueue;

    // --- Audio queue ---------------------------------------------------------
    pthread_mutex_t audioMutex;
    bool audioMutexInit = false;
    pthread_cond_t audioCond;
    bool audioCondInit = false;
    std::deque<EncodedAudioPacket> audioQueue;

    // --- Worker wake ---------------------------------------------------------
    pthread_mutex_t wakeMutex;
    bool wakeMutexInit = false;
    pthread_cond_t wakeCond;
    bool wakeCondInit = false;

    // --- Helper: push a connection event (bounded, terminal preserved) ------
    void pushEvent(const StreamSessionUpdate& update) {
        pthread_mutex_lock(&eventMutex);
        if (eventQueue.size() >= kEventQueueCapacity) {
            // Terminal events must never be dropped.
            bool terminal = (update.type == StreamSessionUpdateType::Connected ||
                             update.type == StreamSessionUpdateType::Terminated ||
                             update.type == StreamSessionUpdateType::Failed ||
                             update.type == StreamSessionUpdateType::StoppedByUser);
            if (terminal) {
                // Drop the oldest non-terminal event if any, else drop oldest.
                bool dropped = false;
                for (size_t i = 0; i < eventQueue.size(); ++i) {
                    auto t = eventQueue[i].type;
                    if (t != StreamSessionUpdateType::Connected &&
                        t != StreamSessionUpdateType::Terminated &&
                        t != StreamSessionUpdateType::Failed &&
                        t != StreamSessionUpdateType::StoppedByUser) {
                        eventQueue.erase(eventQueue.begin() + static_cast<long>(i));
                        dropped = true;
                        break;
                    }
                }
                if (!dropped) {
                    eventQueue.pop_front();
                }
            } else {
                eventQueue.pop_front();
            }
        }
        eventQueue.push_back(update);
        pthread_mutex_unlock(&eventMutex);
    }

    // --- Helper: push an audio packet (bounded) -----------------------------
    void pushAudio(EncodedAudioPacket&& packet) {
        pthread_mutex_lock(&audioMutex);
        if (audioQueue.size() >= kAudioQueueCapacity) {
            audioQueue.pop_front();
        }
        audioQueue.push_back(std::move(packet));
        pthread_cond_signal(&audioCond);
        pthread_mutex_unlock(&audioMutex);
    }
};

// Forward declaration of the active-owner accessor.
StreamSessionManager::Impl* ssmGetActiveOwner();

// ---------------------------------------------------------------------------
// Static callback adapters (moonlight-common-c C callbacks)
// ---------------------------------------------------------------------------

namespace {

// --- Connection listener callbacks ------------------------------------------

void ssmClStageStarting(int stage) {
    const char* name = LiGetStageName(stage);
    char buf[128];
    snprintf(buf, sizeof(buf), "[stream] stage starting: %s", name ? name : "?");
    ssmDiagPrint(buf);
}

void ssmClStageComplete(int stage) {
    const char* name = LiGetStageName(stage);
    char buf[128];
    snprintf(buf, sizeof(buf), "[stream] stage complete: %s", name ? name : "?");
    ssmDiagPrint(buf);
}

void ssmClStageFailed(int stage, int errorCode) {
    const char* name = LiGetStageName(stage);
    char buf[160];
    snprintf(buf, sizeof(buf), "[stream] stage failed: %s (error %d)",
             name ? name : "?", errorCode);
    ssmDiagPrint(buf);
}

void ssmClConnectionStarted(void) {
    ssmDiagPrint("[stream] CONNECTION STARTED (connectionStarted callback fired)");
}

void ssmClConnectionTerminated(int errorCode) {
    char buf[128];
    snprintf(buf, sizeof(buf), "[stream] connection terminated (error %d)", errorCode);
    ssmDiagPrint(buf);

    // Signal the lifecycle worker to tear down the connection.
    StreamSessionManager::Impl* owner = ssmGetActiveOwner();
    if (owner != nullptr) {
        if (owner->inputManager) {
            owner->inputManager->stopAllVibration();
            owner->inputManager->resetLightBars();
        }
        owner->terminationRequested = true;
        owner->terminationErrorCode = errorCode;
        pthread_mutex_lock(&owner->wakeMutex);
        pthread_cond_signal(&owner->wakeCond);
        pthread_mutex_unlock(&owner->wakeMutex);
    }
}

void ssmClLogMessage(const char* format, ...) {
    va_list args;
    va_start(args, format);
    char buf[512];
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    char out[576];
    snprintf(out, sizeof(out), "[moonlight] %s", buf);
    ssmDiagPrint(out);
}

void ssmClRumble(unsigned short controllerNumber, unsigned short lowFreqMotor,
                 unsigned short highFreqMotor) {
    StreamSessionManager::Impl* owner = ssmGetActiveOwner();
    if (owner && owner->inputManager) {
        owner->inputManager->setVibration(static_cast<int>(controllerNumber),
                                          lowFreqMotor, highFreqMotor);
    }
}

void ssmClConnectionStatusUpdate(int connectionStatus) {
    char buf[128];
    snprintf(buf, sizeof(buf), "[stream] connection status: %s",
             connectionStatus == CONN_STATUS_OKAY ? "okay" : "poor");
    ssmDiagPrint(buf);
}

void ssmClSetHdrMode(bool hdrEnabled) {
    char buf[128];
    snprintf(buf, sizeof(buf), "[stream] HDR mode: %s", hdrEnabled ? "on" : "off");
    ssmDiagPrint(buf);
}

void ssmClRumbleTriggers(uint16_t controllerNumber, uint16_t leftTriggerMotor,
                         uint16_t rightTriggerMotor) {
    (void)controllerNumber; (void)leftTriggerMotor; (void)rightTriggerMotor;
}

void ssmClSetMotionEventState(uint16_t controllerNumber, uint8_t motionType,
                              uint16_t reportRateHz) {
    (void)controllerNumber; (void)motionType; (void)reportRateHz;
}

void ssmClSetControllerLED(uint16_t controllerNumber, uint8_t r, uint8_t g, uint8_t b) {
    StreamSessionManager::Impl* owner = ssmGetActiveOwner();
    if (owner && owner->inputManager) {
        bool isReset = (r == 0 && g == 0 && b == 0);
        owner->inputManager->setLightBar(static_cast<int>(controllerNumber),
                                         r, g, b, !isReset);
    }
}

void ssmClSetAdaptiveTriggers(uint16_t controllerNumber, uint8_t eventFlags,
                              uint8_t typeLeft, uint8_t typeRight,
                              uint8_t* left, uint8_t* right) {
    (void)controllerNumber; (void)eventFlags; (void)typeLeft; (void)typeRight;
    (void)left; (void)right;
}

// --- Video renderer callbacks (forward to VideoDecoder) ----------------------

int ssmVideoSetup(int videoFormat, int width, int height, int redrawRate,
                  void* context, int drFlags) {
    StreamSessionManager::Impl* impl =
        static_cast<StreamSessionManager::Impl*>(context);
    (void)drFlags;

    // Validate: only H.264 and H.265/HEVC are supported (the launch advertises
    // the selected codec). H.265/HEVC is only requested on a PS4 Pro.
    if (videoFormat != VIDEO_FORMAT_H264 && videoFormat != VIDEO_FORMAT_H265) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[stream] video setup rejected: unsupported format 0x%x (H.264/H.265 only)",
                 videoFormat);
        ssmDiagPrint(buf);

        StreamSessionUpdate update;
        update.type = StreamSessionUpdateType::Failed;
        update.result = StreamSessionResultCategory::UnsupportedVideoFormat;
        update.hostId = impl->activeSession.hostId;
        update.applicationId = impl->activeSession.applicationId;
        impl->pushEvent(update);

        return -1;  // non-zero = failure
    }

    impl->negotiatedVideoFormat = videoFormat;
    impl->videoWidth = width;
    impl->videoHeight = height;
    impl->videoRedrawRate = redrawRate;

    char buf[160];
    snprintf(buf, sizeof(buf),
             "[stream] video setup: format=%s %dx%d @ %d fps",
             videoFormat == VIDEO_FORMAT_H265 ? "HEVC" : "H264",
             width, height, redrawRate);
    ssmDiagPrint(buf);

    // --- Forward to the VideoDecoder ----------------------------------------
    // Start a decoder session with the negotiated video parameters. The codec
    // is derived from the negotiated video format (H.265/HEVC on PS4 Pro).
    if (impl->videoDecoder != nullptr) {
        ml::VideoDecoderConfiguration vdConfig;
        vdConfig.width = static_cast<uint32_t>(width);
        vdConfig.height = static_cast<uint32_t>(height);
        vdConfig.fps = static_cast<uint32_t>(redrawRate);
        // Requested stream bitrate (kbps). The decoder selects a level high
        // enough to cover this bitrate so the configured level always exceeds
        // the stream (otherwise the decoder core rejects it with
        // SCE_VIDEODEC2_ERROR_OVERSIZE_DECODE).
        vdConfig.bitrateKbps = static_cast<uint32_t>(impl->activeSession.configuration.bitrate);
        vdConfig.codec = (videoFormat == VIDEO_FORMAT_H265)
                             ? ml::VideoCodec::H265
                             : ml::VideoCodec::H264;
        vdConfig.colorSpace = static_cast<uint32_t>(impl->activeSession.configuration.colorSpace);
        vdConfig.colorRange = static_cast<uint32_t>(impl->activeSession.configuration.colorRange);

        ml::VideoDecoderResult vdResult = impl->videoDecoder->start(vdConfig);
        if (!vdResult.succeeded()) {
            char buf2[160];
            snprintf(buf2, sizeof(buf2),
                     "[stream] video decoder start failed (category %d nativeError %d)",
                     static_cast<int>(vdResult.category), vdResult.nativeError);
            ssmDiagPrint(buf2);

            StreamSessionUpdate update;
            update.type = StreamSessionUpdateType::Failed;
            update.result = StreamSessionResultCategory::UnsupportedVideoFormat;
            update.hostId = impl->activeSession.hostId;
            update.applicationId = impl->activeSession.applicationId;
            impl->pushEvent(update);

            return -1;  // non-zero = failure
        }
    }

    return 0;  // success
}

void ssmVideoStart(void) {
    ssmDiagPrint("[stream] video start");
}

void ssmVideoStop(void) {
    ssmDiagPrint("[stream] video stop");

    // NOTE: The VideoDecoder is NOT stopped here. This callback runs on a
    // moonlight-common-c library thread. Stopping the decoder here would race
    // with the main thread's presentation path (which may be concurrently
    // reading a decoded frame). The main thread stops the decoder when it
    // processes the terminal stream update (StoppedByUser/Terminated/Failed)
    // in MoonlightApp::handleStreamSessionUpdate, after m_streamActive is
    // cleared and the held frame is released.
}

void ssmVideoCleanup(void) {
    ssmDiagPrint("[stream] video cleanup");
}

int ssmVideoSubmitDecodeUnit(PDECODE_UNIT decodeUnit) {
    // The submitDecodeUnit callback does not receive a context pointer in this
    // revision. Use the private static active-owner bridge.
    StreamSessionManager::Impl* owner = ssmGetActiveOwner();
    if (owner == nullptr) {
        return DR_OK;
    }

    // Validate the decode unit.
    if (decodeUnit == nullptr || decodeUnit->bufferList == nullptr ||
        decodeUnit->fullLength <= 0) {
        return DR_NEED_IDR;
    }

    // Temporary diagnostic: log only the first received decode unit.
    if (!owner->firstVideoLogged.exchange(true)) {
        char buf[192];
        snprintf(buf, sizeof(buf),
                 "[stream] FIRST VIDEO DECODE UNIT: frame=%u type=%s bytes=%d",
                 static_cast<unsigned>(decodeUnit->frameNumber),
                 decodeUnit->frameType == FRAME_TYPE_IDR ? "IDR" : "PFRAME",
                 decodeUnit->fullLength);
        ssmDiagPrint(buf);
    }

    // --- Forward to the VideoDecoder ----------------------------------------
    // Assemble the DECODE_UNIT buffer chain into a contiguous Annex-B AU and
    // push it to the decoder's input queue.
    if (owner->videoDecoder == nullptr) {
        // No decoder — discard (diagnostic sink behavior).
        return DR_OK;
    }

    // Validate the decoder is running or recovering.
    ml::VideoDecoderState vdState = owner->videoDecoder->state();
    if (vdState != ml::VideoDecoderState::Running &&
        vdState != ml::VideoDecoderState::Starting &&
        vdState != ml::VideoDecoderState::WaitingForIdr) {
        return DR_OK;
    }

    // Assemble the buffer chain into a contiguous AU.
    ml::EncodedVideoUnit unit;
    unit.frameNumber = static_cast<uint64_t>(decodeUnit->frameNumber);
    unit.frameType = static_cast<uint32_t>(decodeUnit->frameType);
    // Set the codec type from the negotiated video format. H.264 = 0x01;
    // H.265/HEVC = 0xEE049 (the Videodec2 HEVC codec type). The decoder
    // validates the unit against the active codec.
    unit.codecType = (owner->negotiatedVideoFormat == VIDEO_FORMAT_H265)
                         ? 0xEE049u
                         : 0x01u;
    unit.rtpTimestamp = decodeUnit->rtpTimestamp;
    unit.presentationTimeUs = decodeUnit->presentationTimeUs;
    unit.fullLength = static_cast<uint32_t>(decodeUnit->fullLength);

    // Count fragments and reserve capacity.
    uint32_t fragmentCount = 0;
    for (PLENTRY e = decodeUnit->bufferList; e != nullptr; e = e->next) {
        fragmentCount++;
    }
    unit.fragmentCount = fragmentCount;

    // Copy the buffer chain into the contiguous AU. The host's SPS/PPS/VPS
    // are passed through unchanged.
    unit.data.reserve(static_cast<size_t>(decodeUnit->fullLength));
    for (PLENTRY e = decodeUnit->bufferList; e != nullptr; e = e->next) {
        if (e->data == nullptr || e->length <= 0) {
            continue;
        }
        unit.data.insert(unit.data.end(),
                         reinterpret_cast<const uint8_t*>(e->data),
                         reinterpret_cast<const uint8_t*>(e->data) + e->length);
    }

    // Verify the assembled length matches the declared fullLength.
    if (unit.data.size() != unit.fullLength) {
        // Mismatch — discard and request IDR.
        return DR_NEED_IDR;
    }

    // Accumulate the received encoded-video bytes for the statistics overlay.
    // This is a project-owned counter (not a moonlight-common-c patch); it
    // counts the assembled access-unit bytes received for the current session.
    owner->statsRxBytes.fetch_add(unit.data.size(), std::memory_order_relaxed);

    // Push the unit to the decoder.
    if (!owner->videoDecoder->pushEncodedUnit(std::move(unit))) {
        // Queue full or decoder not accepting — request IDR.
        return DR_NEED_IDR;
    }

    return DR_OK;
}

// --- Audio renderer callbacks (enqueue for AudioDecodeOutput) ----------------

int ssmAudioInit(int audioConfiguration, const POPUS_MULTISTREAM_CONFIGURATION opusConfig,
                 void* context, int arFlags) {
    StreamSessionManager::Impl* impl =
        static_cast<StreamSessionManager::Impl*>(context);
    (void)arFlags;

    // Validate: only stereo is supported (launch advertised stereo).
    if (CHANNEL_COUNT_FROM_AUDIO_CONFIGURATION(audioConfiguration) != 2) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[stream] audio init rejected: unsupported channel count %d (stereo only)",
                 CHANNEL_COUNT_FROM_AUDIO_CONFIGURATION(audioConfiguration));
        ssmDiagPrint(buf);

        StreamSessionUpdate update;
        update.type = StreamSessionUpdateType::Failed;
        update.result = StreamSessionResultCategory::UnsupportedAudioConfiguration;
        update.hostId = impl->activeSession.hostId;
        update.applicationId = impl->activeSession.applicationId;
        impl->pushEvent(update);

        return -1;  // non-zero = failure
    }

    // Capture the Opus multistream configuration.
    if (opusConfig != nullptr) {
        impl->audioConfig.sampleRate = opusConfig->sampleRate;
        impl->audioConfig.channelCount = opusConfig->channelCount;
        impl->audioConfig.streams = opusConfig->streams;
        impl->audioConfig.coupledStreams = opusConfig->coupledStreams;
        impl->audioConfig.samplesPerFrame = opusConfig->samplesPerFrame;
        for (int i = 0; i < 8 && i < opusConfig->channelCount; ++i) {
            impl->audioConfig.mapping[i] = opusConfig->mapping[i];
        }
    }

    char buf[160];
    snprintf(buf, sizeof(buf),
             "[stream] audio init: %d ch, %d streams, %d coupled, %d samples/frame",
             impl->audioConfig.channelCount,
             impl->audioConfig.streams,
             impl->audioConfig.coupledStreams,
             impl->audioConfig.samplesPerFrame);
    ssmDiagPrint(buf);

    return 0;  // success
}

void ssmAudioStart(void) {
    ssmDiagPrint("[stream] audio start");
}

void ssmAudioStop(void) {
    ssmDiagPrint("[stream] audio stop");
}

void ssmAudioCleanup(void) {
    ssmDiagPrint("[stream] audio cleanup");
}

void ssmAudioDecodeAndPlaySample(char* sampleData, int sampleLength) {
    StreamSessionManager::Impl* owner = ssmGetActiveOwner();
    if (owner == nullptr) {
        return;
    }

    // A zero-length / NULL sample is a packet-loss placeholder emitted by
    // moonlight-common-c's RtpAudioQueue. It must be forwarded to the audio
    // consumer so it can perform Opus packet-loss concealment. Do not reject
    // it here.
    if (sampleLength == 0 && sampleData == nullptr) {
        EncodedAudioPacket packet;  // empty data → PLC placeholder
        owner->pushAudio(std::move(packet));
        return;
    }

    // Validate bounds for real audio packets.
    if (sampleLength <= 0 || sampleLength > static_cast<int>(kMaxAudioPacketSize)) {
        return;
    }
    if (sampleData == nullptr) {
        return;
    }

    // Temporary diagnostic: log only the first received audio packet.
    if (!owner->firstAudioLogged.exchange(true)) {
        char buf[192];
        snprintf(buf, sizeof(buf),
                 "[stream] FIRST AUDIO PACKET: bytes=%d",
                 sampleLength);
        ssmDiagPrint(buf);
    }

    // Copy the encoded Opus data into a project-owned packet.
    EncodedAudioPacket packet;
    packet.data.assign(sampleData, sampleData + sampleLength);

    // Enqueue into the bounded audio queue.
    owner->pushAudio(std::move(packet));
}

} // namespace

// ---------------------------------------------------------------------------
// Private static active-owner bridge
// ---------------------------------------------------------------------------

// The moonlight-common-c submitDecodeUnit callback does not receive a context
// pointer in this revision. We use a private static active-owner pointer inside
// this translation unit. It is set before LiStartConnection() and cleared only
// after the connection is fully stopped and the worker is joined.
static StreamSessionManager::Impl* g_activeOwner = nullptr;

StreamSessionManager::Impl* ssmGetActiveOwner() {
    return g_activeOwner;
}

// ---------------------------------------------------------------------------
// Worker thread entry
// ---------------------------------------------------------------------------

void* StreamSessionManager::lifecycleWorker(void* arg) {
    StreamSessionManager::Impl* impl =
        static_cast<StreamSessionManager::Impl*>(arg);

    ssmDiagPrint("[stream] worker started");

    // --- Build the moonlight-common-c connection configuration --------------
    SERVER_INFORMATION serverInfo;
    LiInitializeServerInformation(&serverInfo);
    serverInfo.address = impl->activeSession.ipv4Address.c_str();
    serverInfo.serverInfoAppVersion = impl->activeSession.serverInfoAppVersion.c_str();
    serverInfo.serverInfoGfeVersion = nullptr;
    serverInfo.rtspSessionUrl = impl->activeSession.sessionUrl.c_str();
    serverInfo.serverCodecModeSupport = impl->activeSession.serverCodecModeSupport;

    STREAM_CONFIGURATION streamConfig;
    LiInitializeStreamConfiguration(&streamConfig);
    streamConfig.width = impl->activeSession.configuration.width;
    streamConfig.height = impl->activeSession.configuration.height;
    streamConfig.fps = impl->activeSession.configuration.fps;
    streamConfig.bitrate = impl->activeSession.configuration.bitrate;
    streamConfig.packetSize = impl->activeSession.configuration.packetSize;
    streamConfig.streamingRemotely = impl->activeSession.configuration.streamingMode;
    streamConfig.audioConfiguration = impl->activeSession.configuration.audioConfiguration;
    streamConfig.supportedVideoFormats = impl->activeSession.configuration.videoFormatMask;
    streamConfig.clientRefreshRateX100 = impl->activeSession.configuration.clientRefreshRateX100;
    streamConfig.colorSpace = impl->activeSession.configuration.colorSpace;
    streamConfig.colorRange = impl->activeSession.configuration.colorRange;
    streamConfig.encryptionFlags = impl->activeSession.configuration.encryptionFlags;

    // Copy the session crypto material (must not be regenerated).
    if (impl->activeSession.remoteInputKey.size() == kRemoteInputKeyLen) {
        memcpy(streamConfig.remoteInputAesKey,
               impl->activeSession.remoteInputKey.data(), kRemoteInputKeyLen);
    }
    if (impl->activeSession.remoteInputIv.size() == kRemoteInputKeyLen) {
        memcpy(streamConfig.remoteInputAesIv,
               impl->activeSession.remoteInputIv.data(), kRemoteInputKeyLen);
    }

    // --- Build the callback structs -----------------------------------------
    CONNECTION_LISTENER_CALLBACKS clCallbacks;
    LiInitializeConnectionCallbacks(&clCallbacks);
    clCallbacks.stageStarting = ssmClStageStarting;
    clCallbacks.stageComplete = ssmClStageComplete;
    clCallbacks.stageFailed = ssmClStageFailed;
    clCallbacks.connectionStarted = ssmClConnectionStarted;
    clCallbacks.connectionTerminated = ssmClConnectionTerminated;
    clCallbacks.logMessage = ssmClLogMessage;
    clCallbacks.rumble = ssmClRumble;
    clCallbacks.connectionStatusUpdate = ssmClConnectionStatusUpdate;
    clCallbacks.setHdrMode = ssmClSetHdrMode;
    clCallbacks.rumbleTriggers = ssmClRumbleTriggers;
    clCallbacks.setMotionEventState = ssmClSetMotionEventState;
    clCallbacks.setControllerLED = ssmClSetControllerLED;
    clCallbacks.setAdaptiveTriggers = ssmClSetAdaptiveTriggers;

    DECODER_RENDERER_CALLBACKS drCallbacks;
    LiInitializeVideoCallbacks(&drCallbacks);
    drCallbacks.setup = ssmVideoSetup;
    drCallbacks.start = ssmVideoStart;
    drCallbacks.stop = ssmVideoStop;
    drCallbacks.cleanup = ssmVideoCleanup;
    drCallbacks.submitDecodeUnit = ssmVideoSubmitDecodeUnit;
    drCallbacks.capabilities = 0;

    AUDIO_RENDERER_CALLBACKS arCallbacks;
    LiInitializeAudioCallbacks(&arCallbacks);
    arCallbacks.init = ssmAudioInit;
    arCallbacks.start = ssmAudioStart;
    arCallbacks.stop = ssmAudioStop;
    arCallbacks.cleanup = ssmAudioCleanup;
    arCallbacks.decodeAndPlaySample = ssmAudioDecodeAndPlaySample;
    arCallbacks.capabilities = 0;

    // --- Set the active owner before starting the connection ----------------
    g_activeOwner = impl;

    // --- Emit Connecting update ---------------------------------------------
    {
        StreamSessionUpdate update;
        update.type = StreamSessionUpdateType::Connecting;
        update.result = StreamSessionResultCategory::Success;
        update.hostId = impl->activeSession.hostId;
        update.applicationId = impl->activeSession.applicationId;
        impl->pushEvent(update);
    }

    // --- Start the connection (blocking) ------------------------------------
    int err = LiStartConnection(&serverInfo, &streamConfig,
                                &clCallbacks, &drCallbacks, &arCallbacks,
                                impl, 0, impl, 0);

    // Temporary diagnostic: log the exact LiStartConnection() return value.
    {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[stream] LiStartConnection() returned %d", err);
        ssmDiagPrint(buf);
    }

    // --- Determine the outcome ----------------------------------------------
    StreamSessionUpdate update;
    update.hostId = impl->activeSession.hostId;
    update.applicationId = impl->activeSession.applicationId;

    if (err == 0) {
        // Connection established. The worker stays alive to own the active
        // connection lifecycle until stop or remote termination.
        impl->state = StreamSessionState::Streaming;
        update.type = StreamSessionUpdateType::Connected;
        update.result = StreamSessionResultCategory::Success;
        update.nativeError = 0;
        impl->pushEvent(update);
        ssmDiagPrint("[stream] connection established");

        // --- Wait for stop or remote termination ----------------------------
        // The worker blocks here while the stream is active. It is woken by
        // stop() (via LiInterruptConnection + wakeCond) or by the
        // connectionTerminated() callback (via wakeCond).
        bool stopRequested = false;
        bool terminated = false;
        int termError = 0;

        for (;;) {
            pthread_mutex_lock(&impl->wakeMutex);
            while (!impl->stopRequested.load() &&
                   !impl->stopCalled.load() &&
                   !impl->terminationRequested.load()) {
                pthread_cond_wait(&impl->wakeCond, &impl->wakeMutex);
            }
            stopRequested = impl->stopRequested.load() || impl->stopCalled.load();
            terminated = impl->terminationRequested.load();
            termError = impl->terminationErrorCode;
            pthread_mutex_unlock(&impl->wakeMutex);

            if (stopRequested || terminated) {
                break;
            }
        }

        // --- Tear down the connection ---------------------------------------
        // LiStopConnection() joins all library threads. It must be called from
        // this worker (never the main thread).
        LiStopConnection();

        // Clear the active owner after all library threads have ceased.
        g_activeOwner = nullptr;

        // Wake any AudioDecodeOutput worker blocked in waitPopAudio() so it
        // can observe the terminal state and exit its wait.
        pthread_mutex_lock(&impl->audioMutex);
        pthread_cond_signal(&impl->audioCond);
        pthread_mutex_unlock(&impl->audioMutex);

        if (terminated) {
            // Remote/server termination.
            impl->state = StreamSessionState::Idle;
            update.type = StreamSessionUpdateType::Terminated;
            update.result = StreamSessionResultCategory::ConnectionTerminated;
            update.nativeError = termError;
            impl->pushEvent(update);
            char buf[128];
            snprintf(buf, sizeof(buf), "[stream] connection terminated (error %d)", termError);
            ssmDiagPrint(buf);
        } else {
            // Local stop.
            impl->state = StreamSessionState::Idle;
            update.type = StreamSessionUpdateType::StoppedByUser;
            update.result = StreamSessionResultCategory::Success;
            update.nativeError = 0;
            impl->pushEvent(update);
            ssmDiagPrint("[stream] connection stopped by user");
        }
    } else {
        // Connection failed or interrupted during startup.
        if (impl->stopCalled.load()) {
            // Local stop during startup.
            impl->state = StreamSessionState::Idle;
            update.type = StreamSessionUpdateType::StoppedByUser;
            update.result = StreamSessionResultCategory::Cancelled;
            update.nativeError = err;
            impl->pushEvent(update);
            ssmDiagPrint("[stream] connection stopped during startup");
        } else {
            impl->state = StreamSessionState::Idle;
            update.type = StreamSessionUpdateType::Failed;
            update.result = StreamSessionResultCategory::ConnectionStartFailure;
            update.nativeError = err;
            impl->pushEvent(update);
            char buf[128];
            snprintf(buf, sizeof(buf), "[stream] connection start failed (error %d)", err);
            ssmDiagPrint(buf);
        }

        // LiStartConnection() internally calls LiStopConnection() on failure,
        // which joins all library threads. Clear the active owner.
        g_activeOwner = nullptr;

        // Wake any AudioDecodeOutput worker blocked in waitPopAudio().
        pthread_mutex_lock(&impl->audioMutex);
        pthread_cond_signal(&impl->audioCond);
        pthread_mutex_unlock(&impl->audioMutex);
    }

    impl->workerActive = false;
    ssmDiagPrint("[stream] worker stopped");
    return nullptr;
}

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

StreamSessionManager::StreamSessionManager()
    : m_impl(new Impl())
{
}

StreamSessionManager::~StreamSessionManager() {
    if (m_impl) {
        shutdown();
        delete m_impl;
        m_impl = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

StreamSessionResult StreamSessionManager::initialize() {
    if (m_impl->running.load()) {
        return StreamSessionResult(StreamSessionResultCategory::InvalidState);
    }

    // Initialize mutexes.
    if (!m_impl->eventMutexInit) {
        int rc = pthread_mutex_init(&m_impl->eventMutex, nullptr);
        if (rc != 0) {
            return StreamSessionResult(StreamSessionResultCategory::InvalidState, rc);
        }
        m_impl->eventMutexInit = true;
    }
    if (!m_impl->wakeMutexInit) {
        int rc = pthread_mutex_init(&m_impl->wakeMutex, nullptr);
        if (rc != 0) {
            return StreamSessionResult(StreamSessionResultCategory::InvalidState, rc);
        }
        m_impl->wakeMutexInit = true;
    }
    if (!m_impl->wakeCondInit) {
        int rc = pthread_cond_init(&m_impl->wakeCond, nullptr);
        if (rc != 0) {
            return StreamSessionResult(StreamSessionResultCategory::InvalidState, rc);
        }
        m_impl->wakeCondInit = true;
    }
    if (!m_impl->audioMutexInit) {
        int rc = pthread_mutex_init(&m_impl->audioMutex, nullptr);
        if (rc != 0) {
            return StreamSessionResult(StreamSessionResultCategory::InvalidState, rc);
        }
        m_impl->audioMutexInit = true;
    }
    if (!m_impl->audioCondInit) {
        int rc = pthread_cond_init(&m_impl->audioCond, nullptr);
        if (rc != 0) {
            return StreamSessionResult(StreamSessionResultCategory::InvalidState, rc);
        }
        m_impl->audioCondInit = true;
    }

    // Reset state.
    m_impl->stopRequested = false;
    m_impl->running = true;
    m_impl->state = StreamSessionState::Idle;
    m_impl->stopCalled = false;
    m_impl->workerActive = false;
    m_impl->terminationRequested = false;
    m_impl->streamEverStarted = false;
    m_impl->sessionGeneration = 0;
    m_impl->terminationErrorCode = 0;
    m_impl->eventQueue.clear();
    m_impl->audioQueue.clear();

    ssmDiagPrint("[stream] manager initialized");
    return StreamSessionResult();
}

// ---------------------------------------------------------------------------
// Start
// ---------------------------------------------------------------------------

StreamSessionResult StreamSessionManager::start(StreamSessionDescriptor&& session) {
    if (!m_impl->running.load()) {
        return StreamSessionResult(StreamSessionResultCategory::InvalidState);
    }

    // --- Validate state -----------------------------------------------------
    StreamSessionState cur = m_impl->state.load();
    if (cur == StreamSessionState::Starting ||
        cur == StreamSessionState::Streaming ||
        cur == StreamSessionState::Stopping) {
        return StreamSessionResult(StreamSessionResultCategory::AlreadyActive);
    }

    // --- Validate the descriptor --------------------------------------------
    if (session.hostId == 0 || session.applicationId == 0 ||
        session.serverId.empty() || session.ipv4Address.empty() ||
        session.sessionUrl.empty()) {
        return StreamSessionResult(StreamSessionResultCategory::InvalidSession);
    }

    // Validate target address: must be numeric IPv4 or a valid hostname.
    // LiStartConnection() performs full DNS name resolution during STAGE_NAME_RESOLUTION.
    OrbisNetInAddr addr;
    if (sceNetInetPton(ORBIS_NET_AF_INET, session.ipv4Address.c_str(), &addr) != 1) {
        if (session.ipv4Address.find(' ') != std::string::npos ||
            session.ipv4Address.size() > 253) {
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "[stream] start rejected: invalid target address '%s'",
                     session.ipv4Address.c_str());
            ssmDiagPrint(buf);
            return StreamSessionResult(StreamSessionResultCategory::InvalidSession);
        }
    }

    // Validate RTSP port.
    if (session.rtspPort < 1 || session.rtspPort > 65535) {
        return StreamSessionResult(StreamSessionResultCategory::InvalidSession);
    }

    // Validate stream configuration.
    const StreamConfiguration& cfg = session.configuration;
    if (cfg.width < 1 || cfg.width > 8192 ||
        cfg.height < 1 || cfg.height > 8192 ||
        cfg.fps < 1 || cfg.fps > 240 ||
        cfg.bitrate < 1 || cfg.bitrate > 200000 ||
        cfg.packetSize < 1 || cfg.packetSize > 65535) {
        return StreamSessionResult(StreamSessionResultCategory::ConfigurationFailure);
    }
    // H.264 or H.265 must be advertised.
    if ((cfg.videoFormatMask & 0x0001) == 0 &&
        (cfg.videoFormatMask & 0x0100) == 0) {
        return StreamSessionResult(StreamSessionResultCategory::ConfigurationFailure);
    }
    // Audio configuration magic byte must be 0xCA.
    if ((cfg.audioConfiguration & 0xFF) != 0xCA) {
        return StreamSessionResult(StreamSessionResultCategory::ConfigurationFailure);
    }

    // Validate crypto material.
    if (session.remoteInputKey.size() != kRemoteInputKeyLen ||
        session.remoteInputIv.size() != kRemoteInputKeyLen) {
        return StreamSessionResult(StreamSessionResultCategory::InvalidSession);
    }

    // Ensure default codec mode support if the server did not specify it.
    if (session.serverCodecModeSupport == 0) {
        session.serverCodecModeSupport = 0x00000001; // SCM_H264
    }

    // Validate server info required by LiStartConnection().
    if (session.serverInfoAppVersion.empty()) {
        session.serverInfoAppVersion = "7.1.431.0";
    }

    // --- Move the descriptor into active session state ----------------------
    m_impl->activeSession = std::move(session);

    // --- Reset per-session state --------------------------------------------
    m_impl->negotiatedVideoFormat = 0;
    m_impl->videoWidth = 0;
    m_impl->videoHeight = 0;
    m_impl->videoRedrawRate = 0;
    m_impl->audioConfig = AudioStreamConfiguration();
    m_impl->stopCalled = false;
    m_impl->workerActive = true;
    m_impl->terminationRequested = false;
    m_impl->terminationErrorCode = 0;
    m_impl->firstVideoLogged = false;
    m_impl->firstAudioLogged = false;
    m_impl->statsRxBytes = 0;

    // Clear stale queues.
    pthread_mutex_lock(&m_impl->eventMutex);
    m_impl->eventQueue.clear();
    pthread_mutex_unlock(&m_impl->eventMutex);
    pthread_mutex_lock(&m_impl->audioMutex);
    m_impl->audioQueue.clear();
    pthread_mutex_unlock(&m_impl->audioMutex);

    // --- Set state to Starting ----------------------------------------------
    m_impl->streamEverStarted = true;
    // Increment the session generation for this new stream session.
    uint32_t newGeneration = m_impl->sessionGeneration.fetch_add(1) + 1;
    m_impl->state = StreamSessionState::Starting;

    {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[stream] session generation %u started", newGeneration);
        ssmDiagPrint(buf);
    }

    // --- Create the lifecycle worker ----------------------------------------
    int rc = pthread_create(&m_impl->workerThread, nullptr,
                            lifecycleWorker, m_impl);
    if (rc != 0) {
        m_impl->state = StreamSessionState::Idle;
        m_impl->workerActive = false;
        m_impl->activeSession = StreamSessionDescriptor();
        return StreamSessionResult(StreamSessionResultCategory::InvalidState, rc);
    }
    m_impl->threadCreated = true;

    char buf[128];
    snprintf(buf, sizeof(buf),
             "[stream] start requested for host id=%llu app id=%u addr='%s' rtspPort=%u",
             static_cast<unsigned long long>(m_impl->activeSession.hostId),
             static_cast<unsigned>(m_impl->activeSession.applicationId),
             m_impl->activeSession.ipv4Address.c_str(),
             static_cast<unsigned>(m_impl->activeSession.rtspPort));
    ssmDiagPrint(buf);

    return StreamSessionResult();
}

// ---------------------------------------------------------------------------
// Stop
// ---------------------------------------------------------------------------

void StreamSessionManager::stop() {
    if (!m_impl) {
        return;
    }

    if (m_impl->inputManager) {
        m_impl->inputManager->stopAllVibration();
        m_impl->inputManager->resetLightBars();
    }

    StreamSessionState cur = m_impl->state.load();
    if (cur == StreamSessionState::Idle || cur == StreamSessionState::Failed) {
        return;
    }

    // Mark stop requested.
    m_impl->stopCalled = true;
    m_impl->state = StreamSessionState::Stopping;

    // Interrupt any pending LiStartConnection() (non-blocking, main thread).
    LiInterruptConnection();

    // Wake the lifecycle worker so it can observe the stop and call
    // LiStopConnection() (which blocks and joins library threads).
    pthread_mutex_lock(&m_impl->wakeMutex);
    pthread_cond_signal(&m_impl->wakeCond);
    pthread_mutex_unlock(&m_impl->wakeMutex);

    // Wake any AudioDecodeOutput worker blocked in waitPopAudio() so it can
    // observe the stop and exit its wait.
    pthread_mutex_lock(&m_impl->audioMutex);
    pthread_cond_signal(&m_impl->audioCond);
    pthread_mutex_unlock(&m_impl->audioMutex);

    ssmDiagPrint("[stream] stop requested");
}

// ---------------------------------------------------------------------------
// Result polling
// ---------------------------------------------------------------------------

bool StreamSessionManager::pollUpdate(StreamSessionUpdate& out) {
    if (!m_impl) {
        return false;
    }

    pthread_mutex_lock(&m_impl->eventMutex);
    bool hasUpdate = !m_impl->eventQueue.empty();
    if (hasUpdate) {
        out = std::move(m_impl->eventQueue.front());
        m_impl->eventQueue.pop_front();
    }
    pthread_mutex_unlock(&m_impl->eventMutex);

    return hasUpdate;
}

// ---------------------------------------------------------------------------
// Audio blocking wait
// ---------------------------------------------------------------------------

bool StreamSessionManager::waitPopAudio(EncodedAudioPacket& outPacket,
                                        uint32_t sessionGeneration) {
    if (!m_impl) {
        return false;
    }

    pthread_mutex_lock(&m_impl->audioMutex);
    for (;;) {
        // If a packet is available, pop it and return.
        if (!m_impl->audioQueue.empty()) {
            outPacket = std::move(m_impl->audioQueue.front());
            m_impl->audioQueue.pop_front();
            pthread_mutex_unlock(&m_impl->audioMutex);
            return true;
        }

        // Exit the wait only when the session this consumer is waiting for
        // has definitively terminated, or a stop/shutdown was requested.
        //
        // The consumer waits for `sessionGeneration`. That session is
        // considered terminated when the current session generation has
        // reached `sessionGeneration` AND the stream state is terminal
        // (Idle/Failed). If the current generation is still below
        // `sessionGeneration`, the session has not started yet — keep
        // waiting even if a previous session already completed.
        StreamSessionState s = m_impl->state.load();
        uint32_t curGen = m_impl->sessionGeneration.load();
        if (m_impl->stopRequested.load() ||
            m_impl->stopCalled.load() ||
            (curGen >= sessionGeneration &&
             (s == StreamSessionState::Idle || s == StreamSessionState::Failed))) {
            pthread_mutex_unlock(&m_impl->audioMutex);
            return false;
        }

        // Wait for a new packet or a stop signal.
        pthread_cond_wait(&m_impl->audioCond, &m_impl->audioMutex);
    }
}

// ---------------------------------------------------------------------------
// Audio configuration accessor
// ---------------------------------------------------------------------------

bool StreamSessionManager::audioConfiguration(AudioStreamConfiguration& out,
                                              uint32_t sessionGeneration) const {
    if (!m_impl) {
        return false;
    }

    // The configuration is valid only if it belongs to the requested session
    // generation (i.e., the current generation matches) AND the config has
    // been captured (sampleRate > 0). This prevents a newly started worker
    // from consuming a previous session's configuration.
    uint32_t curGen = m_impl->sessionGeneration.load();
    if (curGen != sessionGeneration) {
        return false;
    }
    if (m_impl->audioConfig.sampleRate <= 0 ||
        m_impl->audioConfig.channelCount <= 0) {
        return false;
    }

    out = m_impl->audioConfig;
    return true;
}

// ---------------------------------------------------------------------------
// Audio waiter wake
// ---------------------------------------------------------------------------

void StreamSessionManager::wakeAudioWaiters() {
    if (!m_impl) {
        return;
    }

    // Signal the audio cond so any AudioDecodeOutput worker blocked in
    // waitPopAudio() wakes up and can observe the stop/shutdown state.
    pthread_mutex_lock(&m_impl->audioMutex);
    pthread_cond_signal(&m_impl->audioCond);
    pthread_mutex_unlock(&m_impl->audioMutex);
}

// ---------------------------------------------------------------------------
// State query
// ---------------------------------------------------------------------------

StreamSessionState StreamSessionManager::state() const {
    return m_impl ? m_impl->state.load() : StreamSessionState::Idle;
}

uint64_t StreamSessionManager::rxBytes() const {
    return m_impl ? m_impl->statsRxBytes.load(std::memory_order_relaxed) : 0;
}

int StreamSessionManager::negotiatedVideoFormat() const {
    return m_impl ? m_impl->negotiatedVideoFormat : 0;
}

bool StreamSessionManager::readNetworkStats(float& rttMs, float& jitterMs) const {
    // Use the existing moonlight-common-c public API. It reads the ENet peer's
    // round-trip time and RTT variance (jitter) from the active control stream.
    // No dependency patch is required.
    uint32_t rtt = 0;
    uint32_t variance = 0;
    if (LiGetEstimatedRttInfo(&rtt, &variance)) {
        rttMs = static_cast<float>(rtt);
        jitterMs = static_cast<float>(variance);
        return true;
    }
    return false;
}

uint32_t StreamSessionManager::currentSessionGeneration() const {
    return m_impl ? m_impl->sessionGeneration.load() : 0;
}

bool StreamSessionManager::isActive() const {
    if (!m_impl) {
        return false;
    }
    StreamSessionState s = m_impl->state.load();
    return s == StreamSessionState::Starting || s == StreamSessionState::Streaming;
}

// ---------------------------------------------------------------------------
// VideoDecoder wiring
// ---------------------------------------------------------------------------

void StreamSessionManager::setVideoDecoder(VideoDecoder* decoder) {
    if (m_impl) {
        m_impl->videoDecoder = decoder;
    }
}

void StreamSessionManager::setInputManager(ps4::InputManager* inputManager) {
    if (m_impl) {
        m_impl->inputManager = inputManager;
    }
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------

void StreamSessionManager::shutdown() {
    if (!m_impl) {
        return;
    }

    if (m_impl->inputManager) {
        m_impl->inputManager->stopAllVibration();
        m_impl->inputManager->resetLightBars();
    }

    // Signal stop.
    m_impl->stopRequested = true;

    // If a stream is active, interrupt it.
    StreamSessionState cur = m_impl->state.load();
    if (cur == StreamSessionState::Starting ||
        cur == StreamSessionState::Streaming ||
        cur == StreamSessionState::Stopping) {
        m_impl->stopCalled = true;
        m_impl->state = StreamSessionState::Stopping;
        LiInterruptConnection();
    }

    // Wake the worker.
    pthread_mutex_lock(&m_impl->wakeMutex);
    pthread_cond_signal(&m_impl->wakeCond);
    pthread_mutex_unlock(&m_impl->wakeMutex);

    // Wake any AudioDecodeOutput worker blocked in waitPopAudio().
    pthread_mutex_lock(&m_impl->audioMutex);
    pthread_cond_signal(&m_impl->audioCond);
    pthread_mutex_unlock(&m_impl->audioMutex);

    // Join the worker thread.
    if (m_impl->threadCreated) {
        pthread_join(m_impl->workerThread, nullptr);
        m_impl->threadCreated = false;
    }

    // Clear the active owner (all library threads have ceased).
    g_activeOwner = nullptr;

    // Clear queues.
    pthread_mutex_lock(&m_impl->eventMutex);
    m_impl->eventQueue.clear();
    pthread_mutex_unlock(&m_impl->eventMutex);
    pthread_mutex_lock(&m_impl->audioMutex);
    m_impl->audioQueue.clear();
    pthread_mutex_unlock(&m_impl->audioMutex);

    // Clear sensitive session material.
    m_impl->activeSession = StreamSessionDescriptor();

    // Clear the video decoder reference (non-owning; the decoder is owned by
    // MoonlightApp and must outlive the stream manager).
    m_impl->videoDecoder = nullptr;

    // Destroy synchronization primitives.
    if (m_impl->wakeCondInit) {
        pthread_cond_destroy(&m_impl->wakeCond);
        m_impl->wakeCondInit = false;
    }
    if (m_impl->wakeMutexInit) {
        pthread_mutex_destroy(&m_impl->wakeMutex);
        m_impl->wakeMutexInit = false;
    }
    if (m_impl->audioCondInit) {
        pthread_cond_destroy(&m_impl->audioCond);
        m_impl->audioCondInit = false;
    }
    if (m_impl->audioMutexInit) {
        pthread_mutex_destroy(&m_impl->audioMutex);
        m_impl->audioMutexInit = false;
    }
    if (m_impl->eventMutexInit) {
        pthread_mutex_destroy(&m_impl->eventMutex);
        m_impl->eventMutexInit = false;
    }

    m_impl->running = false;
    m_impl->state = StreamSessionState::Idle;
    m_impl->stopCalled = false;
    m_impl->workerActive = false;
    m_impl->streamEverStarted = false;
    m_impl->sessionGeneration = 0;

    ssmDiagPrint("[stream] manager shut down");
}

} // namespace ml