// Copyright (C) 2026 fourletterworkshop.com
// Copyright (C) 2026 natko (github.com/natkokrasevac)
// This file is part of Moonlight MP for PlayStation 4.
// Moonlight MP for PlayStation 4 is free software: you can redistribute it
// and/or modify it under the terms of the GNU General Public License as
// published by the Free Software Foundation, either version 3 of the License,
// or (at your option) any later version.
// Moonlight MP for PlayStation 4 is distributed in the hope that it will be
// useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
// Public License for more details.
// You should have received a copy of the GNU General Public License along with
// Moonlight MP for PlayStation 4. If not, see <https://www.gnu.org/licenses/>.
//

// === UpdateManager.cpp =======================================================
//
// OpenOrbis Moonlight client — Auto-Update Subsystem implementation
// =============================================================================

#include "UpdateManager.h"
#include "HttpClient.h"
#include "ui/UiStrings.h"

#include <pthread.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __ORBIS__
#include <orbis/libkernel.h>
#endif

namespace ml {

namespace {

constexpr const char* kLatestReleaseUrl =
    "https://api.github.com/repos/natkokrasevac/moonlight-mp-ps4/releases/latest";

constexpr const char* kPkgDirGoldHen = "/data/pkg";
constexpr const char* kPkgDirFallback = "/data/moonlight-mp";

void diagPrint(const char* msg) {
    printf("%s\n", msg);
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// Version parsing and comparison
// ---------------------------------------------------------------------------

struct SemVer {
    int major = 0;
    int minor = 0;
    int patch = 0;

    static SemVer parse(const std::string& str) {
        SemVer v;
        size_t start = 0;
        if (!str.empty() && (str[0] == 'v' || str[0] == 'V')) {
            start = 1;
        }
        int* parts[3] = { &v.major, &v.minor, &v.patch };
        int partIdx = 0;
        int currentVal = 0;
        bool hasDigits = false;

        for (size_t i = start; i <= str.size() && partIdx < 3; ++i) {
            if (i < str.size() && str[i] >= '0' && str[i] <= '9') {
                currentVal = currentVal * 10 + (str[i] - '0');
                hasDigits = true;
            } else if (i == str.size() || str[i] == '.' || str[i] == '-') {
                if (hasDigits) {
                    *parts[partIdx] = currentVal;
                    partIdx++;
                    currentVal = 0;
                    hasDigits = false;
                }
                if (i < str.size() && str[i] == '-') {
                    break;
                }
            }
        }
        return v;
    }

    bool operator>(const SemVer& o) const {
        if (major != o.major) return major > o.major;
        if (minor != o.minor) return minor > o.minor;
        return patch > o.patch;
    }
};

// ---------------------------------------------------------------------------
// Minimal zero-dependency JSON field extraction
// ---------------------------------------------------------------------------

bool findJsonStringField(const std::string& json, const std::string& key, std::string& outVal, size_t startPos = 0) {
    std::string needle = "\"" + key + "\"";
    size_t keyPos = json.find(needle, startPos);
    if (keyPos == std::string::npos) return false;

    size_t colonPos = json.find(':', keyPos + needle.length());
    if (colonPos == std::string::npos) return false;

    size_t quoteStart = json.find('"', colonPos + 1);
    if (quoteStart == std::string::npos) return false;

    size_t quoteEnd = json.find('"', quoteStart + 1);
    if (quoteEnd == std::string::npos) return false;

    outVal = json.substr(quoteStart + 1, quoteEnd - quoteStart - 1);
    return true;
}

bool findJsonNumberField(const std::string& json, const std::string& key, uint64_t& outVal, size_t startPos = 0) {
    std::string needle = "\"" + key + "\"";
    size_t keyPos = json.find(needle, startPos);
    if (keyPos == std::string::npos) return false;

    size_t colonPos = json.find(':', keyPos + needle.length());
    if (colonPos == std::string::npos) return false;

    size_t numStart = colonPos + 1;
    while (numStart < json.size() && (json[numStart] == ' ' || json[numStart] == '\t' || json[numStart] == '\r' || json[numStart] == '\n')) {
        numStart++;
    }

    size_t numEnd = numStart;
    while (numEnd < json.size() && (json[numEnd] >= '0' && json[numEnd] <= '9')) {
        numEnd++;
    }

    if (numEnd > numStart) {
        std::string numStr = json.substr(numStart, numEnd - numStart);
        outVal = static_cast<uint64_t>(std::strtoull(numStr.c_str(), nullptr, 10));
        return true;
    }
    return false;
}

bool parseReleaseJson(const std::string& json, UpdateInfo& outInfo) {
    if (!findJsonStringField(json, "tag_name", outInfo.tagName)) {
        return false;
    }
    findJsonStringField(json, "name", outInfo.releaseName);

    // Look for assets array
    size_t assetsPos = json.find("\"assets\"");
    if (assetsPos == std::string::npos) {
        return false;
    }

    size_t openBracket = json.find('[', assetsPos);
    size_t closeBracket = json.find(']', assetsPos);
    if (openBracket == std::string::npos || closeBracket == std::string::npos || closeBracket <= openBracket) {
        return false;
    }

    // Iterate asset objects inside [ ... ]
    size_t cursor = openBracket + 1;
    while (cursor < closeBracket) {
        size_t objStart = json.find('{', cursor);
        if (objStart == std::string::npos || objStart >= closeBracket) break;
        size_t objEnd = json.find('}', objStart);
        if (objEnd == std::string::npos || objEnd > closeBracket) break;

        std::string assetObj = json.substr(objStart, objEnd - objStart + 1);
        std::string name;
        if (findJsonStringField(assetObj, "name", name)) {
            // Check if name ends with ".pkg"
            if (name.size() > 4 && name.compare(name.size() - 4, 4, ".pkg") == 0) {
                outInfo.pkgName = name;
                findJsonStringField(assetObj, "browser_download_url", outInfo.downloadUrl);
                findJsonNumberField(assetObj, "size", outInfo.pkgSize);
                return !outInfo.downloadUrl.empty();
            }
        }
        cursor = objEnd + 1;
    }

    return false;
}

// ---------------------------------------------------------------------------
// PS4 Dynamic App Installation (libSceAppInstUtil.sprx)
// ---------------------------------------------------------------------------

bool tryInstallPkgNative(const std::string& pkgPath) {
#ifdef __ORBIS__
    diagPrint("[update] attempting native pkg install via libSceAppInstUtil...");
    int mod = static_cast<int>(sceKernelLoadStartModule("/system/common/lib/libSceAppInstUtil.sprx",
                                                        0, nullptr, 0, nullptr, nullptr));
    if (mod <= 0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "[update] failed to load libSceAppInstUtil (0x%08X)", static_cast<uint32_t>(mod));
        diagPrint(buf);
        return false;
    }

    typedef int (*AppInstUtilInitFn)();
    typedef int (*AppInstUtilTermFn)();
    typedef int (*AppInstUtilInstallPkgFn)(const char* path, void* reserved);

    AppInstUtilInitFn fnInit = nullptr;
    AppInstUtilTermFn fnTerm = nullptr;
    AppInstUtilInstallPkgFn fnInstall = nullptr;

    sceKernelDlsym(mod, "sceAppInstUtilInitialize", reinterpret_cast<void**>(&fnInit));
    sceKernelDlsym(mod, "sceAppInstUtilTerminate", reinterpret_cast<void**>(&fnTerm));
    sceKernelDlsym(mod, "sceAppInstUtilAppInstallPkg", reinterpret_cast<void**>(&fnInstall));

    if (!fnInit || !fnInstall) {
        diagPrint("[update] failed to resolve libSceAppInstUtil symbols");
        return false;
    }

    int rc = fnInit();
    char buf[128];
    snprintf(buf, sizeof(buf), "[update] sceAppInstUtilInitialize => 0x%08X", static_cast<uint32_t>(rc));
    diagPrint(buf);

    rc = fnInstall(pkgPath.c_str(), nullptr);
    snprintf(buf, sizeof(buf), "[update] sceAppInstUtilAppInstallPkg(%s) => 0x%08X", pkgPath.c_str(), static_cast<uint32_t>(rc));
    diagPrint(buf);

    if (fnTerm) {
        fnTerm();
    }

    return (rc >= 0);
#else
    (void)pkgPath;
    return false;
#endif
}

} // namespace

// ---------------------------------------------------------------------------
// Private Impl
// ---------------------------------------------------------------------------

struct UpdateManager::Impl {
    bool initialized = false;
    HttpClient httpClient;
    HttpCancellationToken cancellationToken;

