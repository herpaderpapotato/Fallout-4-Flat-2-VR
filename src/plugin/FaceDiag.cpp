// FaceDiag.cpp - pipe `facediag`: dump the player's FaceGen head (19 Nov, user: "the player's face is black" with our VR
// mods only). Per geometry under BSFaceGenNiNodeSkinned: cull flag, skin instance, shader property flags/values, and every
// material texture (name, size, format, mean/max colour of a small mip read back from the GPU). Runs at Present on the render
// thread (OnPresent; 20 Nov: never as an F4SE task, see PendingRead); results go to the log as `facediag:` lines.
#include "plugin/FaceDiag.h"
#include "plugin/ConfigStore.h"
#include "plugin/Settings.h"
#include "plugin/StereoPoC.h"

#include <REX/LOG.h>
#include <F4SE/F4SE.h>
#include <RE/Fallout.h>

#include <Windows.h>
#undef ERROR
#include <d3d11.h>

#include <REL/Relocation.h>

#include "plugin/CallChain.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <cstdint>
#include <cstring>
#include <format>
#include <mutex>
#include <vector>
#include <string>

namespace fo4flatvr::facediag
{
    namespace
    {
        struct RenderData  // BSGraphics::Texture (F4SE BSRenderData): +00 SRV, +08 resource, +28 width, +2A height
        {
            ID3D11ShaderResourceView* srv;
            ID3D11Resource*           res;
        };

        void Rgb565(std::uint16_t c, float& r, float& g, float& b)
        {
            r = ((c >> 11) & 31) / 31.0f, g = ((c >> 5) & 63) / 63.0f, b = (c & 31) / 31.0f;
        }

        // 20 Nov (user: "Doing the selfie pose soon after loading, can cause hang/crash. The body appears, and then shortly after
        // crash"): the automatic dump 2 s after selfie-on ran as an F4SE task, and F4SE tasks run on whatever thread drains the
        // task queue (the logs show six different thread ids, mostly not the render thread). Its GPU read-back
        // (CopySubresourceRegion + a blocking Map on the IMMEDIATE context) then raced the game's own rendering on that context:
        // the user's 16:37 log ends in the middle of a dump on a worker thread right after "map failed". Now the whole dump
        // runs at Present on the render thread (OnPresent below, where our own panels use the context too), and the read-back
        // is split: the staging copy is queued at one Present and mapped with DO_NOT_WAIT on later Presents (no GPU stall).
        struct PendingRead
        {
            std::string      prefix;  // "facediag:   tex[N] 'name' rd ... WxH fmt .."
            ID3D11Texture2D* st{ nullptr };
            int              fmt{ 0 };
            UINT             w{ 0 }, h{ 0 };
            int              tries{ 0 };
        };
        std::vector<PendingRead> g_reads;  // render thread only
        std::mutex               g_qLock;
        std::vector<std::string> g_queue;  // dump requests (args), any thread -> OnPresent

        std::string Decode(const D3D11_MAPPED_SUBRESOURCE& m, int f, UINT w, UINT h)
        {
            double        s[4]{}, mx[4]{};
            std::uint64_t n = 0;
            auto          add = [&](float r, float g, float b, float a) {
                const float v[4]{ r, g, b, a };
                for (int i = 0; i < 4; ++i) {
                    s[i] += v[i];
                    mx[i] = (std::max)(mx[i], static_cast<double>(v[i]));
                }
                ++n;
            };
            const auto* p = static_cast<const std::uint8_t*>(m.pData);
            if (f == 28 || f == 29 || f == 87 || f == 91 || f == 88) {  // RGBA8 / BGRA8 (/X)
                const bool bgr = f >= 87;
                for (UINT y = 0; y < h; ++y) {
                    const auto* row = p + y * m.RowPitch;
                    for (UINT x = 0; x < w; ++x) {
                        const auto* px = row + x * 4;
                        add((bgr ? px[2] : px[0]) / 255.0f, px[1] / 255.0f, (bgr ? px[0] : px[2]) / 255.0f, px[3] / 255.0f);
                    }
                }
            } else if (f == 71 || f == 72 || f == 77 || f == 78) {  // BC1 / BC3: colour endpoints
                const UINT bs = (f <= 72) ? 8 : 16, off = (f <= 72) ? 0 : 8;
                for (UINT by = 0; by < (h + 3) / 4; ++by) {
                    const auto* row = p + by * m.RowPitch;
                    for (UINT bx = 0; bx < (w + 3) / 4; ++bx) {
                        const auto*   blk = row + bx * bs + off;
                        std::uint16_t c0, c1;
                        std::memcpy(&c0, blk, 2);
                        std::memcpy(&c1, blk + 2, 2);
                        float r, g, b;
                        Rgb565(c0, r, g, b);
                        add(r, g, b, 1.0f);
                        Rgb565(c1, r, g, b);
                        add(r, g, b, 1.0f);
                    }
                }
            }
            if (!n) return " | format not decoded";
            return std::format(" | mean ({:.3f},{:.3f},{:.3f},{:.3f}) max ({:.3f},{:.3f},{:.3f},{:.3f})", s[0] / n, s[1] / n, s[2] / n, s[3] / n,
                mx[0], mx[1], mx[2], mx[3]);
        }

