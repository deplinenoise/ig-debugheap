local common = {
  Env = {
    CCOPTS = {
      -- clang and GCC
      { "-g"; Config = { "*-gcc-debug", "*-clang-debug" } },
      { "-g -O2"; Config = { "*-gcc-production", "*-clang-production" } },
      { "-O3"; Config = { "*-gcc-release", "*-clang-release" } },
      { "-Wall", "-Werror", "-Wextra", "-Wno-unused-parameter", "-Wno-unused-function"
        ; Config = { "*-gcc-*", "*-clang-*" }
      },
      { "/W4"; Config = { "*-msvc-*" } },
    },

    CPPDEFS = {
      { "NDEBUG"; Config = "*-*-release" },
    },
  },
}

Build {
  Units = function ()

    local demo = Program {
      Name = "demo",
      Sources = {
        "DebugHeap.c",
        "demo.c",
      },
    }

    Default(demo)
  end,

  Configs = {
    Config {
      Name = "macosx-clang",
      Inherit = common,
      DefaultOnHost = "macosx",
      Tools = { "clang-osx", },
    },
    Config {
      Name = "linux-gcc",
      Inherit = common,
      DefaultOnHost = "linux",
      Tools = { "gcc", },
    },
    Config {
      Name = "win64-msvc",
      Inherit = common,
      DefaultOnHost = "windows",
      Tools = { { "msvc-vs2012"; TargetArch = "x64" }, },
    },
  },

  Variants = {
    { Name = "debug",   Options = { GeneratePdb = true } },
    { Name = "release" },
  },
  DefaultVariant = "debug",
}
