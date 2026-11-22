#pragma once
// Generic Mods Config FO4 VR spec.json pages for the VR settings panel (20 Nov; analysis/ui-framework-mods.md section 4/5).
//
// Mods Config FO4 VR (FO4VR's all-mods settings menu) reads Data\ModsConfigFO4\Config\<mod>\spec.json: the mod's pages,
// sections, switchers / sliders / steppers / dropdowns, their file + [section] key, defaults, ranges and visible/enabled
// conditions. This module finds every such spec at the first use (Data through MO2's VFS), parses it, and keeps the panel's
// values for its rows, so ConfigUI can draw any mod's pages; FRIK's (FRIK-flat ships FRIK's own spec.json) first, under the
// FRIK tab, other mods under "Mods".
//
// Files: "root" myGames = Documents\My Games\Fallout4 (flat), data = the game's Data; "pathFromClientState": "configFile" is
// the file the mod actually loaded (FRIK: from FRIK.dll's bridge v6 config table; FRIK-flat keeps it in
// Documents\My Games\Fallout4\FRIK-flat\FRIK_Config\FRIK.toml).
// Values:
//   - a file of FRIK with FRIK.dll's bridge v6 config table: changes are live previews (FRIK session overrides), SAVE goes
//     through FRIK's own config save (keeps FRIK.toml's layout), DISCARD drops the previews (Mods Config's own mechanism);
//   - any other file (or FRIK without the table): read from the file; SAVE writes the changed lines (TOML: value replaced in
//     place, comments and layout kept; INI: WritePrivateProfileString). A mod that watches its file (FRIK-flat does) applies
//     it then; previews before SAVE are not possible without the mod's code.
// Not rendered (listed per mod): controller-binding editors (cf.inputBinding), activation spheres, settings kept in a mod's
// client state, button actions that call a mod's native functions (FRIK's in-world adjustment modes: shown disabled).
// Threads: Presenter thread only (ConfigUI's lock).

#include <cstddef>
#include <string>
#include <vector>

namespace fo4flatvr::specpages
{
    enum class Type
    {
        kBool,
        kFloat,
        kInt,
        kChoice,
        kAction
    };
    struct Option
    {
        std::string value, text, help;
    };
    struct Row
    {
        std::string         group;        // section titles, " / "-joined
        bool                collapsible{ false };
        int                 file{ -1 };   // Mod::files index
        std::string         section, key;
        Type                type{ Type::kBool };
        std::string         def;
        float               mn{ 0 }, mx{ 1 }, step{ 0 };
        int                 decimals{ -1 };
        std::string         unit;
        std::vector<Option> options;
        std::string         label, help;
        std::string         visible, enabled;  // '&'-joined terms: [!]S:Key:Section<op>value | [!]P:f4se plugin | [!]E:esp
        bool                restart{ false };
        std::string         action;     // kAction: the mod's native function
        std::string         flatNote;   // what differs on flat (shown in grey)
        std::string         id;         // unique: "mod\nfile\nsection\nkey" (lower)
    };
    struct Page
    {
        std::string      name, help;
        std::vector<Row> rows;
    };
    struct File
    {
        std::string name, format, path;  // format toml | ini; absolute path
        bool        viaFrik{ false };    // edited through FRIK.dll's bridge v6 table
    };
    struct Mod
    {
        std::string              name, display, help, spec;
        std::vector<File>        files;
        std::vector<Page>        pages;
        std::vector<std::string> skipped;
        bool                     isFrik{ false };
    };

    void                    Load(bool a_force = false);  // discover + parse (first call; a_force = again)
    const std::vector<Mod>& Mods();
    int                     FrikIndex();  // -1 = no FRIK spec found

    // values (a_mod = Mods() index)
    std::string Current(int a_mod, const Row& a_row);  // pending, else saved
    std::string Saved(int a_mod, const Row& a_row);    // the mod's value outside our previews
    bool        IsPending(const Row& a_row);
    void        Set(int a_mod, const Row& a_row, const std::string& a_value);
    bool        Cond(int a_mod, const std::string& a_cond);
    bool        Live(int a_mod, const Row& a_row);  // a change applies at once (FRIK table), else at SAVE / restart

    // Presenter thread, every frame while the panel is open: refresh saved values (1 Hz) and send previews (10 Hz)
    void        Tick(bool a_force = false);
    std::size_t PendingCount();
    int         PendingRestartCount();
    int         Save(std::string& a_how);  // number saved
    std::size_t Discard();
    std::string Status();
    // pipe: "rows <mod> <page>" / "set <mod> <Section> <Key> <value>" / "get <mod> <Section> <Key>"
    std::string Command(const std::string& a_arg);
}
