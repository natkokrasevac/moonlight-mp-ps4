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

// === AudioDecodeOutput.cpp ==================================================
//
// OpenOrbis Moonlight client — PS4 Audio Decode / Output
//
// Implements the AudioDecodeOutput concrete audio decode/output subsystem:
//   - One dedicated audio worker thread.
//   - Decoder-only Opus multistream decoding.
//   - Fixed PCM staging ring for AudioOut grain aggregation.
//   - PS4 AudioOut output (S16 stereo, 256-sample grain, MAIN port).
//   - Session-generation-aware consumption from StreamSessionManager.
//   - Bounded, low-latency buffering.
//   - Project-owned audio playback clock.
//   - Deterministic start/stop/restart/shutdown.
//
// Opus and OpenOrbis SDK headers are confined to this translation unit.
// No Opus or OpenOrbis SDK types escape into public headers.
//
// OpenOrbis adaptation: uses POSIX pthread (pthread_create/join/mutex/cond)
// matching the proven OpenOrbis threading path used by VideoDecoder /
// StreamSessionManager / DiscoveryManager. The SCE reference's ScePthread*
// types are replaced with their POSIX equivalents. OpenOrbis SDK includes use the
// <orbis/...> headers.
// ===============================================================================

#include "AudioDecodeOutput.h"

#include "StreamSessionManager.h"

// --- Opus decoder-only library (confined to this translation unit) -----------
#include "opus_multistream.h"

// --- OpenOrbis SDK headers (private to this translation unit) -----------------------
#include <orbis/AudioOut.h>      // sceAudioOut*
#include <orbis/UserService.h>   // sceUserServiceGetInitialUser, ORBIS_USER_SERVICE_USER_ID_SYSTEM
#include <orbis/libkernel.h>     // sceKernelUsleep

// --- POSIX threading headers (OpenOrbis libc) --------------------------------
#include <pthread.h>             // pthread_create, pthread_join, pthread_mutex_*, pthread_cond_*

#include <cstdio>                // printf, fflush, snprintf
#include <cstring>               // memset, memcpy
#include <deque>
#include <atomic>
#include <utility>               // std::move

// ---------------------------------------------------------------------------
// Diagnostics helper (minimal; consistent with other modules)
// ---------------------------------------------------------------------------