        // queue the staging copy of one small mip (render thread); the line is logged once the copy has been read
        void QueueReadBack(ID3D11Resource* a_res, std::string a_prefix)
        {
            ID3D11Texture2D* tex = nullptr;
            if (!a_res || FAILED(a_res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) || !tex) {
                REX::INFO("{} not a Texture2D", a_prefix);
                return;
            }
            D3D11_TEXTURE2D_DESC d{};
            tex->GetDesc(&d);
            UINT mip = 0;
            while (mip + 1 < d.MipLevels && (d.Width >> (mip + 1)) >= 16 && (d.Height >> (mip + 1)) >= 16) ++mip;
            const UINT w = (std::max)(1u, d.Width >> mip), h = (std::max)(1u, d.Height >> mip);
            a_prefix += std::format(" {}x{} fmt {} mips {} arr {} bind 0x{:X} | mip {} {}x{}", d.Width, d.Height, static_cast<int>(d.Format),
                d.MipLevels, d.ArraySize, d.BindFlags, mip, w, h);
            ID3D11Device* dev = nullptr;
            tex->GetDevice(&dev);
            ID3D11DeviceContext* ctx = nullptr;
            if (dev) dev->GetImmediateContext(&ctx);
            D3D11_TEXTURE2D_DESC sd{};
            sd.Width = w, sd.Height = h, sd.MipLevels = 1, sd.ArraySize = 1, sd.Format = d.Format, sd.SampleDesc.Count = 1;
            sd.Usage = D3D11_USAGE_STAGING, sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ID3D11Texture2D* st = nullptr;
            if (!dev || !ctx || d.SampleDesc.Count != 1 || FAILED(dev->CreateTexture2D(&sd, nullptr, &st)) || !st) {
                REX::INFO("{} | no staging copy", a_prefix);
            } else {
                ctx->CopySubresourceRegion(st, 0, 0, 0, 0, tex, D3D11CalcSubresource(mip, 0, d.MipLevels), nullptr);
                g_reads.push_back({ std::move(a_prefix), st, static_cast<int>(d.Format), w, h, 0 });
            }
            if (ctx) ctx->Release();
            if (dev) dev->Release();
            tex->Release();
        }

        // render thread, every Present: map the queued copies without waiting for the GPU
        void PollReadBacks()
        {
            if (g_reads.empty()) return;
            ID3D11Device* dev = nullptr;
            g_reads.front().st->GetDevice(&dev);
            ID3D11DeviceContext* ctx = nullptr;
            if (dev) dev->GetImmediateContext(&ctx);
            for (auto it = g_reads.begin(); it != g_reads.end();) {
                D3D11_MAPPED_SUBRESOURCE m{};
                const HRESULT hr = ctx ? ctx->Map(it->st, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) : E_FAIL;
                if (hr == DXGI_ERROR_WAS_STILL_DRAWING && ++it->tries < 120) {
                    ++it;
                    continue;
                }
                if (SUCCEEDED(hr)) {
                    REX::INFO("{}{} (read after {} frames)", it->prefix, Decode(m, it->fmt, it->w, it->h), it->tries + 1);
                    ctx->Unmap(it->st, 0);
                } else {
                    REX::INFO("{} | map failed 0x{:08X} after {} frames", it->prefix, static_cast<std::uint32_t>(hr), it->tries + 1);
                }
                it->st->Release();
                it = g_reads.erase(it);
            }
            if (ctx) ctx->Release();
            if (dev) dev->Release();
        }

