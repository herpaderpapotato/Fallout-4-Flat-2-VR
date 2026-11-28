#include "plugin/QuickMenu.h"

#include <F4SE/F4SE.h>
#include <RE/Fallout.h>

#include "plugin/Haptics.h"
#include "plugin/ThreadQueues.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>

namespace fo4flatvr::quickmenu
{
    namespace
    {
        using GValue = Scaleform::GFx::Value;

        // FavoritesMenu ctor (1049880) and its inlined copy in the menu factory's create function (104AF80, the one UI uses):
        // call BSScaleformManager::LoadMovie
        constexpr std::uintptr_t kLoadMovieSites[] = { 0x104B09D, 0x104994D };
        constexpr std::uintptr_t kLoadMovie = 0x1B1B1D0;
        constexpr const char*    kVrMovie = "world_Favorites";
        constexpr const char*    kVrFile = "Data/Interface/world_Favorites.swf";

        std::uintptr_t Base() { return REL::Offset(0).address(); }

        std::atomic<bool> g_enabled{ true };
        std::atomic<bool> g_vrLoaded{ false };  // the last FavoritesMenu got the VR movie
        std::atomic<int>  g_fileState{ -1 };    // -1 unknown, 0 missing, 1 present
        std::atomic<unsigned> g_loads{ 0 }, g_vrLoads{ 0 }, g_entryChanges{ 0 }, g_uses{ 0 }, g_toggles{ 0 }, g_inits{ 0 }, g_unsheathedCalls{ 0 };
        std::atomic<int>  g_entry{ 12 };
        std::string       g_install{ "not installed" }, g_initText{ "-" };
        std::mutex        g_textLock;
        bool              g_hooked = false;
        REL::Trampoline   g_tramp{ "fo4flatvr quickmenu" };

        // XR-thread input state
        struct In
        {
            bool open{ false }, trig{ false }, inited{ false };
            int  entry{ 12 };
            int  frames{ 0 };
            float lx{ 0.0f }, ly{ 0.0f };
        } g_in;

        bool MenuOpen()
        {
            auto* ui = RE::UI::GetSingleton();
            return ui && ui->GetMenuOpen("FavoritesMenu");
        }

        int WeaponState()  // ActorState (Actor+0x128) +0xC bits 1..3, read raw (>= 3 drawn)
        {
            const auto* pl = RE::PlayerCharacter::GetSingleton();
            return pl ? static_cast<int>((*reinterpret_cast<const std::uint32_t*>(reinterpret_cast<const std::uint8_t*>(pl) + 0x134) >> 1) & 7) : -1;
        }

        // 28 Nov (controls agent, hint icon): FO4VR passes SetPlatform(5) to its menus on Touch (IMenu::RefreshPlatform VR
        // (SetPlatform call 142198E5A): controller type 1 -> 5, WMR 2 -> 6); the movie's hint then draws its "OCR_Grab" glyph ('_') in FO4VR's
        // "Controller VR" font. That font comes from Interface/fo4flatvr_vrfonts.swf (cut from FO4VR's fonts_en.swf) through a
        // fontconfig.txt with flat's lines plus a fontlib / map line (plugin/tools/install_vr_favorites.py). Without the font
        // file the movie stays at flat's platform (label only).
        constexpr const char* kVrFontFile = "Data/Interface/fo4flatvr_vrfonts.swf";
        std::atomic<int>      g_fontState{ -1 };
        std::atomic<unsigned> g_platformSets{ 0 };
        bool VrFontThere()
        {
            int f = g_fontState.load();
            if (f < 0) {
                std::error_code ec;
                f = std::filesystem::exists(kVrFontFile, ec) ? 1 : 0;
                g_fontState = f;
                REX::INFO("quick menu: {} {}", kVrFontFile, f ? "found: the movie runs at FO4VR's Touch platform (5), grip glyph in 'Controller VR'" :
                                                                  "MISSING (run plugin/tools/install_vr_favorites.py): hint without its icon");
            }
            return f == 1;
        }

