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

// === VideoDecoder.cpp =======================================================
//
// OpenOrbis Moonlight client — PS4 Video Decode
//
// Implements the VideoDecoder concrete video decode subsystem:
//   - One dedicated decoder worker thread.
//   - OpenOrbis SDK Videodec2 hardware H.264 decoding (loaded dynamically).
//   - Compute queue allocation/release.
//   - Decoder configuration, memory query, and decoder creation.
//   - Compressed-input submission and decoded-output acquisition.
//   - Frame buffer pool with explicit lease/release semantics.
//   - Bounded decoded-frame queue with latest-frame acquisition.
//   - IDR recovery via LiRequestIdrFrame().
//   - Deterministic start/stop/flush/shutdown.
//
// The OpenOrbis SDK's Videodec2.h is a stub (empty declarations). The real
// libSceVideodec2.sprx is loaded dynamically via sceKernelLoadStartModule and
// its symbols resolved via sceKernelDlsym. The reversed Videodec2 types are
// defined privately in this translation unit (matching the proven OpenOrbis
// alt reference implementation).
//
// OpenOrbis SDK headers are confined to this translation unit.
// No OpenOrbis SDK types escape into public headers.
// ===============================================================================

#include "VideoDecoder.h"

// --- moonlight-common-c (C) header — confined to this translation unit --------
#include "Limelight.h"   // LiRequestIdrFrame()

// --- OpenOrbis SDK headers (private to this translation unit) -----------------
#include <orbis/libkernel.h>   // sceKernelAllocateDirectMemory, sceKernelMapDirectMemory,
                               // sceKernelLoadStartModule, sceKernelDlsym, sceKernelGetProcessTime
#include <orbis/Sysmodule.h>   // sceSysmoduleLoadModuleInternal, ORBIS_SYSMODULE_INTERNAL_VDECCORE
#include <orbis/GnmDriver.h>   // sceGnmFlushGarlic

#include <cstdio>                // printf, fflush, snprintf
#include <cstring>               // memset, memcpy
#include <deque>
#include <atomic>
#include <utility>               // std::move
#include <vector>
#include <pthread.h>

// ---------------------------------------------------------------------------
// Diagnostics helper (minimal; consistent with other modules)
// ---------------------------------------------------------------------------

static void vdDiagPrint(const char* msg) {
    printf("%s\n", msg);
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// Reversed Videodec2 types (OpenOrbis alt reference, size-validated)
// ---------------------------------------------------------------------------

namespace {

#define ORBIS_VIDEODEC2_CODEC_AVC 1
#define ORBIS_VIDEODEC2_RESOURCE_TYPE_EMBEDDED 1u
#define ORBIS_VIDEODEC2_PROFILE_HIGH           100u
#define ORBIS_VIDEODEC2_LEVEL_51               51u
#define ORBIS_VIDEODEC2_THREAD_PRIO_DEFAULT    700
#define ORBIS_VIDEODEC2_AFFINITY_ALL           0x3Full
#define ORBIS_VIDEODEC2_MAX_PIPELINE_DEPTH     4

struct Vd2ComputeConfigInfo {
    uint64_t thisSize;
    uint16_t computePipeId;
    uint16_t computeQueueId;
    bool checkMemoryType;
    uint8_t reserved0;
    uint16_t reserved1;
};

struct Vd2ComputeMemoryInfo {
    uint64_t thisSize;
    uint64_t cpuGpuMemorySize;
    void* cpuGpuMemory;
};

struct Vd2DecoderConfigInfo {
    uint64_t thisSize;
    uint32_t resourceType;
    uint32_t codecType;
    uint32_t profile;
    uint32_t maxLevel;
    int32_t maxFrameWidth;
    int32_t maxFrameHeight;
    int32_t maxDpbFrameCount;
    uint32_t decodePipelineDepth;
    void* computeQueue;
    uint64_t cpuAffinityMask;
    int32_t cpuThreadPriority;
    bool optimizeProgressiveVideo;
    bool checkMemoryType;
    uint8_t reserved0;
    uint8_t reserved1;
    void* extraConfigInfo;
};

struct Vd2DecoderMemoryInfo {
    uint64_t thisSize;
    uint64_t cpuMemorySize;
    void* cpuMemory;
    uint64_t gpuMemorySize;
    void* gpuMemory;
    uint64_t cpuGpuMemorySize;
    void* cpuGpuMemory;
    uint64_t maxFrameBufferSize;
    uint32_t frameBufferAlignment;
    uint32_t reserved0;
};

struct Vd2InputData {
    uint64_t thisSize;
    void* auData;
    uint64_t auSize;
    uint64_t ptsData;
    uint64_t dtsData;
    uint64_t attachedData;
};

struct Vd2OutputInfo {
    uint64_t thisSize;
    bool isValid;
    bool isErrorFrame;
    uint8_t pictureCount;
    uint32_t codecType;
    uint32_t frameWidth;
    uint32_t framePitch;
    uint32_t frameHeight;
    void* frameBuffer;
    uint64_t frameBufferSize;
    uint32_t frameFormat;
    uint32_t framePitchInBytes;
};

struct Vd2FrameBuffer {
    uint64_t thisSize;
    void* frameBuffer;
    uint64_t frameBufferSize;
    bool isAccepted;
};

typedef void* Vd2Decoder;
typedef void* Vd2ComputeQueue;

typedef int32_t (*Vd2QueryComputeMemoryInfoFn)(Vd2ComputeMemoryInfo*);
typedef int32_t (*Vd2AllocateComputeQueueFn)(const Vd2ComputeConfigInfo*,
                                             const Vd2ComputeMemoryInfo*,
                                             Vd2ComputeQueue*);
typedef int32_t (*Vd2ReleaseComputeQueueFn)(Vd2ComputeQueue);
typedef int32_t (*Vd2QueryDecoderMemoryInfoFn)(const Vd2DecoderConfigInfo*,
                                               Vd2DecoderMemoryInfo*);
typedef int32_t (*Vd2CreateDecoderFn)(const Vd2DecoderConfigInfo*,
                                      const Vd2DecoderMemoryInfo*,
                                      Vd2Decoder*);
typedef int32_t (*Vd2DeleteDecoderFn)(Vd2Decoder);
typedef int32_t (*Vd2DecodeFn)(Vd2Decoder, const Vd2InputData*,
                               Vd2FrameBuffer*, Vd2OutputInfo*);
typedef int32_t (*Vd2FlushFn)(Vd2Decoder, Vd2FrameBuffer*, Vd2OutputInfo*);
typedef int32_t (*Vd2ResetFn)(Vd2Decoder);

struct Vd2Api {
    int module = -1;
    Vd2ComputeQueue queue = nullptr;
    Vd2QueryComputeMemoryInfoFn QueryComputeMemoryInfo = nullptr;
    Vd2AllocateComputeQueueFn AllocateComputeQueue = nullptr;
    Vd2ReleaseComputeQueueFn ReleaseComputeQueue = nullptr;
    Vd2QueryDecoderMemoryInfoFn QueryDecoderMemoryInfo = nullptr;
    Vd2CreateDecoderFn CreateDecoder = nullptr;
    Vd2DeleteDecoderFn DeleteDecoder = nullptr;
    Vd2DecodeFn Decode = nullptr;
    Vd2FlushFn Flush = nullptr;
    Vd2ResetFn Reset = nullptr;
};

// ---------------------------------------------------------------------------
// Module constants
// ---------------------------------------------------------------------------

/// Compute queue pipe ID (0-4).
constexpr uint16_t kComputePipeId = 2;
/// Compute queue ID (0-7).
constexpr uint16_t kComputeQueueId = 2;

/// Number of output frame buffers in the pool.
/// Matches the SCE reference (16). A larger pool prevents the decoder from
/// being forced to reuse a surface that is still needed as a reference frame
/// (which otherwise produces stale-content corruption that moves with animated
/// content). The pool is bounded by the Videodec2 pipeline depth + presentation
/// slack + DPB reference headroom.
constexpr size_t kFrameBufferCount = 16;

/// Decoded-frame queue depth (latest-frame semantics).
constexpr size_t kDecodedQueueDepth = 2;

/// Decoder update queue capacity.
constexpr size_t kUpdateQueueCapacity = 8;

/// Encoded-unit input queue capacity.
constexpr size_t kInputQueueCapacity = 8;

/// Maximum encoded video unit size (bytes).
constexpr size_t kMaxVideoUnitSize = 1024 * 1024;

/// Worker idle sleep in microseconds (when no input and not stopping).
constexpr uint64_t kWorkerIdleSleepUs = 1000;

/// Direct memory page size (64 KiB).
constexpr size_t kDmemPageSize = 65536;

/// Frame buffer alignment (256 bytes per Videodec2 usage doc).
constexpr size_t kFrameBufferAlignment = 256;

/// Direct memory type for CPU-cacheable decoder working areas (Onion).
constexpr int32_t kDmemTypeOnion = ORBIS_KERNEL_WB_ONION;
/// Direct memory type for GPU-visible framebuffers (WC Garlic).
constexpr int32_t kDmemTypeGarlic = ORBIS_KERNEL_WC_GARLIC;

/// Memory protection for CPU+GPU access.
constexpr int32_t kDmemProt = ORBIS_KERNEL_PROT_CPU_RW | ORBIS_KERNEL_PROT_GPU_ALL;

/// Decoder pipeline depth (frame-parallel decode).
/// Set to 2 to enable frame-parallel decode throughput. The AU input-buffer
/// lifetime contract (retainAu / retireExpiredAus) keeps each compressed AU
/// alive for the full pipeline depth, preventing the stale-content corruption
/// that previously occurred at depth > 1.
constexpr uint32_t kDecodePipelineDepth = 4;

/// DPB frame count.
constexpr int32_t kMaxDpbFrameCount = 4;

} // namespace

// ---------------------------------------------------------------------------
// Private implementation state
// ---------------------------------------------------------------------------

namespace ml {

/// A minimal linear allocator for a single direct-memory block.
struct DmemBlock {
    void*  baseAddress = nullptr;
    off_t  physicalAddress = 0;
    size_t totalSize = 0;
    size_t offset = 0;

    bool allocate(size_t size, int32_t memType, int32_t prot) {
        if (baseAddress != nullptr) {
            return false;
        }
        const size_t alignedSize = (size + kDmemPageSize - 1) & ~(kDmemPageSize - 1);

        // 1. Allocate direct memory.
        int32_t rc = sceKernelAllocateDirectMemory(
            0, sceKernelGetDirectMemorySize(), alignedSize, kDmemPageSize,
            memType, &physicalAddress);
        if (rc != 0) {
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "[video-decoder] dmem alloc failed: size=%zu type=%d rc=0x%08X",
                     alignedSize, memType, static_cast<uint32_t>(rc));
            vdDiagPrint(buf);
            return false;
        }

        // 2. Map direct memory.
        rc = sceKernelMapDirectMemory(
            &baseAddress, alignedSize, prot, 0, physicalAddress, kDmemPageSize);
        if (rc != 0 || baseAddress == nullptr) {
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "[video-decoder] dmem map failed: rc=0x%08X",
                     static_cast<uint32_t>(rc));
            vdDiagPrint(buf);
            sceKernelReleaseDirectMemory(physicalAddress, alignedSize);
            baseAddress = nullptr;
            return false;
        }

        memset(baseAddress, 0, alignedSize);
        totalSize = alignedSize;
        offset = 0;
        return true;
    }

    void release() {
        if (baseAddress) {
            sceKernelMunmap(baseAddress, totalSize);
            sceKernelReleaseDirectMemory(physicalAddress, totalSize);
            baseAddress = nullptr;
            physicalAddress = 0;
            totalSize = 0;
            offset = 0;
        }
    }

    void* alloc(size_t size, size_t alignment) {
        if (!baseAddress) {
            return nullptr;
        }
        const size_t alignedOffset = (offset + alignment - 1) & ~(alignment - 1);
        if (alignedOffset + size > totalSize) {
            return nullptr;
        }
        void* ptr = static_cast<uint8_t*>(baseAddress) + alignedOffset;
        offset = alignedOffset + size;
        return ptr;
    }
};

