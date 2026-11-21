// larecomp_online_tool: see online_tool.h and README.md next to this file.
//
//   larecomp_online_tool --version | --protocol | servers [--master H:P] [--servers a,b] [--current H:P] [--icons DIR]
//   larecomp_online_tool identity <show|check|backup|restore|recovery|recover|cancel|rename> ...   (identity_commands.h)
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

#include "identity_commands.h"
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

// The first line of stdin without its line end: where a password or a recovery code comes in (never on the command line).
// At most 4096 bytes are kept (the identity commands refuse more than 1024).
bool ReadStdinLine(std::string& out) {
    out.clear();
    int c;
    while ((c = std::fgetc(stdin)) != EOF) {
        if (c == '\n') break;
        if (out.size() < 4096) out.push_back(char(c));
    }
    if (!out.empty() && out.back() == '\r') out.pop_back();
    return !(c == EOF && out.empty());
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
    const std::vector<std::string> words = Arguments(argc, argv);
    if (!words.empty() && words[0] == "identity") {
        if (words.size() == 2 && (words[1] == "--help" || words[1] == "-h")) {
            std::fputs(IdentityUsageText(), stdout);
            return 0;
        }
#ifdef _WIN32
        _setmode(_fileno(stdin), _O_BINARY);
#endif
        const IdentityResult r = RunIdentity(std::vector<std::string>(words.begin() + 1, words.end()), ReadStdinLine);
        Print(r.json);
        return r.exit_code;
    }
    const ParseResult parsed = ParseArgs(words);
    if (!parsed.ok) {
        Print(RenderError(CommandName(parsed.args.command), "usage", parsed.error, false));
        return 2;
    }
    const Args& a = parsed.args;
    switch (a.command) {
        case Command::kHelp:
            std::fputs(UsageText(), stdout);
            std::fputs(IdentityUsageText(), stdout);
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