        bool FileThere()
        {
            int f = g_fileState.load();
            if (f < 0) {
                std::error_code ec;
                f = std::filesystem::exists(kVrFile, ec) ? 1 : 0;
                g_fileState = f;
                REX::INFO("quick menu: {} {}", kVrFile, f ? "found (FO4VR movie)" : "MISSING (run plugin/tools/install_vr_favorites.py): the flat favourites menu");
            }
            return f == 1;
        }

        using LoadMovie_t = bool (*)(void*, void*, void*, const char*, const char*, std::uint64_t, std::uint64_t, std::uint64_t);
        LoadMovie_t g_origLoad = nullptr;
        bool HkLoadMovie(void* a_mgr, void* a_menu, void* a_out, const char* a_name, const char* a_path, std::uint64_t a6, std::uint64_t a7, std::uint64_t a8)
        {
            ++g_loads;
            const bool vr = g_enabled.load() && a_name && std::strcmp(a_name, "FavoritesMenu") == 0 && FileThere();
            g_vrLoaded = vr;
            if (vr) {
                ++g_vrLoads;
                REX::INFO("quick menu: FavoritesMenu loads the FO4VR movie '{}' (object '{}')", kVrMovie, a_path ? a_path : "?");
            }
            return g_origLoad(a_mgr, a_menu, a_out, vr ? kVrMovie : a_name, a_path, a6, a7, a8);
        }

        // BGSCodeObj.getUnsheathed (FO4VR's 4th code-object function): weapon drawn
        class Unsheathed final : public Scaleform::GFx::FunctionHandler
        {
        public:
            void Call(const Params& a_params) override
            {
                ++g_unsheathedCalls;
                if (a_params.retVal) *a_params.retVal = WeaponState() >= 3;
            }
        };
        Unsheathed* g_unsheathed = nullptr;

        // BGSCodeObj.useQuickkey wrapper: counts and logs what the movie uses, then calls the game's function
        std::atomic<unsigned> g_useCalls{ 0 };
        std::atomic<int>      g_useLast{ -1 };
        class UseQuickkey final : public Scaleform::GFx::FunctionHandler
        {
        public:
            void Call(const Params& a_params) override
            {
                ++g_useCalls;
                const int idx = a_params.argCount > 0 && a_params.args[0].IsNumber() ? static_cast<int>(a_params.args[0].GetNumber()) :
                                a_params.argCount > 0 && a_params.args[0].IsUInt()   ? static_cast<int>(a_params.args[0].GetUInt()) :
                                a_params.argCount > 0 && a_params.args[0].IsInt()    ? a_params.args[0].GetInt() : -1;
                g_useLast = idx;
                REX::INFO("quick menu: useQuickkey({}) from the movie ({} args)", idx, a_params.argCount);
                // the code object is looked up per call: a GFx value kept across calls held the closed movie alive, and
                // FavoritesMenu then never opened again (28 Nov, controls agent: reopen failed after the first close)
                GValue code;
                if (a_params.self && a_params.self->IsObject()) code = *a_params.self;
                if (!code.IsObject() || !code.HasMember("useQuickkeyGame")) {
                    code = GValue();
                    if (a_params.movie) a_params.movie->GetVariable(&code, "root1.Menu_mc.BGSCodeObj");
                }
                if (code.IsObject()) code.Invoke("useQuickkeyGame", a_params.retVal, a_params.args, a_params.argCount);
            }
        };
        UseQuickkey* g_useFn = nullptr;

        template <class F>
        void WithMenuObj(F&& a_fn)  // main thread
        {
            auto* ui = RE::UI::GetSingleton();
            static const RE::BSFixedString kName("FavoritesMenu");
            auto menu = ui ? ui->GetMenu(kName) : nullptr;
            if (!menu || !menu->uiMovie) return;
            GValue obj;
            if (!menu->uiMovie->GetVariable(&obj, "root1.Menu_mc") || !obj.IsObject()) return;
            a_fn(*menu->uiMovie, obj);
        }

