-- fo4flatvr: flat Fallout 4 1.11.240 -> OpenXR injector (feasibility build)
set_xmakever("3.0.0")
set_project("fo4flatvr")
set_version("0.0.1")
-- MSVC only: from Git Bash xmake otherwise defaults to mingw and CommonLibF4's packages fail.
set_allowedplats("windows")
set_defaultplat("windows")
set_arch("x64")
set_languages("c++23")
set_warnings("allextra")
set_encodings("utf-8")
add_rules("mode.debug", "mode.releasedbg")

includes("extern/commonlibf4")

add_requires("openxr 1.1.49")

-- OpenXR + D3D11 core. No game or CommonLibF4 dependency, so it can be tested standalone.
target("xrcore", function()
    set_kind("static")
    add_files("src/xr/**.cpp")
    add_headerfiles("src/xr/**.h")
    add_includedirs("src", { public = true })
    add_packages("openxr", { public = true })
    add_syslinks("d3d11", "dxgi", "d3dcompiler", { public = true })
    add_defines("XR_USE_GRAPHICS_API_D3D11", "XR_USE_PLATFORM_WIN32", "NOMINMAX", "WIN32_LEAN_AND_MEAN", { public = true })
end)

-- Standalone check of xrcore against any OpenXR runtime (e.g. Meta XR Simulator via XR_RUNTIME_JSON).
target("xr_smoke", function()
    set_kind("binary")
    set_default(false)
    add_deps("xrcore")
    add_files("tests/xr_smoke.cpp")
end)

-- The F4SE plugin.
target("fo4flatvr", function()
    add_rules("commonlibf4.plugin", {
        name = "fo4flatvr",
        author = "local",
        description = "Flat Fallout 4 to OpenXR (feasibility)"
    })
    add_deps("xrcore")
    -- _retired/: code taken out of the build (kept for reference, see _retired/README.txt)
    add_files("src/plugin/**.cpp|_retired/**")
    add_headerfiles("src/plugin/**.h|_retired/**")
end)
