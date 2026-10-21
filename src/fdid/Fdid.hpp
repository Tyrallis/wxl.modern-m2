// wxl.modern-m2: texture FileDataIDs to client paths.
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
#include <string>

namespace wxl_modern_m2
{
    /**
     * @brief The client path of texture @p fileDataId, named by the model at @p modelPath; empty
     *        when nothing names it.
     *
     * Upstream asked wxl-db2's wxl.fdid, which the x64 runtime has no counterpart of. Asked here,
     * in order, of what a model exported for this client comes with:
     *   1. the wow.export manifest beside the model (<model>.manifest.json: "textures" entries of
     *      "fileDataID" and "file", the file relative to the model's folder);
     *   2. <mod>\listfile.csv, "id;path" lines (the community listfile, or a slice of it);
     *   3. wow.export's own naming of an unnamed texture: <model folder>\<model>_<id>.blp.
     * Answers are kept for the session.
     */
    std::string ResolveTexture(const char* modelPath, uint32_t fileDataId);

    /// How many ids were answered, and by what, for the session log.
    struct FdidStats
    {
        uint32_t manifest;
        uint32_t listfile;
        uint32_t naming;
        uint32_t missed;
    };
    FdidStats GetFdidStats();
}
