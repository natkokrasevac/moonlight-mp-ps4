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

// === StoragePaths.h =========================================================
//
// Moonlight client for PlayStation 4 — Centralized storage paths.
//
// Centralizes the filesystem paths used for persistent application data on the
// PlayStation 4 hard drive under /data/. Moonlight MP uses its own dedicated
// directory (/data/moonlight-mp/) to prevent collisions with the original
// Moonlight DX installation (/data/moonlight/).
// =============================================================================

#ifndef MOONLIGHT_STORAGE_PATHS_H
#define MOONLIGHT_STORAGE_PATHS_H

namespace ml {
namespace storage {

/// Primary writable data directory for Moonlight MP.
constexpr const char* kStorageDir = "/data/moonlight-mp";

/// Legacy data directory used by the original Moonlight DX.
constexpr const char* kLegacyStorageDir = "/data/moonlight";

/// Full paths for Moonlight MP persistent data.
constexpr const char* kIdentityFilePath = "/data/moonlight-mp/identity.bin";
constexpr const char* kTrustedHostsFilePath = "/data/moonlight-mp/trusted_hosts.bin";
constexpr const char* kSettingsFilePath = "/data/moonlight-mp/settings.ini";
constexpr const char* kSettingsTempPath = "/data/moonlight-mp/settings.tmp";
constexpr const char* kArtworkRootDir = "/data/moonlight-mp/artwork";

/// Legacy paths for migration import.
constexpr const char* kLegacyIdentityFilePath = "/data/moonlight/identity.bin";
constexpr const char* kLegacyTrustedHostsFilePath = "/data/moonlight/trusted_hosts.bin";
constexpr const char* kLegacySettingsFilePath = "/data/moonlight/settings.ini";

} // namespace storage
} // namespace ml

#endif // MOONLIGHT_STORAGE_PATHS_H