/// A single output frame buffer in the pool.
struct FrameBuffer {
    void* address = nullptr;   ///< Base address (256-byte aligned).
    size_t size = 0;           ///< Buffer size.
    bool leased = false;       ///< True if currently leased to a consumer.
    /// Reuse generation. Incremented each time the surface is re-acquired for
    /// a new decode. The renderer reads this via surfaceGeneration() to detect
    /// whether a surface it still holds has been reused (overwritten) by the
    /// decoder, which would indicate a frame-lifetime violation (stale-content
    /// corruption). Atomic so the render thread can read it without a data
    /// race on the decoder worker thread.
    std::atomic<uint32_t> generation{0};
};

/// Private implementation of VideoDecoder. Holds all OpenOrbis SDK resources.
struct VideoDecoder::Impl {
    // --- Lifecycle ----------------------------------------------------------
    std::atomic<bool> running{false};
    std::atomic<bool> stopRequested{false};
    std::atomic<VideoDecoderState> state{VideoDecoderState::Idle};
    bool threadCreated = false;
    pthread_t workerThread;

    // --- Active configuration -----------------------------------------------
    VideoDecoderConfiguration config;

    // --- Videodec2 resources ------------------------------------------------
    Vd2Api api;
    Vd2Decoder decoder = nullptr;
    DmemBlock computeMem;      ///< Onion, CPU+GPU (compute queue).
    DmemBlock cpuMem;          ///< Onion, CPU (decoder working area).
    DmemBlock gpuMem;          ///< Garlic, CPU+GPU (decoder working area).
    DmemBlock cpuGpuMem;       ///< Onion, CPU+GPU (decoder working area).
    DmemBlock frameBufferMem;  ///< Onion, CPU+GPU (frame buffers).

    // --- Frame buffer pool --------------------------------------------------
    FrameBuffer frameBuffers[kFrameBufferCount];

    // --- Encoded-unit input queue -------------------------------------------
    pthread_mutex_t inputMutex;
    bool inputMutexInit = false;
    std::deque<EncodedVideoUnit> inputQueue;

    // --- Decoded-frame queue ------------------------------------------------
    pthread_mutex_t decodedMutex;
    bool decodedMutexInit = false;
    std::deque<DecodedVideoFrame::Impl*> decodedQueue;

    // --- Update queue -------------------------------------------------------
    pthread_mutex_t updateMutex;
    bool updateMutexInit = false;
    std::deque<VideoDecoderUpdate> updateQueue;

    // --- Worker wake --------------------------------------------------------
    pthread_mutex_t wakeMutex;
    bool wakeMutexInit = false;
    pthread_cond_t wakeCond;
    bool wakeCondInit = false;

    // --- Session state ------------------------------------------------------
    // True until the first IDR of the current session has been decoded.
    std::atomic<bool> awaitingFirstIdr{true};

    // --- Native RTP timestamp normalization state ---------------------------
    bool haveTimestampBase = false;
    uint32_t rtpTimestampBase = 0;