        void DumpGeom(RE::BSGeometry* g, const char* a_who, int a_idx)
        {
            const auto* rt = g->GetRTTI();
            std::string line = std::format("facediag: {} #{} '{}' {} cull {} skin {}", a_who, a_idx, g->name.c_str() ? g->name.c_str() : "",
                rt ? rt->GetName() : "?", (g->flags.flags & 1) ? 1 : 0, static_cast<void*>(g->skinInstance.get()));
            if (auto* si = g->skinInstance.get()) {
                const auto* b = reinterpret_cast<const std::uint8_t*>(si);
                line += std::format(" bones {}", *reinterpret_cast<const std::uint32_t*>(b + 0x38));
            }
            if (auto* ts = g->IsTriShape()) {
                line += std::format(" tris {} verts {}", ts->numTriangles, ts->numVertices);
            }
            line += std::format(" world ({:.1f},{:.1f},{:.1f}) scale {:.3f}", g->world.translate.x, g->world.translate.y, g->world.translate.z, g->world.scale);
            auto* prop = g->properties[1].get();
            const auto* prt = prop ? prop->GetRTTI() : nullptr;
            const char* pname = prt ? prt->GetName() : "";
            line += std::format(" | prop {}", pname ? pname : "?");
            if (prop && pname && std::strstr(pname, "ShaderProperty")) {
                auto* sp = static_cast<RE::BSShaderProperty*>(prop);
                const auto fl = sp->flags.underlying();
                line += std::format(" flags 0x{:016X} alpha {:.2f} lodFade {:.2f} mat {}", fl, sp->alpha, sp->lodFade, static_cast<void*>(sp->material));
                if (std::strcmp(pname, "BSLightingShaderProperty") == 0) {
                    auto* lp = static_cast<RE::BSLightingShaderProperty*>(sp);
                    line += std::format(" tech 0x{:X} dark {:.2f} emit {:.2f}", lp->baseTechniqueID, lp->forcedDarkness, lp->emitColorScale);
                    if (auto* mat = sp->material) {
                        line += std::format(" matFeature {} matType {}", static_cast<int>(mat->GetFeature()), static_cast<int>(mat->GetType()));
                    }
                }
            }
            REX::INFO("{}", line);
            if (prop && pname && std::strcmp(pname, "BSLightingShaderProperty") == 0) {
                auto* sp = static_cast<RE::BSShaderProperty*>(prop);
                if (auto* mat = static_cast<RE::BSLightingShaderMaterialBase*>(sp->material)) {
                    RE::NiTexture* tx[16]{};
                    const auto n = (std::min)(mat->GetTextures(tx, 16), 16u);
                    for (std::uint32_t i = 0; i < n; ++i) {
                        auto* t = tx[i];
                        if (!t) {
                            REX::INFO("facediag:   tex[{}] null", i);
                            continue;
                        }
                        const auto* rd = reinterpret_cast<const RenderData*>(t->rendererTexture);
                        const char* tn = t->name.c_str() ? t->name.c_str() : "";
                        // GPU read-back only for the runtime face textures (a streamed texture can be swapped under us)
                        const bool rb = rd && std::strncmp(tn, "FaceCustomization", 17) == 0;
                        if (rb) {
                            QueueReadBack(rd->res, std::format("facediag:   tex[{}] '{}' rd {}", i, tn, static_cast<const void*>(rd)));
                        } else {
                            REX::INFO("facediag:   tex[{}] '{}' rd {} {}", i, tn, static_cast<const void*>(rd), rd ? "-" : "no renderer texture");
                        }
                    }
                }
            }
        }