    std::atomic<UpdateStatus> status{UpdateStatus::Idle};
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_t workerThread = 0;
    bool workerRunning = false;

    UpdateInfo updateInfo;

    std::atomic<uint64_t> downloadedBytes{0};
    std::atomic<uint64_t> totalBytes{0};

    static void downloadProgressCallback(uint64_t downloaded, uint64_t total, void* userCtx) {
        Impl* self = static_cast<Impl*>(userCtx);
        if (self) {
            self->downloadedBytes.store(downloaded, std::memory_order_relaxed);
            if (total > 0) {
                self->totalBytes.store(total, std::memory_order_relaxed);
            }
        }
    }

    static void* checkThreadFunc(void* arg) {
        Impl* self = static_cast<Impl*>(arg);
        diagPrint("[update] checking for updates on GitHub...");

        HttpRequest req;
        if (!HttpClient::parseUrl(kLatestReleaseUrl, req)) {
            diagPrint("[update] failed to parse latest release URL");
            self->status.store(UpdateStatus::CheckFailed, std::memory_order_release);
            return nullptr;
        }

        req.headers.push_back({"User-Agent", "Moonlight-PS4-Updater"});
        req.headers.push_back({"Accept", "application/vnd.github+json"});
        req.connectTimeoutMs = 8000;
        req.ioTimeoutMs = 12000;

        HttpResponse res;
        HttpResult result = self->httpClient.perform(req, res, &self->cancellationToken);

        if (self->cancellationToken.cancelled()) {
            diagPrint("[update] check cancelled");
            self->status.store(UpdateStatus::Cancelled, std::memory_order_release);
            return nullptr;
        }

        if (!result.succeeded() || res.statusCode != 200 || res.body.empty()) {
            char buf[128];
            snprintf(buf, sizeof(buf), "[update] check request failed (status=%d, err=0x%08X)",
                     res.statusCode, result.nativeError);
            diagPrint(buf);
            self->status.store(UpdateStatus::CheckFailed, std::memory_order_release);
            return nullptr;
        }

        std::string json(reinterpret_cast<const char*>(res.body.data()), res.body.size());
        UpdateInfo info;
        if (!parseReleaseJson(json, info)) {
            diagPrint("[update] failed to parse release JSON or no .pkg asset found");
            self->status.store(UpdateStatus::CheckFailed, std::memory_order_release);
            return nullptr;
        }

        pthread_mutex_lock(&self->mutex);
        self->updateInfo = info;
        pthread_mutex_unlock(&self->mutex);

        char buf[256];
        snprintf(buf, sizeof(buf), "[update] found release tag=%s pkg=%s (size=%llu bytes)",
                 info.tagName.c_str(), info.pkgName.c_str(), static_cast<unsigned long long>(info.pkgSize));
        diagPrint(buf);

        if (UpdateManager::isNewerVersion(info.tagName, ui::UiStrings::kVersionNumber)) {
            diagPrint("[update] newer version available!");
            self->status.store(UpdateStatus::UpdateAvailable, std::memory_order_release);
        } else {
            diagPrint("[update] application is up to date");
            self->status.store(UpdateStatus::UpToDate, std::memory_order_release);
        }

        return nullptr;
    }

