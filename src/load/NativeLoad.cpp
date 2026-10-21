// Native modern-M2 reader: reads a modern MD21 container directly and fills the client's own model
// runtime with it.
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

// MECHANISM (orchestration -- the phases live in M2Demux / M2Fixups / M2Normalize):
//
//   READ        wxl.m2file hands over the model's bytes before the client parses them (upstream:
//               the kInit detour at 0x83CF00). A stock MD20 model pays one magic compare. For an
//               'MD21' container one chunk walk harvests the body and the auxiliary chunks, the
//               body is slid to the buffer base IN PLACE (the allocation stays the client's), the
//               field deltas and the record normalization bring it onto the client's record
//               shapes, and the size is shortened to the body.
//   Parse       x64 port: the client's own parser walks the body, as it would a version 264 model
//               -- the header's version is set to 264 for it, the one value it checks. Upstream
//               kept the modern version because it never let the 32-bit parser see the body: it
//               walked the offsets itself (M2WalkOwned), then called the stock Initialize.
//   PARSED      Before the client's Initialize (skin choice, textures): TXID names injected, ribbon
//               and camera references clamped, and the shadow pass's animate gate lifted.
//   Live half   The file is remembered as native-loaded; the skin rebuild (compat/ModernM2.cpp)
//               keys off that.
//   Safety      Every phase runs under SEH: malformed data becomes a logged refusal, never a crash.

#include "../ExtensionApi.hpp"
#include "M2NativeInternal.hpp"

#include "../fdid/Fdid.hpp"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fmt = wxl::structure::m2;

namespace
{
    using namespace wxl::runtime::m2native::detail;

    // ---------------------------------------------------------------- session counters
    uint32_t g_statNative = 0;
    uint32_t g_statFailed = 0;
    uint32_t g_statTexResolved = 0;
    uint32_t g_statTexUnresolved = 0;
    uint32_t g_statNormalized[kRecordNormalizerCount]{};
    uint32_t g_statSkipTxac = 0;
    uint32_t g_statSkipLdv1 = 0;
    uint32_t g_statSkipAfid = 0;
    uint32_t g_statSkipSkid = 0;
    uint32_t g_statSkipOther = 0;
    uint32_t g_statExtSeqPending = 0;
    uint32_t g_statShadowGateForced = 0;

    /// Files the native reader filled, with the TXID table the PARSED stage still needs. Keyed by
    /// the client's CM2Shared; an address the client reuses is forgotten at its next READ.
    struct Native
    {
        std::vector<uint32_t> txid;
        Outcome               out{};
        bool                  parsed = false;
    };
    std::unordered_map<const void*, Native> g_native;

    /// The READ phase on the raw container. POD locals only: it runs under the SEH guard.
    void ReadCore(uint8_t* buf, uint32_t size, Scan& s, uint32_t& bodySize, Outcome& out)
    {
        if (!ScanContainer(buf, size, s)) { out.fail = "no MD20 body in container"; return; }
        out.skipMask = s.skipMask;

        auto* h = reinterpret_cast<fmt::M2Header*>(buf + s.bodyOff);
        if (h->magic != fmt::kMagicMD20) { out.fail = "body magic"; return; }
        out.version = h->version;
        if (h->version < wxl::modern::assets::m2::kSourceVersionMin ||
            h->version > wxl::modern::assets::m2::kSourceVersionMax)
        {
            out.fail = "inner version outside the supported source window";
            return;
        }

        // A split-skeleton model (SKID / empty bone+sequence arrays) has no home yet: without the
        // .skel splice it would stand unboned. Refuse it (a clean miss) rather than fill a broken model.
        if ((s.skipMask & kSkipSkid) || (!h->bones.count && !h->sequences.count))
        {
            out.fail = "split skeleton (.skel) model -- not supported";
            return;
        }

        // Slide the body onto the allocation base: the chunk-header bytes ahead of it are dead after
        // the harvest, trailing chunks are beyond the moved range and already harvested. The client
        // keeps its allocation pointer, and the header now sits where it parses from.
        if (s.bodyOff != 0)
            std::memmove(buf, buf + s.bodyOff, s.bodySize);
        bodySize = s.bodySize;
        h = reinterpret_cast<fmt::M2Header*>(buf);

        // --- in-place field deltas on the raw body ---
        // Two global-flag bits claim that the runtime owns the texture combo arrays, which would have
        // the teardown free interior pointers of this one buffer. (The client clears them too.)
        h->globalFlags &= ~0x60u;
        FixSequencesRaw(buf, bodySize, h, out.extSeqPending);
        FixMaterialsRaw(buf, bodySize, h);

        // The stock skin chooser walks a 4-entry threshold table indexed 4 - numSkinProfiles; more
        // profiles than the client ever shipped (LDV1-era LOD counts) would underflow it.
        if (h->numSkinProfiles > 4)
        {
            out.skipMask |= kSkipLdv1;
            h->numSkinProfiles = 1;
        }

        // --- record normalization: one record shape for every source era, in place ---
        if (!NormalizeRecords(buf, bodySize, h, out.normalized))
        {
            out.fail = "record normalization rejected a record";
            return;
        }

        // x64 port: the client's parser takes the body from here, and it accepts version 264 only.
        h->version = fmt::kClientVersion;
        out.ok = 1;
    }

    void ReadGuarded(uint8_t* buf, uint32_t size, Scan* s, uint32_t* bodySize, Outcome* out)
    {
        __try
        {
            ReadCore(buf, size, *s, *bodySize, *out);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            out->ok   = 0;
            out->fail = "access violation during the read";
        }
    }

