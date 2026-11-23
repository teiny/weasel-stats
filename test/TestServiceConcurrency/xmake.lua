target("TestServiceConcurrency")
  set_kind("binary")
  add_files("./*.cpp", "$(projectdir)/RimeWithWeasel/DeploymentSnapshot.cpp",
    "$(projectdir)/WeaselIPCServer/SecurityAttribute.cpp")
  add_includedirs("$(projectdir)/WeaselIPCServer")
  add_deps("WeaselUI", "WeaselIPC")
  add_defines("RIME_IMPORTS")
  add_links("rime", "imm32", "usp10", "comctl32", "gdiplus", "shcore")
  add_rules("subcmd")
  after_build(function(target)
    local runtime = is_arch("x86") and "output/Win32/rime.dll" or "output/rime.dll"
    os.cp(path.join("$(projectdir)", runtime), target:targetdir())
  end)
