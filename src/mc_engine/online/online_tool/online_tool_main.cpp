// larecomp_online_tool: see online_tool.h and README.md next to this file.
//
//   larecomp_online_tool --version | --protocol | servers [--master H:P] [--servers a,b] [--current H:P] [--icons DIR]
//
// One JSON document on stdout per call, nothing else there; exit code 0 = the document is the answer, 2 = wrong
// command line. Standalone: no ReXGlue, no game, no settings file.
//
//   cmake --build <build dir> --target larecomp_online_tool
// or, with no build directory at all (Windows):
//   clang++ -O2 -std=c++20 -I src/mc_engine/online/online_tool <the files of the target> -lws2_32 -lshell32

#include <cstdio>
#include <string>
#include <vector>

#include "online_tool.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

// The command line as UTF-8 (main()'s own argv is the ANSI code page on Windows: a folder with an accent in its name
// would not survive).
std::vector<std::string> Arguments(int argc, char** argv) {
    std::vector<std::string> out;
#ifdef _WIN32
    (void)argc;
    (void)argv;
    int n = 0;
    wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &n);
    if (wide) {
        for (int i = 1; i < n; ++i) {
            const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
            std::string s(bytes > 0 ? size_t(bytes - 1) : 0, '\0');
            if (bytes > 1) WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, s.data(), bytes, nullptr, nullptr);
            out.push_back(std::move(s));
        }
        LocalFree(wide);
        return out;
    }
#endif
    for (int i = 1; i < argc; ++i) out.emplace_back(argv[i]);
    return out;
}

void Print(const std::string& text) {
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);  // the document is UTF-8 bytes with plain newlines
#endif
    using namespace larecomp::online_tool;
    const ParseResult parsed = ParseArgs(Arguments(argc, argv));
    if (!parsed.ok) {
        Print(RenderError(CommandName(parsed.args.command), "usage", parsed.error, false));
        return 2;
    }
    const Args& a = parsed.args;
    switch (a.command) {
        case Command::kHelp:
            std::fputs(UsageText(), stdout);
            return 0;
        case Command::kVersion:
            Print(RenderVersion(a.pretty));
            return 0;
        case Command::kProtocol:
            Print(RenderProtocol(a.pretty));
            return 0;
        case Command::kServers:
            Print(RenderServers(GatherServers(a), a.pretty));
            return 0;
        case Command::kNone:
            break;
    }
    Print(RenderError("none", "usage", "no command", false));
    return 2;
}
