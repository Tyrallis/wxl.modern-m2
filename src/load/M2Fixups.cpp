// Native modern-M2 reader: in-place field deltas (sequences/materials) and post-fixup name injections.
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

// The in-place deltas run on the RAW body (array offsets still body-relative), before the client's
// parse; the injections run after it, at WXL_M2FILE_PARSED, where an array's offset is relative to
// the array itself (x64 port: upstream's ran after its own walk, on raw 32-bit pointers). Each is a
// field the source model states differently from the target -- a value rewritten, never dropped.

#include "../ExtensionApi.hpp"
#include "../fdid/Fdid.hpp"
#include "M2NativeInternal.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace fmt = wxl::structure::m2;

namespace wxl::runtime::m2native::detail
{
    namespace
    {
        /// Finds the index of the first sequence whose id equals anim (Animations.cpp semantics).
        int16_t SequenceIndexById(const fmt::M2Sequence* seqs, uint32_t count, uint16_t anim)
        {
            for (uint32_t i = 0; i < count; ++i)
                if (seqs[i].id == anim) return static_cast<int16_t>(i);
            return -1;
        }

        /// Points lookup[newId] at newPos if it still holds oldPos, else rewrites the first entry holding
        /// oldPos (Animations.cpp semantics).
        void PatchSequenceLookup(int16_t* lookup, uint32_t lookupCount, int16_t oldPos, uint16_t newId,
                                 int16_t newPos)
        {
            if (!lookup) return;
            if (newId < lookupCount && lookup[newId] == oldPos) { lookup[newId] = newPos; return; }
            for (uint32_t i = 0; i < lookupCount; ++i)
                if (lookup[i] == oldPos) { lookup[i] = newPos; break; }
        }
    }

    /**
     * @brief Sequence deltas in place: masks the split u16 in|out blendTime to the client's single u32
     *        (read whole it is a huge blend so transitions never complete) and remaps the curated set of
     *        source ids above the client's id table onto client ids, patching the lookup. Counts the
     *        sequences whose data streams from a .anim file (flags bit 0x20 clear -- Phase 2) into
     *        extSeqPending.
     */
    void FixSequencesRaw(uint8_t* base, uint32_t size, fmt::M2Header* h, uint32_t& extSeqPending)
    {
        constexpr uint16_t kClientMaxAnimId = 505;
        if (!h->sequences.count || !h->sequences.offset) return;
        if (h->sequences.offset > size ||
            h->sequences.count * sizeof(fmt::M2Sequence) > size - h->sequences.offset)
            return; // walk will reject it
        auto* seqs = reinterpret_cast<fmt::M2Sequence*>(base + h->sequences.offset);

        int16_t* lookup = nullptr;
        uint32_t lookupCount = 0;
        if (h->sequenceLookup.count && h->sequenceLookup.offset &&
            h->sequenceLookup.offset <= size &&
            h->sequenceLookup.count * 2 <= size - h->sequenceLookup.offset)
        {
            lookup      = reinterpret_cast<int16_t*>(base + h->sequenceLookup.offset);
            lookupCount = h->sequenceLookup.count;
        }

        for (uint32_t i = 0; i < h->sequences.count; ++i)
        {
            const uint16_t id = seqs[i].id;
            if (id > kClientMaxAnimId)
            {
                uint16_t anim = id;
                switch (id)
                {
                    case 564: anim = 37;  break;
                    case 548: anim = 41;  break;
                    case 556: anim = 42;  break;
                    case 552: anim = 43;  break;
                    case 554: anim = 44;  break;
                    case 562: anim = 45;  break;
                    case 572: anim = 39;  break;
                    case 574: anim = 187; break;
                }
                if (anim != id)
                {
                    PatchSequenceLookup(lookup, lookupCount,
                                        SequenceIndexById(seqs, h->sequences.count, anim), anim,
                                        static_cast<int16_t>(i));
                    seqs[i].id = anim;
                }
            }
            seqs[i].blendTime &= 0xFFFFu;
            if (!(seqs[i].flags & 0x20u)) ++extSeqPending;
        }
    }

