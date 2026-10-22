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

// === main.cpp ===============================================================
//
// OpenOrbis Moonlight client — thin entry point.
//
// After the Step-9 coordinator migration, main.cpp is a thin entry point only.
// It constructs the production MoonlightApp composition root, drives its
// lifecycle, and exits. All subsystem ownership, main-loop orchestration, UI
// intent routing, screen/render dispatch, and render-state change tracking
// live in MoonlightApp.
//
// main.cpp no longer owns:
//   subsystem instances;
//   discovery polling;
//   serverinfo scheduling;
//   UI snapshot comparison;
//   intent routing;
//   screen renderer selection;
//   dirty-state tracking;
//   shutdown ordering.
// ===============================================================================

#include <cstdio>

#include <orbis/libkernel.h>   // sceKernelUsleep, sceLibcHeapSize

#include "app/MoonlightApp.h"

// --- PS4 C-HEAP OVERRIDE ------------------------------------------------------
// Tells the PS4 C runtime (libc) to provide a 128 MB memory pool for standard C
// allocation functions (malloc, new). The CPU renderers, compositor, and
// various subsystems allocate transient buffers via malloc; a larger heap
// avoids allocation failures during runtime. The AppArtworkManager decodes
// host-provided artwork PNGs (which may be large) on its worker thread, so
// the heap must accommodate the decoded RGBA buffer in addition to the UI
// surface and other transient allocations.
//
// The OpenOrbis SDK declares this symbol as `extern uint64_t sceLibcHeapSize`
// in <orbis/libkernel.h>; the runtime reads it to size the libc heap. This
// mirrors the SCE reference's `extern "C" size_t sceLibcHeapSize` override.
// -------------------------------------------------------------------------------
extern "C" uint64_t sceLibcHeapSize = 256ull * 1024ull * 1024ull;

int main(void)
{
    // No buffering
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("[main] Moonlight client starting\n");

    // Construct the production composition root.
    MoonlightApp app;

    // Initialize all subsystems in the hardware-proven order.
    if (!app.initialize().succeeded()) {
        printf("[main] MoonlightApp initialize failed\n");
        app.shutdown();
        return 1;
    }

    // Run the main loop until exit is requested.
    app.run();

    // Release all resources in reverse dependency order.
    app.shutdown();

    printf("[main] Moonlight client exited cleanly\n");

    // Keep the process alive after the loop so the last presented frame
    // remains visible on screen.
    for (;;) {
        sceKernelUsleep(1000000);
    }

    return 0;
}