        // The weapon hint of the VR movie (28 Nov, controls agent): Menu_mc.testBox_mc is the movie's own BSButtonHintBar with
        // one BSButtonHint whose data the movie swaps by getUnsheathed: "$PUT AWAY WEAPON" (drawn) / "$DRAW WEAPON" (holstered),
        // the same label on every platform (SetPlatform 5 = FO4VR Oculus, 0, 6, 1 measured). FO4VR's Translate_en.txt has both
        // keys, flat's has neither (the hint read "$PUT AWAY WEAPON"): our Interface/Translations/fo4flatvr_en.txt is added to
        // the game's translator here (F4SE only reads the files of plugins.txt entries) -> "PUT AWAY WEAPON" / "DRAW WEAPON".
        // The icon: the movie's data carry FO4VR's glyph names only ("OCR_Grab" etc., drawn in FO4VR's "Controller VR" font on
        // platform 5): with the font installed the movie runs at platform 5 (TouchPlatform) and the hint shows FO4VR's GRAB
        // glyph (capture 28 Nov 07:09).
        std::atomic<bool> g_trDone{ false };
        std::string       g_hintText{ "-" };  // main thread
        std::string       g_hintShown{ "-" };  // copy under g_textLock (status)

        void LoadTranslations()  // main thread
        {
            if (g_trDone.load()) return;
            auto* mgr = RE::BSScaleformManager::GetSingleton();
            auto* tr = mgr && mgr->loader ? mgr->GetTranslator() : nullptr;
            if (!tr) return;
            tr->AddTranslationsMod("fo4flatvr");
            tr->Release();
            g_trDone = true;
            REX::INFO("quick menu: Interface/Translations/fo4flatvr_<language>.txt added to the game's translator (FO4VR's PUT AWAY / DRAW WEAPON labels)");
        }

        int ChildCount(GValue& a_v)
        {
            GValue nc;
            if (!a_v.GetMember("numChildren", &nc)) return 0;
            return nc.IsNumber() ? static_cast<int>(nc.GetNumber()) : nc.IsInt() ? nc.GetInt() : nc.IsUInt() ? static_cast<int>(nc.GetUInt()) : 0;
        }

        // the label of the BSButtonHint under a_v (its child named textField_tf)
        bool FindHintText(GValue& a_v, int a_d, std::string& a_text)
        {
            const int n = ChildCount(a_v);
            for (int i = 0; i < n && i < 16; ++i) {
                GValue idx(static_cast<double>(i)), ch, nm, tx;
                if (!a_v.Invoke("getChildAt", &ch, &idx, 1) || !ch.IsObject()) continue;
                if (ch.GetMember("name", &nm) && nm.IsString() && std::strcmp(nm.GetString(), "textField_tf") == 0) {
                    a_text = ch.GetMember("text", &tx) && tx.IsString() ? tx.GetString() : "";
                    return true;
                }
                if (a_d < 3 && FindHintText(ch, a_d + 1, a_text)) return true;
            }
            return false;
        }

        void ReadHint(GValue& a_menu)
        {
            GValue bar;
            std::string text;
            if (!a_menu.GetMember("testBox_mc", &bar) || !bar.IsObject() || !FindHintText(bar, 0, text)) return;
            if (text != g_hintText) REX::INFO("quick menu: hint '{}' (weapon state {})", text, WeaponState());
            g_hintText = text;
            std::scoped_lock l(g_textLock);
            g_hintShown = text;
        }

        void TouchPlatform(GValue& a_obj)  // main thread: FO4VR's Touch platform (5) for the movie, kept against flat's refreshes
        {
            if (!VrFontThere()) return;
            GValue pf;
            const double cur = a_obj.GetMember("uiPlatform", &pf) ? (pf.IsNumber() ? pf.GetNumber() : pf.IsUInt() ? pf.GetUInt() : pf.IsInt() ? pf.GetInt() : -1.0) : -1.0;
            if (cur == 5.0) return;
            GValue args[2] = { GValue(5u), GValue(false) };
            const bool ok = a_obj.Invoke("SetPlatform", nullptr, args, 2);
            if (g_platformSets++ < 4) REX::INFO("quick menu: SetPlatform(5) (FO4VR Touch; was {}) {}", cur, ok ? "ok" : "FAILED");
        }

