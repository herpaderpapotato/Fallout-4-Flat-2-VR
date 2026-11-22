#pragma once
// Layered settings (20 Nov, analysis/vr-config-ui.md section 3.3): code default < fo4flatvr.ini (the user's file, never
// written) < MCM's Data\MCM\Settings\fo4flatvr.ini (MO2: overwrite\MCM\Settings) < the VR panel's unsaved previews.
//
// Every module reads its keys with GetPrivateProfile* from Settings::Path(). That path is now a MERGED copy,
// Data\F4SE\Plugins\fo4flatvr.effective.ini (generated; under MO2 it lands in overwrite\F4SE\Plugins), rebuilt whenever one
// of the layers changes, so every reader - also FRIK.dll through the bridge's IniPath - sees the layered values without
// knowing about the layers. It is never named fo4flatvr.ini, so it can never shadow the user's file in MO2's VFS.
// Live rows of the settings table (ConfigSpec.gen.inl) carry the command that applies them at once (ConfigUI); since
// 20 Nov every module also re-reads its keys on a change (Subscribe below), so a file / MCM edit applies live too.
//
// Writes (the panel's Save) go only to the MCM file: through MCM's own Papyrus API (MCM.SetModSetting*) while mcm.dll is
// loaded and Papyrus runs, so MCM's in-memory copy stays right and MCM writes the line; otherwise one line per key
// (WritePrivateProfileString). The user's fo4flatvr.ini is never written.
// Threads: Init/Path from any thread (once); everything else from the Presenter thread.

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace fo4flatvr::cfgstore
{
    const std::string& UserPath();       // Data\F4SE\Plugins\fo4flatvr.ini (absolute)
    const std::string& McmPath();        // Data\MCM\Settings\fo4flatvr.ini (absolute)
    const std::string& EffectivePath();  // the merged file; the user path if it could not be written
    // Builds the merged file once (Settings::Path() calls it first). Any thread.
    void Init();

    // Presenter thread, every frame (cheap): about once a second compares the time stamps of the user ini and the MCM file
    // and rebuilds the merged file. True when a merged value changed (Revision() increments).
    bool Poll();
    std::uint64_t Revision();

    // Layer lookups (case-insensitive section/key). Effective = preview > MCM > user; false = in none (code default).
    bool UserValue(const std::string& a_sec, const std::string& a_key, std::string& a_out);
    bool McmValue(const std::string& a_sec, const std::string& a_key, std::string& a_out);
    bool EffectiveValue(const std::string& a_sec, const std::string& a_key, std::string& a_out);

    // Unsaved previews of the panel: part of the merged file (so modules that re-read it see them) until cleared.
    void SetPreview(const std::string& a_sec, const std::string& a_key, const std::string& a_val);
    void ClearPreview(const std::string& a_sec, const std::string& a_key);
    void ClearAllPreviews();
    std::size_t PreviewCount();
    void Rebuild();  // write the merged file now (after a batch of preview changes)

    enum class VType { kBool, kInt, kFloat, kString };
    // Save one value into the MCM layer. a_how: "MCM API" / "ini line" (+ why). False = nothing was written.
    bool Save(const std::string& a_sec, const std::string& a_key, const std::string& a_val, VType a_type, std::string& a_how);
    // Without MCM: remove the key's line from the MCM file (back to fo4flatvr.ini / the code default). With MCM loaded the
    // line cannot be removed through its API: false (the caller saves the base value instead).
    bool RemoveOverride(const std::string& a_sec, const std::string& a_key);
    bool McmLoaded();  // mcm.dll in the process and the Papyrus VM up

    std::string Status();  // pipe "cfg"

    // ---- 20 Nov (live settings): change notifications --------------------------------------------------------------------
    // Every module that reads its keys from the merged file registers a reload here; when a merged value changes (an edit of
    // fo4flatvr.ini, MCM, a VR-panel preview / save / discard) the module re-reads and re-applies on the right thread, so
    // a key needs a restart only when its module cannot change it while the game runs (analysis/vr-config-ui.md section 9).
    struct Changes
    {
        // lower-case "section\nkey" of every merged value that was added, removed or changed since the last dispatch
        std::vector<std::string> keys;
        bool Has(const char* a_sec, const char* a_key) const;  // that key changed
        bool Any(const char* a_sec) const;                     // any key of that section changed
        bool Empty() const { return keys.empty(); }
    };
    enum class On
    {
        kPresent,  // inline, on the Present thread, at the start of the Presenter's frame (configui::Tick)
        kMain      // tq::RunOnMain: the main thread at the main-idle point (scene graph, actors, menus, code patches)
    };
    // a_watch: comma list of "Section" (any key of it) or "Section/Key". Registration at load (Install); a_fn gets the
    // change set (its own keys and the others).
    void Subscribe(const char* a_name, const char* a_watch, std::function<void(const Changes&)> a_fn, On a_on = On::kPresent);
    // Present thread, every frame (configui::Tick, also with the panel off): when the merged revision moved, runs every
    // subscriber whose keys changed. Cheap otherwise.
    void DispatchChanges();
    // The merged values in memory (lower-case "section" + newline + "key" -> raw value as written to the merged file): what
    // GetPrivateProfile* on Settings::Path() would read, without ~0.1 ms of file parsing per key (Settings::Load).
    std::map<std::string, std::string> MergedValues();
    std::string SubscribersStatus();  // pipe "cfg subs"
}