    /// The PARSED phase's array work, under its own guard.
    void ParsedCore(void* file, const char* name, fmt::M2Header* h, const uint32_t* txid, uint32_t txidCount,
                    Outcome* out)
    {
        __try
        {
            FreeCombinerField(h);
            InjectTxidNames(name, h, txid, txidCount, *out);
            ClampRibbonRefs(h);
            ClampCameraRefs(h);

            // Shadow-path animate gate. The client's Initialize counts the bones carrying one of the
            // flags 0x2F8 (billboards, a transform); the shadow pass skips the bone pass of a model
            // at zero, and its shadow then turns with the camera instead of following the pose.
            // Stock exporters set those flags on practically every bone; a modern one can ship every
            // bone flag empty. One more counted bone puts our models on the path stock content takes.
            // Upstream wrote the count on the runtime object after Initialize; wxl.m2file adds it.
            uint32_t flagged = 0;
            if (const auto* bones = wxl_modern_m2::Ptr<fmt::M2CompBone>(h->bones))
                for (uint32_t i = 0; i < h->bones.count; ++i)
                    if (bones[i].flags & 0x2F8u) ++flagged;
            if (!flagged && h->bones.count)
            {
                wxl_modern_m2::g_m2file->animate_bones(file);
                out->shadowGateForced = 1;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            out->ok   = 0;
            out->fail = "access violation after the parse";
        }
    }
}

namespace wxl_modern_m2
{
    bool IsNativeLoaded(const void* file)
    {
        return g_native.count(file) != 0;
    }

    bool OnModelRead(wxl_m2file& file)
    {
        g_native.erase(file.file); // the client reuses CM2Shared addresses
        auto* buf = static_cast<uint8_t*>(file.data);
        if (!buf || file.size < 8 || Rd32(buf) != fmt::kMagicMD21) return true; // a stock model

        Scan     scan{};
        Outcome  out{};
        uint32_t bodySize = 0;
        ReadGuarded(buf, file.size, &scan, &bodySize, &out);

        if (!out.ok)
        {
            ++g_statFailed;
            WLOG_WARN("m2native: '%s' refused: %s (v=%u skips=0x%X)", file.name ? file.name : "?",
                      out.fail ? out.fail : "unknown", out.version, out.skipMask);
            return false;
        }

        file.size = bodySize;
        Native& native = g_native[file.file];
        native.txid.assign(scan.txid, scan.txid + scan.txidCount);
        native.out = out;
        return true;
    }

    void OnModelParsed(wxl_m2file& file)
    {
        auto it = g_native.find(file.file);
        if (it == g_native.end() || it->second.parsed) return;
        Native&  native = it->second;
        Outcome& out    = native.out;
        native.parsed   = true;

        ParsedCore(file.file, file.name, static_cast<fmt::M2Header*>(file.data), native.txid.data(),
                   static_cast<uint32_t>(native.txid.size()), &out);
        const char* name = file.name ? file.name : "?";
        if (!out.ok)
        {
            ++g_statFailed;
            WLOG_WARN("m2native: '%s' failed after the parse: %s", name, out.fail ? out.fail : "unknown");
            return;
        }

        ++g_statNative;
        g_statTexResolved += out.texResolved;
        g_statTexUnresolved += out.texUnresolved;
        if (out.skipMask & kSkipTxac)  ++g_statSkipTxac;
        if (out.skipMask & kSkipLdv1)  ++g_statSkipLdv1;
        if (out.skipMask & kSkipAfid)  ++g_statSkipAfid;
        if (out.skipMask & kSkipSkid)  ++g_statSkipSkid;
        if (out.skipMask & kSkipOther) ++g_statSkipOther;
        g_statExtSeqPending += out.extSeqPending;
        g_statShadowGateForced += out.shadowGateForced;

        // What this model needed reshaping, named by the normalizer table so a new record type shows
        // up in the log the moment its entry exists.
        char   norm[96]{};
        size_t at = 0;
        for (uint32_t i = 0; i < kRecordNormalizerCount && at + 1 < sizeof norm; ++i)
        {
            const uint32_t n = out.normalized.records[i];
            if (!n) continue;
            g_statNormalized[i] += n;
            const int wrote = std::snprintf(norm + at, sizeof norm - at, " %s=%u", kRecordNormalizers[i].label, n);
            if (wrote <= 0) break;
            at += static_cast<size_t>(wrote);
        }

        WLOG_INFO("m2native: '%s' read natively (v=%u tex=%u/%u skips=0x%X extseq=%u gate-forced=%u normalized:%s)",
                  name, out.version, out.texResolved, out.texResolved + out.texUnresolved, out.skipMask,
                  out.extSeqPending, out.shadowGateForced, norm[0] ? norm : " none");
        if (out.extSeqPending)
            WLOG_INFO("m2native: '%s' has %u streamed (.anim) sequence(s) -- bind pose until they arrive",
                      name, out.extSeqPending);
    }

    void LogSessionStats()
    {
        uint32_t normalized = 0;
        for (uint32_t i = 0; i < kRecordNormalizerCount; ++i) normalized += g_statNormalized[i];
        const FdidStats f = GetFdidStats();
        WLOG_INFO("session: %u model(s) read natively, %u refused; textures %u resolved / %u unresolved "
                  "(manifest %u, listfile %u, naming %u); %u record(s) normalized; skipped chunks: TXAC %u, "
                  "LDV1 %u, AFID %u, SKID %u, other %u; %u streamed sequence(s); %u shadow gate(s) lifted",
                  g_statNative, g_statFailed, g_statTexResolved, g_statTexUnresolved, f.manifest, f.listfile,
                  f.naming, normalized, g_statSkipTxac, g_statSkipLdv1, g_statSkipAfid, g_statSkipSkid,
                  g_statSkipOther, g_statExtSeqPending, g_statShadowGateForced);
    }
}