static void adoDiagPrint(const char* msg) {
    printf("%s\n", msg);
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// Module constants
// ---------------------------------------------------------------------------

namespace {

/// Audio update queue capacity.
constexpr size_t kUpdateQueueCapacity = 8;

/// Maximum encoded audio packet size (bytes).
/// Matches StreamSessionManager's kMaxAudioPacketSize.
constexpr size_t kMaxAudioPacketSize = 1400;

/// Maximum Opus frame duration in samples per channel at 48 kHz.
/// Opus spec: max packet duration is 120 ms = 5760 samples at 48 kHz.
constexpr size_t kMaxOpusSamplesPerChannel = 5760;

/// PS4 AudioOut minimum grain size (samples per channel).
constexpr uint32_t kAudioOutGrainSamples = 256;

/// Number of AudioOut grains staged in the PCM ring.
constexpr size_t kStagingGrainCount = 4;

/// Worker idle sleep in microseconds (when no input and not stopping).
constexpr uint64_t kWorkerIdleSleepUs = 1000;

// --- PS4 AudioOut volume constants -------------------------------------------
// The OpenOrbis SDK does not define these SCE constants. They are defined
// locally (matching the SCE reference usage) so sceAudioOutSetVolume() can be
// called with the standard 0 dB full-volume values.
constexpr int32_t kAudioOutVolume0Db = 0x8000;   ///< SCE_AUDIO_OUT_VOLUME_0DB
constexpr int32_t kAudioOutVolumeFlagLCh = 0x01; ///< SCE_AUDIO_OUT_VOLUME_FLAG_L_CH
constexpr int32_t kAudioOutVolumeFlagRCh = 0x02; ///< SCE_AUDIO_OUT_VOLUME_FLAG_R_CH

} // namespace

// ---------------------------------------------------------------------------
// Private implementation state
// ---------------------------------------------------------------------------

namespace ml {

/// Private implementation of AudioDecodeOutput. Holds all Opus/PS4 resources.
struct AudioDecodeOutput::Impl {
    // --- Lifecycle ----------------------------------------------------------
    std::atomic<bool> running{false};
    std::atomic<bool> stopRequested{false};
    std::atomic<AudioDecodeOutputState> state{AudioDecodeOutputState::Uninitialized};
    bool threadCreated = false;
    pthread_t workerThread;

    // --- StreamSessionManager dependency (non-owning) -----------------------
    StreamSessionManager* ssm = nullptr;

    // --- Active configuration -----------------------------------------------
    AudioStreamConfiguration config;
    uint32_t sessionGeneration = 0;

    // --- Opus decoder -------------------------------------------------------
    OpusMSDecoder* opusDecoder = nullptr;

    // --- PS4 AudioOut -------------------------------------------------------
    int32_t audioOutHandle = -1;

    // --- PCM staging --------------------------------------------------------
    // Fixed ring buffer of int16 interleaved stereo samples.
    // Capacity: kStagingGrainCount grains × kAudioOutGrainSamples × 2 channels.
    int16_t staging[kStagingGrainCount * kAudioOutGrainSamples * 2];
    size_t stagingWriteSamples = 0;   ///< Total interleaved samples written.
    size_t stagingReadSamples = 0;    ///< Total interleaved samples read.
    size_t stagingCapacity = kStagingGrainCount * kAudioOutGrainSamples * 2;

    // --- Reusable decode buffer ---------------------------------------------
    // Max Opus frame: kMaxOpusSamplesPerChannel × 2 channels int16.
    int16_t decodeBuffer[kMaxOpusSamplesPerChannel * 2];

    // --- Reusable output grain buffer ---------------------------------------
    int16_t outputGrain[kAudioOutGrainSamples * 2];

    // --- Playback clock -----------------------------------------------------
    std::atomic<bool> clockValid{false};
    std::atomic<uint64_t> playedSamples{0};
    std::atomic<uint32_t> clockSampleRate{0};

    // --- Update queue -------------------------------------------------------
    pthread_mutex_t updateMutex;
    bool updateMutexInit = false;
    std::deque<AudioDecodeOutputUpdate> updateQueue;

    // --- Temporary diagnostics ----------------------------------------------
    std::atomic<bool> firstPacketLogged{false};
    std::atomic<bool> firstDecodedLogged{false};
    std::atomic<bool> firstOutputLogged{false};

    // --- Temporary statistics (internal only) -------------------------------
    std::atomic<uint64_t> packetsReceived{0};
    std::atomic<uint64_t> packetsDecoded{0};
    std::atomic<uint64_t> plcFrames{0};
    std::atomic<uint64_t> decodeErrors{0};

    // --- Helper: push an update (bounded) -----------------------------------
    void pushUpdate(const AudioDecodeOutputUpdate& update) {
        pthread_mutex_lock(&updateMutex);
        if (updateQueue.size() >= kUpdateQueueCapacity) {
            updateQueue.pop_front();
        }
        updateQueue.push_back(update);
        pthread_mutex_unlock(&updateMutex);
    }

    // --- Helper: staging ring write -----------------------------------------
    // Copies `count` interleaved samples into the ring. Returns false if the
    // ring does not have enough free space (should not happen with bounded
    // decode frames and ring capacity).
    bool stagingWrite(const int16_t* data, size_t count) {
        size_t written = stagingWriteSamples - stagingReadSamples;
        if (written + count > stagingCapacity) {
            return false;
        }
        for (size_t i = 0; i < count; ++i) {
            staging[(stagingWriteSamples + i) % stagingCapacity] = data[i];
        }
        stagingWriteSamples += count;
        return true;
    }

    // --- Helper: staging ring read ------------------------------------------
    // Copies `count` interleaved samples out of the ring. Returns false if
    // fewer than `count` samples are available.
    bool stagingRead(int16_t* out, size_t count) {
        size_t available = stagingWriteSamples - stagingReadSamples;
        if (available < count) {
            return false;
        }
        for (size_t i = 0; i < count; ++i) {
            out[i] = staging[(stagingReadSamples + i) % stagingCapacity];
        }
        stagingReadSamples += count;
        return true;
    }

    // --- Helper: staging available samples ----------------------------------
    size_t stagingAvailable() const {
        return stagingWriteSamples - stagingReadSamples;
    }

    // --- Helper: reset staging ring -----------------------------------------
    void stagingReset() {
        stagingWriteSamples = 0;
        stagingReadSamples = 0;
    }
};

// ---------------------------------------------------------------------------
// Audio worker thread
// ---------------------------------------------------------------------------

void* AudioDecodeOutput::audioWorker(void* arg) {
    AudioDecodeOutput::Impl* impl = static_cast<AudioDecodeOutput::Impl*>(arg);

    adoDiagPrint("[audio] worker started");

    // Determine the session generation this worker will consume.
    //
    // The audio worker is started before StreamSessionManager::start().
    // Capture the current session generation and state. If a session is
    // already active (Starting/Streaming), consume that session (generation
    // == current). Otherwise, wait for the next session to start
    // (generation == current + 1). This makes the wait/termination semantics
    // session-specific.
    uint32_t targetGeneration = 0;
    if (impl->ssm != nullptr) {
        uint32_t curGen = impl->ssm->currentSessionGeneration();
        StreamSessionState s = impl->ssm->state();
        targetGeneration = (s == StreamSessionState::Starting ||
                            s == StreamSessionState::Streaming)
                               ? curGen
                               : curGen + 1;
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[audio] worker consuming session generation %u",
                 targetGeneration);
        adoDiagPrint(buf);
    }
    impl->sessionGeneration = targetGeneration;

    // --- Audio loop ---------------------------------------------------------
    // Consume EncodedAudioPacket objects from StreamSessionManager, decode
    // them with Opus, stage the PCM, and output through PS4 AudioOut.
    for (;;) {
        // Check for stop request.
        if (impl->stopRequested.load()) {
            break;
        }

        // Wait for the next encoded audio packet.
        EncodedAudioPacket packet;
        if (impl->ssm == nullptr ||
            !impl->ssm->waitPopAudio(packet, targetGeneration)) {
            // Stream stopped/terminated or no more audio will arrive.
            break;
        }

        // Check for stop request after wait.
        if (impl->stopRequested.load()) {
            break;
        }

        impl->packetsReceived.fetch_add(1);

        // --- Obtain the audio configuration for this session ---------------
        // The configuration is captured by StreamSessionManager during the
        // Moonlight audio-init callback. It becomes available once the first
        // packet arrives (the config is set before packets are enqueued).
        if (impl->opusDecoder == nullptr) {
            AudioStreamConfiguration cfg;
            if (impl->ssm->audioConfiguration(cfg, targetGeneration)) {
                impl->config = cfg;

                char buf[192];
                snprintf(buf, sizeof(buf),
                         "[audio] init: rate=%d channels=%d streams=%d coupled=%d samples=%d",
                         cfg.sampleRate, cfg.channelCount, cfg.streams,
                         cfg.coupledStreams, cfg.samplesPerFrame);
                adoDiagPrint(buf);

                // --- Validate the configuration ----------------------------
                // Initial scope: 48 kHz stereo, 1 stream, 1 coupled stream.
                if (cfg.sampleRate != 48000 ||
                    cfg.channelCount != 2 ||
                    cfg.streams != 1 ||
                    cfg.coupledStreams != 1) {
                    adoDiagPrint("[audio] unsupported configuration (stereo 48kHz only) — continuing video-only");
                    impl->state = AudioDecodeOutputState::Failed;
                    AudioDecodeOutputUpdate update;
                    update.type = AudioDecodeOutputUpdateType::Failed;
                    update.result = AudioDecodeOutputResultCategory::UnsupportedAudioConfiguration;
                    update.nativeError = 0;
                    impl->pushUpdate(update);
                    break;
                }

                // --- Create the Opus multistream decoder -------------------
                int opusError = 0;
                impl->opusDecoder = opus_multistream_decoder_create(
                    cfg.sampleRate, cfg.channelCount, cfg.streams,
                    cfg.coupledStreams, cfg.mapping, &opusError);
                if (impl->opusDecoder == nullptr || opusError != OPUS_OK) {
                    char buf[128];
                    snprintf(buf, sizeof(buf),
                             "[audio] Opus decoder create failed (error %d)",
                             opusError);
                    adoDiagPrint(buf);
                    impl->state = AudioDecodeOutputState::Failed;
                    AudioDecodeOutputUpdate update;
                    update.type = AudioDecodeOutputUpdateType::Failed;
                    update.result = AudioDecodeOutputResultCategory::OpusInitializationFailure;
                    update.nativeError = opusError;
                    impl->pushUpdate(update);
                    break;
                }
                adoDiagPrint("[audio] decoder ready");

                // --- Open the PS4 AudioOut port ----------------------------
                // S16 stereo, 256-sample grain, 48 kHz, MAIN port.
                // Use the initial logged-in user if available, else SYSTEM.
                // sceAudioOutInit() is called once in initialize().
                int32_t user = ORBIS_USER_SERVICE_USER_ID_SYSTEM;
                int32_t initialUser = -1;
                if (sceUserServiceGetInitialUser(&initialUser) == 0 &&
                    initialUser > 0) {
                    user = initialUser;
                }

                uint32_t fmt = (cfg.channelCount == 1)
                                   ? ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_MONO
                                   : ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO;

                impl->audioOutHandle = sceAudioOutOpen(
                    user, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, 0,
                    kAudioOutGrainSamples, 48000, fmt);
                if (impl->audioOutHandle < 0) {
                    // Typical homebrew fallback: user SYSTEM (0xFF).
                    char buf[128];
                    snprintf(buf, sizeof(buf),
                             "[audio] AudioOut open user=%d failed (error %d); retry SYSTEM",
                             static_cast<int>(user), impl->audioOutHandle);
                    adoDiagPrint(buf);
                    impl->audioOutHandle = sceAudioOutOpen(
                        ORBIS_USER_SERVICE_USER_ID_SYSTEM,
                        ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, 0,
                        kAudioOutGrainSamples, 48000, fmt);
                }
                if (impl->audioOutHandle < 0) {
                    char buf[128];
                    snprintf(buf, sizeof(buf),
                             "[audio] AudioOut open failed (error %d)",
                             impl->audioOutHandle);
                    adoDiagPrint(buf);
                    opus_multistream_decoder_destroy(impl->opusDecoder);
                    impl->opusDecoder = nullptr;
                    impl->state = AudioDecodeOutputState::Failed;
                    AudioDecodeOutputUpdate update;
                    update.type = AudioDecodeOutputUpdateType::Failed;
                    update.result = AudioDecodeOutputResultCategory::AudioOutputInitializationFailure;
                    update.nativeError = impl->audioOutHandle;
                    impl->pushUpdate(update);
                    break;
                }
                {
                    char buf[128];
                    snprintf(buf, sizeof(buf),
                             "[audio] output open (port=%d, grain=%u)",
                             impl->audioOutHandle, kAudioOutGrainSamples);
                    adoDiagPrint(buf);
                }

                // Set full volume (0 dB) on both channels.
                int32_t vol[2] = {kAudioOutVolume0Db, kAudioOutVolume0Db};
                sceAudioOutSetVolume(impl->audioOutHandle,
                                     kAudioOutVolumeFlagLCh |
                                         kAudioOutVolumeFlagRCh,
                                     vol);

                // Reset staging and clock for this session.
                impl->stagingReset();
                impl->clockValid = false;
                impl->playedSamples = 0;
                impl->clockSampleRate = static_cast<uint32_t>(cfg.sampleRate);

                impl->state = AudioDecodeOutputState::Running;
                AudioDecodeOutputUpdate update;
                update.type = AudioDecodeOutputUpdateType::Started;
                update.result = AudioDecodeOutputResultCategory::Success;
                update.nativeError = 0;
                impl->pushUpdate(update);
            } else {
                // Configuration not yet available. This can happen if the
                // host establishes the audio stream but the config callback
                // has not fired yet. Wait briefly and retry on the next
                // packet. Do not fail the stream.
                sceKernelUsleep(kWorkerIdleSleepUs);
                continue;
            }
        }

        // --- Decode the packet ---------------------------------------------
        // If the packet has empty data, it is a packet-loss placeholder from
        // moonlight-common-c. Perform Opus PLC.
        int decodedSamples = 0;
        if (packet.data.empty()) {
            // PLC: decode one missing frame.
            impl->plcFrames.fetch_add(1);
            decodedSamples = opus_multistream_decode(
                impl->opusDecoder, nullptr, 0,
                impl->decodeBuffer,
                impl->config.samplesPerFrame, 0);
            if (decodedSamples < 0) {
                // PLC failure — treat as a recoverable decode error.
                impl->decodeErrors.fetch_add(1);
                continue;
            }
        } else {
            // Validate packet bounds.
            if (packet.data.size() > kMaxAudioPacketSize) {
                impl->decodeErrors.fetch_add(1);
                continue;
            }

            // Temporary diagnostic: log only the first received packet.
            if (!impl->firstPacketLogged.exchange(true)) {
                char buf[192];
                snprintf(buf, sizeof(buf),
                         "[audio] first packet (bytes=%zu)",
                         packet.data.size());
                adoDiagPrint(buf);
            }

            decodedSamples = opus_multistream_decode(
                impl->opusDecoder,
                packet.data.data(),
                static_cast<opus_int32>(packet.data.size()),
                impl->decodeBuffer,
                impl->config.samplesPerFrame, 0);
            if (decodedSamples < 0) {
                // Recoverable decode error — log rate-limited, skip, continue.
                impl->decodeErrors.fetch_add(1);
                continue;
            }
        }

        impl->packetsDecoded.fetch_add(1);

        // Temporary diagnostic: log only the first decoded PCM.
        if (!impl->firstDecodedLogged.exchange(true)) {
            char buf[192];
            snprintf(buf, sizeof(buf),
                     "[audio] first PCM decoded (samples=%d channels=%d rate=%d)",
                     decodedSamples, impl->config.channelCount,
                     impl->config.sampleRate);
            adoDiagPrint(buf);
        }

        // --- Stage the decoded PCM -----------------------------------------
        // The decoded output is interleaved int16: decodedSamples × channels.
        size_t interleavedCount = static_cast<size_t>(decodedSamples) *
                                  static_cast<size_t>(impl->config.channelCount);
        if (!impl->stagingWrite(impl->decodeBuffer, interleavedCount)) {
            // Staging full — should not happen with bounded decode frames.
            impl->decodeErrors.fetch_add(1);
            continue;
        }

        // --- Output AudioOut grains ----------------------------------------
        // Drain the staging ring into full AudioOut grains and submit them.
        // sceAudioOutOutput is blocking, providing natural pacing.
        while (impl->stagingAvailable() >= kAudioOutGrainSamples * 2) {
            // Check for stop request before each blocking output.
            if (impl->stopRequested.load()) {
                break;
            }

            if (!impl->stagingRead(impl->outputGrain, kAudioOutGrainSamples * 2)) {
                break;
            }

            int32_t rc = sceAudioOutOutput(impl->audioOutHandle, impl->outputGrain);
            // sceAudioOutOutput() returns the number of samples queued (the
            // grain size, e.g. 256) on success, or a negative error code on
            // failure. Treat any non-negative return as success.
            if (rc < 0) {
                // Fatal AudioOut failure — report and continue video-only.
                char buf[128];
                snprintf(buf, sizeof(buf),
                         "[audio] AudioOut output failed (error %d)", rc);
                adoDiagPrint(buf);
                impl->state = AudioDecodeOutputState::Failed;
                AudioDecodeOutputUpdate update;
                update.type = AudioDecodeOutputUpdateType::Failed;
                update.result = AudioDecodeOutputResultCategory::AudioOutputFailure;
                update.nativeError = rc;
                impl->pushUpdate(update);
                break;
            }

            // Advance the playback clock (submitted-sample estimate).
            impl->playedSamples.fetch_add(kAudioOutGrainSamples);
            if (!impl->clockValid.load()) {
                impl->clockValid = true;
                AudioDecodeOutputUpdate update;
                update.type = AudioDecodeOutputUpdateType::PlaybackStarted;
                update.result = AudioDecodeOutputResultCategory::Success;
                update.nativeError = 0;
                impl->pushUpdate(update);
            }

            // Temporary diagnostic: log only the first output.
            if (!impl->firstOutputLogged.exchange(true)) {
                char buf[192];
                snprintf(buf, sizeof(buf),
                         "[audio] first PCM submitted (samples=%u buffered=%zu)",
                         kAudioOutGrainSamples,
                         impl->stagingAvailable() / 2);
                adoDiagPrint(buf);
            }
        }
    }

    // --- Teardown -----------------------------------------------------------
    // Close the AudioOut port and destroy the Opus decoder.
    if (impl->audioOutHandle >= 0) {
        sceAudioOutOutput(impl->audioOutHandle, nullptr);  // wait for last block
        sceAudioOutClose(impl->audioOutHandle);
        impl->audioOutHandle = -1;
        adoDiagPrint("[audio] AudioOut closed");
    }
    if (impl->opusDecoder != nullptr) {
        opus_multistream_decoder_destroy(impl->opusDecoder);
        impl->opusDecoder = nullptr;
        adoDiagPrint("[audio] Opus decoder destroyed");
    }

    // Reset staging and clock.
    impl->stagingReset();
    impl->clockValid = false;
    impl->playedSamples = 0;

    impl->state = AudioDecodeOutputState::Idle;
    {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[audio] worker exiting generation %u",
                 targetGeneration);
        adoDiagPrint(buf);
    }
    adoDiagPrint("[audio] worker stopped");
    return nullptr;
}

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

