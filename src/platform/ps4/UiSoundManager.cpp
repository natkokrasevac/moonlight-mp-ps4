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

// === UiSoundManager.cpp ======================================================
//
// Moonlight client for PlayStation 4 — UI Navigation Sound Manager
//
// Implements the UiSoundManager concrete UI audio component:
//   - Minimal RIFF/WAVE parser sufficient for the three UI assets.
//   - Preload of accept.wav / back.wav / cursor.wav into persistent PCM.
//   - Dedicated PS4 AudioOut port for UI cues (logically separate from the
//     stream audio output).
//   - One-shot, single-voice playback on a dedicated worker thread so the
//     main thread never blocks on the blocking sceAudioOutOutput() call.
//
// OpenOrbis adaptation: uses POSIX pthread (pthread_create/join/mutex/cond)
// matching the proven OpenOrbis threading path used by AudioDecodeOutput /
// VideoDecoder / StreamSessionManager / DiscoveryManager. The SCE reference's
// ScePthread* types are replaced with their POSIX equivalents. OpenOrbis SDK
// includes use the <orbis/...> headers.
//
// OpenOrbis SDK headers are confined to this translation unit.
// ===============================================================================

#include "UiSoundManager.h"

// --- OpenOrbis SDK headers (private to this translation unit) -----------------------
#include <orbis/AudioOut.h>      // sceAudioOut*
#include <orbis/UserService.h>   // OrbisUserServiceUserId, ORBIS_USER_SERVICE_USER_ID_SYSTEM
#include <orbis/_types/errors.h> // ORBIS_OK

// --- POSIX threading headers (OpenOrbis libc) --------------------------------
#include <pthread.h>             // pthread_create, pthread_join, pthread_mutex_*, pthread_cond_*

#include <cstdio>                // printf, fflush, snprintf, fopen, fread, fclose
#include <cstring>               // memcpy
#include <atomic>
#include <vector>

// ---------------------------------------------------------------------------
// Diagnostics helper (minimal; consistent with other modules)
// ---------------------------------------------------------------------------