    // --- Statistics ---------------------------------------------------------
    std::atomic<uint64_t> encodedConsumed{0};
    std::atomic<uint64_t> framesDecoded{0};
    std::atomic<uint64_t> decodeErrors{0};
    std::atomic<uint64_t> idrRequests{0};
    std::atomic<uint64_t> decodedFrameDrops{0};
    std::atomic<uint64_t> statsDecodedFrames{0};
    std::atomic<uint64_t> statsDecodeTotalUs{0};
    std::atomic<uint64_t> statsDecodeCount{0};

    // --- Diagnostics --------------------------------------------------------
    std::atomic<bool> firstIdrLogged{false};
    std::atomic<bool> firstFrameLogged{false};

    // --- Retained AU input-buffer lifetime (Videodec2 frame-parallel) -------
    // Videodec2 frame-parallel input lifetime contract:
    //   "the access unit input with sceVideodec2Decode() must be retained
    //    until one cycle of processing completes. In other words, in order
    //    for the access unit to not be overwritten, etc., it must be
    //    protected until the number of sceVideodec2Decode() calls starting
    //    with the call that input the corresponding access unit reaches to
    //    the number set to decodePipelineDepth."
    //
    // Interpretation (exact SDK rule):
    //   - The submitting sceVideodec2Decode() call counts as call 1.
    //   - An AU submitted at decode call S must be retained through calls
    //     S .. S+(decodePipelineDepth-1) (decodePipelineDepth calls total).
    //   - It may be released once the current decode-call sequence reaches
    //     S + decodePipelineDepth.
    //   - Maximum simultaneously retained AUs == decodePipelineDepth.
    struct RetainedAu {
        std::vector<uint8_t> data;   ///< Owned compressed AU bytes (moved in).
        uint64_t submittedAtSeq = 0; ///< decodeCallSeq at submission.
        uint64_t frameNumber = 0;    ///< Frame number (diagnostic).
    };

    /// Monotonic decode-call sequence. Incremented immediately before each
    /// sceVideodec2Decode() call (attempted calls only; early-exit paths that
    /// do not call the decoder do not advance it).
    uint64_t decodeCallSeq = 0;

    /// Configured frame-parallel pipeline depth. Set in start() to the same
    /// value used for decoderCfgInfo.decodePipelineDepth. Bounds the retained
    /// set. Not a magic constant: it is the configured pipeline contract.
    uint32_t decodePipelineDepth = 0;

    /// Bounded retained-AU storage. Holds ownership (moved std::vector) of
    /// each submitted compressed AU for the SDK-required lifetime. Bounded to
    /// decodePipelineDepth entries.
    std::deque<RetainedAu> retainedAus;

    // --- Helper: retire expired retained AUs --------------------------------
    // Retires every retained AU whose SDK-required lifetime has elapsed once
    // the decode call at `currentSeq` has completed. An AU submitted at S is
    // retired when S + decodePipelineDepth <= currentSeq.
    void retireExpiredAus(uint64_t currentSeq) {
        while (!retainedAus.empty()) {
            const RetainedAu& au = retainedAus.front();
            if (au.submittedAtSeq + decodePipelineDepth > currentSeq) {
                break;  // Front AU still within its required lifetime.
            }
            retainedAus.pop_front();
        }
    }

    // --- Helper: retain the current AU --------------------------------------
    // Moves `data` into retained storage (no byte copy) and records the
    // submission sequence. Must be called AFTER sceVideodec2Decode() has
    // returned so the decoder's synchronous use of the AU pointer is complete.
    void retainAu(std::vector<uint8_t>&& data, uint64_t submitSeq,
                  uint64_t frameNumber) {
        RetainedAu au;
        au.data = std::move(data);
        au.submittedAtSeq = submitSeq;
        au.frameNumber = frameNumber;
        retainedAus.push_back(std::move(au));
    }

    // --- Helper: clear all retained AUs -------------------------------------
    // Releases retained AU storage. Only safe when the decoder can no longer
    // reference the input AUs (after a successful reset, or at shutdown after
    // the flush loop has drained all output).
    void clearRetainedAus() {
        retainedAus.clear();
    }

    // --- Helper: push an update (bounded) -----------------------------------
    void pushUpdate(const VideoDecoderUpdate& update) {
        pthread_mutex_lock(&updateMutex);
        if (updateQueue.size() >= kUpdateQueueCapacity) {
            updateQueue.pop_front();
        }
        updateQueue.push_back(update);
        pthread_mutex_unlock(&updateMutex);
    }