        void InitOnMain(int a_hand)
        {
            WithMenuObj([a_hand](Scaleform::GFx::Movie& a_movie, GValue& a_obj) {
                std::string txt;
                GValue code;
                if (a_obj.GetMember("BGSCodeObj", &code) && code.IsObject()) {
                    if (!g_unsheathed) g_unsheathed = new Unsheathed();
                    GValue fn;
                    a_movie.CreateFunction(&fn, g_unsheathed);
                    txt += std::format("getUnsheathed {}", code.SetMember("getUnsheathed", fn) ? "added" : "FAILED");
                    GValue orig;
                    if (code.GetMember("useQuickkey", &orig) && !orig.IsUndefined()) {
                        if (!g_useFn) g_useFn = new UseQuickkey();
                        GValue wrap;
                        a_movie.CreateFunction(&wrap, g_useFn);
                        const bool a = code.SetMember("useQuickkeyGame", orig);
                        const bool b = code.SetMember("useQuickkey", wrap);
                        txt += std::format(" | useQuickkey wrapped {}", a && b ? "ok" : "FAILED");
                    }
                } else {
                    txt += "no BGSCodeObj";
                }
                GValue r;
                txt += std::format(" | updateSheatheState {}", a_obj.Invoke("updateSheatheState", &r, nullptr, 0) ? "ok" : "FAILED");
                LoadTranslations();
                txt += std::format(" | translations {}", g_trDone.load() ? "in" : "NOT added");
                TouchPlatform(a_obj);
                txt += std::format(" | platform {}", VrFontThere() ? "5 (Touch)" : "flat (no VR glyph font)");
                ++g_inits;
                {
                    std::scoped_lock l(g_textLock);
                    g_initText = txt;
                }
                REX::INFO("quick menu: init {}", txt);
            });
        }

        void CursorOnMain(float a_x, float a_y, int a_entry, const char* a_state)
        {
            WithMenuObj([=](Scaleform::GFx::Movie&, GValue& a_obj) {
                GValue a[4] = { GValue(0u), GValue(0u), GValue(static_cast<double>(a_x)), GValue(static_cast<double>(a_y)) };
                a_obj.Invoke("UpdateCursorState", nullptr, a, 4);
                GValue w[2] = { GValue(static_cast<std::uint32_t>(a_entry)), GValue(a_state) };
                a_obj.Invoke("ProcessWandEvent", nullptr, w, 2);
            });
        }

        void SheatheOnMain()
        {
            WithMenuObj([](Scaleform::GFx::Movie&, GValue& a_obj) {
                TouchPlatform(a_obj);
                GValue r;
                a_obj.Invoke("updateSheatheState", &r, nullptr, 0);
                ReadHint(a_obj);
            });
        }

        void ToggleDrawnOnMain()
        {
            auto* pl = RE::PlayerCharacter::GetSingleton();
            if (!pl) return;
            const int  ws = WeaponState();
            const bool draw = ws < 3;
            using Draw_t = void (*)(void*, bool);
            auto** vt = *reinterpret_cast<Draw_t**>(pl);
            vt[0xC9](pl, draw);  // Actor::DrawWeaponMagicHands, as FO4VR's WandGrip and flat's ReadyWeapon hold
            ++g_toggles;
            REX::INFO("quick menu: {} (weapon state {}; Actor::DrawWeaponMagicHands, the menu stays open)", draw ? "draw" : "put away", ws);
        }
    }

    void Install()
    {
        if (g_hooked) return;
        g_tramp.create(64);
        int ok = 0;
        std::string sites;
        for (const auto site : kLoadMovieSites) {
            const auto* p = reinterpret_cast<const std::uint8_t*>(Base() + site);
            std::int32_t rel = 0;
            std::memcpy(&rel, p + 1, 4);
            if (p[0] != 0xE8 || static_cast<std::intptr_t>(site) + 5 + rel != static_cast<std::intptr_t>(kLoadMovie)) {
                REX::WARN("quick menu: call site {:X} is not a call to LoadMovie {:X}: skipped", site, kLoadMovie);
                sites += std::format(" {:X}=skipped", site);
                continue;
            }
            const auto orig = g_tramp.write_call<5>(Base() + site, &HkLoadMovie);
            if (!g_origLoad) g_origLoad = reinterpret_cast<LoadMovie_t>(orig);
            ++ok;
            sites += std::format(" {:X}=ok", site);
        }
        g_hooked = ok > 0;
        g_install = std::format("FavoritesMenu movie hook{}", sites);
        tq::RunOnMain([]() { LoadTranslations(); });
        REX::INFO("quick menu: {}", g_install);
    }