        int Walk(RE::NiAVObject* o, const char* a_who, int& a_idx, int a_depth)
        {
            if (!o || a_depth > 30) return 0;
            if (auto* g = o->IsGeometry()) {
                DumpGeom(g, a_who, a_idx++);
                return 1;
            }
            int n = 0;
            if (auto* nd = o->IsNode()) {
                for (auto& c : nd->children) {
                    if (c) n += Walk(c.get(), a_who, a_idx, a_depth + 1);
                }
            }
            return n;
        }

        void DumpActor(RE::Actor* a, RE::NiAVObject* a_root, const char* a_who)
        {
            if (!a || !a_root) {
                REX::INFO("facediag: {}: no 3D", a_who);
                return;
            }
            auto* face = a_root->GetObjectByName(RE::BSFixedString("BSFaceGenNiNodeSkinned"));
            auto* head = a_root->GetObjectByName(RE::BSFixedString("Head"));
            REX::INFO("facediag: {} {:08X}: root '{}' cull {} | FaceGen node {} cull {} parent '{}' | Head bone {}", a_who, a->GetFormID(),
                a_root->name.c_str() ? a_root->name.c_str() : "", (a_root->flags.flags & 1) ? 1 : 0, static_cast<void*>(face),
                face ? ((face->flags.flags & 1) ? 1 : 0) : -1, face && face->parent && face->parent->name.c_str() ? face->parent->name.c_str() : "-",
                head ? std::format("({:.1f},{:.1f},{:.1f})", head->world.translate.x, head->world.translate.y, head->world.translate.z) : std::string("-"));
            if (face) {
                int idx = 0;
                Walk(face, a_who, idx, 0);
            }
        }

        void Run(std::string a_arg)
        {
            auto* pc = RE::PlayerCharacter::GetSingleton();
            if (!pc) {
                REX::INFO("facediag: no player");
                return;
            }
            DumpActor(pc, static_cast<RE::TESObjectREFR*>(pc)->Get3D(false), "player");
            if (a_arg == "npc") {  // the nearest high-process actor with a FaceGen head, for comparison
                RE::Actor* best = nullptr;
                float      bd = 1e30f;
                if (auto* pl = RE::ProcessLists::GetSingleton()) {
                    for (auto& hnd : pl->highActorHandles) {
                        auto act = hnd.get();
                        if (!act || !act->Get3D()) continue;
                        const auto& l = act->data.location;
                        const auto& p = pc->data.location;
                        const float d = (l.x - p.x) * (l.x - p.x) + (l.y - p.y) * (l.y - p.y) + (l.z - p.z) * (l.z - p.z);
                        if (d < bd && act->Get3D()->GetObjectByName(RE::BSFixedString("BSFaceGenNiNodeSkinned"))) {
                            bd = d;
                            best = act.get();
                        }
                    }
                }
                if (best) DumpActor(best, best->Get3D(), "npc");
                else REX::INFO("facediag: no NPC with a FaceGen head nearby");
            }
            REX::INFO("facediag: {} | end", stereo::EngineOffscreenStatus());
        }
    }

    // ---- FaceGen customization render guard (19 Nov black-face fix) ----
    // BSFaceGenUtils::RenderFaceCustomizationTextures (6ED320) renders the player's face tint/normal/spec textures (the
    // FaceCustomization* textures of the FaceGen head's face material) on the main thread at the start of Main::Swap
    // (UpdatePendingCustomizationTextures 6DF490, called C332B2) and from GenerateFaceCustomizationForNPC (6EBED0). That is
    // before this frame's first SetCameraData, so the stereo draw gate still holds the previous frame's camera; the guard
    // tells the stereo hooks that these draws are an engine off-screen render (no conversion). Both call sites byte-checked.
    namespace
    {
        using RenderFC_t = std::uint64_t (*)(void*, void*);
        RenderFC_t g_origFC[2]{};
        int        g_fcLogs = 0;

