// Generic Mods Config FO4 VR spec.json pages (see SpecPages.h).
#include <F4SE/F4SE.h>  // CommonLib before any Windows header
#include <RE/Fallout.h>

#include <REX/LOG.h>

#include "plugin/SpecPages.h"

#include "plugin/FrikBridge.h"
#include "plugin/FrikBridgeApi.h"

#include <Windows.h>
#undef ERROR
#include <ShlObj.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace fo4flatvr::specpages
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
            const auto a = s.find_first_not_of(" \t\r\n");
            if (a == std::string::npos) return {};
            const auto b = s.find_last_not_of(" \t\r\n");
            return s.substr(a, b - a + 1);
        }
        bool Num(const std::string& s, double& v)
        {
            const std::string t = Trim(s);
            if (t.empty()) return false;
            char* e = nullptr;
            v = std::strtod(t.c_str(), &e);
            return e && *e == 0;
        }
        bool SameValue(const std::string& a, const std::string& b)
        {
            double x, y;
            if (Num(a, x) && Num(b, y)) return std::abs(x - y) < 1e-4;
            return Lower(Trim(a)) == Lower(Trim(b));
        }

        // ---- a small JSON reader (spec.json is plain JSON) ----
        struct J
        {
            enum class T
            {
                kNull,
                kBool,
                kNum,
                kStr,
                kArr,
                kObj
            } t{ T::kNull };
            bool                                 b{ false };
            std::string                          s;  // string, or a number's own text
            std::vector<J>                       a;
            std::vector<std::pair<std::string, J>> o;
            const J* get(const char* k) const
            {
                if (t != T::kObj) return nullptr;
                for (const auto& [n, v] : o)
                    if (n == k) return &v;
                return nullptr;
            }
            std::string str(const char* k, const char* d = "") const
            {
                const J* v = get(k);
                return v && v->t == T::kStr ? v->s : std::string(d);
            }
            bool flag(const char* k) const
            {
                const J* v = get(k);
                return v && v->t == T::kBool && v->b;
            }
            double num(const char* k, double d) const
            {
                const J* v = get(k);
                return v && v->t == T::kNum ? std::strtod(v->s.c_str(), nullptr) : d;
            }
            // a value as the panel keeps it: "true"/"false", the number's text, the string
            std::string text() const
            {
                switch (t) {
                case T::kBool: return b ? "true" : "false";
                case T::kNum:
                case T::kStr: return s;
                default: return {};
                }
            }
        };
        struct Parser
        {
            const std::string& in;
            std::size_t        p{ 0 };
            bool               ok{ true };
            void ws()
            {
                while (p < in.size() && (in[p] == ' ' || in[p] == '\t' || in[p] == '\r' || in[p] == '\n')) ++p;
            }
            void put(std::string& out, unsigned cp)
            {
                if (cp < 0x80) out += static_cast<char>(cp);
                else if (cp < 0x800) out += static_cast<char>(0xC0 | (cp >> 6)), out += static_cast<char>(0x80 | (cp & 0x3F));
                else if (cp < 0x10000)
                    out += static_cast<char>(0xE0 | (cp >> 12)), out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)), out += static_cast<char>(0x80 | (cp & 0x3F));
                else
                    out += static_cast<char>(0xF0 | (cp >> 18)), out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)),
                        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)), out += static_cast<char>(0x80 | (cp & 0x3F));
            }
            std::string string()
            {
                std::string out;
                ++p;  // "
                while (p < in.size() && in[p] != '"') {
                    char c = in[p++];
                    if (c != '\\') {
                        out += c;
                        continue;
                    }
                    if (p >= in.size()) break;
                    c = in[p++];
                    switch (c) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'u': {
                        unsigned cp = p + 4 <= in.size() ? static_cast<unsigned>(std::strtoul(in.substr(p, 4).c_str(), nullptr, 16)) : 0;
                        p += 4;
                        if (cp >= 0xD800 && cp < 0xDC00 && p + 6 <= in.size() && in[p] == '\\' && in[p + 1] == 'u') {
                            const unsigned lo = static_cast<unsigned>(std::strtoul(in.substr(p + 2, 4).c_str(), nullptr, 16));
                            p += 6;
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        }
                        put(out, cp);
                        break;
                    }
                    default: out += c;
                    }
                }
                ++p;  // "
                return out;
            }
            J value(int depth = 0)
            {
                J v;
                ws();
                if (p >= in.size() || depth > 64) {
                    ok = false;
                    return v;
                }
                const char c = in[p];
                if (c == '{') {
                    v.t = J::T::kObj;
                    ++p;
                    ws();
                    if (p < in.size() && in[p] == '}') {
                        ++p;
                        return v;
                    }
                    while (ok && p < in.size()) {
                        ws();
                        if (p >= in.size() || in[p] != '"') {
                            ok = false;
                            break;
                        }
                        std::string k = string();
                        ws();
                        if (p >= in.size() || in[p] != ':') {
                            ok = false;
                            break;
                        }
                        ++p;
                        v.o.emplace_back(std::move(k), value(depth + 1));
                        ws();
                        if (p < in.size() && in[p] == ',') {
                            ++p;
                            continue;
                        }
                        if (p < in.size() && in[p] == '}') {
                            ++p;
                            break;
                        }
                        ok = false;
                    }
                } else if (c == '[') {
                    v.t = J::T::kArr;
                    ++p;
                    ws();
                    if (p < in.size() && in[p] == ']') {
                        ++p;
                        return v;
                    }
                    while (ok && p < in.size()) {
                        v.a.push_back(value(depth + 1));
                        ws();
                        if (p < in.size() && in[p] == ',') {
                            ++p;
                            continue;
                        }
                        if (p < in.size() && in[p] == ']') {
                            ++p;
                            break;
                        }
                        ok = false;
                    }
                } else if (c == '"') {
                    v.t = J::T::kStr;
                    v.s = string();
                } else if (in.compare(p, 4, "true") == 0) {
                    v.t = J::T::kBool, v.b = true, p += 4;
                } else if (in.compare(p, 5, "false") == 0) {
                    v.t = J::T::kBool, v.b = false, p += 5;
                } else if (in.compare(p, 4, "null") == 0) {
                    p += 4;
                } else {
                    const std::size_t a = p;
                    while (p < in.size() && (std::isdigit(static_cast<unsigned char>(in[p])) || in[p] == '-' || in[p] == '+' || in[p] == '.' ||
                                                in[p] == 'e' || in[p] == 'E'))
                        ++p;
                    if (p == a) {
                        ok = false;
                        return v;
                    }
                    v.t = J::T::kNum;
                    v.s = in.substr(a, p - a);
                }
                return v;
            }
        };

        // ---- paths ----
        std::string Documents()
        {
            PWSTR       docs = nullptr;
            std::string out;
            if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs)) && docs) {
                out = std::filesystem::path(docs).string();
            }
            if (docs) ::CoTaskMemFree(docs);
            return out;
        }
        std::string DataDir()
        {
            char exe[MAX_PATH]{};
            ::GetModuleFileNameA(nullptr, exe, MAX_PATH);
            return (std::filesystem::path(exe).parent_path() / "Data").string();
        }
        std::string FrikConfigFile()
        {
            const auto* api = frikbridge::ConfigApi();
            if (!api || !api->ConfigFile) return {};
            char buf[1024]{};
            api->ConfigFile(buf, sizeof(buf));
            return buf;
        }

        // ---- TOML (flat key = value tables; enough for these specs: a [section] and a scalar key) ----
        struct TomlCache
        {
            std::filesystem::file_time_type    t{};
            std::map<std::string, std::string> v;  // "sec\nkey" lower -> value text (strings unquoted)
            bool                               have{ false };
        };
        std::map<std::string, TomlCache> g_toml;

        // "key = value  # comment" -> key, value text (as written), where the value starts / ends
        bool TomlLine(const std::string& a_line, std::string& a_key, std::string& a_val, std::size_t& a_vs, std::size_t& a_ve)
        {
            const std::string t = Trim(a_line);
            if (t.empty() || t[0] == '#' || t[0] == '[') return false;
            const auto eq = a_line.find('=');
            if (eq == std::string::npos) return false;
            a_key = Trim(a_line.substr(0, eq));
            if (a_key.size() >= 2 && (a_key.front() == '"' || a_key.front() == '\'')) a_key = a_key.substr(1, a_key.size() - 2);
            std::size_t vs = a_line.find_first_not_of(" \t", eq + 1);
            if (vs == std::string::npos) vs = a_line.size();
            std::size_t ve = vs;
            if (vs < a_line.size() && (a_line[vs] == '"' || a_line[vs] == '\'')) {
                const char q = a_line[vs];
                ve = vs + 1;
                while (ve < a_line.size() && a_line[ve] != q) ve += (a_line[ve] == '\\' && q == '"') ? 2 : 1;
                ve = (std::min)(ve + 1, a_line.size());
            } else {
                while (ve < a_line.size() && a_line[ve] != '#') ++ve;
                while (ve > vs && (a_line[ve - 1] == ' ' || a_line[ve - 1] == '\t' || a_line[ve - 1] == '\r')) --ve;
            }
            a_val = a_line.substr(vs, ve - vs);
            a_vs = vs, a_ve = ve;
            return true;
        }
        std::string Unquote(const std::string& v)
        {
            if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front()) return v.substr(1, v.size() - 2);
            return v;
        }
        std::string TableName(const std::string& t)  // "[ a ]" -> a; "" for [[arrays]] / not a header
        {
            if (t.size() < 2 || t[0] != '[' || t[1] == '[') return {};
            const auto e = t.find(']');
            return e == std::string::npos ? std::string() : Trim(t.substr(1, e - 1));
        }
        std::vector<std::string> Lines(const std::string& a_text)
        {
            std::vector<std::string> out;
            std::string              cur;
            std::istringstream       is(a_text);
            while (std::getline(is, cur)) out.push_back(cur);
            return out;
        }
        std::string ReadAll(const std::string& a_path)
        {
            std::ifstream f(a_path, std::ios::binary);
            std::ostringstream ss;
            ss << f.rdbuf();
            return ss.str();
        }
        const TomlCache& Toml(const std::string& a_path)
        {
            auto&           c = g_toml[Lower(a_path)];
            std::error_code ec;
            const auto      t = std::filesystem::last_write_time(a_path, ec);
            if (ec) {
                c = {};
                return c;
            }
            if (c.have && t == c.t) return c;
            c = {};
            c.t = t, c.have = true;
            std::string sec;
            for (const auto& l : Lines(ReadAll(a_path))) {
                const std::string tl = Trim(l);
                if (!tl.empty() && tl[0] == '[') {
                    sec = TableName(tl);
                    continue;
                }
                std::string k, v;
                std::size_t a = 0, b = 0;
                if (TomlLine(l, k, v, a, b)) c.v[Lower(sec) + "\n" + Lower(k)] = Unquote(v);
            }
            return c;
        }
        bool TomlWrite(const std::string& a_path, const std::vector<std::pair<std::pair<std::string, std::string>, std::string>>& a_vals,
            std::string& a_why)
        {
            const std::string text = ReadAll(a_path);
            if (text.empty()) {
                a_why = "file empty or missing";
                return false;
            }
            const bool               crlf = text.find("\r\n") != std::string::npos;
            std::vector<std::string> L = Lines(text);
            for (auto& l : L)
                if (!l.empty() && l.back() == '\r') l.pop_back();
            for (const auto& [sk, val] : a_vals) {
                const std::string lsec = Lower(sk.first), lkey = Lower(sk.second);
                std::string       sec;
                int               secEnd = -1, secStart = -1;
                bool              done = false;
                for (int i = 0; i < static_cast<int>(L.size()) && !done; ++i) {
                    const std::string tl = Trim(L[i]);
                    if (!tl.empty() && tl[0] == '[') {
                        if (Lower(sec) == lsec && secStart >= 0) secEnd = i;
                        sec = TableName(tl);
                        if (Lower(sec) == lsec) secStart = i, secEnd = -1;
                        continue;
                    }
                    std::string k, v;
                    std::size_t a = 0, b = 0;
                    if (Lower(sec) == lsec && TomlLine(L[i], k, v, a, b) && Lower(k) == lkey) {
                        const bool quoted = !v.empty() && (v.front() == '"' || v.front() == '\'');
                        L[i] = L[i].substr(0, a) + (quoted ? "\"" + val + "\"" : val) + L[i].substr(b);
                        done = true;
                    }
                }
                if (done) continue;
                double d;
                const bool        bare = Num(val, d) || val == "true" || val == "false";
                const std::string line = sk.second + " = " + (bare ? val : "\"" + val + "\"");
                if (secStart >= 0) {
                    const int at = secEnd >= 0 ? secEnd : static_cast<int>(L.size());
                    int       ins = at;
                    while (ins > secStart + 1 && Trim(L[ins - 1]).empty()) --ins;
                    L.insert(L.begin() + ins, line);
                } else {
                    L.push_back("");
                    L.push_back("[" + sk.first + "]");
                    L.push_back(line);
                }
            }
            std::string out;
            for (const auto& l : L) out += l + (crlf ? "\r\n" : "\n");
            const std::string tmp = a_path + ".tmp-fo4flatvr";
            {
                std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
                if (!f) {
                    a_why = "cannot write " + tmp;
                    return false;
                }
                f << out;
            }
            if (!::MoveFileExA(tmp.c_str(), a_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                a_why = std::format("MoveFileEx {}", ::GetLastError());
                return false;
            }
            return true;
        }

        // ---- state ----
        bool                               g_loaded{ false };
        std::vector<Mod>                   g_mods;
        int                                g_frik{ -1 };
        std::map<std::string, std::string> g_pending;  // row id -> value
        std::set<std::string>              g_dirty;    // FRIK-table rows: preview / discard not sent yet
        std::map<std::string, std::string> g_saved;    // row id -> value outside our previews
        std::map<std::string, std::pair<int, const Row*>> g_rowById;
        Clock::time_point                  g_nextRefresh{}, g_nextFlush{};
        std::uint64_t                      g_sent{ 0 }, g_saves{ 0 };

        const std::map<std::string, std::string>& FrikFlatNotes()
        {
            static const std::map<std::string, std::string> m = {
                { "fvrscale", "No effect on flat: the game has no fVrScale (fo4flatvr's world scale is Height > units per metre)." },
                { "setscale", "No effect on flat: the game has no fVrScale." },
                { "playerheight", "FRIK-flat: the recentre calibration (C36) replaces it once you recentre; Game's default scale ignores it." },
                { "selfieoutfrontdistance", "FRIK's own selfie toggle only; the hands-up selfie uses Selfie > Distance." },
                { "pipboyopenwhenlookat", "FRIK's wrist Pip-Boy opens the game's Pip-Boy only with FRIK > fo4flatvr > FRIK's wrist Pip-Boy opening on." },
            };
            return m;
        }

        std::string CondOf(const J* c, bool& a_never, const std::map<std::string, std::string>& a_client)
        {
            if (!c || c->t != J::T::kObj) return {};
            if (const J* all = c->get("all"); all && all->t == J::T::kArr) {
                std::string out;
                for (const auto& x : all->a) {
                    const std::string t = CondOf(&x, a_never, a_client);
                    if (!t.empty()) out += (out.empty() ? "" : "&") + t;
                }
                return out;
            }
            if (const J* n = c->get("not")) {
                bool              never = false;
                const std::string t = CondOf(n, never, a_client);
                if (never) return {};  // not(false) = true
                if (t.empty()) {
                    a_never = true;  // not(true)
                    return {};
                }
                if (t.find('&') != std::string::npos) return {};  // not(a & b): not used by the specs here; shown
                return t[0] == '!' ? t.substr(1) : "!" + t;
            }
            if (const J* st = c->get("setting"); st && st->t == J::T::kStr) {
                for (const auto& [op, sym] : { std::pair{ "equals", "==" }, { "notEquals", "!=" }, { "greaterThan", ">" }, { "lessThan", "<" } })
                    if (const J* v = c->get(op)) return std::format("S:{}{}{}", st->s, sym, v->text());
                return {};
            }
            if (const J* cs = c->get("clientState"); cs && cs->t == J::T::kStr) {
                const auto it = a_client.find(cs->s);
                if (it == a_client.end()) {
                    a_never = true;
                    return {};
                }
                const J* eq = c->get("equals");
                const J* ne = c->get("notEquals");
                const bool ok = eq ? SameValue(it->second, eq->text()) : ne ? !SameValue(it->second, ne->text()) : true;
                if (!ok) a_never = true;
                return {};
            }
            if (const J* fp = c->get("f4sePlugin"); fp && fp->t == J::T::kStr) return "P:" + fp->s;
            if (const J* pl = c->get("plugin"); pl && pl->t == J::T::kStr) return "E:" + pl->s;
            return {};
        }

        void Walk(Mod& m, Page& pg, const J& items, std::vector<std::pair<std::string, bool>> groups, const std::string& vis,
            const std::string& ena, const std::string& appliesDefault, const std::map<std::string, std::string>& client, int defFile)
        {
            if (items.t != J::T::kArr) return;
            for (const auto& it : items.a) {
                const std::string t = it.str("type");
                if (t == "text" || t == "image" || t == "spacer" || t.empty()) continue;
                bool              never = false;
                const std::string cv = CondOf(it.get("visibleWhen"), never, client);
                const std::string ce = CondOf(it.get("enabledWhen"), never, client);
                if (never) {
                    m.skipped.push_back(std::format("{} / {} (condition never true on flat)", pg.name, it.str("text")));
                    continue;
                }
                const std::string v = vis + (vis.empty() || cv.empty() ? "" : "&") + cv;
                const std::string e = ena + (ena.empty() || ce.empty() ? "" : "&") + ce;
                if (t == "section") {
                    auto g = groups;
                    g.emplace_back(it.str("text"), it.flag("isCollapsible"));
                    if (const J* c = it.get("content")) Walk(m, pg, *c, g, v, e, appliesDefault, client, defFile);
                    continue;
                }
                Row r;
                for (const auto& [gn, gc] : groups) r.group += (r.group.empty() ? "" : " / ") + gn;
                r.collapsible = !groups.empty() && groups.back().second;
                r.label = it.str("text");
                r.help = it.str("help");
                r.visible = v, r.enabled = e;
                r.restart = it.str("applies", appliesDefault.c_str()) == "restart";
                if (t == "button") {
                    const J* act = it.get("action");
                    if (!act || act->str("type") != "CallNativeFunction") {
                        m.skipped.push_back(std::format("{} / {} (button {})", pg.name, r.label, act ? act->str("type") : "?"));
                        continue;
                    }
                    r.type = Type::kAction;
                    r.action = act->str("function");
                    r.id = Lower(m.name + "\n\naction\n" + r.action);
                    pg.rows.push_back(std::move(r));
                    continue;
                }
                const J* st = it.get("storage");
                const J* path = st ? st->get("path") : nullptr;
                if (!path || path->t != J::T::kArr || path->a.size() != 2 || path->a[0].t != J::T::kStr || path->a[1].t != J::T::kStr) {
                    m.skipped.push_back(std::format("{} / {} ({}, not a [section] key of a file)", pg.name, r.label, t));
                    continue;
                }
                r.file = defFile;
                if (const std::string fn = st->str("file"); !fn.empty()) {
                    r.file = -1;
                    for (int i = 0; i < static_cast<int>(m.files.size()); ++i)
                        if (m.files[i].name == fn) r.file = i;
                }
                if (r.file < 0) {
                    m.skipped.push_back(std::format("{} / {} (file not found)", pg.name, r.label));
                    continue;
                }
                r.section = path->a[0].s, r.key = path->a[1].s;
                if (const J* d = it.get("default")) r.def = d->text();
                const J* vo = it.get("valueOptions");
                if (const J* disp = it.get("display")) {
                    r.decimals = static_cast<int>(disp->num("decimals", -1));
                    r.unit = disp->str("unit");
                }
                if (t == "switcher" || t == "checkbox" || t == "toggle") {
                    r.type = Type::kBool;
                } else if (t == "slider") {
                    r.type = it.str("valueType") == "int" ? Type::kInt : Type::kFloat;
                    if (vo) r.mn = static_cast<float>(vo->num("min", 0)), r.mx = static_cast<float>(vo->num("max", 1)), r.step = static_cast<float>(vo->num("step", 0));
                } else if (t == "stepper" || t == "dropdown") {
                    r.type = Type::kChoice;
                    const J* opts = vo ? vo->get("options") : nullptr;
                    if (opts && opts->t == J::T::kArr)
                        for (const auto& o : opts->a)
                            if (const J* ov = o.get("value")) r.options.push_back({ ov->text(), o.str("text"), o.str("help") });
                    if (r.options.empty()) {
                        m.skipped.push_back(std::format("{} / {} ({} without fixed options)", pg.name, r.label, t));
                        continue;
                    }
                } else {
                    m.skipped.push_back(std::format("{} / {} ({})", pg.name, r.label, t));
                    continue;
                }
                if (m.isFrik) {
                    if (const auto n = FrikFlatNotes().find(Lower(r.key)); n != FrikFlatNotes().end()) r.flatNote = n->second;
                }
                r.id = Lower(std::format("{}\n{}\n{}\n{}", m.name, m.files[r.file].name, r.section, r.key));
                pg.rows.push_back(std::move(r));
            }
        }

        void LoadSpec(const std::filesystem::path& a_spec)
        {
            const std::string text = ReadAll(a_spec.string());
            Parser            ps{ text };
            const J           root = ps.value();
            if (!ps.ok || root.t != J::T::kObj) {
                REX::WARN("specpages: {} is not valid JSON (at byte {}) - skipped", a_spec.string(), ps.p);
                return;
            }
            Mod m;
            m.spec = a_spec.string();
            m.name = root.str("modName", a_spec.parent_path().filename().string().c_str());
            m.display = root.str("displayName", m.name.c_str());
            m.help = root.str("help");
            m.isFrik = Lower(m.name) == "frik";
            // client states the specs test, as they are on flat: FRIK-flat's Pip-Boy is always on the wrist (C13)
            std::map<std::string, std::string> client;
            if (m.isFrik) client["pipboyPlacement"] = "0";
            const std::string docsGame = (std::filesystem::path(Documents()) / "My Games" / "Fallout4").string();
            const std::string frikFile = m.isFrik ? FrikConfigFile() : std::string();
            if (const J* files = root.get("files"); files && files->t == J::T::kObj) {
                for (const auto& [fname, f] : files->o) {
                    File   fl;
                    fl.name = fname;
                    fl.format = Lower(f.str("format", "ini"));
                    const std::string rootName = f.str("root", "data");
                    std::filesystem::path p = (rootName == "myGames" ? std::filesystem::path(docsGame) : std::filesystem::path(DataDir())) /
                                              std::filesystem::path(f.str("path"));
                    if (m.isFrik) {
                        // FRIK-flat keeps FRIK_Config under My Games\Fallout4\FRIK-flat (never FO4VR's folder); the loaded file
                        // (Mods Config's clientState configFile) comes from FRIK.dll when its bridge table is there
                        p = (std::filesystem::path(docsGame) / "FRIK-flat" / std::filesystem::path(f.str("path")));
                        if (!f.str("pathFromClientState").empty() && !frikFile.empty()) p = frikFile;
                        fl.viaFrik = !frikFile.empty() && Lower(std::filesystem::path(p).lexically_normal().string()) ==
                                                              Lower(std::filesystem::path(frikFile).lexically_normal().string());
                    }
                    fl.path = p.lexically_normal().string();
                    m.files.push_back(std::move(fl));
                }
            }
            int defFile = 0;
            const std::string df = root.str("defaultFile");
            for (int i = 0; i < static_cast<int>(m.files.size()); ++i)
                if (m.files[i].name == df) defFile = i;
            if (m.files.empty()) {
                REX::WARN("specpages: {} has no files - skipped", m.spec);
                return;
            }
            const std::string appliesDefault = root.str("applies", "live");
            if (const J* pages = root.get("pages"); pages && pages->t == J::T::kArr) {
                for (const auto& pj : pages->a) {
                    Page pg;
                    pg.name = pj.str("pageDisplayName", pj.str("text").c_str());
                    pg.help = pj.str("help");
                    bool              never = false;
                    const std::string pv = CondOf(pj.get("visibleWhen"), never, client);
                    if (never) {
                        m.skipped.push_back(std::format("page {} (not on flat)", pg.name));
                        continue;
                    }
                    if (const J* c = pj.get("content")) Walk(m, pg, *c, {}, pv, "", appliesDefault, client, defFile);
                    if (!pg.rows.empty()) m.pages.push_back(std::move(pg));
                    else m.skipped.push_back(std::format("page {} (nothing the panel can edit)", pg.name));
                }
            }
            std::size_t n = 0;
            for (const auto& pg : m.pages) n += pg.rows.size();
            std::string files;
            for (const auto& f : m.files) files += std::format(" {}={} ({}{})", f.name, f.path, f.format, f.viaFrik ? ", via FRIK.dll" : "");
            REX::INFO("specpages: {} '{}': {} pages, {} rows, {} skipped |{}", m.spec, m.display, m.pages.size(), n, m.skipped.size(), files);
            for (const auto& s : m.skipped) REX::INFO("specpages:   {} skipped: {}", m.name, s);
            g_mods.push_back(std::move(m));
        }

        const File& FileOf(int a_mod, const Row& r) { return g_mods[a_mod].files[r.file]; }

        std::string ReadValue(int a_mod, const Row& r)
        {
            const File& f = FileOf(a_mod, r);
            if (f.viaFrik) {
                if (const auto* api = frikbridge::ConfigApi()) {
                    char v[512]{};
                    return api->Get(r.section.c_str(), r.key.c_str(), v, sizeof(v)) ? std::string(v) : r.def;
                }
            }
            if (f.format == "toml") {
                const auto& c = Toml(f.path);
                const auto  it = c.v.find(Lower(r.section) + "\n" + Lower(r.key));
                return it != c.v.end() ? it->second : r.def;
            }
            char buf[512]{};
            ::GetPrivateProfileStringA(r.section.c_str(), r.key.c_str(), r.def.c_str(), buf, sizeof(buf), f.path.c_str());
            return buf;
        }

        bool Ready()
        {
            Load();
            return true;
        }
    }

    void Load(bool a_force)
    {
        if (g_loaded && !a_force) return;
        g_loaded = true;
        g_mods.clear();
        g_rowById.clear();
        g_frik = -1;
        const std::filesystem::path dir = std::filesystem::path(DataDir()) / "ModsConfigFO4" / "Config";
        std::error_code             ec;
        std::vector<std::filesystem::path> specs;
        for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            const auto p = it->path() / "spec.json";
            if (std::filesystem::is_regular_file(p, ec)) specs.push_back(p);
        }
        std::sort(specs.begin(), specs.end());
        for (const auto& p : specs) LoadSpec(p);
        // FRIK first
        std::stable_sort(g_mods.begin(), g_mods.end(), [](const Mod& a, const Mod& b) { return a.isFrik && !b.isFrik; });
        for (int i = 0; i < static_cast<int>(g_mods.size()); ++i) {
            if (g_mods[i].isFrik && g_frik < 0) g_frik = i;
            for (const auto& pg : g_mods[i].pages)
                for (const auto& r : pg.rows) g_rowById[r.id] = { i, &r };
        }
        REX::INFO("specpages: {} Mods Config spec(s) in {}{}", g_mods.size(), dir.string(), g_frik >= 0 ? " (FRIK's first)" : "");
        g_nextRefresh = {};
    }

    const std::vector<Mod>& Mods()
    {
        Ready();
        return g_mods;
    }
    int FrikIndex()
    {
        Ready();
        return g_frik;
    }

    std::string Saved(int a_mod, const Row& r)
    {
        if (r.type == Type::kAction) return {};
        if (const auto it = g_saved.find(r.id); it != g_saved.end()) return it->second;
        const std::string v = ReadValue(a_mod, r);
        g_saved[r.id] = v;
        return v;
    }
    std::string Current(int a_mod, const Row& r)
    {
        if (const auto it = g_pending.find(r.id); it != g_pending.end()) return it->second;
        return Saved(a_mod, r);
    }
    bool IsPending(const Row& r) { return g_pending.contains(r.id); }
    bool Live(int a_mod, const Row& r) { return !r.restart && FileOf(a_mod, r).viaFrik && frikbridge::ConfigApi(); }

    void Set(int a_mod, const Row& r, const std::string& a_v)
    {
        if (r.type == Type::kAction || a_mod < 0 || a_mod >= static_cast<int>(g_mods.size())) return;
        if (SameValue(a_v, Saved(a_mod, r))) {
            if (g_pending.erase(r.id) && FileOf(a_mod, r).viaFrik) g_dirty.insert(r.id);
        } else {
            g_pending[r.id] = a_v;
            if (FileOf(a_mod, r).viaFrik) g_dirty.insert(r.id);
        }
    }

    bool Cond(int a_mod, const std::string& a_cond)
    {
        if (a_cond.empty()) return true;
        std::size_t s = 0;
        while (s <= a_cond.size()) {
            const auto        e = a_cond.find('&', s);
            std::string       t = a_cond.substr(s, e == std::string::npos ? std::string::npos : e - s);
            s = e == std::string::npos ? a_cond.size() + 1 : e + 1;
            const bool neg = !t.empty() && t[0] == '!';
            if (neg) t = t.substr(1);
            bool r = true;
            if (t.rfind("P:", 0) == 0) {
                r = ::GetModuleHandleA((t.substr(2) + ".dll").c_str()) != nullptr;
            } else if (t.rfind("E:", 0) == 0) {
                auto* dh = RE::TESDataHandler::GetSingleton();
                r = dh && dh->LookupModByName(t.substr(2)) != nullptr;
            } else if (t.rfind("S:", 0) == 0) {
                // S:Key:Section<op>value
                std::size_t opAt = std::string::npos;
                std::string op;
                for (const char* o : { "==", "!=", ">", "<" }) {
                    const auto at = t.find(o, 2);
                    if (at != std::string::npos && at < opAt) opAt = at, op = o;
                }
                if (opAt == std::string::npos) continue;
                const std::string ks = t.substr(2, opAt - 2), want = t.substr(opAt + op.size());
                const auto        colon = ks.find(':');
                const std::string key = ks.substr(0, colon), sec = colon == std::string::npos ? std::string() : ks.substr(colon + 1);
                std::string       cur;
                bool              found = false;
                for (const auto& pg : g_mods[a_mod].pages) {
                    for (const auto& rw : pg.rows)
                        if (rw.type != Type::kAction && Lower(rw.key) == Lower(key) && Lower(rw.section) == Lower(sec)) {
                            cur = Current(a_mod, rw);
                            found = true;
                            break;
                        }
                    if (found) break;
                }
                if (!found) continue;  // a key the panel does not show: no restriction
                double x = 0, y = 0;
                const bool num = Num(cur, x) && Num(want, y);
                if (op == "==") r = SameValue(cur, want);
                else if (op == "!=") r = !SameValue(cur, want);
                else if (op == ">") r = num && x > y;
                else r = num && x < y;
            }
            if (neg) r = !r;
            if (!r) return false;
        }
        return true;
    }

    void Tick(bool a_force)
    {
        Ready();
        const auto now = Clock::now();
        if (a_force || now >= g_nextRefresh) {
            g_nextRefresh = now + std::chrono::seconds(1);
            for (const auto& [id, mr] : g_rowById) {
                if (g_pending.contains(id) || g_dirty.contains(id) || mr.second->type == Type::kAction) continue;
                g_saved[id] = ReadValue(mr.first, *mr.second);
            }
        }
        const auto* api = frikbridge::ConfigApi();
        if (!g_dirty.empty() && api && (a_force || now >= g_nextFlush)) {
            g_nextFlush = now + std::chrono::milliseconds(100);
            for (const auto& id : g_dirty) {
                const auto it = g_rowById.find(id);
                if (it == g_rowById.end()) continue;
                const Row& r = *it->second.second;
                if (const auto p = g_pending.find(id); p != g_pending.end()) api->Preview(r.section.c_str(), r.key.c_str(), p->second.c_str());
                else api->Discard(r.section.c_str(), r.key.c_str());
                ++g_sent;
            }
            g_dirty.clear();
        }
    }

    std::size_t PendingCount() { return g_pending.size(); }
    int         PendingRestartCount()
    {
        int n = 0;
        for (const auto& [id, v] : g_pending)
            if (const auto it = g_rowById.find(id); it != g_rowById.end() && it->second.second->restart) ++n;
        return n;
    }

    int Save(std::string& a_how)
    {
        Tick(true);
        const auto* api = frikbridge::ConfigApi();
        int         n = 0, fails = 0;
        // per file: FRIK's table, else one rewrite of the file
        std::map<std::pair<int, int>, std::vector<std::pair<std::pair<std::string, std::string>, std::string>>> perFile;
        for (const auto& [id, v] : g_pending) {
            const auto it = g_rowById.find(id);
            if (it == g_rowById.end()) continue;
            const int   mi = it->second.first;
            const Row&  r = *it->second.second;
            const File& f = FileOf(mi, r);
            if (f.viaFrik && api) {
                const bool ok = api->Save(r.section.c_str(), r.key.c_str(), v.c_str()) != 0;
                REX::INFO("specpages: save {} {}.{}={} -> {}", g_mods[mi].name, r.section, r.key, v, ok ? "FRIK.dll (FRIK's own save)" : "FAILED");
                ok ? (g_saved[id] = v, ++n) : ++fails;
            } else {
                perFile[{ mi, r.file }].push_back({ { r.section, r.key }, v });
                g_saved[id] = v;
            }
        }
        for (const auto& [mf, vals] : perFile) {
            const File& f = g_mods[mf.first].files[mf.second];
            std::string why;
            bool        ok = true;
            if (f.format == "toml") {
                ok = TomlWrite(f.path, vals, why);
            } else {
                for (const auto& [sk, v] : vals)
                    ok = ::WritePrivateProfileStringA(sk.first.c_str(), sk.second.c_str(), v.c_str(), f.path.c_str()) != 0 && ok;
            }
            REX::INFO("specpages: save {} value(s) to {} -> {}{}", vals.size(), f.path, ok ? "ok" : "FAILED ", why);
            ok ? n += static_cast<int>(vals.size()) : fails += static_cast<int>(vals.size());
        }
        g_pending.clear();
        g_saves += n;
        g_nextRefresh = Clock::now() + std::chrono::milliseconds(1500);  // FRIK writes on its next frame
        a_how = fails ? std::format("{} FAILED", fails) : std::string();
        return n;
    }

    std::size_t Discard()
    {
        const std::size_t n = g_pending.size();
        for (const auto& [id, v] : g_pending) {
            const auto it = g_rowById.find(id);
            if (it != g_rowById.end() && FileOf(it->second.first, *it->second.second).viaFrik) g_dirty.insert(id);
        }
        g_pending.clear();
        Tick(true);
        return n;
    }

    std::string Status()
    {
        Ready();
        std::string out = std::format("specs={} frik={} pending={} sent={} saves={}", g_mods.size(), g_frik, g_pending.size(), g_sent, g_saves);
        for (const auto& m : g_mods) {
            std::size_t n = 0;
            for (const auto& pg : m.pages) n += pg.rows.size();
            out += std::format(" | {}: {} pages {} rows {} skipped{}", m.name, m.pages.size(), n, m.skipped.size(),
                !m.files.empty() && m.files[0].viaFrik ? " (via FRIK.dll)" : "");
        }
        if (const auto* api = frikbridge::ConfigApi()) {
            char st[512]{};
            api->Status(st, sizeof(st));
            out += std::string(" | ") + st;
        }
        return out;
    }

    std::string Command(const std::string& a_arg)
    {
        Ready();
        std::istringstream is(a_arg);
        std::string        verb, mod;
        is >> verb >> mod;
        int mi = -1;
        for (int i = 0; i < static_cast<int>(g_mods.size()); ++i)
            if (Lower(g_mods[i].name) == Lower(mod)) mi = i;
        if (verb == "reload") {
            Load(true);
            return Status();
        }
        if (verb.empty() || verb == "status") return Status();
        if (mi < 0) return "error: spec <rows|get|set> <mod> ... (mods: " + [] {
            std::string s;
            for (const auto& m : g_mods) s += m.name + " ";
            return s;
        }() + ")";
        if (verb == "rows") {
            std::string page;
            std::getline(is, page);
            page = Trim(page);
            std::string out;
            for (const auto& pg : g_mods[mi].pages) {
                if (!page.empty() && Lower(pg.name) != Lower(page)) continue;
                out += "[" + pg.name + "]";
                for (const auto& r : pg.rows)
                    out += r.type == Type::kAction ? std::format(" {}(action)", r.action)
                                                   : std::format(" {}.{}={}{}{}", r.section, r.key, Current(mi, r), IsPending(r) ? "*" : "",
                                                         Cond(mi, r.visible) ? "" : "(hidden)");
                out += " ";
            }
            return out;
        }
        std::string sec, key, val;
        is >> sec >> key >> val;
        for (const auto& pg : g_mods[mi].pages)
            for (const auto& r : pg.rows)
                if (r.type != Type::kAction && Lower(r.section) == Lower(sec) && Lower(r.key) == Lower(key)) {
                    if (verb == "set") {
                        Set(mi, r, val);
                        Tick(true);
                    }
                    return std::format("{} {}.{} current={} saved={} pending={} live={} file={}", g_mods[mi].name, r.section, r.key, Current(mi, r),
                        Saved(mi, r), IsPending(r) ? 1 : 0, Live(mi, r) ? 1 : 0, FileOf(mi, r).path);
                }
        return std::format("error: {} has no row {}.{}", g_mods[mi].name, sec, key);
    }
}