AudioDecodeOutput::AudioDecodeOutput()
    : m_impl(new Impl())
{
}

AudioDecodeOutput::~AudioDecodeOutput() {
    if (m_impl) {
        shutdown();
        delete m_impl;
        m_impl = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

AudioDecodeOutputResult AudioDecodeOutput::initialize(StreamSessionManager* ssm) {
    if (m_impl->running.load()) {
        return AudioDecodeOutputResult(AudioDecodeOutputResultCategory::InvalidState);
    }
    if (ssm == nullptr) {
        return AudioDecodeOutputResult(AudioDecodeOutputResultCategory::InvalidState);
    }

    m_impl->ssm = ssm;

    // Initialize mutexes.
    if (!m_impl->updateMutexInit) {
        int rc = pthread_mutex_init(&m_impl->updateMutex, nullptr);
        if (rc != 0) {
            return AudioDecodeOutputResult(AudioDecodeOutputResultCategory::InvalidState, rc);
        }
        m_impl->updateMutexInit = true;
    }

    // Initialize the PS4 audio output subsystem. This must be called once
    // before any sceAudioOutOpen(). It is application-lifetime (not per
    // session). If already initialized, sceAudioOutInit() returns
    // ORBIS_AUDIO_OUT_ERROR_ALREADY_INIT, which is acceptable.
    {
        int32_t initRc = sceAudioOutInit();
        if (initRc != 0 && initRc != static_cast<int32_t>(0x8026000Eu)) {
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "[audio] AudioOut init failed (error %d)", initRc);
            adoDiagPrint(buf);
            return AudioDecodeOutputResult(
                AudioDecodeOutputResultCategory::AudioOutputInitializationFailure,
                initRc);
        }
        adoDiagPrint("[audio] AudioOut initialized");
    }

    // Reset state.
    m_impl->stopRequested = false;
    m_impl->running = true;
    m_impl->state = AudioDecodeOutputState::Idle;
    m_impl->firstPacketLogged = false;
    m_impl->firstDecodedLogged = false;
    m_impl->firstOutputLogged = false;
    m_impl->packetsReceived = 0;
    m_impl->packetsDecoded = 0;
    m_impl->plcFrames = 0;
    m_impl->decodeErrors = 0;
    m_impl->stagingReset();
    m_impl->clockValid = false;
    m_impl->playedSamples = 0;
    m_impl->clockSampleRate = 0;

    adoDiagPrint("[audio] initialized");
    return AudioDecodeOutputResult();
}

// ---------------------------------------------------------------------------
// Start
// ---------------------------------------------------------------------------

AudioDecodeOutputResult AudioDecodeOutput::start() {
    if (!m_impl->running.load()) {
        return AudioDecodeOutputResult(AudioDecodeOutputResultCategory::InvalidState);
    }
    if (m_impl->state.load() != AudioDecodeOutputState::Idle) {
        return AudioDecodeOutputResult(AudioDecodeOutputResultCategory::InvalidState);
    }

    m_impl->state = AudioDecodeOutputState::Starting;

    // Reset per-session state.
    m_impl->stopRequested = false;
    m_impl->firstPacketLogged = false;
    m_impl->firstDecodedLogged = false;
    m_impl->firstOutputLogged = false;
    m_impl->packetsReceived = 0;
    m_impl->packetsDecoded = 0;
    m_impl->plcFrames = 0;
    m_impl->decodeErrors = 0;
    m_impl->stagingReset();
    m_impl->clockValid = false;
    m_impl->playedSamples = 0;
    m_impl->clockSampleRate = 0;

    // Clear stale update queue.
    pthread_mutex_lock(&m_impl->updateMutex);
    m_impl->updateQueue.clear();
    pthread_mutex_unlock(&m_impl->updateMutex);

    // Create the audio worker thread.
    int rc = pthread_create(&m_impl->workerThread, nullptr,
                            audioWorker, m_impl);
    if (rc != 0) {
        m_impl->state = AudioDecodeOutputState::Idle;
        return AudioDecodeOutputResult(AudioDecodeOutputResultCategory::InvalidState, rc);
    }
    m_impl->threadCreated = true;

    adoDiagPrint("[audio] started");
    return AudioDecodeOutputResult();
}

// ---------------------------------------------------------------------------
// Stop
// ---------------------------------------------------------------------------

void AudioDecodeOutput::stop() {
    if (!m_impl) {
        return;
    }

    AudioDecodeOutputState cur = m_impl->state.load();
    if (cur == AudioDecodeOutputState::Idle ||
        cur == AudioDecodeOutputState::Uninitialized) {
        return;
    }

    // Mark stop requested.
    m_impl->stopRequested = true;
    m_impl->state = AudioDecodeOutputState::Stopping;

    // If a stream is active, stop it so the audio worker's waitPopAudio()
    // exits (StreamSessionManager::stop() sets stopCalled and wakes the audio
    // cond). This is necessary when AudioDecodeOutput is stopped independently
    // of the stream (e.g., during application shutdown before
    // StreamSessionManager::shutdown()).
    if (m_impl->ssm != nullptr && m_impl->ssm->isActive()) {
        m_impl->ssm->stop();
    } else if (m_impl->ssm != nullptr) {
        // No active stream, but wake any audio consumer blocked in
        // waitPopAudio() so the worker can observe the stop request.
        m_impl->ssm->wakeAudioWaiters();
    }

    // Join the worker thread. The worker exits its waitPopAudio() when the
    // target session terminates (or stop is requested), then tears down.
    if (m_impl->threadCreated) {
        pthread_join(m_impl->workerThread, nullptr);
        m_impl->threadCreated = false;
    }

    m_impl->state = AudioDecodeOutputState::Idle;

    // Emit Stopped update.
    {
        AudioDecodeOutputUpdate update;
        update.type = AudioDecodeOutputUpdateType::Stopped;
        update.result = AudioDecodeOutputResultCategory::Success;
        update.nativeError = 0;
        m_impl->pushUpdate(update);
    }

    adoDiagPrint("[audio] stopped");
}

// ---------------------------------------------------------------------------
// State query
// ---------------------------------------------------------------------------

AudioDecodeOutputState AudioDecodeOutput::state() const {
    return m_impl ? m_impl->state.load() : AudioDecodeOutputState::Uninitialized;
}

// ---------------------------------------------------------------------------
// Update polling
// ---------------------------------------------------------------------------

bool AudioDecodeOutput::pollUpdate(AudioDecodeOutputUpdate& out) {
    if (!m_impl) {
        return false;
    }

    pthread_mutex_lock(&m_impl->updateMutex);
    bool hasUpdate = !m_impl->updateQueue.empty();
    if (hasUpdate) {
        out = std::move(m_impl->updateQueue.front());
        m_impl->updateQueue.pop_front();
    }
    pthread_mutex_unlock(&m_impl->updateMutex);

    return hasUpdate;
}

// ---------------------------------------------------------------------------
// Playback clock
// ---------------------------------------------------------------------------

AudioPlaybackClock AudioDecodeOutput::playbackClock() const {
    AudioPlaybackClock clock;
    if (!m_impl) {
        return clock;
    }
    clock.valid = m_impl->clockValid.load();
    clock.sessionGeneration = m_impl->sessionGeneration;
    clock.playedSamples = m_impl->playedSamples.load();
    clock.sampleRate = m_impl->clockSampleRate.load();
    return clock;
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------

void AudioDecodeOutput::shutdown() {
    if (!m_impl) {
        return;
    }

    // Stop any active audio session.
    stop();

    // Clear the StreamSessionManager reference.
    m_impl->ssm = nullptr;

    // Destroy synchronization primitives.
    if (m_impl->updateMutexInit) {
        pthread_mutex_destroy(&m_impl->updateMutex);
        m_impl->updateMutexInit = false;
    }

    m_impl->running = false;
    m_impl->state = AudioDecodeOutputState::Uninitialized;

    adoDiagPrint("[audio] shut down");
}

} // namespace ml
