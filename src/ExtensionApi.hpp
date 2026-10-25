// wxl.modern-m2: what every translation unit of the mod shares -- the services, the log, the
// settings, and the way a parsed model's arrays are reached on the x64 client.
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

#include "M2Format.hpp"

#include "wxl/sdk.hpp"
#include "wxl/services/m2file.h"
#include "wxl/services/storage.hpp"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace wxl_modern_m2
{
    inline constexpr const char* kTag = "modern-m2";

    /// True builds the whole native M2 pipeline in; false leaves the client on its stock reader.
    inline constexpr bool kEnabled = true;

    inline const wxl::Service<wxl_m2file_api> g_m2file(WXL_M2FILE_SERVICE, WXL_M2FILE_SERVICE_VERSION);
    inline const wxl::StorageService           g_storage;

    void LogLine(wxl_log_level level, const char* fmt, ...);

    /// <mod>: the folder mod.json is in.
    const std::string& ModDirectory();

    // --- Settings: <mod>\wxl-modern-m2.cfg, `Name=Value` lines ------------------------------------

    bool ConfigTruthy(const char* raw, bool fallback);
    bool ConfigRaw(const char* name, char* buf, size_t cap);
    /// A feature toggle, default on: a falsy value in the cfg turns it off.
    bool ConfigFlag(const char* name);

    // --- Arrays of a parsed model -------------------------------------------------------------
    //
    // Before the client parses a file, an array's offset is from the start of the file. Once it
    // has, the offset is from the array itself (M2Array::Data in the client), signed, and anything
    // an array is pointed at must be within reach of that 32-bit offset: Near() hands out memory
    // that is, through wxl.m2file's alloc_near. Nothing allocated for a model is freed, as upstream.

    template <class T>
    inline T* Ptr(wxl::structure::m2::M2Array& a)
    {
        if (!a.count) return nullptr;
        return reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(&a) + static_cast<intptr_t>(static_cast<int32_t>(a.offset)));
    }

    template <class T>
    inline const T* Ptr(const wxl::structure::m2::M2Array& a)
    {
        return Ptr<T>(const_cast<wxl::structure::m2::M2Array&>(a));
    }

    /// Points a parsed array at `data` (count entries). False when `data` is out of reach.
    inline bool Point(wxl::structure::m2::M2Array& a, const void* data, uint32_t count)
    {
        if (!data || !count)
        {
            a.count = 0;
            a.offset = 0;
            return true;
        }
        const intptr_t delta = reinterpret_cast<intptr_t>(data) - reinterpret_cast<intptr_t>(&a);
        if (delta < INT32_MIN || delta > INT32_MAX) return false;
        a.offset = static_cast<uint32_t>(static_cast<int32_t>(delta));
        a.count  = count;
        return true;
    }

    /// `bytes` zeroed bytes the array at `anchor` can point at; null when none are left in reach.
    inline void* Near(const void* anchor, size_t bytes)
    {
        if (!g_m2file || bytes == 0 || bytes > UINT32_MAX) return nullptr;
        return g_m2file->alloc_near(anchor, static_cast<uint32_t>(bytes));
    }

    /// Copies `count` records into memory the array can reach and points it there.
    template <class T>
    inline bool Replace(wxl::structure::m2::M2Array& a, const T* data, size_t count)
    {
        if (!count) return Point(a, nullptr, 0);
        void* copy = Near(&a, count * sizeof(T));
        if (!copy) return false;
        std::memcpy(copy, data, count * sizeof(T));
        return Point(a, copy, static_cast<uint32_t>(count));
    }

    /// One stage of a model file, as Module.cpp hands it over from the runtime's model events
    /// (ModelLoad, ModelParsed, SkinProfileParsed, ModelSequenceLoad).
    enum M2FileStage : uint32_t
    {
        kStageRead = 1,     ///< read whole, not parsed: `data` and `size` may be rewritten and shortened
        kStageParsed = 2,   ///< parsed: `data` is the header, arrays self-relative
        kStageSkin = 3,     ///< its skin profile parsed: `skin` is its header
        kStageSequence = 4, ///< an external sequence read: `sequence` may be rewritten in place
    };

    struct M2File
    {
        uint32_t    stage = 0;
        void*       file = nullptr; ///< the client's CM2Shared; null at kStageRead (the event does not say)
        const char* name = nullptr; ///< the path the client opened it by
        void*       data = nullptr;
        uint32_t    size = 0;
        void*       skin = nullptr;
        void*       sequence = nullptr;
        uint32_t    sequence_size = 0;
        uint32_t    sequence_index = 0;
    };

    // Per-stage entry points, called by Module.cpp.
    bool OnModelRead(M2File& file);      // load/NativeLoad.cpp: kStageRead
    void OnModelParsed(M2File& file);    // load/NativeLoad.cpp: kStageParsed
    void OnSkinParsed(M2File& file);     // compat/ModernM2.cpp: kStageSkin
    void OnSequenceRead(M2File& file);   // load/AnimUnwrap.cpp: kStageSequence
    void LogSessionStats();                  // load/NativeLoad.cpp

    // load/NativeLoad.cpp: the files the native reader filled (the registry upstream keyed by model).
    bool IsNativeLoaded(const void* file);
}

#define WLOG_TRACE(...) ::wxl_modern_m2::LogLine(WXL_LOG_TRACE, __VA_ARGS__)
#define WLOG_DEBUG(...) ::wxl_modern_m2::LogLine(WXL_LOG_DEBUG, __VA_ARGS__)
#define WLOG_INFO(...)  ::wxl_modern_m2::LogLine(WXL_LOG_INFO, __VA_ARGS__)
#define WLOG_WARN(...)  ::wxl_modern_m2::LogLine(WXL_LOG_WARN, __VA_ARGS__)
#define WLOG_ERROR(...) ::wxl_modern_m2::LogLine(WXL_LOG_ERROR, __VA_ARGS__)