    void SetEnabled(bool a_on) { g_enabled = a_on; }

    int WeaponStateRaw() { return WeaponState(); }

    void ToggleDrawn() { tq::RunOnMain([]() { ToggleDrawnOnMain(); }); }

    bool Active() { return g_vrLoaded.load() && MenuOpen(); }

    void Input(int a_hand, float a_x, float a_y, bool a_trigger, bool a_gripPressed)
    {
        In& s = g_in;
        if (!Active()) {
            s = In{};
            return;
        }
        if (!s.inited) {
            s.inited = true;
            s.entry = 12;
            tq::RunOnMain([a_hand]() { InitOnMain(a_hand); });
        }
        // FUN_1412f4060: entry from the stick direction (up = 0, clockwise, 30 deg each, offset 15 deg); centred = 12
        int         entry = 12;
        const float len = std::sqrt(a_x * a_x + a_y * a_y);
        if (len > 0.5f) {
            float a = std::acos(std::clamp(a_y / len, -1.0f, 1.0f));
            if (a_x < 0.0f) a = 6.2831855f - a;
            float deg = a * 57.2957795f + 15.0f;
            if (deg >= 360.0f) deg -= 360.0f;
            entry = static_cast<int>(deg * 0.033333335f);
        }
        if (entry != s.entry) {
            s.entry = entry;
            g_entry = entry;
            ++g_entryChanges;
            haptics::Request(a_hand, 0.3f, 0.01f);  // FO4VR 0.3 / 0.01 (1437B05A0 / 1437B05B8)
        }
        const bool moved = std::abs(a_x - s.lx) > 0.01f || std::abs(a_y - s.ly) > 0.01f;
        if (moved || (s.frames % 30) == 0) {
            s.lx = a_x, s.ly = a_y;
            tq::RunOnMain([a_x, a_y, entry]() { CursorOnMain(a_x, a_y, entry, "pressed"); });
        }
        if (s.trig && !a_trigger) {  // WandTrigger released (FO4VR FUN_1412f4900): use the entry; centred (12) = close, nothing used
            ++g_uses;
            const int e = s.entry;
            tq::RunOnMain([a_x, a_y, e]() { CursorOnMain(a_x, a_y, e, "just_released"); });
        }
        s.trig = a_trigger;
        if (a_gripPressed) tq::RunOnMain([]() { ToggleDrawnOnMain(); });
        if ((++s.frames % 5) == 0) tq::RunOnMain([]() { SheatheOnMain(); });  // the hint follows the weapon state
    }

    Geometry GetGeometry() { return Geometry{}; }

    void Close(const char* a_why)
    {
        tq::RunOnMain([a_why]() {
            if (auto* q = RE::UIMessageQueue::GetSingleton()) {
                static const RE::BSFixedString kName("FavoritesMenu");
                q->AddMessage(kName, RE::UI_MESSAGE_TYPE::kHide);
                REX::INFO("quick menu: closed ({})", a_why);
            }
        });
    }

    std::string Status()
    {
        std::string init, hint;
        {
            std::scoped_lock l(g_textLock);
            init = g_initText;
            hint = g_hintShown;
        }
        return std::format("quickmenu on={} file={} {} | loads={} vr={} active={} entry={} changes={} uses={} useQuickkey={}({}) toggles={} unsheathedCalls={} ws={} hint='{}' vrFont={} platformSets={} | init: {}",
            g_enabled.load() ? 1 : 0, g_fileState.load(), g_install, g_loads.load(), g_vrLoads.load(), Active() ? 1 : 0, g_entry.load(),
            g_entryChanges.load(), g_uses.load(), g_useCalls.load(), g_useLast.load(), g_toggles.load(), g_unsheathedCalls.load(), WeaponState(), hint, g_fontState.load(), g_platformSets.load(), init);
    }

