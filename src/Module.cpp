// wxl.modern-m2: native M2 (MD21) reader, as an x64 WarcraftXL mod. Entry point.
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

// A port of manatactical/wxl-modern-m2 (7f793b1) from the 32-bit extension ABI, which detoured the
// client's model loader and renderer at fixed addresses, to the x64 mod ABI, which does not. The
// recompiled client hands the loader over through wxl.m2file at four points, and this mod runs
// upstream's reader at each:
//
//   WXL_M2FILE_READ      load/NativeLoad.cpp   demux, field deltas, record normalization
//   WXL_M2FILE_PARSED    load/NativeLoad.cpp   TXID texture names, ribbon/camera clamps, shadow gate
//   WXL_M2FILE_SKIN      compat/ModernM2.cpp   bone-budget split, material/texunit contract rebuild
//   WXL_M2FILE_SEQUENCE  load/AnimUnwrap.cpp   'AFM2' external animation unwrap
//
// What upstream did that is engine work rather than format work now lives in the client's own
// source: 32-bit submesh index starts (wxl-core's m2native-indices, upstream's DIP start-index
// expansion) and the guard on packed shader ids past the client's table (upstream's
// BuildBatchMaterial detour). See README.md for the full map.

#include "ExtensionApi.hpp"

#include <windows.h>

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <string>
#include <unordered_map>

namespace wxl_modern_m2
{
    void SetBoneSplit(bool enabled); // compat/ModernM2.cpp

    void LogLine(wxl_log_level level, const char* fmt, ...)
    {
        char    line[1024];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(line, sizeof line, fmt, args);
        va_end(args);
        wxl::Log(level, kTag, line);
    }

    const std::string& ModDirectory()
    {
        static const std::string dir = [] {
            HMODULE self = nullptr;
            if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    reinterpret_cast<LPCSTR>(&LogLine), &self))
                return std::string();
            char        path[MAX_PATH];
            const DWORD n = GetModuleFileNameA(self, path, MAX_PATH);
            if (n == 0 || n >= MAX_PATH) return std::string();
            std::string d(path, n);
            // <mod>\bin\x64\wxl-modern-m2.dll -> <mod>
            for (int up = 0; up < 3; ++up)
            {
                const size_t slash = d.find_last_of("\\/");
                if (slash == std::string::npos) return std::string();
                d.resize(slash);
            }
            return d;
        }();
        return dir;
    }

    namespace
    {
        std::unordered_map<std::string, std::string>& Settings()
        {
            static std::unordered_map<std::string, std::string> settings = [] {
                std::unordered_map<std::string, std::string> out;
                std::ifstream in(ModDirectory() + "\\wxl-modern-m2.cfg");
                std::string   line;
                while (std::getline(in, line))
                {
                    const size_t eq = line.find('=');
                    if (eq == std::string::npos || line[0] == '#' || line[0] == ';') continue;
                    auto trim = [](std::string s) {
                        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
                        size_t i = 0;
                        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
                        return s.substr(i);
                    };
                    out[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
                }
                return out;
            }();
            return settings;
        }
    }

    bool ConfigTruthy(const char* raw, bool fallback)
    {
        if (!raw || !*raw) return fallback;
        const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(raw[0])));
        if (c == '0' || c == 'n' || c == 'f') return false;
        if (c == 'o') return std::tolower(static_cast<unsigned char>(raw[1])) != 'f'; // on / off
        return true;
    }

    bool ConfigRaw(const char* name, char* buf, size_t cap)
    {
        auto it = Settings().find(name);
        if (it == Settings().end() || cap == 0) return false;
        std::snprintf(buf, cap, "%s", it->second.c_str());
        return true;
    }

    bool ConfigFlag(const char* name)
    {
        char value[16] = {};
        return !ConfigRaw(name, value, sizeof value) || ConfigTruthy(value, true);
    }
}

namespace
{
    using namespace wxl_modern_m2;

    uint32_t g_watch = 0;

    wxl_bool WXL_CALL OnFile(wxl_m2file* file, void*)
    {
        switch (file->stage)
        {
        case WXL_M2FILE_READ:     return OnModelRead(*file) ? WXL_TRUE : WXL_FALSE;
        case WXL_M2FILE_PARSED:   OnModelParsed(*file); break;
        case WXL_M2FILE_SKIN:     OnSkinParsed(*file); break;
        case WXL_M2FILE_SEQUENCE: OnSequenceRead(*file); break;
        default: break;
        }
        return WXL_TRUE;
    }

    class ModernM2Mod final : public wxl::Mod
    {
    public:
        wxl_result OnLoad() override
        {
            if (!g_m2file)
            {
                WLOG_ERROR("wxl.m2file is not published (an x64 runtime with the M2 loader seams is needed); nothing to do");
                return WXL_E_UNSUPPORTED;
            }
            if (!kEnabled || !ConfigFlag("Enabled"))
            {
                WLOG_INFO("disabled in wxl-modern-m2.cfg");
                return WXL_E_UNSUPPORTED;
            }
            SetBoneSplit(ConfigFlag("BoneSplit"));
            const wxl_result watched = g_m2file->watch_files(&OnFile, nullptr, &g_watch);
            if (watched != WXL_OK) return watched;
            WLOG_INFO("native M2 reader active (source versions %u-%u, read in place, parsed by the client)",
                      wxl::modern::assets::m2::kSourceVersionMin, wxl::modern::assets::m2::kSourceVersionMax);
            return WXL_OK;
        }

        void OnShutdown() override { LogSessionStats(); }

        void OnUnload() override
        {
            if (g_m2file && g_watch) g_m2file->unwatch_files(g_watch);
            g_watch = 0;
        }
    };
}

WXL_MOD(ModernM2Mod, "wxl.modern-m2", "Modern M2", 1)
