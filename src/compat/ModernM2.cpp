// wxl.modern-m2: the live-engine half -- what the client must be taught to do with a model the native
// reader filled, once its skin profile is parsed.
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

// Upstream subscribed to the core's OnM2SkinFinalize (its kFinalizeSkin detour, 0x837A40 --
// CM2Shared::InitializeSkinProfile in the client's source); here wxl.m2file hands the same moment over
// as WXL_M2FILE_SKIN. What runs is upstream's: the universal bone-budget split, then, for files the
// native reader filled, the material / texunit contract rebuild -- or, for a stock model that was
// split, only the batch re-point. Upstream's draw-time halves (alpha-key cutoff, ribbon combine) are
// not here: see the README.

#include "../ExtensionApi.hpp"
#include "BoneBudget.hpp"
#include "Skin.hpp"

#include <windows.h>

#include <vector>

namespace
{
    namespace fmt = wxl::structure::m2;
    namespace bn  = wxl::modern::assets::common::bones;

    bool g_boneSplit = true;

    void Finalize(fmt::M2Header* md, fmt::M2SkinHeader* header, bool native, const char* name)
    {
        bn::LiveSkin sk(header);

        std::vector<bn::SplitSection> sections;
        std::vector<bn::SplitRun>     splitMap;
        uint32_t                      splitCount = 0;
        const bool split = g_boneSplit && bn::SplitSubmeshes(md, &sk, sections, splitMap, splitCount, name) &&
                           splitCount > 0;
        if (split)
            WLOG_INFO("modern-assets: '%s' bone-splitter produced %u extra sub-draw(s)", name, splitCount);

        if (native)
            wxl::modern::assets::m2::skin::Rebuild(md, &sk, splitMap, name);
        else if (split)
            bn::RepointBatchesAfterSplit(&sk, splitMap);

        if (native)
            WLOG_INFO("modern-assets: '%s' skin finalized: %u submesh(es), %u batch(es), %u index(es), "
                      "%u extra sub-draw(s) from the bone split",
                      name, header->submeshes.count, header->batches.count, header->indices.count, splitCount);
    }

    /// SEH shell: a fault on malformed data leaves the skin as the client parsed it.
    bool FinalizeGuarded(fmt::M2Header* md, fmt::M2SkinHeader* header, bool native, const char* name)
    {
        __try
        {
            Finalize(md, header, native, name);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }
}

namespace wxl_modern_m2
{
    void SetBoneSplit(bool enabled) { g_boneSplit = enabled; }

    void OnSkinParsed(wxl_m2file& file)
    {
        if (!file.data || !file.skin) return;
        const char* name = file.name ? file.name : "?";
        if (!FinalizeGuarded(static_cast<fmt::M2Header*>(file.data), static_cast<fmt::M2SkinHeader*>(file.skin),
                             IsNativeLoaded(file.file), name))
            WLOG_WARN("modern-assets: '%s' skin finalize faulted; the skin is left as parsed", name);
    }
}