    /**
     * @brief Material deltas in place: a blend mode above the client's 7-entry blend table is clamped to
     *        Add (4, flags forced |0x5) and flags are masked to the low 5 bits -- the same FixRenderFlags
     *        contract the skin-finalize half re-applies idempotently.
     */
    void FixMaterialsRaw(uint8_t* base, uint32_t size, fmt::M2Header* h)
    {
        if (!h->materials.count || !h->materials.offset) return;
        if (h->materials.offset > size || h->materials.count * 4 > size - h->materials.offset)
            return;
        auto* mats = reinterpret_cast<uint16_t*>(base + h->materials.offset);
        for (uint32_t i = 0; i < h->materials.count; ++i)
        {
            uint16_t& flag  = mats[i * 2 + 0];
            uint16_t& blend = mats[i * 2 + 1];
            if (blend > 6) { blend = 4; flag |= 0x5; }
            flag &= 0x1F;
        }
    }

    /**
     * @brief Points each hardcoded (type 0) texture with no inline name at its TXID-resolved client path.
     *        The header is parsed: the name is copied into memory the texture's filename array can
     *        reach and the array pointed at it. The client's Initialize then creates a texture for
     *        exactly that path; an unresolved id keeps count 0 and falls back to the stock solid-white
     *        placeholder.
     */
    void InjectTxidNames(const char* modelPath, fmt::M2Header* h, const uint32_t* txid, uint32_t txidCount,
                         Outcome& out)
    {
        auto* tex = wxl_modern_m2::Ptr<fmt::M2Texture>(h->textures);
        if (!tex) return;
        for (uint32_t i = 0; i < h->textures.count; ++i)
        {
            if (tex[i].type != fmt::kTexTypeHardcoded) continue;   // dynamic slots stay empty
            if (tex[i].filename.count >= 2) continue;               // inline name kept
            const uint32_t fdid = i < txidCount ? txid[i] : 0;
            if (!fdid) continue;
            const std::string path = wxl_modern_m2::ResolveTexture(modelPath, fdid);
            if (path.empty())
            {
                ++out.texUnresolved;
                WLOG_WARN("m2native: TXID %u unresolved (texture %u) -- solid white fallback", fdid, i);
                continue;
            }
            if (!wxl_modern_m2::Replace(tex[i].filename, path.c_str(), path.size() + 1))
            {
                ++out.texUnresolved;
                WLOG_WARN("m2native: TXID %u: no memory in reach for its name -- solid white fallback", fdid);
                continue;
            }
            ++out.texResolved;
        }
    }

