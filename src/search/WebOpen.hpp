#pragma once

// Brave's own sites and search engine in the overview search, by way of
// omarchy-web-open. NixOs-dots docs/manual.md#web-search-in-hyprdrive

#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace WebOpen {
    inline constexpr const char* TOOL = "@webOpen@";

    struct SState {
        std::mutex                                       mutex;
        std::vector<std::pair<std::string, std::string>> sites; // label, url; best first
        std::string                                      engine = "the web";
        std::atomic<uint64_t>                            generation{0};
        std::atomic<bool>                                loading{false};
    };
    inline SState g_state;

    // Reread at every overview open, off the compositor thread: history moves.
    inline void warm() {
        if (g_state.loading.exchange(true))
            return;
        std::thread([] {
            std::vector<std::pair<std::string, std::string>> sites;
            std::string                                      engine;
            if (FILE* pipe = popen((std::string{TOOL} + " --hyprdrive 2>/dev/null").c_str(), "r")) {
                char*  line  = nullptr;
                size_t cap   = 0;
                bool   first = true;
                while (getline(&line, &cap, pipe) > 0) {
                    std::string s{line};
                    while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
                        s.pop_back();
                    const auto TAB = s.find('\t');
                    if (TAB != std::string::npos && first && s.compare(0, TAB, "engine") == 0)
                        engine = s.substr(TAB + 1);
                    else if (TAB != std::string::npos)
                        sites.emplace_back(s.substr(0, TAB), s.substr(TAB + 1));
                    first = false;
                }
                std::free(line);
                pclose(pipe);
            }
            {
                std::lock_guard lock(g_state.mutex);
                g_state.sites = std::move(sites);
                if (!engine.empty())
                    g_state.engine = engine;
            }
            g_state.generation++;
            g_state.loading = false;
        }).detach();
    }

    inline uint64_t generation() {
        return g_state.generation.load();
    }

    inline std::string engine() {
        std::lock_guard lock(g_state.mutex);
        return g_state.engine;
    }

    inline std::string lower(std::string s) {
        for (auto& c : s)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    // The same rule omarchy-web-open applies to typed text.
    inline bool looksLikeUrl(const std::string& text) {
        if (text.find("://") != std::string::npos)
            return true;
        const auto HOST = text.substr(0, text.find('/'));
        if (HOST.starts_with("localhost:"))
            return true;
        return text.find(' ') == std::string::npos && HOST.find('.') != std::string::npos && !HOST.ends_with('.');
    }

    // Sites whose name or address holds the query, in Brave's ranking.
    inline std::vector<std::pair<std::string, std::string>> matches(const std::string& query, size_t limit) {
        std::vector<std::pair<std::string, std::string>> out;
        const auto                                       Q = lower(query);
        std::lock_guard                                  lock(g_state.mutex);
        for (const auto& [LABEL, URL] : g_state.sites) {
            if (out.size() >= limit)
                break;
            if (lower(LABEL).find(Q) != std::string::npos || lower(URL).find(Q) != std::string::npos)
                out.emplace_back(LABEL, URL);
        }
        return out;
    }

    inline std::string quoted(const std::string& s) {
        std::string out = "'";
        for (const char c : s)
            out += c == '\'' ? std::string{"'\\''"} : std::string(1, c);
        return out + "'";
    }

    inline std::string openCommand(const std::string& target) {
        return std::string{TOOL} + " --open " + quoted(target);
    }

    inline std::string searchCommand(const std::string& text) {
        return std::string{TOOL} + " --search " + quoted(text);
    }
}