    static void* downloadThreadFunc(void* arg) {
        Impl* self = static_cast<Impl*>(arg);
        diagPrint("[update] starting download worker...");

        UpdateInfo info;
        pthread_mutex_lock(&self->mutex);
        info = self->updateInfo;
        pthread_mutex_unlock(&self->mutex);

        if (info.downloadUrl.empty() || info.pkgName.empty()) {
            diagPrint("[update] download URL or pkgName is empty");
            self->status.store(UpdateStatus::DownloadFailed, std::memory_order_release);
            return nullptr;
        }

        // Determine destination path: prefer /data/pkg/ (GoldHEN default), fallback to /data/moonlight-mp/
        mkdir("/data", 0777);
        std::string targetDir = kPkgDirGoldHen;
        if (mkdir(targetDir.c_str(), 0777) != 0 && access(targetDir.c_str(), W_OK) != 0) {
            targetDir = kPkgDirFallback;
            mkdir(targetDir.c_str(), 0777);
        }

        std::string localPath = targetDir + "/" + info.pkgName;
        pthread_mutex_lock(&self->mutex);
        self->updateInfo.localPkgPath = localPath;
        pthread_mutex_unlock(&self->mutex);

        self->downloadedBytes.store(0, std::memory_order_relaxed);
        self->totalBytes.store(info.pkgSize, std::memory_order_relaxed);

        HttpRequest req;
        if (!HttpClient::parseUrl(info.downloadUrl, req)) {
            diagPrint("[update] failed to parse download URL");
            self->status.store(UpdateStatus::DownloadFailed, std::memory_order_release);
            return nullptr;
        }

        req.headers.push_back({"User-Agent", "Moonlight-PS4-Updater"});
        req.headers.push_back({"Accept", "application/octet-stream"});
        req.connectTimeoutMs = 15000;
        req.ioTimeoutMs = 30000;

        HttpResponse res;
        HttpResult result = self->httpClient.performDownload(req, localPath, res,
                                                            downloadProgressCallback,
                                                            self,
                                                            &self->cancellationToken);

        if (self->cancellationToken.cancelled()) {
            diagPrint("[update] download was cancelled");
            self->status.store(UpdateStatus::Cancelled, std::memory_order_release);
            return nullptr;
        }

        if (!result.succeeded()) {
            char buf[128];
            snprintf(buf, sizeof(buf), "[update] download failed (err=0x%08X)", result.nativeError);
            diagPrint(buf);
            self->status.store(UpdateStatus::DownloadFailed, std::memory_order_release);
            return nullptr;
        }

        diagPrint("[update] download complete! Checking for native installer...");
        self->status.store(UpdateStatus::Installing, std::memory_order_release);

        bool installed = tryInstallPkgNative(localPath);
        if (installed) {
            diagPrint("[update] native install queued successfully!");
            self->status.store(UpdateStatus::InstallStarted, std::memory_order_release);
        } else {
            diagPrint("[update] pkg saved to disk for GoldHEN package installer");
            self->status.store(UpdateStatus::Downloaded, std::memory_order_release);
        }

        return nullptr;
    }
};

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

UpdateManager::UpdateManager()
    : m_impl(new Impl())
{
}

UpdateManager::~UpdateManager() {
    shutdown();
    delete m_impl;
    m_impl = nullptr;
}

bool UpdateManager::initialize() {
    if (!m_impl || m_impl->initialized) {
        return false;
    }
    HttpResult res = m_impl->httpClient.initialize();
    if (!res.succeeded()) {
        diagPrint("[update] failed to initialize HttpClient");
        return false;
    }
    m_impl->initialized = true;
    diagPrint("[update] UpdateManager initialized");
    return true;
}

void UpdateManager::shutdown() {
    if (!m_impl || !m_impl->initialized) {
        return;
    }
    cancel();
    if (m_impl->workerRunning && m_impl->workerThread != 0) {
        pthread_join(m_impl->workerThread, nullptr);
        m_impl->workerThread = 0;
        m_impl->workerRunning = false;
    }
    m_impl->httpClient.shutdown();
    m_impl->initialized = false;
    diagPrint("[update] UpdateManager shut down");
}

bool UpdateManager::isInitialized() const {
    return m_impl && m_impl->initialized;
}

void UpdateManager::checkForUpdatesAsync() {
    if (!m_impl || !m_impl->initialized) {
        return;
    }
    if (m_impl->workerRunning) {
        return;
    }

    m_impl->cancellationToken.reset();
    m_impl->status.store(UpdateStatus::Checking, std::memory_order_release);
    m_impl->workerRunning = true;

    int rc = pthread_create(&m_impl->workerThread, nullptr, Impl::checkThreadFunc, m_impl);
    if (rc != 0) {
        diagPrint("[update] pthread_create failed for checkThreadFunc");
        m_impl->workerRunning = false;
        m_impl->status.store(UpdateStatus::CheckFailed, std::memory_order_release);
    }
}

void UpdateManager::startDownloadAsync() {
    if (!m_impl || !m_impl->initialized) {
        return;
    }
    if (m_impl->workerRunning) {
        pthread_join(m_impl->workerThread, nullptr);
        m_impl->workerThread = 0;
        m_impl->workerRunning = false;
    }

    m_impl->cancellationToken.reset();
    m_impl->status.store(UpdateStatus::Downloading, std::memory_order_release);
    m_impl->workerRunning = true;

    int rc = pthread_create(&m_impl->workerThread, nullptr, Impl::downloadThreadFunc, m_impl);
    if (rc != 0) {
        diagPrint("[update] pthread_create failed for downloadThreadFunc");
        m_impl->workerRunning = false;
        m_impl->status.store(UpdateStatus::DownloadFailed, std::memory_order_release);
    }
}

void UpdateManager::cancel() {
    if (m_impl) {
        m_impl->cancellationToken.cancel();
    }
}

UpdateStatus UpdateManager::status() const {
    if (!m_impl) return UpdateStatus::Idle;

    // Reap worker thread if it has finished
    if (m_impl->workerRunning) {
        UpdateStatus s = m_impl->status.load(std::memory_order_acquire);
        if (s != UpdateStatus::Checking && s != UpdateStatus::Downloading) {
            pthread_join(m_impl->workerThread, nullptr);
            m_impl->workerThread = 0;
            m_impl->workerRunning = false;
        }
    }

    return m_impl->status.load(std::memory_order_acquire);
}

UpdateInfo UpdateManager::updateInfo() const {
    UpdateInfo info;
    if (m_impl) {
        pthread_mutex_lock(&m_impl->mutex);
        info = m_impl->updateInfo;
        pthread_mutex_unlock(&m_impl->mutex);
    }
    return info;
}

void UpdateManager::getDownloadProgress(uint64_t& outDownloaded, uint64_t& outTotal, float& outFraction) const {
    if (!m_impl) {
        outDownloaded = 0;
        outTotal = 0;
        outFraction = 0.0f;
        return;
    }
    outDownloaded = m_impl->downloadedBytes.load(std::memory_order_relaxed);
    outTotal = m_impl->totalBytes.load(std::memory_order_relaxed);
    if (outTotal > 0) {
        outFraction = static_cast<float>(outDownloaded) / static_cast<float>(outTotal);
        if (outFraction > 1.0f) outFraction = 1.0f;
    } else {
        outFraction = 0.0f;
    }
}

void UpdateManager::reset() {
    if (m_impl) {
        m_impl->status.store(UpdateStatus::Idle, std::memory_order_release);
    }
}

bool UpdateManager::isNewerVersion(const std::string& candidate, const std::string& current) {
    SemVer cand = SemVer::parse(candidate);
    SemVer curr = SemVer::parse(current);
    return cand > curr;
}

} // namespace ml