    std::string Command(const std::string& a_arg)
    {
        if (a_arg == "vars") {  // movie state (test aid; main thread)
            return "quickmenu vars " + tq::RunOnMainWait([]() -> std::string {
                std::string out = "no movie";
                WithMenuObj([&out](Scaleform::GFx::Movie& a_movie, GValue& a_obj) {
                    out.clear();
                    static constexpr const char* kNum[] = { "selectedIndex", "_weaponState", "lastTouchWedge", "lastCursorState" };
                    for (const char* k : kNum) {
                        GValue v;
                        if (a_obj.GetMember(k, &v)) {
                            if (v.IsNumber()) out += std::format("{}={} ", k, v.GetNumber());
                            else if (v.IsBoolean()) out += std::format("{}={} ", k, v.GetBoolean() ? "true" : "false");
                            else if (v.IsString()) out += std::format("{}='{}' ", k, v.GetString());
                            else out += std::format("{}=obj ", k);
                        } else {
                            out += std::format("{}=- ", k);
                        }
                    }
                    std::string dump;
                    std::function<void(const GValue&, const std::string&, int)> walk = [&](const GValue& a_v, const std::string& a_p, int a_d) {
                        if (dump.size() > 900 || !a_v.IsObject()) return;
                        a_v.VisitMembers([&](const char* a_n, const GValue& a_m) {
                            if (dump.size() > 900 || !a_n) return;
                            const std::string n = a_p + "." + a_n;
                            if (a_m.IsString()) dump += std::format("{}='{}' ", n, a_m.GetString());
                            else if (a_m.IsDisplayObject() && a_d < 4) walk(a_m, n, a_d + 1);
                        });
                    };
                    std::string names;
                    a_obj.VisitMembers([&](const char* a_n, const GValue& a_m) {
                        if (names.size() < 700 && a_n) names += std::format("{}{} ", a_n, a_m.IsDisplayObject() ? "*" : a_m.IsObject() ? "+" : "");
                    });
                    out += "| members: " + names;
                    GValue sb;
                    if (a_obj.GetMember("SheatheButton_mc", &sb)) walk(sb, "Sheathe", 0);
                    else dump += "SheatheButton_mc absent ";
                    GValue tf;
                    if (a_movie.GetVariable(&tf, "root1.Menu_mc.SheatheButton_mc.textField_tf.text") && tf.IsString()) dump += std::format("hintText='{}' ", tf.GetString());
                    out += "| " + dump + "| ";
                    GValue fav;
                    if (a_obj.GetMember("favInfoArray", &fav) && fav.IsArray()) out += std::format("favInfoArray={} ", fav.GetArraySize());
                    static constexpr const char* kTxt[] = { "root1.Menu_mc.ItemName_tf.text", "root1.Menu_mc.SheatheButton_mc.ButtonText",
                        "root1.Menu_mc.PutAwayData.ButtonText", "root1.Menu_mc.DrawData.ButtonText" };
                    for (const char* k : kTxt) {
                        GValue v;
                        out += std::format("{}='{}' ", k + 14, a_movie.GetVariable(&v, k) && v.IsString() ? v.GetString() : "-");
                    }
                });
                return out;
            }, std::chrono::milliseconds(1000), "main thread busy");
        }
        if (a_arg.starts_with("get ") || a_arg.starts_with("call ")) {  // test aid: "get <path>" / "call <path> <fn> [num]" in the open movie
            return "quickmenu " + tq::RunOnMainWait([a_arg]() -> std::string {
                auto* ui = RE::UI::GetSingleton();
                static const RE::BSFixedString kName("FavoritesMenu");
                auto menu = ui ? ui->GetMenu(kName) : nullptr;
                if (!menu || !menu->uiMovie) return std::string("no movie");
                std::istringstream is(a_arg);
                std::string op, path, fn, num;
                is >> op >> path >> fn >> num;
                GValue v;
                if (!menu->uiMovie->GetVariable(&v, path.c_str())) return path + " (not found)";
                if (op == "call") {
                    GValue r, arg[2];
                    std::uint32_t n = 0;
                    if (!num.empty()) arg[n++] = GValue(std::stod(num)), arg[n++] = GValue(false);
                    const bool ok = v.Invoke(fn.c_str(), &r, arg, n);
                    return std::format("{}.{}({}) {}", path, fn, num, ok ? "ok" : "FAILED");
                }
                if (v.IsBoolean()) return path + " = " + (v.GetBoolean() ? "true" : "false");
                if (v.IsNumber()) return std::format("{} = {}", path, v.GetNumber());
                if (v.IsInt()) return std::format("{} = {}", path, v.GetInt());
                if (v.IsUInt()) return std::format("{} = {}", path, v.GetUInt());
                if (v.IsString()) return path + " = '" + v.GetString() + "'";
                if (v.IsObject()) {
                    std::string names;
                    v.VisitMembers([&](const char* a_n, const GValue& a_m) {
                        if (names.size() < 800 && a_n) names += std::format("{}{} ", a_n, a_m.IsString() ? "='" + std::string(a_m.GetString()) + "'" : "");
                    });
                    return path + " = object { " + names + "}";
                }
                return path + (v.IsUndefined() ? " = undefined" : " = other");
            }, std::chrono::milliseconds(1000), "main thread busy");
        }
        if (a_arg == "tree") {  // display tree of the open movie (test aid; main thread): name, visible, x/y, text
            return "quickmenu tree " + tq::RunOnMainWait([]() -> std::string {
                std::string out = "no movie";
                auto* ui = RE::UI::GetSingleton();
                static const RE::BSFixedString kName("FavoritesMenu");
                auto menu = ui ? ui->GetMenu(kName) : nullptr;
                if (!menu || !menu->uiMovie) return out;
                GValue root;
                if (!menu->uiMovie->GetVariable(&root, "root1") || !root.IsObject()) return out;
                out.clear();
                std::function<void(GValue&, int)> walk = [&](GValue& a_v, int a_d) {
                    if (out.size() > 9000) return;
                    GValue nm, vis, x, y, tx, nc;
                    a_v.GetMember("name", &nm);
                    a_v.GetMember("visible", &vis);
                    a_v.GetMember("x", &x);
                    a_v.GetMember("y", &y);
                    out += std::format("{}{}{}@{:.0f},{:.0f}", std::string(a_d, '.'), nm.IsString() ? nm.GetString() : "?",
                        vis.IsBoolean() && !vis.GetBoolean() ? "(hidden)" : "", x.IsNumber() ? x.GetNumber() : -1.0, y.IsNumber() ? y.GetNumber() : -1.0);
                    if (a_v.GetMember("text", &tx) && tx.IsString() && *tx.GetString()) {
                        out += std::format("='{}'", tx.GetString());
                        GValue h, tw;
                        if (nm.IsString() && std::strcmp(nm.GetString(), "Icon_tf") == 0) {
                            if (a_v.GetMember("htmlText", &h) && h.IsString()) out += std::format("[html {}]", h.GetString());
                            if (a_v.GetMember("textWidth", &tw) && tw.IsNumber()) out += std::format("[textWidth {:.1f}]", tw.GetNumber());
                        }
                    }
                    out += " ";
                    if (a_d >= 8 || !a_v.GetMember("numChildren", &nc)) return;
                    const int n = nc.IsNumber() ? static_cast<int>(nc.GetNumber()) : nc.IsInt() ? nc.GetInt() : nc.IsUInt() ? static_cast<int>(nc.GetUInt()) : 0;
                    for (int i = 0; i < n && i < 40; ++i) {
                        GValue idx(static_cast<double>(i)), ch;
                        if (a_v.Invoke("getChildAt", &ch, &idx, 1) && ch.IsObject()) walk(ch, a_d + 1);
                    }
                };
                walk(root, 0);
                return out;
            }, std::chrono::milliseconds(1500), "main thread busy");
        }
        if (a_arg == "toggle") {
            tq::RunOnMain([]() { ToggleDrawnOnMain(); });
            return "quickmenu toggle queued | " + Status();
        }
        return Status();
    }
}
