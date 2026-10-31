// Assembler wrapper for the Wii U game build: works around a Cemu recompiler bug.
//
// Cemu 2.6's optimising recompiler can give `mfcr` a stale cr0 when cr0 was set earlier in the same
// function (seen with `mr.` + `beq` and GCC spilling the condition across a call with
// mfcr/stw ... lwz/mtcrf). The game then takes the wrong branch, but only in Cemu and only once a
// function is hot. A `mcrf 0,0` (copies cr0 onto itself, a no-op on the real CPU) right before the
// mfcr makes Cemu read the right bits. This wrapper adds it in the compiler's assembly before
// handing it to the real assembler:
//   - before every mfcr in a function, except prologue CR saves (an mfcr before the function's first
//     local label and before anything in it that sets a CR field).
//
// GCC finds it through -B<dir of this exe>/ (see sa/wiiu-game/CMakeLists.txt); the exe must be named
// as.exe. The real assembler is devkitPPC's powerpc-eabi/bin/as.exe, found relative to this exe:
// <repo>/sa/tools/as-crfix/bin/as.exe -> <repo>/buildtools/msys64/opt/devkitpro/devkitPPC/powerpc-eabi/bin/as.exe
//
// Build (host): clang++ -O2 -std=c++20 -static as_crfix.cpp -o bin/as.exe
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <process.h>
#include <sstream>
#include <string>
#include <vector>
#include <windows.h>

namespace {
std::string Trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t");
    if (b == std::string::npos) {
        return {};
    }
    const auto e = s.find_last_not_of(" \t\r");
    return s.substr(b, e - b + 1);
}

std::string Mnemonic(const std::string& line) {
    const auto t = Trim(line);
    if (t.empty() || t[0] == '.' || t[0] == '#' || t.back() == ':') {
        return {};
    }
    const auto end = t.find_first_of(" \t");
    return end == std::string::npos ? t : t.substr(0, end);
}

bool SetsCr(const std::string& m) {
    if (m.empty()) {
        return false;
    }
    return m.back() == '.' || m.rfind("cmp", 0) == 0 || m.rfind("fcmp", 0) == 0 || m.rfind("cr", 0) == 0
        || m.rfind("mcr", 0) == 0 || m.rfind("mtcr", 0) == 0 || m.rfind("mtocrf", 0) == 0;
}

bool StartsFunction(const std::string& t) {
    return t.rfind(".cfi_startproc", 0) == 0 || (t.rfind(".type", 0) == 0 && t.find("@function") != std::string::npos);
}

// Conditional branch mnemonic -> the opposite condition, or "" if it isn't one we can invert.
std::string InvertBranch(std::string m) {
    while (!m.empty() && (m.back() == '+' || m.back() == '-')) {
        m.pop_back(); // drop prediction hints
    }
    static const char* pairs[][2] = { { "beq", "bne" }, { "blt", "bge" }, { "bgt", "ble" }, { "bso", "bns" },
                                      { "bun", "bnu" }, { "bnl", "blt" }, { "bng", "bgt" }, { "bdnz", "bdz" } };
    for (auto& p : pairs) {
        if (m == p[0]) {
            return p[1];
        }
        if (m == p[1] && std::strcmp(p[0], "bnl") != 0 && std::strcmp(p[0], "bng") != 0) {
            return p[0];
        }
    }
    return {};
}

