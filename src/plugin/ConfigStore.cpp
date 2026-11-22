#include <F4SE/F4SE.h>
#include <RE/Fallout.h>

#include "plugin/ConfigStore.h"
#include "plugin/ThreadQueues.h"

#include <REX/LOG.h>

#include <Windows.h>
#undef ERROR

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <vector>

namespace fo4flatvr::cfgstore
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        std::string Lower(std::string s)
        {
            for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }
        std::string Trim(const std::string& s)
        {
            const auto b = s.find_first_not_of(" \t\r\n");
            if (b == std::string::npos) return {};
            const auto e = s.find_last_not_of(" \t\r\n");
            return s.substr(b, e - b + 1);
        }

        struct Section
        {
            std::string                                      name;  // as first written
            std::vector<std::pair<std::string, std::string>> kv;    // key as written, raw value
        };
        struct Layer
        {
            std::vector<Section> secs;
            Section* Find(const std::string& a_sec)
            {
                const auto l = Lower(a_sec);
                for (auto& s : secs)
                    if (Lower(s.name) == l) return &s;
                return nullptr;
            }
            const std::string* Get(const std::string& a_sec, const std::string& a_key)
            {
                auto* s = Find(a_sec);
                if (!s) return nullptr;
                const auto lk = Lower(a_key);
                for (auto& [k, v] : s->kv)
                    if (Lower(k) == lk) return &v;
                return nullptr;
            }
            void Set(const std::string& a_sec, const std::string& a_key, const std::string& a_val)
            {
                auto* s = Find(a_sec);
                if (!s) {
                    secs.push_back({ a_sec, {} });
                    s = &secs.back();
                }
                const auto lk = Lower(a_key);
                for (auto& [k, v] : s->kv)
                    if (Lower(k) == lk) {
                        v = a_val;
                        return;
                    }
                s->kv.emplace_back(a_key, a_val);
            }
            bool Erase(const std::string& a_sec, const std::string& a_key)
            {
                auto* s = Find(a_sec);
                if (!s) return false;
                const auto lk = Lower(a_key);
                const auto n = s->kv.size();
                std::erase_if(s->kv, [&](const auto& p) { return Lower(p.first) == lk; });
                return s->kv.size() != n;
            }
            std::size_t Count() const
            {
                std::size_t n = 0;
                for (const auto& s : secs) n += s.kv.size();
                return n;
            }
        };

        // A save sent through MCM's API: held above the MCM layer until the file shows it (or it times out)
        struct Sent
        {
            std::string       sec, key, val;
            Clock::time_point at;
        };

        std::mutex g_lock;
        bool       g_init{ false };
        std::string g_user, g_mcm, g_eff, g_effTarget;
        bool        g_effOk{ false };
        Layer       g_userL, g_mcmL, g_prevL;
        std::vector<Sent> g_sent;
        std::string g_lastText;
        std::map<std::string, std::string> g_lastMerged;  // "sec\nkey" lower -> value
        std::uint64_t g_rev{ 0 }, g_writes{ 0 }, g_writeFails{ 0 }, g_mcmSends{ 0 }, g_lineWrites{ 0 }, g_confirmed{ 0 }, g_fallbacks{ 0 };
        Clock::time_point g_nextPoll{};
        std::filesystem::file_time_type g_userT{}, g_mcmT{};
        bool g_userEx{ false }, g_mcmEx{ false };
        std::string g_lastWriteError;

        Layer ReadLayer(const std::string& a_path)
        {
            Layer L;
            std::error_code ec;
            if (!std::filesystem::exists(a_path, ec)) return L;
            std::vector<char> names(65536);
            const DWORD n = ::GetPrivateProfileSectionNamesA(names.data(), static_cast<DWORD>(names.size()), a_path.c_str());
            std::vector<char> buf(131072);
            for (DWORD i = 0; i < n && names[i];) {
                const std::string sec(names.data() + i);
                i += static_cast<DWORD>(sec.size()) + 1;
                if (L.Find(sec)) continue;  // duplicate section: the API reads the first
                Section S{ sec, {} };
                const DWORD m = ::GetPrivateProfileSectionA(sec.c_str(), buf.data(), static_cast<DWORD>(buf.size()), a_path.c_str());
                for (DWORD j = 0; j < m && buf[j];) {
                    const std::string line(buf.data() + j);
                    j += static_cast<DWORD>(line.size()) + 1;
                    const auto t = Trim(line);
                    if (t.empty() || t[0] == ';' || t[0] == '#') continue;
                    const auto eq = t.find('=');
                    if (eq == std::string::npos) continue;
                    const auto k = Trim(t.substr(0, eq));
                    if (k.empty()) continue;
                    bool dup = false;
                    for (const auto& p : S.kv)
                        if (Lower(p.first) == Lower(k)) dup = true;  // the API reads the first
                    if (!dup) S.kv.emplace_back(k, Trim(t.substr(eq + 1)));
                }
                L.secs.push_back(std::move(S));
            }
            return L;
        }

        bool Stamp(const std::string& a_path, std::filesystem::file_time_type& a_t, bool& a_ex)
        {
            std::error_code ec;
            const auto t = std::filesystem::last_write_time(a_path, ec);
            const bool ex = !ec;
            const bool changed = ex != a_ex || (ex && t != a_t);
            a_ex = ex;
            if (ex) a_t = t;
            return changed;
        }

        bool SameValue(const std::string& a, const std::string& b)
        {
            char *ea = nullptr, *eb = nullptr;
            const double x = std::strtod(a.c_str(), &ea), y = std::strtod(b.c_str(), &eb);
            if (ea != a.c_str() && eb != b.c_str() && *ea == 0 && *eb == 0) return std::abs(x - y) < 1e-4;
            return Lower(Trim(a)) == Lower(Trim(b));
        }

        // merged layers (lock held)
        Layer Merge()
        {
            Layer M = g_userL;
            for (auto* L : { &g_mcmL })
                for (const auto& s : L->secs)
                    for (const auto& [k, v] : s.kv) M.Set(s.name, k, v);
            for (const auto& s : g_sent) M.Set(s.sec, s.key, s.val);
            for (const auto& s : g_prevL.secs)
                for (const auto& [k, v] : s.kv) M.Set(s.name, k, v);
            // 20 Nov (user: "no option to disable any scaling from us, and to just use the game's default scale"): [General]
            // bNoScaling=1 puts the game's own 70 units per metre (1 unit = 1.43 cm) in the merged file, whatever fUnitsPerMeter
            // the layers hold, so every module that reads it (head / hands / roomscale / FRIK bridge / gun) uses the default.
            // The FRIK body side of the switch is in FRIK-flat (bridge v6 NoScaling).
            if (const auto* ns = M.Get("General", "bNoScaling"); ns && std::strtol(ns->c_str(), nullptr, 0) != 0) {
                M.Set("General", "fUnitsPerMeter", "70");
            }
            return M;
        }

        bool WriteMerged()  // lock held; true = merged values changed since the last call
        {
            const Layer M = Merge();
            std::map<std::string, std::string> flat;
            std::string text =
                "; fo4flatvr.effective.ini - GENERATED by fo4flatvr (ConfigStore.h); rewritten whenever a layer changes.\n"
                "; = fo4flatvr.ini, then MCM's Data\\MCM\\Settings\\fo4flatvr.ini, then unsaved VR-panel previews.\n"
                "; Do not edit this file: edit fo4flatvr.ini, or use MCM / the VR settings panel.\n";
            for (const auto& s : M.secs) {
                text += "\n[" + s.name + "]\n";
                for (const auto& [k, v] : s.kv) {
                    text += k + "=" + v + "\n";
                    flat[Lower(s.name) + "\n" + Lower(k)] = v;
                }
            }
            const bool changed = flat != g_lastMerged;
            g_lastMerged = std::move(flat);
            if (text != g_lastText) {
                const std::string tmp = g_effTarget + ".tmp";
                bool ok = false;
                {
                    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
                    if (f) {
                        f << text;
                        f.close();
                        ok = !f.fail();
                    }
                }
                if (ok && !::MoveFileExA(tmp.c_str(), g_effTarget.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                    g_lastWriteError = std::format("MoveFileEx {}", ::GetLastError());
                    std::ofstream f(g_effTarget, std::ios::binary | std::ios::trunc);  // fallback: in place
                    ok = static_cast<bool>(f);
                    if (ok) {
                        f << text;
                        f.close();
                        ok = !f.fail();
                    }
                    std::error_code ec;
                    std::filesystem::remove(tmp, ec);
                }
                if (ok) {
                    g_lastText = text;
                    ++g_writes;
                    if (!g_effOk) {
                        g_effOk = true;
                        g_eff = g_effTarget;
                    }
                } else {
                    ++g_writeFails;
                    if (g_lastWriteError.empty()) g_lastWriteError = "write failed";
                    REX::WARN("cfgstore: could not write {} ({}); modules read {} directly", g_effTarget, g_lastWriteError, g_user);
                }
            }
            if (changed) ++g_rev;
            return changed;
        }

        void ReadAll()  // lock held
        {
            g_userL = ReadLayer(g_user);
            g_mcmL = ReadLayer(g_mcm);
            // sends the file now shows (or that waited too long) leave the send list
            const auto now = Clock::now();
            std::erase_if(g_sent, [&](const Sent& s) {
                const std::string* v = g_mcmL.Get(s.sec, s.key);
                if (v && SameValue(*v, s.val)) {
                    ++g_confirmed;
                    return true;
                }
                if (now - s.at > std::chrono::seconds(5)) {
                    // MCM did not write it: write the line ourselves (MCM's menu may show its old value until a restart)
                    std::error_code ec;
                    std::filesystem::create_directories(std::filesystem::path(g_mcm).parent_path(), ec);
                    const bool ok = ::WritePrivateProfileStringA(s.sec.c_str(), s.key.c_str(), s.val.c_str(), g_mcm.c_str()) != 0;
                    ++g_fallbacks;
                    REX::WARN("cfgstore: MCM did not write {}:{}={} within 5 s; wrote the line directly ({})", s.key, s.sec, s.val, ok ? "ok" : "FAILED");
                    if (ok) g_mcmL.Set(s.sec, s.key, s.val);
                    return true;
                }
                return false;
            });
        }
    }

    const std::string& UserPath()
    {
        Init();
        return g_user;
    }
    const std::string& McmPath()
    {
        Init();
        return g_mcm;
    }
    const std::string& EffectivePath()
    {
        Init();
        return g_eff;
    }

    void Init()
    {
        std::scoped_lock l(g_lock);
        if (g_init) return;
        g_init = true;
        g_user = std::filesystem::absolute("Data/F4SE/Plugins/fo4flatvr.ini").string();
        g_mcm = std::filesystem::absolute("Data/MCM/Settings/fo4flatvr.ini").string();
        g_effTarget = std::filesystem::absolute("Data/F4SE/Plugins/fo4flatvr.effective.ini").string();
        g_eff = g_user;  // until the merged file exists
        Stamp(g_user, g_userT, g_userEx);
        Stamp(g_mcm, g_mcmT, g_mcmEx);
        ReadAll();
        WriteMerged();
        REX::INFO("cfgstore: settings layers fo4flatvr.ini ({} keys{}) < MCM {} ({} keys{}) -> {} ({})", g_userL.Count(),
            g_userEx ? "" : ", MISSING", g_mcm, g_mcmL.Count(), g_mcmEx ? "" : ", none yet", g_eff,
            g_effOk ? "merged file" : "merge failed: the user ini is read directly");
        g_nextPoll = Clock::now() + std::chrono::seconds(1);
    }

    bool Poll()
    {
        Init();
        std::scoped_lock l(g_lock);
        const auto now = Clock::now();
        if (now < g_nextPoll) return false;
        g_nextPoll = now + std::chrono::seconds(1);
        const bool a = Stamp(g_user, g_userT, g_userEx);
        const bool b = Stamp(g_mcm, g_mcmT, g_mcmEx);
        if (!a && !b && g_sent.empty()) return false;
        ReadAll();
        const bool changed = WriteMerged();
        if (changed && (a || b)) {
            REX::INFO("cfgstore: {} changed -> merged values changed (revision {})", a && b ? "fo4flatvr.ini + MCM file" : a ? "fo4flatvr.ini" : "MCM file",
                g_rev);
        }
        return changed;
    }

    std::uint64_t Revision()
    {
        std::scoped_lock l(g_lock);
        return g_rev;
    }

    bool UserValue(const std::string& a_sec, const std::string& a_key, std::string& a_out)
    {
        Init();
        std::scoped_lock l(g_lock);
        if (const auto* v = g_userL.Get(a_sec, a_key)) {
            a_out = *v;
            return true;
        }
        return false;
    }

    bool McmValue(const std::string& a_sec, const std::string& a_key, std::string& a_out)
    {
        Init();
        std::scoped_lock l(g_lock);
        for (auto it = g_sent.rbegin(); it != g_sent.rend(); ++it) {
            if (Lower(it->sec) == Lower(a_sec) && Lower(it->key) == Lower(a_key)) {
                a_out = it->val;
                return true;
            }
        }
        if (const auto* v = g_mcmL.Get(a_sec, a_key)) {
            a_out = *v;
            return true;
        }
        return false;
    }

    bool EffectiveValue(const std::string& a_sec, const std::string& a_key, std::string& a_out)
    {
        Init();
        {
            std::scoped_lock l(g_lock);
            if (const auto* v = g_prevL.Get(a_sec, a_key)) {
                a_out = *v;
                return true;
            }
        }
        return McmValue(a_sec, a_key, a_out) || UserValue(a_sec, a_key, a_out);
    }

    void SetPreview(const std::string& a_sec, const std::string& a_key, const std::string& a_val)
    {
        std::scoped_lock l(g_lock);
        g_prevL.Set(a_sec, a_key, a_val);
    }
    void ClearPreview(const std::string& a_sec, const std::string& a_key)
    {
        std::scoped_lock l(g_lock);
        g_prevL.Erase(a_sec, a_key);
    }
    void ClearAllPreviews()
    {
        std::scoped_lock l(g_lock);
        g_prevL.secs.clear();
    }
    std::size_t PreviewCount()
    {
        std::scoped_lock l(g_lock);
        return g_prevL.Count();
    }
    void Rebuild()
    {
        Init();
        std::scoped_lock l(g_lock);
        WriteMerged();
    }

    bool McmLoaded()
    {
        if (!::GetModuleHandleA("mcm.dll")) return false;
        auto* game = RE::GameVM::GetSingleton();
        return game && game->GetVM();
    }

    bool Save(const std::string& a_sec, const std::string& a_key, const std::string& a_val, VType a_type, std::string& a_how)
    {
        Init();
        if (McmLoaded()) {
            auto* game = RE::GameVM::GetSingleton();
            auto  vm = game ? game->GetVM() : nullptr;
            if (vm) {
                const RE::BSFixedString mod("fo4flatvr");
                const RE::BSFixedString setting(std::format("{}:{}", a_key, a_sec));
                const RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> cb;
                bool ok = false;
                switch (a_type) {
                case VType::kBool:
                    ok = vm->DispatchStaticCall(RE::BSFixedString("MCM"), RE::BSFixedString("SetModSettingBool"), cb, mod, setting,
                        a_val != "0" && Lower(a_val) != "false");
                    break;
                case VType::kInt:
                    ok = vm->DispatchStaticCall(RE::BSFixedString("MCM"), RE::BSFixedString("SetModSettingInt"), cb, mod, setting,
                        static_cast<std::int32_t>(std::strtol(a_val.c_str(), nullptr, 0)));
                    break;
                case VType::kFloat:
                    ok = vm->DispatchStaticCall(RE::BSFixedString("MCM"), RE::BSFixedString("SetModSettingFloat"), cb, mod, setting,
                        std::strtof(a_val.c_str(), nullptr));
                    break;
                default:
                    ok = vm->DispatchStaticCall(RE::BSFixedString("MCM"), RE::BSFixedString("SetModSettingString"), cb, mod, setting,
                        RE::BSFixedString(a_val));
                    break;
                }
                if (ok) {
                    std::scoped_lock l(g_lock);
                    std::erase_if(g_sent, [&](const Sent& s) { return Lower(s.sec) == Lower(a_sec) && Lower(s.key) == Lower(a_key); });
                    g_sent.push_back({ a_sec, a_key, a_val, Clock::now() });
                    ++g_mcmSends;
                    a_how = "MCM API";
                    g_nextPoll = Clock::now() + std::chrono::milliseconds(300);
                    return true;
                }
                REX::WARN("cfgstore: MCM.SetModSetting for {}:{} could not be dispatched; writing the line", a_key, a_sec);
            }
        }
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(McmPath()).parent_path(), ec);
        const bool ok = ::WritePrivateProfileStringA(a_sec.c_str(), a_key.c_str(), a_val.c_str(), McmPath().c_str()) != 0;
        {
            std::scoped_lock l(g_lock);
            if (ok) {
                g_mcmL.Set(a_sec, a_key, a_val);
                ++g_lineWrites;
                g_nextPoll = Clock::now();
            }
        }
        a_how = ok ? "ini line (MCM not loaded)" : std::format("ini line FAILED ({})", ::GetLastError());
        return ok;
    }

    bool RemoveOverride(const std::string& a_sec, const std::string& a_key)
    {
        Init();
        if (McmLoaded()) return false;
        std::string cur;
        {
            std::scoped_lock l(g_lock);
            if (!g_mcmL.Get(a_sec, a_key)) return true;  // nothing to remove
        }
        const bool ok = ::WritePrivateProfileStringA(a_sec.c_str(), a_key.c_str(), nullptr, McmPath().c_str()) != 0;
        if (ok) {
            std::scoped_lock l(g_lock);
            g_mcmL.Erase(a_sec, a_key);
            g_nextPoll = Clock::now();
        }
        return ok;
    }

    std::string Status()
    {
        Init();
        const bool mcm = McmLoaded();
        std::scoped_lock l(g_lock);
        return std::format("user={} ({} keys{}) mcm={} ({} keys{}) effective={} ({}) previews={} sentPending={} rev={} writes={} writeFails={} "
                           "mcmApi={} mcmSends={} confirmed={} fallbacks={} lineWrites={}{}",
            g_user, g_userL.Count(), g_userEx ? "" : " MISSING", g_mcm, g_mcmL.Count(), g_mcmEx ? "" : " none", g_eff,
            g_effOk ? "merged" : "user ini directly", g_prevL.Count(), g_sent.size(), g_rev, g_writes, g_writeFails, mcm ? 1 : 0, g_mcmSends,
            g_confirmed, g_fallbacks, g_lineWrites, g_lastWriteError.empty() ? "" : " lastWriteError=" + g_lastWriteError);
    }
    // ---- 20 Nov (live settings): change notifications ----
    namespace
    {
        struct Sub
        {
            std::string                           name;
            std::vector<std::string>              watch;  // lower: "sec" or "sec" + newline + "key"
            std::function<void(const Changes&)>   fn;
            On                                    on{ On::kPresent };
            std::uint64_t                         runs{ 0 };
        };
        std::mutex                         g_subLock;
        std::vector<Sub>                   g_subs;
        std::map<std::string, std::string> g_dispMerged;  // merged values as of the last dispatch
        bool                               g_dispInit{ false };
        std::uint64_t                      g_dispRev{ 0 }, g_dispatches{ 0 }, g_dispKeys{ 0 };
        std::string                        g_lastDispatch{ "-" };

        bool Matches(const Sub& a_s, const Changes& a_ch)
        {
            for (const auto& w : a_s.watch) {
                const bool whole = w.find('\n') == std::string::npos;
                for (const auto& k : a_ch.keys) {
                    if (whole ? (k.size() > w.size() && k.compare(0, w.size(), w) == 0 && k[w.size()] == '\n') : k == w) return true;
                }
            }
            return false;
        }
    }

    bool Changes::Has(const char* a_sec, const char* a_key) const
    {
        const auto k = Lower(a_sec) + "\n" + Lower(a_key);
        return std::find(keys.begin(), keys.end(), k) != keys.end();
    }

    bool Changes::Any(const char* a_sec) const
    {
        const auto p = Lower(a_sec) + "\n";
        for (const auto& k : keys)
            if (k.compare(0, p.size(), p) == 0) return true;
        return false;
    }

    void Subscribe(const char* a_name, const char* a_watch, std::function<void(const Changes&)> a_fn, On a_on)
    {
        Sub s{ a_name ? a_name : "?", {}, std::move(a_fn), a_on };
        std::string w = a_watch ? a_watch : "";
        std::size_t i = 0;
        while (i <= w.size()) {
            auto e = w.find(',', i);
            if (e == std::string::npos) e = w.size();
            auto t = Trim(w.substr(i, e - i));
            if (!t.empty()) {
                if (const auto sl = t.find('/'); sl != std::string::npos) t = Trim(t.substr(0, sl)) + "\n" + Trim(t.substr(sl + 1));
                s.watch.push_back(Lower(t));
            }
            i = e + 1;
        }
        std::scoped_lock l(g_subLock);
        g_subs.push_back(std::move(s));
    }

    void DispatchChanges()
    {
        Changes ch;
        {
            std::scoped_lock l(g_lock);
            if (!g_dispInit) {
                g_dispInit = true;
                g_dispMerged = g_lastMerged;
                g_dispRev = g_rev;
                return;
            }
            if (g_rev == g_dispRev) return;
            g_dispRev = g_rev;
            for (const auto& [k, v] : g_lastMerged) {
                const auto it = g_dispMerged.find(k);
                if (it == g_dispMerged.end() || !SameValue(it->second, v)) ch.keys.push_back(k);
            }
            for (const auto& [k, v] : g_dispMerged)
                if (!g_lastMerged.contains(k)) ch.keys.push_back(k);
            g_dispMerged = g_lastMerged;
        }
        if (ch.Empty()) return;
        ++g_dispatches;
        g_dispKeys += ch.keys.size();
        std::string keys, names;
        for (std::size_t i = 0; i < ch.keys.size() && i < 12; ++i) {
            auto k = ch.keys[i];
            std::replace(k.begin(), k.end(), '\n', '/');
            keys += (i ? ", " : "") + k;
        }
        if (ch.keys.size() > 12) keys += std::format(" (+{})", ch.keys.size() - 12);
        std::vector<std::pair<std::function<void(const Changes&)>, On>> run;
        {
            std::scoped_lock l(g_subLock);
            for (auto& s : g_subs) {
                if (!Matches(s, ch)) continue;
                ++s.runs;
                names += (names.empty() ? "" : ", ") + s.name + (s.on == On::kMain ? " (main)" : "");
                run.emplace_back(s.fn, s.on);
            }
        }
        g_lastDispatch = keys + " -> " + (names.empty() ? std::string("no module") : names);
        REX::INFO("cfgstore: live change {}", g_lastDispatch);
        for (auto& [fn, on] : run) {
            if (on == On::kMain) {
                if (!tq::RunOnMain([fn, ch]() { fn(ch); })) fn(ch);
            } else {
                try {
                    fn(ch);
                } catch (const std::exception& e) {
                    REX::ERROR("cfgstore: reload threw: {}", e.what());
                }
            }
        }
    }

    std::map<std::string, std::string> MergedValues()
    {
        Init();
        std::scoped_lock l(g_lock);
        return g_lastMerged;
    }

    std::string SubscribersStatus()
    {
        std::scoped_lock l(g_subLock);
        std::string s = std::format("dispatches={} keys={} last: {} | subscribers {}:", g_dispatches, g_dispKeys, g_lastDispatch, g_subs.size());
        for (const auto& x : g_subs) s += std::format(" {}{}={}", x.name, x.on == On::kMain ? "(main)" : "", x.runs);
        return s;
    }
}