    // --- Helper: acquire a free frame buffer --------------------------------
    int acquireFreeFrameBuffer() {
        for (size_t i = 0; i < kFrameBufferCount; ++i) {
            if (!frameBuffers[i].leased) {
                frameBuffers[i].leased = true;
                // Increment the reuse generation so the renderer can detect
                // that this surface has been re-acquired for a new decode.
                frameBuffers[i].generation.fetch_add(1, std::memory_order_relaxed);
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    // --- Helper: release a frame buffer back to the pool --------------------
    void releaseFrameBuffer(int index) {
        if (index >= 0 && index < static_cast<int>(kFrameBufferCount)) {
            frameBuffers[index].leased = false;
        }
    }
};

// ---------------------------------------------------------------------------
// DecodedVideoFrame private implementation
// ---------------------------------------------------------------------------

/// Private implementation of DecodedVideoFrame.
/// Holds the frame metadata and a release token (frame buffer index).
struct DecodedVideoFrame::Impl {
    VideoDecoder::Impl* decoder = nullptr;  ///< Owning decoder (for release).
    int frameBufferIndex = -1;              ///< Frame buffer pool index.
    bool valid = false;

    uint64_t frameNumber = 0;
    uint64_t presentationTimeUs = 0;
    uint64_t decodeTimeUs = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t surfaceHeight = 0;
    uint32_t surfaceSlot = 0;
    uint32_t pitch = 0;
    VideoPixelFormat format = VideoPixelFormat::NV12;
    void* yPlane = nullptr;
    void* uvPlane = nullptr;
    uint32_t colorSpace = 1;
    uint32_t colorRange = 0;

    ~Impl() {
        // Release the surface back to the decoder if not already released.
        if (valid && decoder != nullptr && frameBufferIndex >= 0) {
            decoder->releaseFrameBuffer(frameBufferIndex);
        }
        valid = false;
    }
};

// ---------------------------------------------------------------------------
// DecodedVideoFrame public API
// ---------------------------------------------------------------------------

DecodedVideoFrame::DecodedVideoFrame()
    : m_impl(nullptr)
{
}

DecodedVideoFrame::~DecodedVideoFrame() {
    if (m_impl) {
        delete m_impl;
        m_impl = nullptr;
    }
}

DecodedVideoFrame::DecodedVideoFrame(DecodedVideoFrame&& other) noexcept
    : m_impl(other.m_impl)
{
    other.m_impl = nullptr;
}

DecodedVideoFrame& DecodedVideoFrame::operator=(DecodedVideoFrame&& other) noexcept {
    if (this != &other) {
        if (m_impl) {
            delete m_impl;
        }
        m_impl = other.m_impl;
        other.m_impl = nullptr;
    }
    return *this;
}

void DecodedVideoFrame::release() {
    if (m_impl) {
        delete m_impl;
        m_impl = nullptr;
    }
}

bool DecodedVideoFrame::isValid() const {
    return m_impl != nullptr && m_impl->valid;
}

uint64_t DecodedVideoFrame::frameNumber() const {
    return m_impl ? m_impl->frameNumber : 0;
}

uint64_t DecodedVideoFrame::presentationTimeUs() const {
    return m_impl ? m_impl->presentationTimeUs : 0;
}

uint64_t DecodedVideoFrame::decodeTimeUs() const {
    return m_impl ? m_impl->decodeTimeUs : 0;
}

uint32_t DecodedVideoFrame::width() const {
    return m_impl ? m_impl->width : 0;
}

uint32_t DecodedVideoFrame::height() const {
    return m_impl ? m_impl->height : 0;
}

uint32_t DecodedVideoFrame::surfaceHeight() const {
    return m_impl ? m_impl->surfaceHeight : 0;
}

uint32_t DecodedVideoFrame::surfaceSlot() const {
    return m_impl ? m_impl->surfaceSlot : 0;
}

uint32_t DecodedVideoFrame::pitch() const {
    return m_impl ? m_impl->pitch : 0;
}

VideoPixelFormat DecodedVideoFrame::format() const {
    return m_impl ? m_impl->format : VideoPixelFormat::NV12;
}

void* DecodedVideoFrame::yPlane() const {
    return m_impl ? m_impl->yPlane : nullptr;
}

void* DecodedVideoFrame::uvPlane() const {
    return m_impl ? m_impl->uvPlane : nullptr;
}

uint32_t DecodedVideoFrame::colorSpace() const {
    return m_impl ? m_impl->colorSpace : 1;
}

uint32_t DecodedVideoFrame::colorRange() const {
    return m_impl ? m_impl->colorRange : 0;
}

// ---------------------------------------------------------------------------
// Videodec2 dynamic loading helpers
// ---------------------------------------------------------------------------

namespace {

int vdDlsym(int mod, const char* name, void** out) {
    int rc = sceKernelDlsym(mod, name, out);
    if (rc != 0 || !*out) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[video-decoder] Dlsym %s => 0x%08x", name, static_cast<uint32_t>(rc));
        vdDiagPrint(buf);
        return -1;
    }
    return 0;
}

int vdLoadModule(const char* path) {
    int rc = static_cast<int>(sceKernelLoadStartModule(path, 0, nullptr, 0, nullptr, nullptr));
    char buf[160];
    snprintf(buf, sizeof(buf),
             "[video-decoder] LoadStartModule(%s) => 0x%08x", path, static_cast<uint32_t>(rc));
    vdDiagPrint(buf);
    return rc;
}

int vdLoadVdecDeps() {
    static const char* mods[] = {
        "/system/common/lib/libSceVdecCore.sprx",
        "/system/common/lib/libSceVdecSavc.sprx",
        "/system/common/lib/libSceVdecSavc2.sprx",
        "/system/common/lib/libSceVdecwrap.sprx",
    };
    for (size_t i = 0; i < sizeof(mods) / sizeof(mods[0]); ++i) {
        int rc = vdLoadModule(mods[i]);
        if (rc < 0) {
            char buf[160];
            snprintf(buf, sizeof(buf),
                     "[video-decoder] optional %s => 0x%08x", mods[i], static_cast<uint32_t>(rc));
            vdDiagPrint(buf);
        }
    }
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_VDECCORE);
    return 0;
}

int vdResolveApi(Vd2Api* api, int mod) {
    api->module = mod;
    if (vdDlsym(mod, "sceVideodec2QueryComputeMemoryInfo", (void**)&api->QueryComputeMemoryInfo) < 0) return -1;
    if (vdDlsym(mod, "sceVideodec2AllocateComputeQueue", (void**)&api->AllocateComputeQueue) < 0) return -1;
    if (vdDlsym(mod, "sceVideodec2ReleaseComputeQueue", (void**)&api->ReleaseComputeQueue) < 0) return -1;
    if (vdDlsym(mod, "sceVideodec2QueryDecoderMemoryInfo", (void**)&api->QueryDecoderMemoryInfo) < 0) return -1;
    if (vdDlsym(mod, "sceVideodec2CreateDecoder", (void**)&api->CreateDecoder) < 0) return -1;
    if (vdDlsym(mod, "sceVideodec2DeleteDecoder", (void**)&api->DeleteDecoder) < 0) return -1;
    if (vdDlsym(mod, "sceVideodec2Decode", (void**)&api->Decode) < 0) return -1;
    if (vdDlsym(mod, "sceVideodec2Flush", (void**)&api->Flush) < 0) return -1;
    if (vdDlsym(mod, "sceVideodec2Reset", (void**)&api->Reset) < 0) return -1;
    vdDiagPrint("[video-decoder] Videodec2 symbols resolved");
    return 0;
}

int vdLoadApi(Vd2Api* api) {
    if (!api) return -1;
    memset(api, 0, sizeof(*api));
    api->module = -1;

    vdLoadVdecDeps();

    int mod = vdLoadModule("/system/common/lib/libSceVideodec2.sprx");
    if (mod < 0) {
        mod = vdLoadModule("libSceVideodec2.sprx");
        if (mod < 0) return -1;
    }

    if (vdResolveApi(api, mod) != 0) return -1;
    return 0;
}

void vdFillDecoderConfig(Vd2DecoderConfigInfo* dc, Vd2ComputeQueue q,
                         int w, int h, int pipelineDepth, int cpuPriority) {
    if (pipelineDepth < 1) pipelineDepth = 1;
    if (pipelineDepth > ORBIS_VIDEODEC2_MAX_PIPELINE_DEPTH) pipelineDepth = ORBIS_VIDEODEC2_MAX_PIPELINE_DEPTH;
    if (cpuPriority <= 0) cpuPriority = ORBIS_VIDEODEC2_THREAD_PRIO_DEFAULT;

    memset(dc, 0, sizeof(*dc));
    dc->thisSize = sizeof(*dc);
    dc->resourceType = ORBIS_VIDEODEC2_RESOURCE_TYPE_EMBEDDED;
    dc->codecType = ORBIS_VIDEODEC2_CODEC_AVC;
    dc->profile = ORBIS_VIDEODEC2_PROFILE_HIGH;
    dc->maxLevel = ORBIS_VIDEODEC2_LEVEL_51;
    dc->maxFrameWidth = w;
    dc->maxFrameHeight = (h + 15) & ~15;  // macroblock-align
    dc->maxDpbFrameCount = kMaxDpbFrameCount;
    dc->decodePipelineDepth = static_cast<uint32_t>(pipelineDepth);
    dc->computeQueue = q;
    dc->cpuAffinityMask = ORBIS_VIDEODEC2_AFFINITY_ALL;
    dc->cpuThreadPriority = cpuPriority;
    dc->optimizeProgressiveVideo = true;
    dc->checkMemoryType = false;
}

} // namespace

// ---------------------------------------------------------------------------
// Decoder worker thread
// ---------------------------------------------------------------------------

void* VideoDecoder::decoderWorker(void* arg) {
    VideoDecoder::Impl* impl = static_cast<VideoDecoder::Impl*>(arg);

    vdDiagPrint("[video-decoder] worker started");

    // --- Decode loop --------------------------------------------------------
    for (;;) {
        // Check for stop request.
        if (impl->stopRequested.load()) {
            break;
        }

        // Wait for the next encoded video unit.
        EncodedVideoUnit unit;
        bool gotUnit = false;
        pthread_mutex_lock(&impl->inputMutex);
        if (!impl->inputQueue.empty()) {
            unit = std::move(impl->inputQueue.front());
            impl->inputQueue.pop_front();
            gotUnit = true;
        }
        pthread_mutex_unlock(&impl->inputMutex);

        if (!gotUnit) {
            // No input available. Wait briefly or check stop.
            if (impl->stopRequested.load()) {
                break;
            }
            sceKernelUsleep(kWorkerIdleSleepUs);
            continue;
        }

        // Check for stop request after wait.
        if (impl->stopRequested.load()) {
            break;
        }

        // --- Validate the unit ----------------------------------------------
        if (unit.codecType != 0x01 /* VIDEO_FORMAT_H264 */ ||
            unit.fullLength == 0 ||
            unit.data.size() != unit.fullLength ||
            unit.data.size() > kMaxVideoUnitSize) {
            // Invalid unit — discard and request IDR.
            impl->decodeErrors.fetch_add(1);
            LiRequestIdrFrame();
            impl->idrRequests.fetch_add(1);
            impl->state = VideoDecoderState::WaitingForIdr;
            continue;
        }

        // --- First-IDR reset ------------------------------------------------
        // On the first IDR of a new session, reset the decoder to a clean
        // state and request a fresh IDR from the host.
        if (impl->awaitingFirstIdr.load() &&
            unit.frameType == 1 /* FRAME_TYPE_IDR */) {
            if (impl->decoder && impl->api.Reset) {
                impl->api.Reset(impl->decoder);
            }
            // Reset aborts in-flight frame-parallel processing. The decoder
            // no longer references any input AUs after a successful reset,
            // so the retained set can be cleared safely.
            impl->clearRetainedAus();
            impl->awaitingFirstIdr.store(false);
            LiRequestIdrFrame();
            impl->idrRequests.fetch_add(1);
            if (!impl->firstIdrLogged.exchange(true)) {
                char buf[128];
                snprintf(buf, sizeof(buf),
                         "[video] first IDR submitted (frame=%llu)",
                         static_cast<unsigned long long>(unit.frameNumber));
                vdDiagPrint(buf);
            }
        }

        // --- WaitingForIdr handling -----------------------------------------
        if (impl->state.load() == VideoDecoderState::WaitingForIdr) {
            if (unit.frameType != 1 /* FRAME_TYPE_IDR */) {
                // Discard P-frame while waiting for IDR.
                impl->encodedConsumed.fetch_add(1);
                continue;
            }
            // This is an IDR frame — submit it to recover.
            impl->state = VideoDecoderState::Running;
            {
                VideoDecoderUpdate update;
                update.type = VideoDecoderUpdateType::Recovered;
                update.result = VideoDecoderResultCategory::Success;
                impl->pushUpdate(update);
            }
        }

        // --- Submit the access unit to the decoder --------------------------
        Vd2InputData inputData;
        memset(&inputData, 0, sizeof(inputData));
        inputData.thisSize = sizeof(inputData);
        inputData.auData = unit.data.data();
        inputData.auSize = unit.data.size();

        // Normalize the RTP timestamp to a stream-local origin.
        if (!impl->haveTimestampBase) {
            impl->rtpTimestampBase = unit.rtpTimestamp;
            impl->haveTimestampBase = true;
        }
        {
            const uint32_t delta = unit.rtpTimestamp - impl->rtpTimestampBase;
            const uint64_t pts = static_cast<uint64_t>(delta);
            const uint64_t dts = pts;  // IP-only stream: PTS == DTS
            inputData.ptsData = pts;
            inputData.dtsData = dts;
        }
        // Attach the frame number so it can be recovered from the output.
        inputData.attachedData = unit.frameNumber;

        // Acquire a free output slot.
        int bufIndex = impl->acquireFreeFrameBuffer();
        if (bufIndex < 0) {
            // No free output slot — drop this decode.
            impl->decodedFrameDrops.fetch_add(1);
            impl->encodedConsumed.fetch_add(1);
            continue;
        }

        // Set the output frame buffer.
        Vd2FrameBuffer frameBuffer;
        memset(&frameBuffer, 0, sizeof(frameBuffer));
        frameBuffer.thisSize = sizeof(frameBuffer);
        frameBuffer.frameBuffer = impl->frameBuffers[bufIndex].address;
        frameBuffer.frameBufferSize = impl->frameBuffers[bufIndex].size;

        // Output info.
        Vd2OutputInfo outputInfo;
        memset(&outputInfo, 0, sizeof(outputInfo));
        outputInfo.thisSize = sizeof(outputInfo);

        // Time the decode.
        const uint64_t tDecode = static_cast<uint64_t>(sceKernelGetProcessTime());
        // --- Advance the decode-call sequence -------------------------------
        // Every attempted sceVideodec2Decode() call advances the sequence.
        // The submitting call counts as call 1 for the AU's lifetime.
        const uint64_t submitSeq = ++impl->decodeCallSeq;
        int32_t rc = impl->api.Decode(impl->decoder, &inputData, &frameBuffer, &outputInfo);
        const uint64_t elapsedUs = static_cast<uint64_t>(sceKernelGetProcessTime()) - tDecode;
        impl->statsDecodeTotalUs.fetch_add(elapsedUs, std::memory_order_relaxed);
        impl->statsDecodeCount.fetch_add(1, std::memory_order_relaxed);

        // --- Retire expired AUs, then retain the current AU -----------------
        // The decoder has synchronously consumed inputData.auData. Retire any
        // AU whose SDK-required lifetime has elapsed (based on the completed
        // decode call submitSeq), then move unit.data into retained storage so
        // it stays alive for the frame-parallel pipeline depth. Retire-before-
        // retain keeps the retained set strictly bounded to decodePipelineDepth.
        // unit.data is moved (no byte copy); the decoder no longer needs the
        // pointer after sceVideodec2Decode() returns.
        impl->retireExpiredAus(submitSeq);
        impl->retainAu(std::move(unit.data), submitSeq, unit.frameNumber);

        // Signal that all CPU-side command-buffer submissions are complete.
        // This is the OpenOrbis equivalent of sce::Gnm::submitDone() in the
        // SCE reference. It is REQUIRED to prevent CPU_FAULT_SUBMITDONE_
        // TIMEOUT_IN_SUSPEND_ASYNC when the app is suspended/killed while the
        // decoder has outstanding GPU work: the suspend path waits for the
        // GPU's submit-done state, and without this call it never arrives.
        // Called on the decoder worker thread after each decode so the
        // decoder's GPU work always has a submit-done.
        sceGnmSubmitDone();

        // The input AU is now consumed.
        impl->encodedConsumed.fetch_add(1);

        if (rc != 0) {
            impl->releaseFrameBuffer(bufIndex);
            impl->decodeErrors.fetch_add(1);

            // Map Videodec2 return codes to the decoder error policy.
            // (Return codes are negative; treat any failure as recoverable
            // by requesting an IDR, except fatal state which stops.)
            impl->state = VideoDecoderState::WaitingForIdr;
            LiRequestIdrFrame();
            impl->idrRequests.fetch_add(1);
            continue;
        }

        // --- Check the output ------------------------------------------------
        if (!outputInfo.isValid || outputInfo.isErrorFrame) {
            // No valid output for this AU, or the frame is an error frame.
            impl->releaseFrameBuffer(bufIndex);
            continue;
        }

        // --- Build the DecodedVideoFrame ------------------------------------
        DecodedVideoFrame::Impl* frameImpl = new DecodedVideoFrame::Impl();
        frameImpl->decoder = impl;
        frameImpl->frameBufferIndex = bufIndex;
        frameImpl->valid = true;
        frameImpl->frameNumber = unit.frameNumber;
        frameImpl->presentationTimeUs = unit.presentationTimeUs;
        frameImpl->decodeTimeUs = elapsedUs;
        frameImpl->width = outputInfo.frameWidth;
        frameImpl->height = impl->config.height;
        frameImpl->surfaceHeight = outputInfo.frameHeight;
        frameImpl->surfaceSlot = static_cast<uint32_t>(bufIndex);
        frameImpl->pitch = outputInfo.framePitchInBytes;
        frameImpl->format = VideoPixelFormat::NV12;
        frameImpl->yPlane = outputInfo.frameBuffer;
        // UV plane is at offset pitch * surfaceHeight (NV12 layout).
        frameImpl->uvPlane = static_cast<uint8_t*>(outputInfo.frameBuffer) +
                             outputInfo.framePitchInBytes * outputInfo.frameHeight;
        frameImpl->colorSpace = impl->config.colorSpace;
        frameImpl->colorRange = impl->config.colorRange;

        // Push into the decoded queue (bounded, latest-frame semantics).
        pthread_mutex_lock(&impl->decodedMutex);
        // Drop the oldest frame if the queue is full (low-latency policy).
        while (impl->decodedQueue.size() >= kDecodedQueueDepth) {
            DecodedVideoFrame::Impl* old = impl->decodedQueue.front();
            impl->decodedQueue.pop_front();
            impl->decodedFrameDrops.fetch_add(1);
            delete old;  // releases the surface back to the pool
        }
        impl->decodedQueue.push_back(frameImpl);
        pthread_mutex_unlock(&impl->decodedMutex);

        impl->framesDecoded.fetch_add(1);
        impl->statsDecodedFrames.fetch_add(1, std::memory_order_relaxed);

        if (!impl->firstFrameLogged.exchange(true)) {
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "[video] first frame decoded (frame=%llu %ux%u)",
                     static_cast<unsigned long long>(unit.frameNumber),
                     outputInfo.frameWidth, outputInfo.frameHeight);
            vdDiagPrint(buf);
        }
    }