// In a function near the 32 KB reach of conditional branches, the added mcrf lines can push a branch GCC
// sized as short out of range ("operand out of range" from the assembler). There, every conditional
// branch to a local label becomes the long form: the opposite condition skips an unconditional b.
void LongBranches(std::vector<std::string>& lines) {
    constexpr int kBigFunction = 7000; // instructions (~28 KB)
    size_t start = 0;
    auto fix = [&](size_t from, size_t to) {
        int count = 0;
        for (size_t i = from; i < to; i++) {
            if (!Mnemonic(lines[i]).empty()) {
                count++;
            }
        }
        if (count < kBigFunction) {
            return;
        }
        for (size_t i = from; i < to; i++) {
            const auto m = Mnemonic(lines[i]);
            const auto inv = InvertBranch(m);
            if (inv.empty()) {
                continue;
            }
            auto ops = Trim(Trim(lines[i]).substr(m.size()));
            const auto comma = ops.find(',');
            const auto crField = comma == std::string::npos ? std::string() : ops.substr(0, comma + 1);
            const auto target = comma == std::string::npos ? ops : Trim(ops.substr(comma + 1));
            if (target.rfind(".L", 0) != 0) {
                continue;
            }
            lines[i] = "\t" + inv + " " + crField + "1f\n\tb " + target + "\n1:";
        }
    };
    for (size_t i = 0; i < lines.size(); i++) {
        if (Trim(lines[i]).rfind(".type", 0) == 0 && lines[i].find("@function") != std::string::npos) {
            fix(start, i);
            start = i;
        }
    }
    fix(start, lines.size());
}

// Returns the number of mcrf lines added
int Transform(const std::string& in, const std::string& out) {
    std::ifstream f(in, std::ios::binary);
    if (!f) {
        return -1;
    }
    std::vector<std::string> lines;
    std::string line;
    bool inPrologue = true;
    int added = 0;
    while (std::getline(f, line)) {
        const auto t = Trim(line);
        if (StartsFunction(t)) {
            inPrologue = true;
        } else if (!t.empty() && t.back() == ':' && t.rfind(".L", 0) == 0) {
            inPrologue = false;
        } else {
            const auto m = Mnemonic(line);
            if (m == "mfcr" && !inPrologue) {
                lines.push_back("\tmcrf 0,0");
                added++;
            }
            if (SetsCr(m)) {
                inPrologue = false;
            }
        }
        lines.push_back(line);
    }
    if (added) {
        LongBranches(lines);
    }
    std::ostringstream o;
    for (auto& l : lines) {
        o << l << '\n';
    }
    std::ofstream w(out, std::ios::binary);
    w << o.str();
    return w ? added : -1;
}

std::string RealAs() {
    char self[MAX_PATH], full[MAX_PATH];
    GetModuleFileNameA(nullptr, self, MAX_PATH);
    GetFullPathNameA(self, MAX_PATH, full, nullptr); // GCC runs it as .../sa/wiiu-game/../tools/...
    std::string p = full;
    for (int i = 0; i < 5; i++) { // as.exe, bin, as-crfix, tools, sa
        p = p.substr(0, p.find_last_of("\\/"));
    }
    return p + "\\buildtools\\msys64\\opt\\devkitpro\\devkitPPC\\powerpc-eabi\\bin\\as.exe";
}
} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv, argv + argc);
    std::vector<std::string> temps;
    for (int i = 1; i < argc; i++) {
        if (args[i] == "-o") {
            i++;
            continue;
        }
        const auto& a = args[i];
        if (a.size() > 2 && a[0] != '-' && (a.ends_with(".s") || a.ends_with(".S"))) {
            const auto tmp = a + ".crfix.s";
            if (Transform(a, tmp) < 0) {
                std::fprintf(stderr, "as-crfix: can't rewrite %s\n", a.c_str());
                return 1;
            }
            args[i] = tmp;
            temps.push_back(tmp);
        }
    }
    const auto real = RealAs();
    args[0] = real;
    std::vector<const char*> cargs;
    for (auto& a : args) {
        cargs.push_back(a.c_str());
    }
    cargs.push_back(nullptr);
    const auto rc = _spawnv(_P_WAIT, real.c_str(), cargs.data());
    for (auto& t : temps) {
        std::remove(t.c_str());
    }
    if (rc < 0) {
        std::fprintf(stderr, "as-crfix: can't run %s\n", real.c_str());
        return 1;
    }
    return (int)rc;
}
