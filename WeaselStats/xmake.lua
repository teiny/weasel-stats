target("WeaselStats")
  set_kind("binary")
  add_files("./*.cpp")
  add_rules("add_rcfiles", "subwin")
  add_links("kernel32", "advapi32")

  add_ldflags("/DEBUG /OPT:REF /OPT:ICF /LARGEADDRESSAWARE /ERRORREPORT:QUEUE")
  before_build(function(target)
    local target_dir = path.join(target:targetdir(), target:name())
    if not os.exists(target_dir) then
      os.mkdir(target_dir)
    end
    target:set("targetdir", target_dir)
  end)
  after_build(function(target)
    if is_arch("x86") then
      os.cp(path.join(target:targetdir(), "WeaselStats.exe"), "$(projectdir)/output/Win32")
      os.cp(path.join(target:targetdir(), "WeaselStats.pdb"), "$(projectdir)/output/Win32")
    else
      os.cp(path.join(target:targetdir(), "WeaselStats.exe"), "$(projectdir)/output")
      os.cp(path.join(target:targetdir(), "WeaselStats.pdb"), "$(projectdir)/output")
    end
  end)
