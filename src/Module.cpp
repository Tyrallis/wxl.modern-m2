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
#include <vector>
#include <cstring>

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

    // The runtime raises the loader's stages as events; each is subscribed raw and handed to the
    // stage's entry point as an M2File.
    wxl::Subscription g_load, g_parsed, g_skin, g_sequence;

    std::string Path(wxl_str path) { return path.data ? std::string(path.data, path.size) : std::string(); }

    /// ModelLoad: the bytes as read, before the client parses them. A modern model (MD21) is
    /// rewritten in a copy, handed back as the replacement the client parses instead.
    void WXL_CALL OnModelLoadEvent(void*, const void* raw)
    {
        const auto* args = static_cast<const wxl_model_load_args*>(raw);
        const auto* bytes = static_cast<const uint8_t*>(args->data);
        if (!bytes || args->size < 8 || !args->replacement || !args->replacement_size) return;
        uint32_t magic = 0;
        std::memcpy(&magic, bytes, sizeof magic);
        if (magic != wxl::structure::m2::kMagicMD21) return; // a stock model: parsed as read

        // Kept until the next model: the runtime copies the replacement into the client's buffer.
        static std::vector<uint8_t> s_copy;
        static std::string          s_name;
        static const uint8_t        kRefused[4] = {};
        s_copy.assign(bytes, bytes + args->size);
        s_name = Path(args->path);
        M2File file;
        file.stage = kStageRead;
        file.name  = s_name.c_str();
        file.data  = s_copy.data();
        file.size  = static_cast<uint32_t>(s_copy.size());
        if (!OnModelRead(file))
        {
            // Nothing the client can parse: it reports the file corrupt, as for a bad read.
            *args->replacement      = kRefused;
            *args->replacement_size = sizeof kRefused;
            return;
        }
        *args->replacement      = s_copy.data();
        *args->replacement_size = file.size;
    }

    void WXL_CALL OnModelParsedEvent(void*, const void* raw)
    {
        const auto* args = static_cast<const wxl_model_parsed_args*>(raw);
        const std::string name = Path(args->path);
        M2File file;
        file.stage = kStageParsed;
        file.file  = args->file;
        file.name  = name.c_str();
        file.data  = args->header;
        file.size  = args->size;
        OnModelParsed(file);
    }

    void WXL_CALL OnSkinParsedEvent(void*, const void* raw)
    {
        const auto* args = static_cast<const wxl_skin_profile_parsed_args*>(raw);
        const std::string name = Path(args->path);
        M2File file;
        file.stage = kStageSkin;
        file.file  = args->file;
        file.name  = name.c_str();
        file.data  = args->header;
        file.skin  = args->skin;
        OnSkinParsed(file);
    }

    void WXL_CALL OnSequenceEvent(void*, const void* raw)
    {
        const auto* args = static_cast<const wxl_model_sequence_args*>(raw);
        const std::string name = Path(args->path);
        M2File file;
        file.stage          = kStageSequence;
        file.file           = args->file;
        file.name           = name.c_str();
        file.sequence       = const_cast<void*>(args->data); // the client's own buffer, unwrapped in place
        file.sequence_size  = args->size;
        file.sequence_index = args->sequence_index;
        OnSequenceRead(file);
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
            g_load     = wxl::Subscribe(WXL_EV_ModelLoad, &OnModelLoadEvent, nullptr);
            g_parsed   = wxl::Subscribe(WXL_EV_ModelParsed, &OnModelParsedEvent, nullptr);
            g_skin     = wxl::Subscribe(WXL_EV_SkinProfileParsed, &OnSkinParsedEvent, nullptr);
            g_sequence = wxl::Subscribe(WXL_EV_ModelSequenceLoad, &OnSequenceEvent, nullptr);
            if (!g_load || !g_parsed || !g_skin || !g_sequence)
            {
                WLOG_ERROR("the runtime does not raise the model loader's events (ModelLoad, ModelParsed, "
                           "SkinProfileParsed, ModelSequenceLoad); nothing to do");
                return WXL_E_UNSUPPORTED;
            }
            WLOG_INFO("native M2 reader active (source versions %u-%u, read in place, parsed by the client)",
                      wxl::modern::assets::m2::kSourceVersionMin, wxl::modern::assets::m2::kSourceVersionMax);
            return WXL_OK;
        }

        void OnShutdown() override { LogSessionStats(); }

        void OnUnload() override
        {
            g_load     = wxl::Subscription();
            g_parsed   = wxl::Subscription();
            g_skin     = wxl::Subscription();
            g_sequence = wxl::Subscription();
        }
    };
}

WXL_MOD(ModernM2Mod, "wxl.modern-m2", "Modern M2")
