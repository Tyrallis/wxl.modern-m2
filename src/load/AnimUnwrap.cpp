// Native modern-M2 reader: unwraps a modern external animation file in place.
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

// A sequence whose keyframes are not stored in the model streams them from a companion file, and a
// modern companion file puts the very same flat track blob behind a one-chunk wrapper. Unwrapping is
// therefore the whole of the difference, and it belongs here rather than anywhere upstream of the
// read: the bytes are resident, nothing has looked at them yet, and the payload slides onto the
// buffer base the sequence's offsets are relative to.
//
// A file that is already flat is left exactly as it arrived -- which is also what makes this safe to
// run once per sequence sharing the buffer: after the first, the wrapper is gone.
//
// One shape stays out of reach: a model with an externalized skeleton splits the same companion file
// into separate event / attachment / bone chunks instead of one, which is a different unwrap and has
// no home while such models are refused at load.
//
// x64 port: upstream detoured the completion (M2.AnimLoadComplete) and shortened the I/O record;
// wxl.m2file hands the buffer over at WXL_M2FILE_SEQUENCE, before the client parses it.

#include "../ExtensionApi.hpp"
#include "M2NativeInternal.hpp"

#include <windows.h>

#include <cstdint>
#include <cstring>

namespace
{
    namespace fmt = wxl::structure::m2;
    using wxl::runtime::m2native::detail::Rd32;

    constexpr uint32_t kChunkHeaderBytes = 8; // the wrapper's tag plus its payload size

    /// Slides a wrapped payload onto the buffer base. POD locals only (SEH guard below).
    void UnwrapCore(uint8_t* bytes, uint32_t size, uint32_t* unwrappedTo)
    {
        if (!bytes || size < kChunkHeaderBytes) return;
        if (Rd32(bytes) != fmt::kMagicAFM2) return;

        const uint32_t payload = Rd32(bytes + 4);
        if (static_cast<uint64_t>(kChunkHeaderBytes) + payload > size) return;

        std::memmove(bytes, bytes + kChunkHeaderBytes, payload);
        *unwrappedTo = payload;
    }

    void UnwrapGuarded(uint8_t* bytes, uint32_t size, uint32_t* unwrappedTo)
    {
        __try { UnwrapCore(bytes, size, unwrappedTo); }
        __except (EXCEPTION_EXECUTE_HANDLER) { *unwrappedTo = 0; }
    }
}

namespace wxl_modern_m2
{
    void OnSequenceRead(wxl_m2file& file)
    {
        uint32_t unwrappedTo = 0;
        UnwrapGuarded(static_cast<uint8_t*>(file.sequence), file.sequence_size, &unwrappedTo);
        if (unwrappedTo)
            WLOG_INFO("m2native-anim: '%s' sequence %u unwrapped to %u byte(s)", file.name ? file.name : "?",
                      file.sequence_index, unwrappedTo);
    }
}
