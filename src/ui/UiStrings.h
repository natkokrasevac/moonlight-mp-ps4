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

// === UiStrings.h ============================================================
//
// Moonlight client for PlayStation 4 — shared static UI caption strings.
//
// This header centralizes the genuinely static UI vocabulary (captions, action
// labels, dialog buttons, section headers, and statistics row labels) used by
// the dedicated CPU renderers. It is deliberately NOT a localization framework
// and does NOT own application-state text.
//
// This header exposes only project-owned types. No OpenOrbis SDK types appear here.
// =============================================================================

#ifndef UI_UI_STRINGS_H
#define UI_UI_STRINGS_H

namespace ui {

// ---------------------------------------------------------------------------
// Static UI caption strings
// ---------------------------------------------------------------------------

namespace UiStrings {

// --- Navigation / action labels -------------------------------------------
/// Accept (legend accept action). Used by HostList and HostGrid legends.
constexpr const char* kAccept = "Connect";
/// Delete Host (legend delete action). Used by HostList and HostGrid legends.
constexpr const char* kDeleteHost = "Unpair";
/// Add Host Manually (legend manual-add action). Used by HostList and HostGrid
/// legends.
constexpr const char* kAddHostManually = "Add Host Manually";
/// Settings (legend settings action / panel title). Used by HostList, HostGrid,
/// AppList, AppGrid legends and the Settings panel title.
constexpr const char* kSettings = "Settings";
/// Launch (legend launch action / focused-card footer). Used by AppList and
/// AppGrid legends and the AppList focused-card footer.
constexpr const char* kLaunch = "Launch";
/// Resume (legend accept action / focused-card footer). Used by AppList and
/// AppGrid when the focused app is already running.
constexpr const char* kResume = "Resume";
/// Back to Hosts (legend back action). Used by AppList and AppGrid legends.
constexpr const char* kBackToHosts = "Back to Hosts";
/// Quit App (legend quit action). Used by AppList and AppGrid legends.
constexpr const char* kQuitApp = "Quit App";
/// Quit App confirmation dialog message (Triangle quit).
constexpr const char* kQuitCurrentApp = "Quit current app?";
/// Quit App confirmation dialog message (quit-and-launch).
constexpr const char* kQuitAndLaunch = "Quit running app and launch this one?";
/// Cancel (cancel action). Used by AddHost (form + status modal) and the
/// Pairing legend.
constexpr const char* kCancel = "Cancel";
/// Connect (focused-card footer). Used by the HostList focused-card footer.
constexpr const char* kConnect = "Connect";
/// Exit (in-stream exit hint). Used by the StreamExitHint overlay.
constexpr const char* kExit = "Exit";
/// Stream Exit (in-stream exit hint label). Used by the StreamExitHint overlay.
constexpr const char* kStreamExit = "Exit";
/// Mouse Assist (in-stream exit hint label). Used by the StreamExitHint
/// overlay for the mouse-emulation toggle chord.
constexpr const char* kMouseAssist = "Mouse Emulation";
/// Mouse Emulation (in-stream mouse emulation state overlay label). Used by
/// the MouseEmulationOverlayRenderer for the mouse-emulation state caption.
constexpr const char* kMouseEmulation = "Mouse Emulation";
/// On (binary state value). Used by the MouseEmulationOverlayRenderer for the
/// mouse-emulation ON state.
constexpr const char* kOn = "ON";
/// Off (binary state value). Used by the MouseEmulationOverlayRenderer for the
/// mouse-emulation OFF state.
constexpr const char* kOff = "OFF";

// --- Dialog buttons --------------------------------------------------------
/// Yes (confirmation modal choice). Used by the Delete Host modal.
constexpr const char* kYes = "Yes";
/// No (confirmation modal choice). Used by the Delete Host modal.
constexpr const char* kNo = "No";
/// OK (dismissal button). Used by the Different-app-running modal and the
/// Add Host terminal status modal.
constexpr const char* kOk = "OK";
/// Add (form submit button). Used by the Add Host form overlay.
constexpr const char* kAdd = "Add";
/// Add Host (panel title). Used by the Add Host form overlay and status modal.
constexpr const char* kAddHost = "Add Host";
/// Pairing (screen title). Used by the Pairing screen panel.
constexpr const char* kPairing = "Pairing";
/// Help (screen title / legend label). Used by the Help screen title and the
/// Host/App bottom-legend Help entry.
constexpr const char* kHelp = "Help";
/// Close (legend action). Used by the Help screen bottom legend (OK button).
constexpr const char* kClose = "Back";
/// Reset Moonlight (legend action / modal title). Used by the Help screen
/// bottom legend (Triangle button) and the Reset Moonlight confirmation modal.
constexpr const char* kResetMoonlight = "Reset Moonlight MP";
/// Reset Moonlight warning (modal message line 1). Used by the Reset Moonlight
/// confirmation modal.
constexpr const char* kResetMoonlightWarning = "All settings and pairings will be lost when resetting.";
/// Reset Moonlight confirm (modal message line 2). Used by the Reset Moonlight
/// confirmation modal.
constexpr const char* kResetMoonlightConfirm = "Do you really want to reset Moonlight MP?";

// --- Help screen content ---------------------------------------------------
/// In-stream button assignments (Help section header). Used by the Help screen.
constexpr const char* kHelpInStreamAssignments = "IN-STREAM BUTTON ASSIGNMENTS";
/// PS/Share reserved note (Help note). Used by the Help screen.
constexpr const char* kHelpPsShareReserved = "PS and Share buttons on the controller cannot be mapped and used in-stream as they are OS reserved.";
/// Exit stream (Help instruction). Used by the Help screen.
constexpr const char* kHelpExitStream = "To exit stream, press [Options] + [L3]";
/// PS button (Help instruction). Used by the Help screen.
constexpr const char* kHelpPsButton = "For [PS] button, press [Options]";
/// SELECT / START buttons (Help instruction). Used by the Help screen.
constexpr const char* kHelpSelectStart = "For SELECT / START, press [TouchpadL] / [TouchpadR]";
/// Mouse emulation toggle (Help instruction). Used by the Help screen.
constexpr const char* kHelpMouseEmulation = "Toggle Mouse Emulation On/Off with [Options] + [Touchpad]";
/// Mouse buttons (Help instruction). Used by the Help screen.
constexpr const char* kHelpMouseButtons = "Mouse buttons are [Cross] (Left) and [Circle] (Right)";
/// Keyboard/mouse passthrough (Help note). Used by the Help screen.
constexpr const char* kHelpKeyboardMouse = "Keyboards and Mice attached to PlayStation 4 are passed through, tested with USB and Bluetooth devices.";
/// Bluetooth mouse note (Help note). Used by the Help screen.
constexpr const char* kHelpBluetoothNote = "Note: Most modern Bluetooth mice will not pair with PlayStation 4. See my keyparty project on fourletterworkshop.com";
/// Settings saved (Help note). Used by the Help screen.
constexpr const char* kHelpSettingsSaved = "Settings are saved when settings overlay is closed.";
/// VSYNC / Frame Pacing / Pro support (Help note). Used by the Help screen.
constexpr const char* kHelpVsyncFramePacing = "There are currently no settings for VSYNC (ON) or Frame Pacing (OFF). PlayStation 4 Pro h265/4K support is planned.";
/// More details (Help footer). Used by the Help screen.
constexpr const char* kHelpMoreDetails = "Thank you to the OpenOrbis and Moonlight projects.";
/// Copyright/license note (Help footer). Used by the Help screen.
constexpr const char* kHelpCopyright = "Copyright (C) 2006 fourletterworkshop.com. Released under the GNU General Public License v3.0. This program comes with ABSOLUTELY NO WARRANTY.";

// --- Settings section headers ----------------------------------------------
/// STREAM (settings section header). Used by the Settings panel.
constexpr const char* kStreamSection = "STREAM";
/// CLIENT (settings section header). Used by the Settings panel.
constexpr const char* kClientSection = "CLIENT";
/// HOST (settings section header). Used by the Settings panel.
constexpr const char* kHostSection = "HOST";
/// Planned (disabled-row value). Used by the Settings panel.
constexpr const char* kPlanned = "Planned";

// --- Settings footer --------------------------------------------------------
/// App name (Settings footer). Used by the Settings panel footer.
constexpr const char* kAppName = "Moonlight MP for PlayStation 4";
/// Version label (Settings footer). Used by the Settings panel footer.
constexpr const char* kVersionLabel = "fourletterworkshop.com";
/// Version number (Settings footer). Used by the Settings panel footer.
constexpr const char* kVersionNumber = "2.2";

// --- Stream statistics row labels ------------------------------------------
/// Resolution (statistics row label). Used by the StreamStatistics overlay.
constexpr const char* kStatResolution = "Stream Resolution";
/// FPS (statistics row label). Used by the StreamStatistics overlay.
constexpr const char* kStatFps = "Stream FPS";
/// Bitrate (statistics row label). Used by the StreamStatistics overlay.
constexpr const char* kStatBitrate = "Stream Bitrate";
/// Codec (statistics row label). Used by the StreamStatistics overlay.
constexpr const char* kStatCodec = "Codec";
/// Decode (statistics row label). Used by the StreamStatistics overlay.
constexpr const char* kStatDecode = "Decode";
/// Network (statistics row label). Used by the StreamStatistics overlay.
constexpr const char* kStatNetwork = "Network";
/// Jitter (statistics row label). Used by the StreamStatistics overlay.
constexpr const char* kStatJitter = "Jitter";
/// Frame Decoding (statistics row label). Used by the StreamStatistics overlay.
constexpr const char* kStatFrameDecode = "Frame Decoding";
/// Frame Presentation (statistics row label). Used by the StreamStatistics overlay.
constexpr const char* kStatFramePresent = "Frame Presentation";
/// Time per Frame (statistics row label). Used by the StreamStatistics overlay.
constexpr const char* kStatFrameTotal = "Time per Frame";

} // namespace UiStrings

} // namespace ui

#endif // UI_UI_STRINGS_H
