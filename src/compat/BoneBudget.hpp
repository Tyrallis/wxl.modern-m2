// Bone budget: partition a submesh whose per-draw bone palette exceeds the client ceiling into
// sub-sections, and re-point batches across the resulting sub-section run.
// Copyright (C) 2026 WarcraftXL
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#pragma once

#include <cstdint>
#include <vector>

#include "../M2Format.hpp"

/**
 * @brief Partitions any drawn submesh whose per-draw bone palette exceeds the client ceiling into
 *        sub-sections sized within the ceiling.
 *
 * The client vertex shader holds the bone palette from register 31, 3 registers per matrix, so
 * (256 - 31) / 3 = 75 bones per draw is the ceiling. This is a hard client-engine constraint on
 * the M2Header/M2SkinProfile client contract, not a modern-M2-source concern, so it applies
 * unconditionally at every skin finalize regardless of which pipeline produced the model (native,
 * a downported modern M2, or a client M2 another format baked, e.g. m3's M2Build). A greedy
 * triangle bin-packer splits each over-ceiling submesh into sub-sections, each with its own
 * compact boneCombos slice, deduplicated vertex block (bones[] remapped to the slice), and
 * global-indexed triangle block, rebuilding the live skin geometry arrays and header.boneCombos.
 * RepointBatchesAfterSplit re-points a model's EXISTING, already-valid batches across the run of
 * sub-sections their original submesh became; a caller that also needs to reinterpret batch
 * contents (e.g. a modern-M2 source's packed shaderId) does that re-pointing itself instead, using
 * the same SplitRun map.
 */
namespace wxl::modern::assets::common::bones
{
    /**
     * @brief A parsed skin profile, as upstream's wxl::game::m2::M2SkinProfile spelled it: each array
     *        as a pointer and a count.
     *
     * x64 port: the client's profile keeps each array as a self-relative 32-bit offset, so the
     * functions below read the pointers off the header (the constructor), work on them as upstream
     * does, and point the header at what they replaced (Commit). Replacement memory comes from
     * NewArray, which is within reach of the header's array.
     */
    struct LiveSkin
    {
        wxl::structure::m2::M2SkinHeader*  header = nullptr;
        uint16_t*                          vertexLookup = nullptr;
        uint32_t                           vertexCount = 0;
        uint16_t*                          indices = nullptr;
        uint32_t                           indexCount = 0;
        uint8_t*                           bones = nullptr;
        uint32_t                           boneCount = 0;
        wxl::structure::m2::M2SkinSection* submeshes = nullptr;
        uint32_t                           submeshCount = 0;
        wxl::structure::m2::M2Batch*       batches = nullptr;
        uint32_t                           batchCount = 0;

        explicit LiveSkin(wxl::structure::m2::M2SkinHeader* h);

        /// Points the header's arrays at what this holds now. False when one is out of reach.
        bool Commit();
    };

    /// A copy of `bytes` in memory the skin header's array `array` can point at; null when none.
    void* NewArray(wxl::structure::m2::M2Array& array, const void* data, size_t bytes);

    // The client per-draw bone budget. A submesh past it is split (or, when a split is skipped, clamped).
    constexpr uint16_t kMaxBonesPerDraw = 75;
    // A submesh / batch count past this is treated as malformed; the commit is capped well under any value
    // that would overflow the native sizing.
    constexpr uint32_t kMaxBatches = 0x4000;

    /** @brief One rebuilt sub-section plus the original submesh it came from. */
    struct SplitSection { wxl::structure::m2::M2SkinSection section; uint16_t origSubmesh; };
    /** @brief The contiguous run of new sub-section indices one original submesh became. */
    struct SplitRun { uint16_t first; uint16_t count; };

    /**
     * @brief Whether a submesh's level is the high half of its index start rather than a LOD marker.
     *
     * x64 port: upstream decided this by the model's path (character\\ and item\\objectcomponents\\
     * only), and parked a level>0 submesh of any other model -- a creature skin past 65535 indices
     * lost every submesh beyond. The x64 client folds the high half in by the rule wxl-core's
     * m2native-indices used (Engine/Source/Model2/M2Data.h, M2SkinSectionIndexStart): taken only
     * when the window it names lies in the skin's index array. This classification is that rule, so
     * the splitter, the rebuild and the client's draw cannot disagree.
     */
    bool IsWide(const wxl::structure::m2::M2SkinSection& section, const LiveSkin& skin);

    /// A submesh's full index start: (level << 16) | indexStart when IsWide, else indexStart.
    uint32_t FullIndexStart(const wxl::structure::m2::M2SkinSection& section, const LiveSkin& skin);

    /**
     * @brief Partitions over-ceiling submeshes, rebuilding the live skin geometry and header.boneCombos.
     * @param md          Parsed model header (boneCombos array is a raw pointer here).
     * @param skin        Live skin profile whose geometry arrays are rebuilt on success.
     * @param outSections Receives the rebuilt sub-sections.
     * @param splitMap    Receives the per-original-submesh sub-section run, indexed by original submesh.
     * @param splitCount  Receives the count of extra sub-draws produced.
     * @param name        Model path, used for logging.
     * @return true on commit; false (no commit) on any overflow, allocation failure, or missing array, leaving
     *         the caller on the clamp path.
     */
    bool SplitSubmeshes(wxl::structure::m2::M2Header* md, LiveSkin* skin, std::vector<SplitSection>& outSections, std::vector<SplitRun>& splitMap, uint32_t& splitCount, const char* name);

    /**
     * @brief Re-points a skin's existing batches across the sub-section run their original submesh
     *        became, duplicating a batch per extra sub-section without touching any other field.
     *
     * For a caller that also needs to reinterpret batch contents while splitting (a modern-M2
     * source's packed shaderId), do that combined pass instead; this is only for a caller whose
     * batches are already valid client batches and only need re-pointing.
     * @param skin      Live skin profile whose batches are rebuilt in place.
     * @param splitMap  Per-original-submesh sub-section run, as produced by SplitSubmeshes.
     */
    void RepointBatchesAfterSplit(LiveSkin* skin, const std::vector<SplitRun>& splitMap);
}