    vdDiagPrint("[video-decoder] worker stopped");
    return nullptr;
}

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

VideoDecoder::VideoDecoder()
    : m_impl(new Impl())
{
}

VideoDecoder::~VideoDecoder() {
    if (m_impl) {
        shutdown();
        delete m_impl;
        m_impl = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

VideoDecoderResult VideoDecoder::initialize() {
    if (m_impl->running.load()) {
        return VideoDecoderResult(VideoDecoderResultCategory::InvalidState);
    }

    // Initialize mutexes.
    if (!m_impl->inputMutexInit) {
        if (pthread_mutex_init(&m_impl->inputMutex, nullptr) != 0) {
            return VideoDecoderResult(VideoDecoderResultCategory::InvalidState);
        }
        m_impl->inputMutexInit = true;
    }
    if (!m_impl->decodedMutexInit) {
        if (pthread_mutex_init(&m_impl->decodedMutex, nullptr) != 0) {
            return VideoDecoderResult(VideoDecoderResultCategory::InvalidState);
        }
        m_impl->decodedMutexInit = true;
    }
    if (!m_impl->updateMutexInit) {
        if (pthread_mutex_init(&m_impl->updateMutex, nullptr) != 0) {
            return VideoDecoderResult(VideoDecoderResultCategory::InvalidState);
        }
        m_impl->updateMutexInit = true;
    }
    if (!m_impl->wakeMutexInit) {
        if (pthread_mutex_init(&m_impl->wakeMutex, nullptr) != 0) {
            return VideoDecoderResult(VideoDecoderResultCategory::InvalidState);
        }
        m_impl->wakeMutexInit = true;
    }
    if (!m_impl->wakeCondInit) {
        if (pthread_cond_init(&m_impl->wakeCond, nullptr) != 0) {
            return VideoDecoderResult(VideoDecoderResultCategory::InvalidState);
        }
        m_impl->wakeCondInit = true;
    }

    // Reset state.
    m_impl->stopRequested = false;
    m_impl->running = true;
    m_impl->state = VideoDecoderState::Idle;
    m_impl->awaitingFirstIdr = true;
    m_impl->haveTimestampBase = false;
    m_impl->rtpTimestampBase = 0;
    m_impl->firstIdrLogged = false;
    m_impl->firstFrameLogged = false;
    m_impl->encodedConsumed = 0;
    m_impl->framesDecoded = 0;
    m_impl->decodeErrors = 0;
    m_impl->idrRequests = 0;
    m_impl->decodedFrameDrops = 0;
    m_impl->statsDecodedFrames = 0;
    m_impl->statsDecodeTotalUs = 0;
    m_impl->statsDecodeCount = 0;

    // Clear retained AUs and reset the decode-call sequence.
    m_impl->retainedAus.clear();
    m_impl->decodeCallSeq = 0;

    // Clear queues.
    pthread_mutex_lock(&m_impl->inputMutex);
    m_impl->inputQueue.clear();
    pthread_mutex_unlock(&m_impl->inputMutex);
    pthread_mutex_lock(&m_impl->decodedMutex);
    while (!m_impl->decodedQueue.empty()) {
        delete m_impl->decodedQueue.front();
        m_impl->decodedQueue.pop_front();
    }
    pthread_mutex_unlock(&m_impl->decodedMutex);
    pthread_mutex_lock(&m_impl->updateMutex);
    m_impl->updateQueue.clear();
    pthread_mutex_unlock(&m_impl->updateMutex);

    vdDiagPrint("[video-decoder] manager initialized");
    return VideoDecoderResult();
}

// ---------------------------------------------------------------------------
// Start
// ---------------------------------------------------------------------------

VideoDecoderResult VideoDecoder::start(const VideoDecoderConfiguration& config) {
    if (!m_impl->running.load()) {
        return VideoDecoderResult(VideoDecoderResultCategory::InvalidState);
    }

    // --- Validate the configuration -----------------------------------------
    if (config.width < 1 || config.width > 8192 ||
        config.height < 1 || config.height > 8192 ||
        config.fps < 1 || config.fps > 240) {
        return VideoDecoderResult(VideoDecoderResultCategory::InvalidConfiguration);
    }
    if (config.codec != VideoCodec::H264) {
        return VideoDecoderResult(VideoDecoderResultCategory::UnsupportedCodec);
    }

    // --- Check state ---------------------------------------------------------
    VideoDecoderState cur = m_impl->state.load();
    if (cur == VideoDecoderState::Starting ||
        cur == VideoDecoderState::Running ||
        cur == VideoDecoderState::Stopping) {
        return VideoDecoderResult(VideoDecoderResultCategory::InvalidState);
    }

    // --- Load the Videodec2 API (idempotent) ---------------------------------
    if (m_impl->api.module < 0) {
        if (vdLoadApi(&m_impl->api) != 0) {
            vdDiagPrint("[video-decoder] Videodec2 load failed");
            return VideoDecoderResult(VideoDecoderResultCategory::DecoderInitializationFailure);
        }
    }

    // --- Allocate the compute queue ------------------------------------------
    if (m_impl->api.queue == nullptr) {
        Vd2ComputeMemoryInfo mi;
        memset(&mi, 0, sizeof(mi));
        mi.thisSize = sizeof(mi);

        int32_t rc = m_impl->api.QueryComputeMemoryInfo(&mi);
        if (rc != 0) {
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "[video-decoder] QueryComputeMemoryInfo failed: 0x%08X",
                     static_cast<uint32_t>(rc));
            vdDiagPrint(buf);
            return VideoDecoderResult(VideoDecoderResultCategory::DecoderInitializationFailure, rc);
        }

        if (!m_impl->computeMem.allocate(static_cast<size_t>(mi.cpuGpuMemorySize),
                                         kDmemTypeOnion, kDmemProt)) {
            return VideoDecoderResult(VideoDecoderResultCategory::DecoderResourceFailure);
        }
        mi.cpuGpuMemory = m_impl->computeMem.baseAddress;

        Vd2ComputeConfigInfo ci;
        memset(&ci, 0, sizeof(ci));
        ci.thisSize = sizeof(ci);
        ci.computePipeId = kComputePipeId;
        ci.computeQueueId = kComputeQueueId;
        ci.checkMemoryType = true;

        Vd2ComputeQueue q = nullptr;
        rc = m_impl->api.AllocateComputeQueue(&ci, &mi, &q);
        if (rc != 0 || q == nullptr) {
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "[video-decoder] AllocateComputeQueue failed: 0x%08X",
                     static_cast<uint32_t>(rc));
            vdDiagPrint(buf);
            m_impl->computeMem.release();
            return VideoDecoderResult(VideoDecoderResultCategory::DecoderInitializationFailure, rc);
        }
        m_impl->api.queue = q;
        vdDiagPrint("[video-decoder] compute queue allocated");
    }

    // --- Query decoder memory ------------------------------------------------
    Vd2DecoderConfigInfo dc;
    vdFillDecoderConfig(&dc, m_impl->api.queue,
                        static_cast<int>(config.width),
                        static_cast<int>(config.height),
                        static_cast<int>(kDecodePipelineDepth),
                        ORBIS_VIDEODEC2_THREAD_PRIO_DEFAULT);

    Vd2DecoderMemoryInfo dm;
    memset(&dm, 0, sizeof(dm));
    dm.thisSize = sizeof(dm);

    int32_t rc = m_impl->api.QueryDecoderMemoryInfo(&dc, &dm);
    if (rc != 0) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[video-decoder] QueryDecoderMemoryInfo failed: 0x%08X",
                 static_cast<uint32_t>(rc));
        vdDiagPrint(buf);
        return VideoDecoderResult(VideoDecoderResultCategory::DecoderInitializationFailure, rc);
    }

    // --- Allocate decoder memories -------------------------------------------
    // cpuMemory / cpuGpuMemory are the decoder's CPU working areas (entropy
    // parsing, DPB). Onion (WB, CPU-cacheable + GPU-visible) is correct.
    // gpuMemory stays Garlic (GPU work).
    if (dm.cpuMemorySize > 0) {
        if (!m_impl->cpuMem.allocate(static_cast<size_t>(dm.cpuMemorySize),
                                     kDmemTypeOnion, kDmemProt)) {
            return VideoDecoderResult(VideoDecoderResultCategory::DecoderResourceFailure);
        }
        dm.cpuMemory = m_impl->cpuMem.baseAddress;
    }
    if (dm.gpuMemorySize > 0) {
        if (!m_impl->gpuMem.allocate(static_cast<size_t>(dm.gpuMemorySize),
                                     kDmemTypeGarlic, kDmemProt)) {
            return VideoDecoderResult(VideoDecoderResultCategory::DecoderResourceFailure);
        }
        dm.gpuMemory = m_impl->gpuMem.baseAddress;
    }
    if (dm.cpuGpuMemorySize > 0) {
        if (!m_impl->cpuGpuMem.allocate(static_cast<size_t>(dm.cpuGpuMemorySize),
                                        kDmemTypeOnion, kDmemProt)) {
            return VideoDecoderResult(VideoDecoderResultCategory::DecoderResourceFailure);
        }
        dm.cpuGpuMemory = m_impl->cpuGpuMem.baseAddress;
    }

    // --- Allocate frame buffers ----------------------------------------------
    if (dm.maxFrameBufferSize > 0) {
        const size_t align = dm.frameBufferAlignment ? dm.frameBufferAlignment
                                                     : kFrameBufferAlignment;
        const size_t need = (static_cast<size_t>(dm.maxFrameBufferSize) + align - 1) &
                            ~(align - 1);
        const size_t total = need * kFrameBufferCount;
        if (!m_impl->frameBufferMem.allocate(total, kDmemTypeOnion, kDmemProt)) {
            return VideoDecoderResult(VideoDecoderResultCategory::DecoderResourceFailure);
        }
        for (size_t i = 0; i < kFrameBufferCount; ++i) {
            m_impl->frameBuffers[i].address =
                static_cast<uint8_t*>(m_impl->frameBufferMem.baseAddress) + i * need;
            m_impl->frameBuffers[i].size = need;
            m_impl->frameBuffers[i].leased = false;
        }
    }

    // --- Create the decoder --------------------------------------------------
    Vd2Decoder dec = nullptr;
    rc = m_impl->api.CreateDecoder(&dc, &dm, &dec);
    if (rc != 0 || dec == nullptr) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[video-decoder] CreateDecoder failed: 0x%08X",
                 static_cast<uint32_t>(rc));
        vdDiagPrint(buf);
        return VideoDecoderResult(VideoDecoderResultCategory::DecoderInitializationFailure, rc);
    }
    m_impl->decoder = dec;

    // --- Store the active configuration --------------------------------------
    m_impl->config = config;

    // --- Set the frame-parallel pipeline depth ------------------------------
    // Must match the value used in vdFillDecoderConfig() above. Bounds the
    // retained-AU set in the decode loop.
    m_impl->decodePipelineDepth = kDecodePipelineDepth;

    // --- Reset per-session state ---------------------------------------------
    m_impl->awaitingFirstIdr = true;
    m_impl->haveTimestampBase = false;
    m_impl->rtpTimestampBase = 0;
    m_impl->firstIdrLogged = false;
    m_impl->firstFrameLogged = false;
    m_impl->encodedConsumed = 0;
    m_impl->framesDecoded = 0;
    m_impl->decodeErrors = 0;
    m_impl->idrRequests = 0;
    m_impl->decodedFrameDrops = 0;
    m_impl->statsDecodedFrames = 0;
    m_impl->statsDecodeTotalUs = 0;
    m_impl->statsDecodeCount = 0;

    // Clear retained AUs and reset the decode-call sequence.
    m_impl->retainedAus.clear();
    m_impl->decodeCallSeq = 0;

    // Clear queues.
    pthread_mutex_lock(&m_impl->inputMutex);
    m_impl->inputQueue.clear();
    pthread_mutex_unlock(&m_impl->inputMutex);
    pthread_mutex_lock(&m_impl->decodedMutex);
    while (!m_impl->decodedQueue.empty()) {
        delete m_impl->decodedQueue.front();
        m_impl->decodedQueue.pop_front();
    }
    pthread_mutex_unlock(&m_impl->decodedMutex);
    pthread_mutex_lock(&m_impl->updateMutex);
    m_impl->updateQueue.clear();
    pthread_mutex_unlock(&m_impl->updateMutex);

    // --- Start the worker thread ---------------------------------------------
    m_impl->stopRequested = false;
    m_impl->state = VideoDecoderState::Starting;

    int prc = pthread_create(&m_impl->workerThread, nullptr, decoderWorker, m_impl);
    if (prc != 0) {
        m_impl->state = VideoDecoderState::Idle;
        return VideoDecoderResult(VideoDecoderResultCategory::InvalidState, prc);
    }
    m_impl->threadCreated = true;

    m_impl->state = VideoDecoderState::Running;

    char buf[160];
    snprintf(buf, sizeof(buf),
             "[video] decoder initialized: H264 %ux%u @ %u fps",
             config.width, config.height, config.fps);
    vdDiagPrint(buf);

    {
        VideoDecoderUpdate update;
        update.type = VideoDecoderUpdateType::Started;
        update.result = VideoDecoderResultCategory::Success;
        m_impl->pushUpdate(update);
    }

    return VideoDecoderResult();
}