static void uiSoundDiagPrint(const char* msg) {
    printf("%s\n", msg);
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// Module constants
// ---------------------------------------------------------------------------

namespace {

/// PS4 AudioOut grain size (samples per channel). Matches the stream audio
/// output grain so the UI port uses the same pacing.
constexpr uint32_t kUiAudioOutGrainSamples = 256;

/// The UI WAV asset paths (mounted at /app0).
constexpr const char* kAcceptPath       = "/app0/assets/audio/accept.wav";
constexpr const char* kBackPath         = "/app0/assets/audio/back.wav";
constexpr const char* kCursorPath       = "/app0/assets/audio/cursor.wav";
constexpr const char* kSettingsOpenPath = "/app0/assets/audio/option_open.wav";
constexpr const char* kSettingsClosePath = "/app0/assets/audio/option_close.wav";

// --- PS4 AudioOut volume constants -------------------------------------------
// The OpenOrbis SDK does not define these SCE constants. They are defined
// locally (matching the SCE reference usage) so sceAudioOutSetVolume() can be
// called with the standard 0 dB full-volume values.
constexpr int32_t kUiVolume = 0x8000;   ///< SCE_AUDIO_OUT_VOLUME_0DB
constexpr int32_t kUiVolumeFlagLCh = 0x01; ///< SCE_AUDIO_OUT_VOLUME_FLAG_L_CH
constexpr int32_t kUiVolumeFlagRCh = 0x02; ///< SCE_AUDIO_OUT_VOLUME_FLAG_R_CH

} // namespace

// ---------------------------------------------------------------------------
// Private implementation state
// ---------------------------------------------------------------------------

namespace ps4 {

/// Private implementation of UiSoundManager. Holds all PS4 AudioOut state, the
/// retained PCM buffers, and the worker thread.
struct UiSoundManager::Impl {
    // --- Lifecycle ----------------------------------------------------------
    bool initialized = false;
    bool threadCreated = false;
    pthread_t workerThread;

    // --- Worker control -----------------------------------------------------
    std::atomic<bool> stopRequested{false};
    pthread_mutex_t queueMutex;
    bool queueMutexInit = false;
    pthread_cond_t queueCond;
    bool queueCondInit = false;
    /// The single pending cue request. `requestPending` is true when a cue is
    /// waiting to be played. A new play() overwrites the previous pending cue,
    /// so a new navigation input interrupts the currently-playing cue (the
    /// newest cue wins).
    UiSound pendingSound = UiSound::Cursor;
    bool requestPending = false;

    // --- PS4 AudioOut -------------------------------------------------------
    int32_t audioOutHandle = -1;

    // --- Retained PCM assets ------------------------------------------------
    /// Interleaved int16 stereo PCM for each cue. Empty if that cue failed to
    /// load. Retained for the lifetime of the manager (preloaded once).
    std::vector<int16_t> acceptPcm;
    std::vector<int16_t> backPcm;
    std::vector<int16_t> cursorPcm;
    std::vector<int16_t> settingsOpenPcm;
    std::vector<int16_t> settingsClosePcm;

    /// Sample rate of the loaded assets (all must match; 0 if none loaded).
    uint32_t sampleRate = 0;

    /// Returns the retained PCM for a cue (empty if unavailable).
    const std::vector<int16_t>& pcmFor(UiSound sound) const {
        switch (sound) {
        case UiSound::Accept:        return acceptPcm;
        case UiSound::Back:          return backPcm;
        case UiSound::Cursor:        return cursorPcm;
        case UiSound::SettingsOpen:  return settingsOpenPcm;
        case UiSound::SettingsClose: return settingsClosePcm;
        }
        return acceptPcm;
    }
};

// ---------------------------------------------------------------------------
// Minimal RIFF/WAVE parser
// ---------------------------------------------------------------------------

namespace {

/// Read a little-endian uint16 from a byte buffer.
static uint16_t readU16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) |
           (static_cast<uint16_t>(p[1]) << 8);
}

/// Read a little-endian uint32 from a byte buffer.
static uint32_t readU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

/// Parse a PCM16 WAV file into interleaved int16 stereo PCM.
///
/// Supports PCM 16-bit mono or stereo at 48 kHz. Mono is expanded to stereo
/// (each sample duplicated) so the output is always interleaved stereo,
/// matching the PS4 AudioOut S16 stereo format. Returns false on any
/// unsupported/malformed input.
///
/// The parser walks the RIFF chunk list and tolerates unrelated chunks by
/// skipping them correctly (it does not assume fmt and data are adjacent).
static bool parseWavPcm16(const uint8_t* data, size_t size,
                          std::vector<int16_t>& outPcm,
                          uint32_t& outSampleRate) {
    outPcm.clear();
    outSampleRate = 0;

    // --- RIFF header --------------------------------------------------------
    if (size < 12) {
        return false;
    }
    if (memcmp(data, "RIFF", 4) != 0) {
        return false;
    }
    if (memcmp(data + 8, "WAVE", 4) != 0) {
        return false;
    }

    // --- Walk chunks --------------------------------------------------------
    size_t offset = 12;
    bool haveFmt = false;
    bool haveData = false;

    uint16_t audioFormat = 0;
    uint16_t numChannels = 0;
    uint32_t sampleRate = 0;
    uint16_t bitsPerSample = 0;
    const uint8_t* dataPtr = nullptr;
    size_t dataSize = 0;

    while (offset + 8 <= size) {
        const uint8_t* chunkHeader = data + offset;
        const uint32_t chunkSize = readU32(chunkHeader + 4);
        const uint8_t* chunkData = chunkHeader + 8;

        // Guard against chunk size overflow past the buffer.
        if (chunkSize > size - 8 - (offset - 0)) {
            // Chunk extends past the file; stop parsing (malformed).
            break;
        }

        if (memcmp(chunkHeader, "fmt ", 4) == 0 && !haveFmt) {
            // fmt chunk: audioFormat(2) numChannels(2) sampleRate(4)
            // byteRate(4) blockAlign(2) bitsPerSample(2).
            if (chunkSize >= 16) {
                audioFormat = readU16(chunkData + 0);
                numChannels = readU16(chunkData + 2);
                sampleRate = readU32(chunkData + 4);
                bitsPerSample = readU16(chunkData + 14);
                haveFmt = true;
            }
        } else if (memcmp(chunkHeader, "data", 4) == 0 && !haveData) {
            dataPtr = chunkData;
            dataSize = chunkSize;
            haveData = true;
        }

        // Advance past this chunk (chunks are word-aligned: pad to even).
        offset += 8 + chunkSize;
        if (chunkSize & 1) {
            ++offset;
        }
    }

    if (!haveFmt || !haveData) {
        return false;
    }

    // --- Validate the format ------------------------------------------------
    // Only uncompressed PCM (audioFormat == 1), 16-bit, 48 kHz, mono or stereo.
    if (audioFormat != 1) {
        return false;
    }
    if (bitsPerSample != 16) {
        return false;
    }
    if (sampleRate != 48000) {
        return false;
    }
    if (numChannels != 1 && numChannels != 2) {
        return false;
    }

    // --- Convert to interleaved int16 stereo --------------------------------
    const size_t frameCount = dataSize / (2 * numChannels);
    outPcm.reserve(frameCount * 2);

    if (numChannels == 2) {
        // Already interleaved stereo: copy directly.
        const int16_t* src = reinterpret_cast<const int16_t*>(dataPtr);
        const size_t sampleCount = frameCount * 2;
        outPcm.assign(src, src + sampleCount);
    } else {
        // Mono: expand each sample to both channels.
        const int16_t* src = reinterpret_cast<const int16_t*>(dataPtr);
        outPcm.reserve(frameCount * 2);
        for (size_t i = 0; i < frameCount; ++i) {
            const int16_t s = src[i];
            outPcm.push_back(s);
            outPcm.push_back(s);
        }
    }

    outSampleRate = sampleRate;
    return true;
}

/// Read a whole file into a heap buffer. Returns false on failure.
static bool readWholeFile(const char* path, std::vector<uint8_t>& out) {
    out.clear();
    FILE* f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    long len = ftell(f);
    if (len < 0) {
        fclose(f);
        return false;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }
    out.resize(static_cast<size_t>(len));
    if (len > 0) {
        size_t read = fread(out.data(), 1, static_cast<size_t>(len), f);
        if (read != static_cast<size_t>(len)) {
            fclose(f);
            out.clear();
            return false;
        }
    }
    fclose(f);
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// UI sound worker thread
// ---------------------------------------------------------------------------

/// Worker thread entry point. Plays the single pending UI cue request and
/// outputs its PCM through the AudioOut port. sceAudioOutOutput() blocks until
/// playback starts, so this blocking work runs on the worker thread, never the
/// main thread.
///
/// Interrupt semantics: after each grain (~5.3 ms), the worker checks whether
/// a new cue request arrived. If so, it aborts the current cue immediately and
/// plays the new one, so rapid navigation inputs interrupt the current sound
/// rather than queueing behind it.
void* UiSoundManager::uiSoundWorker(void* arg) {
    Impl* impl = static_cast<Impl*>(arg);

    for (;;) {
        // Wait for a cue request or a stop request.
        pthread_mutex_lock(&impl->queueMutex);
        while (!impl->requestPending && !impl->stopRequested.load()) {
            pthread_cond_wait(&impl->queueCond, &impl->queueMutex);
        }
        const bool stop = impl->stopRequested.load();
        const bool hasCue = impl->requestPending;
        const UiSound sound = impl->pendingSound;
        impl->requestPending = false;
        pthread_mutex_unlock(&impl->queueMutex);

        if (stop) {
            break;
        }
        if (!hasCue) {
            continue;
        }

        // Output the cue's PCM as a sequence of AudioOut grains.
        const std::vector<int16_t>& pcm = impl->pcmFor(sound);
        if (pcm.empty() || impl->audioOutHandle < 0) {
            continue;   // Cue failed to load or port unavailable; skip.
        }

        const size_t grainSamples = kUiAudioOutGrainSamples * 2;   // interleaved stereo
        size_t offset = 0;
        const size_t total = pcm.size();

        while (offset < total) {
            // If a new cue request arrived while playing, interrupt the current
            // cue and process the new one on the next loop iteration. Also
            // break if a stop was requested (shutdown) so a long cue does not
            // delay teardown.
            pthread_mutex_lock(&impl->queueMutex);
            const bool interrupted = impl->requestPending;
            const bool stopping = impl->stopRequested.load();
            pthread_mutex_unlock(&impl->queueMutex);
            if (interrupted || stopping) {
                break;
            }

            const size_t remaining = total - offset;
            const size_t count = (remaining < grainSamples) ? remaining : grainSamples;

            // A partial final grain is zero-padded to the full grain size.
            int16_t grain[kUiAudioOutGrainSamples * 2];
            memset(grain, 0, sizeof(grain));
            memcpy(grain, pcm.data() + offset, count * sizeof(int16_t));

            int32_t rc = sceAudioOutOutput(impl->audioOutHandle, grain);
            if (rc < 0) {
                // AudioOut failure — stop submitting and log (non-fatal).
                char buf[128];
                snprintf(buf, sizeof(buf),
                         "[UiSound] AudioOut output failed (error %d)", rc);
                uiSoundDiagPrint(buf);
                break;
            }

            offset += count;
        }
    }

    return nullptr;
}

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

UiSoundManager::UiSoundManager()
    : m_impl(new Impl())
{
}

UiSoundManager::~UiSoundManager() {
    if (m_impl) {
        shutdown();
        delete m_impl;
        m_impl = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

bool UiSoundManager::initialize() {
    if (m_impl->initialized) {
        return true;   // Already initialized.
    }

    uiSoundDiagPrint("[UiSound] initialization start");

    // --- Preload the three WAV assets ---------------------------------------
    // Each asset is parsed independently so a missing/unsupported file does
    // not prevent the working assets from loading.
    struct AssetSpec {
        const char* path;
        const char* name;
        std::vector<int16_t>* pcm;
    };
    AssetSpec specs[] = {
        {kAcceptPath,       "accept.wav",       &m_impl->acceptPcm},
        {kBackPath,         "back.wav",         &m_impl->backPcm},
        {kCursorPath,       "cursor.wav",       &m_impl->cursorPcm},
        {kSettingsOpenPath, "option_open.wav",  &m_impl->settingsOpenPcm},
        {kSettingsClosePath, "option_close.wav", &m_impl->settingsClosePcm},
    };

    uint32_t loadedRate = 0;
    bool anyLoaded = false;
    for (auto& spec : specs) {
        std::vector<uint8_t> fileData;
        if (!readWholeFile(spec.path, fileData)) {
            char buf[192];
            snprintf(buf, sizeof(buf),
                     "[UiSound] %s: failed to read file (missing?)",
                     spec.name);
            uiSoundDiagPrint(buf);
            continue;
        }

        uint32_t rate = 0;
        if (!parseWavPcm16(fileData.data(), fileData.size(), *spec.pcm, rate)) {
            char buf[192];
            snprintf(buf, sizeof(buf),
                     "[UiSound] %s: unsupported or malformed WAV (skipped)",
                     spec.name);
            uiSoundDiagPrint(buf);
            spec.pcm->clear();
            continue;
        }

        // All loaded assets must share the same sample rate (48 kHz).
        if (loadedRate == 0) {
            loadedRate = rate;
        } else if (loadedRate != rate) {
            char buf[192];
            snprintf(buf, sizeof(buf),
                     "[UiSound] %s: sample rate %u differs from %u (skipped)",
                     spec.name, rate, loadedRate);
            uiSoundDiagPrint(buf);
            spec.pcm->clear();
            continue;
        }

        // Concise startup diagnostic.
        {
            const size_t frames = spec.pcm->size() / 2;
            char buf[256];
            snprintf(buf, sizeof(buf),
                     "[UiSound] %s %uHz stereo 16-bit samples=%zu",
                     spec.name, rate, frames);
            uiSoundDiagPrint(buf);
        }
        anyLoaded = true;
    }

    if (!anyLoaded) {
        uiSoundDiagPrint("[UiSound] no UI sounds loaded — continuing silently");
        m_impl->initialized = true;   // Mark initialized so shutdown is clean.
        return true;
    }

    m_impl->sampleRate = loadedRate;

    // --- Initialize the PS4 AudioOut subsystem ------------------------------
    // sceAudioOutInit() is application-lifetime. It may already have been
    // called by AudioDecodeOutput::initialize(); ORBIS_AUDIO_OUT_ERROR_ALREADY_INIT
    // is acceptable.
    {
        int32_t initRc = sceAudioOutInit();
        if (initRc != ORBIS_OK && initRc != ORBIS_AUDIO_OUT_ERROR_ALREADY_INIT) {
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "[UiSound] AudioOut init failed (error %d)", initRc);
            uiSoundDiagPrint(buf);
            m_impl->initialized = true;   // Mark initialized so shutdown is clean.
            return true;
        }
    }

    // --- Open a dedicated AudioOut port for UI cues -------------------------
    // S16 stereo, 256-sample grain, 48 kHz, MAIN virtual device.
    //
    // The MAIN virtual device supports up to eight ports, so opening a
    // separate MAIN port (index 0) for UI cues does not conflict with the
    // stream audio port (which also uses MAIN). The two MAIN ports mix
    // together; UI sounds are suppressed during streaming (gated centrally in
    // MoonlightApp), so they never interfere with stream audio.
    //
    // The `index` argument to sceAudioOutOpen() is documented as "not used
    // (specify 0)", so index 0 is required (a non-zero index returns
    // ORBIS_AUDIO_OUT_ERROR_INVALID_PORT).
    m_impl->audioOutHandle = sceAudioOutOpen(
        ORBIS_USER_SERVICE_USER_ID_SYSTEM,
        ORBIS_AUDIO_OUT_PORT_TYPE_MAIN,
        0,
        kUiAudioOutGrainSamples,
        48000,
        ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
    if (m_impl->audioOutHandle < 0) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[UiSound] AudioOut open failed (error %d)",
                 m_impl->audioOutHandle);
        uiSoundDiagPrint(buf);
        m_impl->audioOutHandle = -1;
        m_impl->initialized = true;   // Mark initialized so shutdown is clean.
        return true;
    }
    {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[UiSound] AudioOut port opened (handle=%d, type=MAIN)",
                 m_impl->audioOutHandle);
        uiSoundDiagPrint(buf);
    }

    // Set the fixed UI volume (0 dB) on both channels.
    int32_t vol[2] = {kUiVolume, kUiVolume};
    sceAudioOutSetVolume(m_impl->audioOutHandle,
                         kUiVolumeFlagLCh |
                             kUiVolumeFlagRCh,
                         vol);

    // --- Initialize the worker synchronization primitives -------------------
    if (!m_impl->queueMutexInit) {
        int rc = pthread_mutex_init(&m_impl->queueMutex, nullptr);
        if (rc != 0) {
            uiSoundDiagPrint("[UiSound] queue mutex init failed");
            m_impl->initialized = true;
            return true;
        }
        m_impl->queueMutexInit = true;
    }
    if (!m_impl->queueCondInit) {
        int rc = pthread_cond_init(&m_impl->queueCond, nullptr);
        if (rc != 0) {
            uiSoundDiagPrint("[UiSound] queue cond init failed");
            m_impl->initialized = true;
            return true;
        }
        m_impl->queueCondInit = true;
    }

    // --- Create the UI sound worker thread ----------------------------------
    m_impl->stopRequested = false;
    int rc = pthread_create(&m_impl->workerThread, nullptr,
                            uiSoundWorker, m_impl);
    if (rc != 0) {
        uiSoundDiagPrint("[UiSound] worker thread create failed");
        m_impl->initialized = true;   // Mark initialized so shutdown is clean.
        return true;
    }
    m_impl->threadCreated = true;

    m_impl->initialized = true;
    uiSoundDiagPrint("[UiSound] initialization success");
    return true;
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------

void UiSoundManager::shutdown() {
    if (!m_impl) {
        return;
    }

    // Signal the worker to stop and join it.
    if (m_impl->threadCreated) {
        m_impl->stopRequested = true;
        pthread_mutex_lock(&m_impl->queueMutex);
        pthread_cond_signal(&m_impl->queueCond);
        pthread_mutex_unlock(&m_impl->queueMutex);
        pthread_join(m_impl->workerThread, nullptr);
        m_impl->threadCreated = false;
    }

    if (m_impl->audioOutHandle >= 0) {
        sceAudioOutClose(m_impl->audioOutHandle);
        m_impl->audioOutHandle = -1;
    }

    // Release the retained PCM buffers.
    m_impl->acceptPcm.clear();
    m_impl->backPcm.clear();
    m_impl->cursorPcm.clear();
    m_impl->settingsOpenPcm.clear();
    m_impl->settingsClosePcm.clear();
    m_impl->sampleRate = 0;

    // Destroy synchronization primitives.
    if (m_impl->queueCondInit) {
        pthread_cond_destroy(&m_impl->queueCond);
        m_impl->queueCondInit = false;
    }
    if (m_impl->queueMutexInit) {
        pthread_mutex_destroy(&m_impl->queueMutex);
        m_impl->queueMutexInit = false;
    }

    m_impl->initialized = false;
}

// ---------------------------------------------------------------------------
// State query
// ---------------------------------------------------------------------------

bool UiSoundManager::isInitialized() const {
    return m_impl && m_impl->initialized;
}

// ---------------------------------------------------------------------------
// Playback (non-blocking, interrupt/restart)
// ---------------------------------------------------------------------------

void UiSoundManager::play(UiSound sound) {
    if (!m_impl || !m_impl->initialized || m_impl->audioOutHandle < 0) {
        return;
    }

    const std::vector<int16_t>& pcm = m_impl->pcmFor(sound);
    if (pcm.empty()) {
        return;   // This cue failed to load; no-op.
    }

    // Set the single pending cue request. Non-blocking: returns immediately.
    // If a cue is currently playing, the worker interrupts it (checked between
    // grains) and plays this new cue instead. If a cue is already pending, it
    // is overwritten so the newest navigation input wins.
    pthread_mutex_lock(&m_impl->queueMutex);
    m_impl->pendingSound = sound;
    m_impl->requestPending = true;
    pthread_cond_signal(&m_impl->queueCond);
    pthread_mutex_unlock(&m_impl->queueMutex);
}

} // namespace ps4
