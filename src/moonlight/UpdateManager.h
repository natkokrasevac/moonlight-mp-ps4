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

// === UpdateManager.h =========================================================
//
// OpenOrbis Moonlight client — Auto-Update Subsystem
//
// Checks for new releases on GitHub (natkokrasevac/moonlight-mp-ps4), streams
// PKG assets directly to /data/pkg/, and initiates installation on PS4.
//
// Threading: Performs HTTP requests on background worker threads using POSIX
// pthreads and reports thread-safe status updates to the main thread.
// =============================================================================

#ifndef MOONLIGHT_UPDATE_MANAGER_H
#define MOONLIGHT_UPDATE_MANAGER_H

#include <cstdint>
#include <string>
#include <atomic>

namespace ml {

// ---------------------------------------------------------------------------
// Update Status
// ---------------------------------------------------------------------------

enum class UpdateStatus : uint8_t {
    Idle,
    Checking,
    UpdateAvailable,
    UpToDate,
    CheckFailed,
    Downloading,
    Downloaded,
    Installing,
    InstallStarted,
    InstallFailed,
    DownloadFailed,
    Cancelled
};

// ---------------------------------------------------------------------------
// Release / PKG metadata
// ---------------------------------------------------------------------------

struct UpdateInfo {
    std::string tagName;         ///< e.g. "v3.11"
    std::string releaseName;     ///< e.g. "Moonlight MP for PS4 v3.11"
    std::string pkgName;         ///< e.g. "moonlight-mp-ps4_3.11.pkg"
    std::string downloadUrl;     ///< GitHub asset download URL
    uint64_t    pkgSize = 0;     ///< File size in bytes
    std::string localPkgPath;    ///< Target path on PS4, e.g. "/data/pkg/moonlight-mp-ps4_3.11.pkg"
};

// ---------------------------------------------------------------------------
// UpdateManager
// ---------------------------------------------------------------------------

class UpdateManager {
public:
    UpdateManager();
    ~UpdateManager();

    // Non-copyable, non-movable
    UpdateManager(const UpdateManager&) = delete;
    UpdateManager& operator=(const UpdateManager&) = delete;
    UpdateManager(UpdateManager&&) = delete;
    UpdateManager& operator=(UpdateManager&&) = delete;

    /// Initialize the update manager and its underlying HTTP client.
    bool initialize();

    /// Terminate active background operations and release resources.
    void shutdown();

    /// Returns true if initialized and ready.
    bool isInitialized() const;

    /// Begin asynchronous check against GitHub Releases API.
    /// Non-blocking; status transitions from Checking -> UpdateAvailable / UpToDate / CheckFailed.
    void checkForUpdatesAsync();

    /// Begin asynchronous download of the latest PKG asset.
    /// Non-blocking; status transitions from Downloading -> Downloaded / InstallStarted / DownloadFailed.
    void startDownloadAsync();

    /// Cancel active check or download operation.
    void cancel();

    /// Retrieve the current update manager status. Safe to call from any thread.
    UpdateStatus status() const;

    /// Retrieve the parsed release info (valid when status is UpdateAvailable, Downloaded, etc.).
    UpdateInfo updateInfo() const;

    /// Retrieve download progress: downloaded bytes, total bytes, and fraction (0.0f..1.0f).
    void getDownloadProgress(uint64_t& outDownloaded, uint64_t& outTotal, float& outFraction) const;

    /// Reset status back to Idle (e.g. after user dismisses notification or error).
    void reset();

    /// Helper to compare two version strings (e.g. "v3.11" vs "3.10").
    /// Returns true if candidate is newer than current.
    static bool isNewerVersion(const std::string& candidate, const std::string& current);

private:
    struct Impl;
    Impl* m_impl;
};

} // namespace ml

#endif // MOONLIGHT_UPDATE_MANAGER_H