// ---------------------------------------------------------------------------
// Stop
// ---------------------------------------------------------------------------

void VideoDecoder::stop() {
    if (!m_impl) {
        return;
    }

    VideoDecoderState cur = m_impl->state.load();
    if (cur == VideoDecoderState::Idle || cur == VideoDecoderState::Failed) {
        return;
    }

    // Mark stop requested.
    m_impl->stopRequested = true;
    m_impl->state = VideoDecoderState::Stopping;

    // Join the worker thread.
    if (m_impl->threadCreated) {
        pthread_join(m_impl->workerThread, nullptr);
        m_impl->threadCreated = false;
    }

    // Flush the decoder (drain any pending output).
    if (m_impl->decoder && m_impl->api.Flush) {
        Vd2FrameBuffer fb;
        memset(&fb, 0, sizeof(fb));
        fb.thisSize = sizeof(fb);
        Vd2OutputInfo out;
        memset(&out, 0, sizeof(out));
        out.thisSize = sizeof(out);
        m_impl->api.Flush(m_impl->decoder, &fb, &out);
    }

    // Clear retained AUs. The decoder can no longer reference the input AUs
    // after the flush loop has drained all output.
    m_impl->clearRetainedAus();
    m_impl->decodeCallSeq = 0;

    // Release the decoder.
    if (m_impl->decoder && m_impl->api.DeleteDecoder) {
        m_impl->api.DeleteDecoder(m_impl->decoder);
    }
    m_impl->decoder = nullptr;

    // Release the compute queue.
    if (m_impl->api.queue && m_impl->api.ReleaseComputeQueue) {
        m_impl->api.ReleaseComputeQueue(m_impl->api.queue);
    }
    m_impl->api.queue = nullptr;

    // Release all direct memory.
    m_impl->frameBufferMem.release();
    m_impl->cpuGpuMem.release();
    m_impl->gpuMem.release();
    m_impl->cpuMem.release();
    m_impl->computeMem.release();

    // Reset frame buffers.
    for (size_t i = 0; i < kFrameBufferCount; ++i) {
        m_impl->frameBuffers[i].address = nullptr;
        m_impl->frameBuffers[i].size = 0;
        m_impl->frameBuffers[i].leased = false;
    }

    // Clear the decoded queue (release any outstanding surfaces).
    pthread_mutex_lock(&m_impl->decodedMutex);
    while (!m_impl->decodedQueue.empty()) {
        delete m_impl->decodedQueue.front();
        m_impl->decodedQueue.pop_front();
    }
    pthread_mutex_unlock(&m_impl->decodedMutex);

    // Clear the input queue.
    pthread_mutex_lock(&m_impl->inputMutex);
    m_impl->inputQueue.clear();
    pthread_mutex_unlock(&m_impl->inputMutex);

    m_impl->state = VideoDecoderState::Idle;

    {
        VideoDecoderUpdate update;
        update.type = VideoDecoderUpdateType::Stopped;
        update.result = VideoDecoderResultCategory::Success;
        m_impl->pushUpdate(update);
    }

    vdDiagPrint("[video-decoder] session stopped");
}

