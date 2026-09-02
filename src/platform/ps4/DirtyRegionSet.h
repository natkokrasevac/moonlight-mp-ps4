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

// === DirtyRegionSet.h ========================================================
//
// OpenOrbis PS4 — Minimal dirty-region accumulator for the CPU UI compositor.
//
// Copied unchanged from the SCE reference (moonlight-ps4-SCE-RC).
//
// This is a tiny, self-contained helper. It accumulates a small fixed-capacity
// set of dirty rectangles for the region-limited recomposition / partial-upload
// model. It is deliberately NOT a generic region library: no heap allocation,
// no optimal polygon/region decomposition, no SIMD.
//
// Responsibilities:
//   - Hold a small fixed array of dirty rectangles (8–16).
//   - Normalize / clip each added rectangle to the surface bounds.
//   - Discard empty rectangles.
//   - Merge overlapping / directly-adjacent rectangles into their bounding box.
//   - Fall back to a single full-surface rectangle on capacity overflow.
//
// Coordinates are integer pixel coordinates (half-open: [x, x+w) × [y, y+h)).
// No GNM/GNMX types are exposed.
// =============================================================================

#ifndef PS4_DIRTY_REGION_SET_H
#define PS4_DIRTY_REGION_SET_H

#include <cstdint>

#include "UiCompositor.h"   // For ps4::Rect.

namespace ps4 {

/// A small fixed-capacity accumulator of dirty rectangles.
///
/// Main-thread owned. Used by the dirty-region recomposition model to track
/// which regions of the persistent CPU surface changed and must be recomposed
/// and re-uploaded. Correctness is prioritized over minimality: on capacity
/// overflow the set collapses to a single full-surface rectangle.
class DirtyRegionSet {
public:
    /// Fixed capacity of the dirty-rectangle array. No heap allocation.
    static constexpr int kCapacity = 12;

    DirtyRegionSet() = default;

    /// Remove all dirty rectangles.
    void clear() { m_count = 0; }

    /// Returns true when no dirty rectangles are pending.
    bool empty() const { return m_count == 0; }

    /// Returns the number of dirty rectangles currently held.
    int count() const { return m_count; }

    /// Returns the i-th dirty rectangle. Valid for i in [0, count()).
    const Rect& rect(int i) const { return m_rects[i]; }

    /// Add a rectangle to the dirty set. The rectangle is normalized, clipped
    /// to the surface bounds, and discarded if empty. Overlapping or directly
    /// adjacent rectangles are merged into their bounding box. On capacity
    /// overflow the set collapses to a single full-surface rectangle.
    void add(const Rect& r, int surfaceW, int surfaceH) {
        // Normalize (handle negative w/h) and clip to the surface.
        Rect clipped = normalizeAndClip(r, surfaceW, surfaceH);
        if (clipped.w <= 0 || clipped.h <= 0) {
            return;   // Empty after clipping; discard.
        }

        // If we already hold a full-surface rectangle, nothing more to do.
        if (m_count == 1 && isFullSurface(m_rects[0], surfaceW, surfaceH)) {
            return;
        }

        // Try to merge with an existing rectangle.
        for (int i = 0; i < m_count; ++i) {
            if (shouldMerge(m_rects[i], clipped)) {
                m_rects[i] = boundingBox(m_rects[i], clipped);
                return;
            }
        }

        // No merge; append if capacity allows.
        if (m_count < kCapacity) {
            m_rects[m_count] = clipped;
            ++m_count;
            return;
        }

        // Capacity overflow: collapse to a single full-surface rectangle.
        m_count = 1;
        m_rects[0] = Rect{0, 0, surfaceW, surfaceH};
    }

    /// Add a full-surface rectangle (used for explicit full redraws and
    /// fallback recovery).
    void addFullSurface(int surfaceW, int surfaceH) {
        m_count = 1;
        m_rects[0] = Rect{0, 0, surfaceW, surfaceH};
    }

    /// Compute the total dirty pixel area across all held rectangles.
    /// Used for diagnostics.
    uint64_t totalPixelArea() const {
        uint64_t area = 0;
        for (int i = 0; i < m_count; ++i) {
            area += static_cast<uint64_t>(m_rects[i].w) *
                    static_cast<uint64_t>(m_rects[i].h);
        }
        return area;
    }

private:
    /// Normalize a rectangle (handle negative w/h) and clip it to the surface
    /// bounds. Returns an empty rectangle (w<=0 or h<=0) if nothing remains.
    static Rect normalizeAndClip(const Rect& in, int surfaceW, int surfaceH) {
        int x0 = in.x;
        int y0 = in.y;
        int x1 = in.x + in.w;
        int y1 = in.y + in.h;
        if (x1 < x0) { int t = x0; x0 = x1; x1 = t; }
        if (y1 < y0) { int t = y0; y0 = y1; y1 = t; }

        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > surfaceW) x1 = surfaceW;
        if (y1 > surfaceH) y1 = surfaceH;

        return Rect{x0, y0, x1 - x0, y1 - y0};
    }

    /// Returns true if `a` and `b` overlap or are directly adjacent (within a
    /// small threshold). A conservative merge policy: rectangles that touch or
    /// nearly touch are merged to avoid many tiny fragments.
    static bool shouldMerge(const Rect& a, const Rect& b) {
        constexpr int kMergeThreshold = 2;   // px of allowed gap between rects.
        const int ax0 = a.x, ay0 = a.y, ax1 = a.x + a.w, ay1 = a.y + a.h;
        const int bx0 = b.x, by0 = b.y, bx1 = b.x + b.w, by1 = b.y + b.h;

        // Overlap test (with a small expansion for adjacency).
        const bool overlapX = (ax0 - kMergeThreshold <= bx1) &&
                              (bx0 - kMergeThreshold <= ax1);
        const bool overlapY = (ay0 - kMergeThreshold <= by1) &&
                              (by0 - kMergeThreshold <= ay1);
        return overlapX && overlapY;
    }

    /// Compute the bounding box of two rectangles.
    static Rect boundingBox(const Rect& a, const Rect& b) {
        const int x0 = a.x < b.x ? a.x : b.x;
        const int y0 = a.y < b.y ? a.y : b.y;
        const int x1 = (a.x + a.w) > (b.x + b.w) ? (a.x + a.w) : (b.x + b.w);
        const int y1 = (a.y + a.h) > (b.y + b.h) ? (a.y + a.h) : (b.y + b.h);
        return Rect{x0, y0, x1 - x0, y1 - y0};
    }

    /// Returns true if `r` covers the entire surface.
    static bool isFullSurface(const Rect& r, int surfaceW, int surfaceH) {
        return r.x <= 0 && r.y <= 0 &&
               (r.x + r.w) >= surfaceW && (r.y + r.h) >= surfaceH;
    }

    Rect m_rects[kCapacity];
    int m_count = 0;
};

} // namespace ps4

#endif // PS4_DIRTY_REGION_SET_H
