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

#include "Fdid.hpp"

#include "../ExtensionApi.hpp"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

namespace
{
    std::unordered_map<uint32_t, std::string> g_known;     // every id answered so far
    std::unordered_set<std::string>           g_manifests; // manifests already read (lowercased paths)
    bool                                      g_listfileRead = false;
    wxl_modern_m2::FdidStats                  g_stats{};

    std::string Lower(std::string s)
    {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    bool IsBlp(const std::string& path)
    {
        return path.size() > 4 && Lower(path.substr(path.size() - 4)) == ".blp";
    }

    std::string Folder(const std::string& path)
    {
        const size_t slash = path.find_last_of("\\/");
        return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
    }

    std::string Stem(const std::string& path)
    {
        const size_t slash = path.find_last_of("\\/");
        std::string  name  = slash == std::string::npos ? path : path.substr(slash + 1);
        const size_t dot   = name.find_last_of('.');
        return dot == std::string::npos ? name : name.substr(0, dot);
    }

    std::string Backslashed(std::string path)
    {
        for (char& c : path)
            if (c == '/') c = '\\';
        return path;
    }

    /// Reads one quoted JSON string value starting at `at` (on the opening quote).
    bool JsonString(const std::string& text, size_t at, std::string& out)
    {
        if (at >= text.size() || text[at] != '"') return false;
        out.clear();
        for (size_t i = at + 1; i < text.size(); ++i)
        {
            const char c = text[i];
            if (c == '"') return true;
            if (c == '\\' && i + 1 < text.size())
            {
                const char e = text[++i];
                out.push_back(e == '/' ? '/' : e == '\\' ? '\\' : e);
                continue;
            }
            out.push_back(c);
        }
        return false;
    }

    /**
     * Learns every texture of a wow.export manifest: objects carrying "fileDataID" and "file".
     * Read as text, without a JSON library: each "fileDataID" is paired with the nearest "file"
     * after it inside the same object.
     */
    void ReadManifest(const std::string& modelPath)
    {
        const std::string folder   = Folder(modelPath);
        const std::string manifest = folder + Stem(modelPath) + ".manifest.json";
        if (!g_manifests.insert(Lower(manifest)).second) return;

        auto text = wxl_modern_m2::g_storage.ReadText(manifest);
        if (!text) return;
        const std::string& json = *text;

        uint32_t learned = 0;
        for (size_t at = json.find("\"fileDataID\""); at != std::string::npos; at = json.find("\"fileDataID\"", at + 1))
        {
            const size_t colon = json.find(':', at);
            if (colon == std::string::npos) break;
            const uint32_t id = static_cast<uint32_t>(std::strtoul(json.c_str() + colon + 1, nullptr, 10));
            const size_t close = json.find('}', colon);
            const size_t file  = json.find("\"file\"", colon);
            if (!id || file == std::string::npos || file > close) continue;
            const size_t quote = json.find('"', json.find(':', file) + 1);
            std::string  name;
            if (!JsonString(json, quote, name) || name.empty()) continue;
            const std::string path = Backslashed(folder + name);
            if (IsBlp(path) && g_known.emplace(id, path).second) ++learned;
        }
        if (learned) WLOG_INFO("fdid: %u texture id(s) from %s", learned, manifest.c_str());
    }

    /// <mod>\listfile.csv, read once, on the first id nothing else answered.
    void ReadListfile()
    {
        if (g_listfileRead) return;
        g_listfileRead = true;
        const std::string path = wxl_modern_m2::ModDirectory() + "\\listfile.csv";
        std::ifstream     in(path);
        if (!in) return;
        uint32_t    learned = 0;
        std::string line;
        while (std::getline(in, line))
        {
            const size_t sep = line.find(';');
            if (sep == std::string::npos) continue;
            const uint32_t id = static_cast<uint32_t>(std::strtoul(line.c_str(), nullptr, 10));
            if (!id) continue;
            std::string file = line.substr(sep + 1);
            while (!file.empty() && (file.back() == '\r' || file.back() == ' ')) file.pop_back();
            if (IsBlp(file) && g_known.emplace(id, Backslashed(file)).second) ++learned;
        }
        WLOG_INFO("fdid: %u texture id(s) from %s", learned, path.c_str());
    }
}

namespace wxl_modern_m2
{
    std::string ResolveTexture(const char* modelPath, uint32_t fileDataId)
    {
        if (!fileDataId) return {};
        const std::string model = modelPath ? modelPath : "";

        if (auto it = g_known.find(fileDataId); it != g_known.end()) return it->second;

        if (!model.empty())
        {
            ReadManifest(model);
            if (auto it = g_known.find(fileDataId); it != g_known.end())
            {
                ++g_stats.manifest;
                return it->second;
            }
        }

        ReadListfile();
        if (auto it = g_known.find(fileDataId); it != g_known.end())
        {
            ++g_stats.listfile;
            return it->second;
        }

        if (!model.empty())
        {
            const std::string named = Folder(model) + Stem(model) + "_" + std::to_string(fileDataId) + ".blp";
            if (g_storage.Exists(named))
            {
                ++g_stats.naming;
                g_known.emplace(fileDataId, named);
                return named;
            }
        }

        ++g_stats.missed;
        return {};
    }

    FdidStats GetFdidStats() { return g_stats; }
}