        template <int I>
        std::uint64_t HkRenderFC(void* a_data, void* a_out)
        {
            stereo::BeginEngineOffscreen();
            const auto r = g_origFC[I](a_data, a_out);
            stereo::EndEngineOffscreen();
            if (g_fcLogs < 12) {
                ++g_fcLogs;
                REX::INFO("facegen: customization textures rendered ({}) | {}", I == 0 ? "UpdatePendingCustomizationTextures" : "GenerateFaceCustomizationForNPC",
                    stereo::EngineOffscreenStatus());
            }
            return r;
        }
    }

    void Install()
    {
        const auto path = Settings::Path();
        const int  mode = ::GetPrivateProfileIntA("Stereo", "bFaceGenOffscreen", 1, path.c_str()) != 0 ? 0 : 1;
        stereo::SetEngineOffscreenMode(mode);
        // 20 Nov (live settings, ConfigStore.h): the mode is taken at each offscreen render's entry, so a change applies live
        cfgstore::Subscribe("facegen", "Stereo/bFaceGenOffscreen", [](const cfgstore::Changes&) {
            const int m = ::GetPrivateProfileIntA("Stereo", "bFaceGenOffscreen", 1, Settings::Path().c_str()) != 0 ? 0 : 1;
            stereo::SetEngineOffscreenMode(m);
            REX::INFO("facediag: [Stereo] bFaceGenOffscreen -> offscreen mode {} (live)", m);
        });
        auto* const base = reinterpret_cast<std::uint8_t*>(::GetModuleHandleW(nullptr));
        const std::uintptr_t target = reinterpret_cast<std::uintptr_t>(base) + 0x6ED320;
        constexpr std::uintptr_t kSites[2] = { 0x6DF69B, 0x6EC07E };
        std::string res;
        auto& tramp = REL::GetTrampoline();
        for (int i = 0; i < 2; ++i) {
            const auto site = reinterpret_cast<std::uintptr_t>(base) + kSites[i];
            std::string why;
            if (!callchain::Hookable(site, target, why)) {
                res += std::format(" site {:X}: {} (not hooked)", kSites[i], why);
                continue;
            }
            g_origFC[i] = reinterpret_cast<RenderFC_t>(tramp.write_call<5>(site, i == 0 ? &HkRenderFC<0> : &HkRenderFC<1>));
            res += std::format(" site {:X}: {}", kSites[i], why);
        }
        REX::INFO("facegen: customization render guard{} | stereo off-screen mode {} ([Stereo] bFaceGenOffscreen={})", res,
            mode == 0 ? "bypass" : "pass (old behaviour)", mode == 0 ? 1 : 0);
    }

    void AutoDump(const char* a_why)
    {
        static std::atomic<int> s_left{ 4 };  // a few per session (compact log)
        if (s_left.fetch_sub(1) <= 0) {
            return;
        }
        REX::INFO("facediag: automatic dump in 2 s ({})", a_why);
        std::thread([] {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            std::scoped_lock l(g_qLock);
            g_queue.emplace_back("");  // run at the next Present (render thread), never as an F4SE task (any thread)
        }).detach();
    }

    void OnPresent()
    {
        std::vector<std::string> q;
        {
            std::scoped_lock l(g_qLock);
            if (g_queue.empty() && g_reads.empty()) return;
            q.swap(g_queue);
        }
        for (const auto& a : q) {
            // not while a load replaces the 3D (the dump walks the player's head nodes)
            const auto* ui = RE::UI::GetSingleton();
            if (ui && (ui->GetMenuOpen("LoadingMenu") || ui->GetMenuOpen("MainMenu"))) {
                REX::INFO("facediag: dump skipped (loading / main menu)");
                continue;
            }
            Run(a);
        }
        PollReadBacks();
    }

    std::string Command(const std::string& a_arg)
    {
        if (a_arg.rfind("mode ", 0) == 0) {  // facediag mode 0|1|2: bypass | pass (old) | force world gate (diagnostic)
            stereo::SetEngineOffscreenMode(std::atoi(a_arg.c_str() + 5));
            return stereo::EngineOffscreenStatus();
        }
        if (a_arg == "status") {
            return stereo::EngineOffscreenStatus();
        }
        std::scoped_lock l(g_qLock);
        g_queue.push_back(a_arg);
        return "queued for the next Present (log: facediag lines)";
    }
}
