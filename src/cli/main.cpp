// ws.exe: Win顺 on the command line (docs/design-cli.md). Everything is in
// Run.cpp and the files next to it; this only hands over the arguments.
//
// wmain: the C runtime's wide arguments, split by the same rules as
// CommandLineToArgvW (never the ANSI ones, which lose Chinese), without
// loading shell32.dll on every run.

#include "Run.h"

#include <string>
#include <vector>

int wmain(int argc, wchar_t** argv)
{
    std::vector<std::wstring> args;
    args.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0);
    for (int i = 1; i < argc; ++i)
        args.emplace_back(argv[i]);
    return ws::cli::run(args);
}