// ---------------------------------------------------------------------------
// State query
// ---------------------------------------------------------------------------

VideoDecoderState VideoDecoder::state() const {
    return m_impl ? m_impl->state.load() : VideoDecoderState::Idle;
}

// ---------------------------------------------------------------------------
// Result polling
// ---------------------------------------------------------------------------

bool VideoDecoder::pollUpdate(VideoDecoderUpdate& out) {
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
// Encoded-unit input
// ---------------------------------------------------------------------------

bool VideoDecoder::pushEncodedUnit(EncodedVideoUnit&& unit) {
    if (!m_impl) {
        return false;
    }

    // Reject if not running.
    VideoDecoderState s = m_impl->state.load();
    if (s != VideoDecoderState::Running && s != VideoDecoderState::Starting) {
        return false;
    }

    pthread_mutex_lock(&m_impl->inputMutex);
    if (m_impl->inputQueue.size() >= kInputQueueCapacity) {
        // Queue full — drop the oldest unit (low-latency policy).
        m_impl->inputQueue.pop_front();
    }
    m_impl->inputQueue.push_back(std::move(unit));
    pthread_mutex_unlock(&m_impl->inputMutex);

    return true;
}

// ---------------------------------------------------------------------------
// Decoded-frame acquisition
// ---------------------------------------------------------------------------

bool VideoDecoder::tryAcquireLatestFrame(DecodedVideoFrame& out) {
    if (!m_impl) {
        return false;
    }

    pthread_mutex_lock(&m_impl->decodedMutex);
    if (m_impl->decodedQueue.empty()) {
        pthread_mutex_unlock(&m_impl->decodedMutex);
        return false;
    }

    // Latest-frame semantics: drop all but the newest frame.
    while (m_impl->decodedQueue.size() > 1) {
        DecodedVideoFrame::Impl* old = m_impl->decodedQueue.front();
        m_impl->decodedQueue.pop_front();
        m_impl->decodedFrameDrops.fetch_add(1);
        delete old;  // releases the surface back to the pool
    }

    DecodedVideoFrame::Impl* frameImpl = m_impl->decodedQueue.front();
    m_impl->decodedQueue.pop_front();
    pthread_mutex_unlock(&m_impl->decodedMutex);

    // Transfer ownership to the caller.
    out.m_impl = frameImpl;
    return true;
}

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

void VideoDecoder::readDecodeStats(StreamStatsCounters& out) const {
    if (!m_impl) {
        return;
    }
    out.decodedFrames = m_impl->statsDecodedFrames.load(std::memory_order_relaxed);
    out.decodeTotalUs = m_impl->statsDecodeTotalUs.load(std::memory_order_relaxed);
    out.decodeCount = m_impl->statsDecodeCount.load(std::memory_order_relaxed);
    out.width = static_cast<int>(m_impl->config.width);
    out.height = static_cast<int>(m_impl->config.height);
}

uint32_t VideoDecoder::surfaceGeneration(uint32_t surfaceSlot) const {
    if (!m_impl || surfaceSlot >= kFrameBufferCount) {
        return 0;
    }
    return m_impl->frameBuffers[surfaceSlot].generation.load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------

void VideoDecoder::shutdown() {
    if (!m_impl) {
        return;
    }

    // Stop the decoder session if active.
    VideoDecoderState cur = m_impl->state.load();
    if (cur == VideoDecoderState::Starting ||
        cur == VideoDecoderState::Running ||
        cur == VideoDecoderState::Stopping ||
        cur == VideoDecoderState::WaitingForIdr) {
        stop();
    }

    // Destroy synchronization primitives.
    if (m_impl->wakeCondInit) {
        pthread_cond_destroy(&m_impl->wakeCond);
        m_impl->wakeCondInit = false;
    }
    if (m_impl->wakeMutexInit) {
        pthread_mutex_destroy(&m_impl->wakeMutex);
        m_impl->wakeMutexInit = false;
    }
    if (m_impl->updateMutexInit) {
        pthread_mutex_destroy(&m_impl->updateMutex);
        m_impl->updateMutexInit = false;
    }
    if (m_impl->decodedMutexInit) {
        pthread_mutex_destroy(&m_impl->decodedMutex);
        m_impl->decodedMutexInit = false;
    }
    if (m_impl->inputMutexInit) {
        pthread_mutex_destroy(&m_impl->inputMutex);
        m_impl->inputMutexInit = false;
    }

    m_impl->running = false;
    m_impl->state = VideoDecoderState::Idle;

    vdDiagPrint("[video-decoder] manager shut down");
}

} // namespace ml