    /**
     * @brief Makes the header's textureCombinerCombos field (0x130) safe to write.
     *
     * x64 port fix (upstream has the same overlap): the header ends at 0x130 unless global flag 0x8
     * says it carries that field, and a file without the flag can keep data right there - the
     * global-loop lengths of creature\\infernal2, say. The skin rebuild then points the field at its
     * combos and writes over those lengths, and every global-loop animation (scrolling textures,
     * particles) runs past its keys and freezes. Any array whose data lies in those 8 bytes is moved
     * to memory of its own when its records hold no arrays of their own (moving one that does would
     * break their self-relative offsets); the field is then zeroed and the flag set.
     * @return true when the field can be written; false leaves the header as it was.
     */
    bool FreeCombinerField(fmt::M2Header* h)
    {
        if (h->globalFlags & fmt::kFlagUseTextureCombinerCombos) return true;
        const uint8_t* lo = h->base() + offsetof(fmt::M2Header, textureCombinerCombos);
        const uint8_t* hi = lo + sizeof(fmt::M2Array);

        struct Field { size_t at; uint32_t stride; bool flat; };
        static const Field kFields[] = {
            { offsetof(fmt::M2Header, name), 1, true },
            { offsetof(fmt::M2Header, globalLoops), 4, true },
            { offsetof(fmt::M2Header, sequences), 0x40, true },
            { offsetof(fmt::M2Header, sequenceLookup), 2, true },
            { offsetof(fmt::M2Header, bones), 0x58, false },
            { offsetof(fmt::M2Header, boneLookup), 2, true },
            { offsetof(fmt::M2Header, vertices), 0x30, true },
            { offsetof(fmt::M2Header, colors), 0x28, false },
            { offsetof(fmt::M2Header, textures), 0x10, false },
            { offsetof(fmt::M2Header, textureWeights), 0x14, false },
            { offsetof(fmt::M2Header, textureTransforms), 0x3C, false },
            { offsetof(fmt::M2Header, textureReplacements), 2, true },
            { offsetof(fmt::M2Header, materials), 4, true },
            { offsetof(fmt::M2Header, boneCombos), 2, true },
            { offsetof(fmt::M2Header, textureCombos), 2, true },
            { offsetof(fmt::M2Header, textureUnitLookup), 2, true },
            { offsetof(fmt::M2Header, textureWeightCombos), 2, true },
            { offsetof(fmt::M2Header, textureTransformCombos), 2, true },
            { offsetof(fmt::M2Header, collisionIndices), 2, true },
            { offsetof(fmt::M2Header, collisionPositions), 12, true },
            { offsetof(fmt::M2Header, collisionNormals), 12, true },
            { offsetof(fmt::M2Header, attachments), 0x28, false },
            { offsetof(fmt::M2Header, attachmentLookup), 2, true },
            { offsetof(fmt::M2Header, events), 0x24, false },
            { offsetof(fmt::M2Header, lights), 0x9C, false },
            { offsetof(fmt::M2Header, cameras), 0x64, false },
            { offsetof(fmt::M2Header, cameraLookup), 2, true },
            { offsetof(fmt::M2Header, ribbonEmitters), 0xB0, false },
            { offsetof(fmt::M2Header, particleEmitters), 0x1DC, false },
        };
        for (const Field& f : kFields)
        {
            auto& a = *reinterpret_cast<fmt::M2Array*>(h->base() + f.at);
            const uint8_t* data = wxl_modern_m2::Ptr<uint8_t>(a);
            if (!data) continue;
            const uint8_t* end = data + static_cast<size_t>(a.count) * f.stride;
            if (end <= lo || data >= hi) continue;
            const uint32_t count = a.count;
            if (!f.flat || !wxl_modern_m2::Replace(a, data, static_cast<size_t>(count) * f.stride))
            {
                WLOG_WARN("m2native: header field 0x%X overlaps the combiner-combo slot and cannot move: "
                          "two-texture batches keep the client's own decode", static_cast<uint32_t>(f.at));
                return false;
            }
            a.count = count;                                              // Replace counted bytes
        }
        h->textureCombinerCombos = fmt::M2Array{ 0, 0 };
        h->globalFlags |= fmt::kFlagUseTextureCombinerCombos;
        return true;
    }

    /// Drops each camera reference that points past the camera array. The source exporter already emits
    /// the "no camera" sentinel for unused slots, so a consumer reads a dropped entry the same way.
    void ClampCameraRefs(fmt::M2Header* h)
    {
        auto* lookup = wxl_modern_m2::Ptr<int16_t>(h->cameraLookup);
        if (!lookup) return;
        const auto limit = static_cast<int32_t>(h->cameras.count);
        for (uint32_t i = 0; i < h->cameraLookup.count; ++i)
            if (lookup[i] >= limit) lookup[i] = -1;
    }

    /// Clamps each ribbon's texture/material reference values into the header tables: the source
    /// exporter can emit indices past what the model itself declares.
    void ClampRibbonRefs(fmt::M2Header* h)
    {
        auto* rib = wxl_modern_m2::Ptr<fmt::M2Ribbon>(h->ribbonEmitters);
        if (!rib) return;
        for (uint32_t i = 0; i < h->ribbonEmitters.count; ++i)
        {
            auto clamp = [](fmt::M2Array& a, uint32_t limit) {
                auto* v = wxl_modern_m2::Ptr<uint16_t>(a);
                if (!v || !limit) return;
                for (uint32_t n = 0; n < a.count; ++n)
                    if (v[n] >= limit) v[n] = static_cast<uint16_t>(limit - 1);
            };
            clamp(rib[i].textureIndices, h->textures.count);
            clamp(rib[i].materialIndices, h->materials.count);
        }
    }
}
