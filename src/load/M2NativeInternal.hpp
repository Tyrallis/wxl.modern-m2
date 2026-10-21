// Native modern-M2 reader: internal contract shared across the reader's translation units.
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

// The reader is split by load phase -- demux (M2Demux), in-place field deltas + post-parse
// injections (M2Fixups), the per-era record normalization (M2Normalize + the per-record rewrites,
// driven by the table in M2NormalizeLayout) -- all orchestrated by NativeLoad.cpp.
//
// x64 port: upstream also owned the offset->pointer walk (M2WalkOwned / M2WalkLayout), because the
// 32-bit client's parser could not be handed a modern body. The x64 client parses a body that is
// already target-shaped with its own walker, so the walk is the client's and those two files are
// not carried; the injections that ran after upstream's walk run at WXL_M2FILE_PARSED instead, on
// the client's self-relative arrays.

#include "M2NormalizeLayout.hpp"
#include "../M2Format.hpp"

#include <cstdint>
#include <cstring>

namespace wxl::runtime::m2native::detail
{
    inline uint32_t Rd32(const void* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
    inline uint16_t Rd16(const void* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }
    inline void     Wr32(void* p, uint32_t v) { std::memcpy(p, &v, 4); }
    inline void     Wr16(void* p, uint16_t v) { std::memcpy(p, &v, 2); }

    /// True when count records of stride bytes starting at offset all lie inside a body of size bytes.
    inline bool FitsInBody(uint32_t size, uint32_t offset, uint32_t count, uint32_t stride)
    {
        return offset <= size &&
               static_cast<uint64_t>(count) * stride <= static_cast<uint64_t>(size) - offset;
    }

    // skipMask bits reported in the load log.
    enum : uint32_t
    {
        kSkipTxac      = 0x01,
        kSkipLdv1      = 0x02,
        kSkipAfid      = 0x04,
        kSkipSkid      = 0x08,
        kSkipPhysBone  = 0x10, // PFID / BFID (no home in the client's own format, permanently dropped)
        kSkipOther     = 0x80, // any other auxiliary chunk (EXP2, PFDC, ...)
    };

    constexpr uint32_t kMaxTxid = 128;

    /// Everything harvested from the container walk.
    struct Scan
    {
        uint32_t bodyOff;
        uint32_t bodySize;
        uint32_t txid[kMaxTxid];
        uint32_t txidCount;
        uint32_t sfidFirst;
        uint32_t sfidCount;
        uint32_t skipMask;
    };

    /// Outcome of one model's load, for the stats and the log.
    struct Outcome
    {
        int      ok;
        uint32_t version;
        uint32_t texResolved;
        uint32_t texUnresolved;
        uint32_t skipMask;
        uint32_t extSeqPending;
        uint32_t shadowGateForced;
        NormalizeReport normalized;
        const char* fail;
    };

    // ---------------------------------------------------------------- phase entry points
    /// M2Demux: walks the MD21 container.
    bool ScanContainer(const uint8_t* buf, uint32_t size, Scan& s);

    /// M2Fixups: sequence deltas in place on the raw body.
    void FixSequencesRaw(uint8_t* base, uint32_t size, wxl::structure::m2::M2Header* h, uint32_t& extSeqPending);
    /// M2Fixups: material deltas in place on the raw body.
    void FixMaterialsRaw(uint8_t* base, uint32_t size, wxl::structure::m2::M2Header* h);
    /// M2Fixups: points each hardcoded texture with no inline name at its TXID-resolved client path
    /// (parsed header).
    void InjectTxidNames(const char* modelPath, wxl::structure::m2::M2Header* h, const uint32_t* txid,
                         uint32_t txidCount, Outcome& out);
    /// M2Fixups: frees the header's textureCombinerCombos slot (0x130) a file without flag 0x8 may
    /// keep data in, so the skin rebuild can write it. False: the slot stays as it was.
    bool FreeCombinerField(wxl::structure::m2::M2Header* h);
    /// M2Fixups: clamps each ribbon's texture/material reference values into the header tables.
    void ClampRibbonRefs(wxl::structure::m2::M2Header* h);
    /// M2Fixups: drops each camera reference that points past the camera array.
    void ClampCameraRefs(wxl::structure::m2::M2Header* h);

    /// M2Normalize: rewrites every array listed in kRecordNormalizers to the target shape, in place.
    bool NormalizeRecords(uint8_t* base, uint32_t size, wxl::structure::m2::M2Header* h,
                          NormalizeReport& report);
}
