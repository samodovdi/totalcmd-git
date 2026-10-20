// totalcmd-git: posh-git style status in the Total Commander command line prompt.
//
// The DLL poses as a content plugin (WDX) so that TC loads it into its own
// process. Once loaded, a worker thread finds the prompt label to the left of
// the command line ("d:\foo>") and subclasses it. Every time TC sets a new path
// there, the text is decorated with the cached git status of that directory,
// and `git status` is re-run in the background to refresh it.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string>
#include <unordered_map>
#include <vector>
#include "contentplug.h"

namespace {

// Roles of the status parts, colored like posh-git's defaults.
enum Color { kDefault = -1, kDelim, kBranchSync, kAhead, kBehind, kDiverged, kGone, kIndex, kWorking, kStaged, kColorCount };

// totalcmd-git.ini keys of the colors in [ColorsLight] / [ColorsDark], in Color order.
const wchar_t* const kColorKeys[kColorCount] = {
    L"delimiters", L"branchidentical", L"branchahead", L"branchbehind", L"branchdiverged",
    L"branchgone", L"index",           L"working",     L"staged",
};

// Written next to the DLL when missing; also the source of all defaults.
// The colors are posh-git's console colors, adjusted to stay readable on
// light and dark panels.
const char kDefaultIni[] = u8R"(; totalcmd-git: posh-git style git status in the Total Commander command line prompt.
; Saved changes are picked up automatically, no TC restart needed.
; Quote values that start or end with spaces.

[General]
; 0 turns the plugin off.
Enabled=1
; Full path to git.exe; empty means search PATH.
GitPath=
; Time limit for one "git status" run, ms.
Timeout=5000
; Shorten the middle of long paths (d:\…\srcpp) so the status always fits.
ShortenPath=1
; Re-run git status when the TC window becomes active again.
RefreshOnActivate=1

[Format]
BeforeStatus=" ["
DelimStatus=" |"
AfterStatus="]"
BranchIdentical=≡
BranchAhead=↑
BranchBehind=↓
BranchGone=×
LocalWorking=!
LocalStaged=~
; 0 hides zero counters: "+0 ~1 -0" becomes "~1".
ShowStatusWhenZero=1
Ellipsis=…

; Colors as #RRGGBB. [ColorsLight] is used when TC draws dark text (light
; theme), [ColorsDark] when it draws light text.
[ColorsLight]
Delimiters=#9A6A00
BranchIdentical=#007C9C
BranchAhead=#1E8C1E
BranchBehind=#C82A2A
BranchDiverged=#9A6A00
BranchGone=#005F73
Index=#106E10
Working=#A81C1C
Staged=#007C9C

[ColorsDark]
Delimiters=#E5C07B
BranchIdentical=#56C8D8
BranchAhead=#8CD06C
BranchBehind=#F06C75
BranchDiverged=#E5C07B
BranchGone=#3AA0B0
Index=#5FB85F
Working=#E06060
Staged=#56C8D8
)";

struct Config {
    bool enabled, shortenPath, refreshOnActivate, showZero;
    std::wstring gitExe;
    DWORD timeoutMs;
    std::wstring before, delim, after, identical, ahead, behind, gone, working, staged, ellipsis;
    COLORREF light[kColorCount], dark[kColorCount];
};

struct Seg {
    std::wstring text;
    int color;
};
using Segments = std::vector<Seg>;

struct State {
    SRWLOCK lock = SRWLOCK_INIT;
    CONDITION_VARIABLE wake = CONDITION_VARIABLE_INIT;
    HWND label = nullptr;
    WNDPROC origProc = nullptr;
    std::wstring rawText;  // prompt text as TC set it, e.g. L"d:\\foo>"
    std::wstring pending;  // directory waiting for a git status run
    std::unordered_map<std::wstring, Segments> cache;  // dir -> " [main ≡]" parts, empty outside git
    Config config;
    std::wstring iniPath;
    FILETIME iniTime{};
};

// Allocated once and never freed: the DLL is pinned and the worker thread
// may still be running while the process shuts down.
State* g = nullptr;
UINT g_refreshMsg = 0;

struct Lock {
    Lock() { AcquireSRWLockExclusive(&g->lock); }
    ~Lock() { ReleaseSRWLockExclusive(&g->lock); }
};

// Appends to %TEMP%\totalcmd-git.log.
void Log(const std::wstring& s) {
    wchar_t path[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, path);
    if (!n || n > MAX_PATH - 20) return;
    wcscat_s(path, L"totalcmd-git.log");
    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t stamp[64];
    swprintf_s(stamp, L"%02d:%02d:%02d.%03d [%lu:%lu] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
               GetCurrentProcessId(), GetCurrentThreadId());
    std::wstring line = stamp + s + L"\r\n";
    int len = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), nullptr, 0, nullptr, nullptr);
    std::string utf8(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), utf8.data(), len, nullptr, nullptr);
    DWORD written;
    WriteFile(h, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
    CloseHandle(h);
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

bool IsDir(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool IsFile(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::string ReadSmallFile(const std::wstring& p) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    char buf[4096];
    DWORD n = 0;
    ReadFile(h, buf, sizeof(buf), &n, nullptr);
    CloseHandle(h);
    std::string s(buf, n);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

// Walks up from `dir` looking for .git (directory, or file with "gitdir: ...").
// Returns the git directory, or empty if `dir` is not inside a work tree.
std::wstring FindGitDir(std::wstring dir) {
    while (!dir.empty() && dir.back() == L'\\') dir.pop_back();
    for (;;) {
        std::wstring dotGit = dir + L"\\.git";
        if (IsDir(dotGit)) return dotGit;
        if (IsFile(dotGit)) {
            std::wstring ref = Utf8ToWide(ReadSmallFile(dotGit));
            if (ref.rfind(L"gitdir: ", 0) != 0) return {};
            ref = ref.substr(8);
            for (auto& c : ref) if (c == L'/') c = L'\\';
            bool absolute = ref.size() > 1 && (ref[1] == L':' || ref[0] == L'\\');
            return absolute ? ref : dir + L"\\" + ref;
        }
        size_t slash = dir.find_last_of(L'\\');
        if (slash == std::wstring::npos) return {};
        dir.resize(slash);
    }
}

bool IsInsideGitDir(const std::wstring& dir) {
    std::wstring d = dir + L"\\";
    for (auto& c : d) c = towlower(c);
    return d.find(L"\\.git\\") != std::wstring::npos;
}

std::string ReadWholeFile(const std::wstring& p) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::string s;
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n) s.append(buf, n);
    CloseHandle(h);
    return s;
}

std::wstring Trim(const std::wstring& s) {
    size_t b = s.find_first_not_of(L" \t\r\n"), e = s.find_last_not_of(L" \t\r\n");
    return b == std::wstring::npos ? std::wstring() : s.substr(b, e - b + 1);
}

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = towlower(c);
    return s;
}

// Applies the settings of an ini text (UTF-8) on top of `c`.
void ApplyIni(Config& c, const std::string& utf8) {
    std::wstring text = Utf8ToWide(utf8), section;
    size_t pos = text.size() && text[0] == 0xFEFF ? 1 : 0;
    while (pos < text.size()) {
        size_t eol = text.find(L'\n', pos);
        if (eol == std::wstring::npos) eol = text.size();
        std::wstring line = Trim(text.substr(pos, eol - pos));
        pos = eol + 1;
        if (line.empty() || line[0] == L';' || line[0] == L'#') continue;
        if (line[0] == L'[') {
            section = Lower(Trim(line.substr(1, line.find(L']') - 1)));
            continue;
        }
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring key = Lower(Trim(line.substr(0, eq))), value = Trim(line.substr(eq + 1));
        if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"') value = value.substr(1, value.size() - 2);
        bool flag = value == L"1" || Lower(value) == L"true" || Lower(value) == L"yes";

        if (section == L"general") {
            if (key == L"enabled") c.enabled = flag;
            else if (key == L"gitpath") c.gitExe = value;
            else if (key == L"timeout") c.timeoutMs = wcstoul(value.c_str(), nullptr, 10);
            else if (key == L"shortenpath") c.shortenPath = flag;
            else if (key == L"refreshonactivate") c.refreshOnActivate = flag;
        } else if (section == L"format") {
            if (key == L"beforestatus") c.before = value;
            else if (key == L"delimstatus") c.delim = value;
            else if (key == L"afterstatus") c.after = value;
            else if (key == L"branchidentical") c.identical = value;
            else if (key == L"branchahead") c.ahead = value;
            else if (key == L"branchbehind") c.behind = value;
            else if (key == L"branchgone") c.gone = value;
            else if (key == L"localworking") c.working = value;
            else if (key == L"localstaged") c.staged = value;
            else if (key == L"showstatuswhenzero") c.showZero = flag;
            else if (key == L"ellipsis") c.ellipsis = value;
        } else if (section == L"colorslight" || section == L"colorsdark") {
            COLORREF* palette = section == L"colorslight" ? c.light : c.dark;
            unsigned rgb = 0;
            if (value.size() != 7 || value[0] != L'#' || swscanf_s(value.c_str() + 1, L"%x", &rgb) != 1) continue;
            for (int i = 0; i < kColorCount; i++)
                if (key == kColorKeys[i]) palette[i] = RGB(rgb >> 16, (rgb >> 8) & 0xFF, rgb & 0xFF);
        }
    }
}

bool IniWriteTime(FILETIME& t) {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(g->iniPath.c_str(), GetFileExInfoStandard, &fa)) return false;
    t = fa.ftLastWriteTime;
    return true;
}

// Reads totalcmd-git.ini into g->config, creating it with the defaults if missing.
void LoadConfig() {
    Config c{};
    ApplyIni(c, kDefaultIni);
    FILETIME time{};
    if (IniWriteTime(time)) {
        ApplyIni(c, ReadWholeFile(g->iniPath));
    } else {
        std::string text = "\xEF\xBB\xBF";
        for (const char* p = kDefaultIni; *p; p++) text += *p == '\n' ? std::string("\r\n") : std::string(1, *p);
        HANDLE h = CreateFileW(g->iniPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD written;
            WriteFile(h, text.data(), (DWORD)text.size(), &written, nullptr);
            CloseHandle(h);
        }
        IniWriteTime(time);
    }
    if (c.timeoutMs < 500) c.timeoutMs = 500;
    if (c.gitExe.empty()) {
        wchar_t path[MAX_PATH];
        c.gitExe = SearchPathW(nullptr, L"git.exe", nullptr, MAX_PATH, path, nullptr)
                       ? path
                       : L"C:\\Program Files\\Git\\cmd\\git.exe";
    }
    Lock l;
    g->config = c;
    g->iniTime = time;
}

// True when totalcmd-git.ini was saved since it was last loaded.
bool ConfigChanged() {
    FILETIME time;
    if (!IniWriteTime(time)) return false;
    Lock l;
    return CompareFileTime(&time, &g->iniTime) != 0;
}

Config GetConfig() {
    Lock l;
    return g->config;
}

// Runs git in `dir` and returns its stdout; false on failure or timeout.
bool RunGit(const Config& c, const std::wstring& dir, const wchar_t* args, std::string& out) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_EXISTING, 0, nullptr);

    // Restrict inheritance to our two handles, so that TC's own inheritable
    // handles don't leak into git and other processes don't keep our pipe open.
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    auto attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(GetProcessHeap(), 0, attrSize);
    InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize);
    HANDLE inherit[2] = {wr, nul};
    UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(inherit), nullptr, nullptr);

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = nul;
    si.StartupInfo.hStdOutput = wr;
    si.StartupInfo.hStdError = nul;
    si.lpAttributeList = attrs;

    std::wstring cmd = L"\"" + c.gitExe + L"\" " + args;
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, dir.c_str(),
                             &si.StartupInfo, &pi);
    CloseHandle(wr);
    CloseHandle(nul);
    DeleteProcThreadAttributeList(attrs);
    HeapFree(GetProcessHeap(), 0, attrs);
    if (!ok) {
        CloseHandle(rd);
        return false;
    }

    out.clear();
    bool exited = false;
    ULONGLONG deadline = GetTickCount64() + c.timeoutMs;
    char buf[8192];
    for (;;) {
        DWORD avail = 0, n = 0;
        if (!PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr)) break;  // writer closed
        if (avail) {
            if (!ReadFile(rd, buf, sizeof(buf), &n, nullptr)) break;
            out.append(buf, n);
            continue;
        }
        if (GetTickCount64() > deadline) {
            TerminateProcess(pi.hProcess, 1);
            break;
        }
        WaitForSingleObject(pi.hProcess, 20);
    }
    DWORD code = 1;
    exited = WaitForSingleObject(pi.hProcess, 1000) == WAIT_OBJECT_0 && GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(rd);
    return exited && code == 0;
}

// "|MERGING", "|REBASE-i 2/5", ... like posh-git; empty when nothing in progress.
std::wstring RepoState(const std::wstring& gitDir) {
    auto step = [&](const std::wstring& d) {
        std::string n = ReadSmallFile(d + L"\\msgnum"), e = ReadSmallFile(d + L"\\end");
        if (n.empty()) n = ReadSmallFile(d + L"\\next");
        if (e.empty()) e = ReadSmallFile(d + L"\\last");
        return (n.empty() || e.empty()) ? std::wstring() : L" " + Utf8ToWide(n) + L"/" + Utf8ToWide(e);
    };
    if (IsDir(gitDir + L"\\rebase-merge")) {
        bool interactive = IsFile(gitDir + L"\\rebase-merge\\interactive");
        return (interactive ? L"|REBASE-i" : L"|REBASE-m") + step(gitDir + L"\\rebase-merge");
    }
    if (IsDir(gitDir + L"\\rebase-apply")) {
        const wchar_t* what = IsFile(gitDir + L"\\rebase-apply\\rebasing") ? L"|REBASE"
                              : IsFile(gitDir + L"\\rebase-apply\\applying") ? L"|AM"
                                                                              : L"|AM/REBASE";
        return what + step(gitDir + L"\\rebase-apply");
    }
    if (IsFile(gitDir + L"\\MERGE_HEAD")) return L"|MERGING";
    if (IsFile(gitDir + L"\\CHERRY_PICK_HEAD")) return L"|CHERRY-PICKING";
    if (IsFile(gitDir + L"\\REVERT_HEAD")) return L"|REVERTING";
    if (IsFile(gitDir + L"\\BISECT_LOG")) return L"|BISECTING";
    return {};
}

struct Counts {
    int added = 0, modified = 0, deleted = 0, unmerged = 0;
    bool Any() const { return added || modified || deleted || unmerged; }
    std::wstring Format(const Config& c) const {
        std::wstring s;
        if (added || c.showZero) s += L" +" + std::to_wstring(added);
        if (modified || c.showZero) s += L" ~" + std::to_wstring(modified);
        if (deleted || c.showZero) s += L" -" + std::to_wstring(deleted);
        if (unmerged) s += L" !" + std::to_wstring(unmerged);
        return s;
    }
};

void Count(char c, Counts& k) {
    switch (c) {
        case 'A': k.added++; break;
        case 'M': case 'T': case 'R': case 'C': k.modified++; break;
        case 'D': k.deleted++; break;
    }
}

// Builds the posh-git style decoration for `dir`, e.g. " [main ≡ +0 ~1 -0 !]".
Segments ComputeDecoration(const Config& c, const std::wstring& dir) {
    if (!IsDir(dir) || IsInsideGitDir(dir)) return {};
    std::wstring gitDir = FindGitDir(dir);
    if (gitDir.empty()) return {};

    std::string out;
    if (!RunGit(c, dir, L"--no-optional-locks status --porcelain=v2 --branch", out)) {
        Log(L"git status failed in " + dir);
        return {};
    }

    std::string head, oid;
    bool hasUpstream = false, hasAb = false;
    int ahead = 0, behind = 0;
    Counts index, work;
    size_t pos = 0;
    while (pos < out.size()) {
        size_t eol = out.find('\n', pos);
        if (eol == std::string::npos) eol = out.size();
        std::string line = out.substr(pos, eol - pos);
        pos = eol + 1;
        if (line.empty()) continue;
        if (line.rfind("# branch.head ", 0) == 0) head = line.substr(14);
        else if (line.rfind("# branch.oid ", 0) == 0) oid = line.substr(13);
        else if (line.rfind("# branch.upstream ", 0) == 0) hasUpstream = true;
        else if (line.rfind("# branch.ab ", 0) == 0) {
            hasAb = sscanf_s(line.c_str() + 12, "+%d -%d", &ahead, &behind) == 2;
        } else if ((line[0] == '1' || line[0] == '2') && line.size() > 3) {
            Count(line[2], index);
            Count(line[3], work);
        } else if (line[0] == 'u') {
            work.unmerged++;
        } else if (line[0] == '?') {
            work.added++;
        }
    }

    std::wstring branch = head == "(detached)"
                              ? L"(" + Utf8ToWide(oid.substr(0, 7)) + L"...)"
                              : Utf8ToWide(head);
    auto symbol = [](const std::wstring& sym) { return sym.empty() ? sym : L" " + sym; };
    std::wstring branchStatus;
    int branchColor = kBranchSync;
    if (hasUpstream && !hasAb) {
        branchStatus = symbol(c.gone);
        branchColor = kGone;
    } else if (hasAb && ahead == 0 && behind == 0) {
        branchStatus = symbol(c.identical);
    } else if (hasAb) {
        if (behind) branchStatus += L" " + c.behind + std::to_wstring(behind);
        if (ahead) branchStatus += L" " + c.ahead + std::to_wstring(ahead);
        branchColor = ahead && behind ? kDiverged : ahead ? kAhead : kBehind;
    }

    Segments s{{c.before, kDelim}, {branch + RepoState(gitDir) + branchStatus, branchColor}};
    if (index.Any()) s.push_back({index.Format(c), kIndex});
    if (index.Any() && work.Any()) s.push_back({c.delim, kDelim});
    if (work.Any()) s.push_back({work.Format(c) + symbol(c.working), kWorking});
    else if (index.Any()) s.push_back({symbol(c.staged), kStaged});
    s.push_back({c.after, kDelim});
    return s;
}

// "d:\foo>" -> "d:\foo"; empty if this does not look like a prompt.
std::wstring DirFromPrompt(const std::wstring& raw) {
    if (raw.size() < 3 || raw.back() != L'>' || raw[1] != L':') return {};
    return raw.substr(0, raw.size() - 1);
}

// Remembers `raw` as the current prompt and schedules a git status run for it.
void OnPromptSet(const std::wstring& raw) {
    std::wstring dir = DirFromPrompt(raw);
    Lock l;
    g->rawText = raw;
    if (dir.empty()) return;
    g->pending = dir;
    WakeConditionVariable(&g->wake);
}

// ---- Text drawing hooks. TC paints the prompt from its own copy of the
// caption, not from the window text, so the decoration is inserted into the
// string while it is being drawn. The hooks only act while the prompt label
// is handling WM_PAINT on this thread.

thread_local HWND t_painting = nullptr;

// The prompt to paint instead of TC's text, and the settings to paint it with.
struct Prompt {
    Segments segs;  // path, git status parts, '>'
    Config config;
};

// If `s` is the prompt being painted ("...>"), returns it split into colored
// parts: the path, the git status of the current directory, and the '>'.
bool Substitute(const wchar_t* s, int n, Prompt& out) {
    if (!t_painting || !s || n <= 0 || s[n - 1] != L'>') return false;
    Segments deco;
    {
        Lock l;
        if (!g->config.enabled) return false;
        auto it = g->cache.find(DirFromPrompt(g->rawText));
        if (it != g->cache.end()) deco = it->second;
        out.config = g->config;
    }
    if (deco.empty()) return false;
    out.segs.clear();
    out.segs.push_back({std::wstring(s, n - 1), kDefault});
    out.segs.insert(out.segs.end(), deco.begin(), deco.end());
    out.segs.push_back({L">", kDefault});
    return true;
}

int TextWidth(HDC dc, const std::wstring& t) {
    SIZE size{};
    GetTextExtentPoint32W(dc, t.c_str(), (int)t.size(), &size);
    return size.cx;
}

// Shortens the path (first part) with a middle ellipsis, "d:\…\src\app",
// keeping as many trailing directories as fit so that the prompt is at most
// `width` wide. Leaves it as is when it fits or cannot be shortened.
void FitPath(HDC dc, Prompt& p, int width) {
    if (!p.config.shortenPath) return;
    std::wstring& path = p.segs[0].text;
    int rest = 0;
    for (size_t i = 1; i < p.segs.size(); i++) rest += TextWidth(dc, p.segs[i].text);
    if (TextWidth(dc, path) + rest <= width) return;

    size_t root = path.find(L'\\');
    if (root == std::wstring::npos) return;
    std::wstring head = path.substr(0, root + 1) + p.config.ellipsis;
    std::vector<std::wstring> dirs;
    for (size_t pos = root + 1; pos < path.size();) {
        size_t next = path.find(L'\\', pos);
        if (next == std::wstring::npos) next = path.size();
        if (next > pos) dirs.push_back(path.substr(pos, next - pos));
        pos = next + 1;
    }
    if (dirs.size() < 2) return;

    std::wstring best;
    for (size_t keep = dirs.size() - 1; keep >= 1; keep--) {
        best = head;
        for (size_t i = dirs.size() - keep; i < dirs.size(); i++) best += L"\\" + dirs[i];
        if (TextWidth(dc, best) + rest <= width) break;
    }
    path = best;
}

std::wstring Join(const Segments& segs) {
    std::wstring s;
    for (auto& seg : segs) s += seg.text;
    return s;
}

// Lets the longer text use the whole label instead of the original text box.
RECT Widen(const RECT& r) {
    RECT client, w = r;
    if (GetClientRect(t_painting, &client) && client.right > w.right) w.right = client.right;
    return w;
}

// Draws `segs` left to right starting at `x`, each in its own color;
// `draw(x, seg)` outputs one part with the DC's current text color.
template <typename Draw>
void DrawSegments(HDC dc, int x, const Prompt& p, Draw draw) {
    COLORREF text = GetTextColor(dc);
    bool darkText = GetRValue(text) * 299 + GetGValue(text) * 587 + GetBValue(text) * 114 < 128000;
    const COLORREF* palette = darkText ? p.config.light : p.config.dark;
    for (auto& seg : p.segs) {
        SetTextColor(dc, seg.color == kDefault ? text : palette[seg.color]);
        draw(x, seg.text);
        x += TextWidth(dc, seg.text);
    }
    SetTextColor(dc, text);
}

// Per-part drawing needs left-aligned output that does not move the current position.
bool CanDrawSegments(HDC dc) {
    return (GetTextAlign(dc) & (TA_CENTER | TA_RIGHT | TA_UPDATECP)) == 0;
}

decltype(&ExtTextOutW) g_extTextOutW;
decltype(&TextOutW) g_textOutW;
decltype(&DrawTextW) g_drawTextW;

BOOL WINAPI HookExtTextOutW(HDC dc, int x, int y, UINT opts, const RECT* rc, LPCWSTR s, UINT n, const INT* dx) {
    Prompt p;
    if (!(opts & ETO_GLYPH_INDEX) && Substitute(s, (int)n, p)) {
        RECT r = rc ? Widen(*rc) : RECT{};
        const RECT* pr = rc ? &r : nullptr;
        if (rc) FitPath(dc, p, r.right - x);
        if (!CanDrawSegments(dc)) {
            std::wstring text = Join(p.segs);
            return g_extTextOutW(dc, x, y, opts, pr, text.c_str(), (UINT)text.size(), nullptr);
        }
        if (rc && (opts & ETO_OPAQUE)) g_extTextOutW(dc, 0, 0, ETO_OPAQUE, &r, L"", 0, nullptr);
        DrawSegments(dc, x, p, [&](int sx, const std::wstring& t) {
            g_extTextOutW(dc, sx, y, opts & ~ETO_OPAQUE, pr, t.c_str(), (UINT)t.size(), nullptr);
        });
        return TRUE;
    }
    return g_extTextOutW(dc, x, y, opts, rc, s, n, dx);
}

BOOL WINAPI HookTextOutW(HDC dc, int x, int y, LPCWSTR s, int n) {
    Prompt p;
    if (Substitute(s, n, p)) {
        if (!CanDrawSegments(dc)) {
            std::wstring text = Join(p.segs);
            return g_textOutW(dc, x, y, text.c_str(), (int)text.size());
        }
        DrawSegments(dc, x, p, [&](int sx, const std::wstring& t) {
            g_textOutW(dc, sx, y, t.c_str(), (int)t.size());
        });
        return TRUE;
    }
    return g_textOutW(dc, x, y, s, n);
}

int WINAPI HookDrawTextW(HDC dc, LPCWSTR s, int n, LPRECT rc, UINT fmt) {
    Prompt p;
    if (s && n < 0) n = (int)wcslen(s);
    if (Substitute(s, n, p)) {
        if (fmt & DT_CALCRECT) {
            std::wstring text = Join(p.segs);
            return g_drawTextW(dc, text.c_str(), (int)text.size(), rc, fmt);
        }
        // TC draws the prompt right-aligned against the command line box.
        RECT r = (fmt & (DT_RIGHT | DT_CENTER)) ? *rc : Widen(*rc);
        FitPath(dc, p, r.right - r.left);
        std::wstring text = Join(p.segs);
        fmt &= ~DT_MODIFYSTRING;
        if (!CanDrawSegments(dc) || (fmt & DT_WORDBREAK))
            return g_drawTextW(dc, text.c_str(), (int)text.size(), &r, fmt);
        int total = TextWidth(dc, text);
        int x = (fmt & DT_RIGHT)    ? r.right - total
                : (fmt & DT_CENTER) ? r.left + (r.right - r.left - total) / 2
                                    : r.left;
        int height = 0;
        fmt = (fmt | DT_NOPREFIX) & ~(DT_RIGHT | DT_CENTER | DT_END_ELLIPSIS | DT_PATH_ELLIPSIS | DT_WORD_ELLIPSIS);
        DrawSegments(dc, x, p, [&](int sx, const std::wstring& t) {
            RECT part = r;
            part.left = sx;
            height = g_drawTextW(dc, t.c_str(), (int)t.size(), &part, fmt);
        });
        return height;
    }
    return g_drawTextW(dc, s, n, rc, fmt);
}

// ANSI variants: while the prompt is painted, convert and go through the
// Unicode hooks above (which fall back to plain Unicode drawing).

decltype(&ExtTextOutA) g_extTextOutA;
decltype(&TextOutA) g_textOutA;
decltype(&DrawTextA) g_drawTextA;

std::wstring AnsiToWide(LPCSTR s, int n) {
    if (n < 0) n = (int)strlen(s);
    int len = MultiByteToWideChar(CP_ACP, 0, s, n, nullptr, 0);
    std::wstring w(len, L'\0');
    MultiByteToWideChar(CP_ACP, 0, s, n, w.data(), len);
    return w;
}

BOOL WINAPI HookExtTextOutA(HDC dc, int x, int y, UINT opts, const RECT* rc, LPCSTR s, UINT n, const INT* dx) {
    if (!t_painting || !s || (opts & ETO_GLYPH_INDEX)) return g_extTextOutA(dc, x, y, opts, rc, s, n, dx);
    std::wstring w = AnsiToWide(s, (int)n);
    return HookExtTextOutW(dc, x, y, opts, rc, w.c_str(), (UINT)w.size(), nullptr);
}

BOOL WINAPI HookTextOutA(HDC dc, int x, int y, LPCSTR s, int n) {
    if (!t_painting || !s) return g_textOutA(dc, x, y, s, n);
    std::wstring w = AnsiToWide(s, n);
    return HookTextOutW(dc, x, y, w.c_str(), (int)w.size());
}

int WINAPI HookDrawTextA(HDC dc, LPCSTR s, int n, LPRECT rc, UINT fmt) {
    if (!t_painting || !s || (fmt & DT_MODIFYSTRING)) return g_drawTextA(dc, s, n, rc, fmt);
    std::wstring w = AnsiToWide(s, n);
    return HookDrawTextW(dc, w.c_str(), (int)w.size(), rc, fmt);
}

// Replaces the import address table entry of `module` that points to `target`.
bool PatchImport(HMODULE module, void* target, void* hook) {
    auto base = (BYTE*)module;
    auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto& dirEntry = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dirEntry.VirtualAddress) return false;
    bool patched = false;
    for (auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dirEntry.VirtualAddress); imp->Name; imp++) {
        for (auto thunk = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk); thunk->u1.Function; thunk++) {
            if ((void*)thunk->u1.Function != target) continue;
            DWORD old;
            VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
            thunk->u1.Function = (ULONG_PTR)hook;
            VirtualProtect(&thunk->u1.Function, sizeof(void*), old, &old);
            patched = true;
        }
    }
    return patched;
}

void InstallDrawHooks() {
    HMODULE exe = GetModuleHandleW(nullptr);
    // Resolve the final addresses (past forwarders), which is what TC's IAT holds.
    auto real = [](const wchar_t* dll, const char* name) {
        return (void*)GetProcAddress(GetModuleHandleW(dll), name);
    };
    g_extTextOutW = (decltype(g_extTextOutW))real(L"gdi32.dll", "ExtTextOutW");
    g_textOutW = (decltype(g_textOutW))real(L"gdi32.dll", "TextOutW");
    g_drawTextW = (decltype(g_drawTextW))real(L"user32.dll", "DrawTextW");
    g_extTextOutA = (decltype(g_extTextOutA))real(L"gdi32.dll", "ExtTextOutA");
    g_textOutA = (decltype(g_textOutA))real(L"gdi32.dll", "TextOutA");
    g_drawTextA = (decltype(g_drawTextA))real(L"user32.dll", "DrawTextA");
    std::wstring patched;
    if (PatchImport(exe, (void*)g_extTextOutW, (void*)&HookExtTextOutW)) patched += L" ExtTextOutW";
    if (PatchImport(exe, (void*)g_textOutW, (void*)&HookTextOutW)) patched += L" TextOutW";
    if (PatchImport(exe, (void*)g_drawTextW, (void*)&HookDrawTextW)) patched += L" DrawTextW";
    if (PatchImport(exe, (void*)g_extTextOutA, (void*)&HookExtTextOutA)) patched += L" ExtTextOutA";
    if (PatchImport(exe, (void*)g_textOutA, (void*)&HookTextOutA)) patched += L" TextOutA";
    if (PatchImport(exe, (void*)g_drawTextA, (void*)&HookDrawTextA)) patched += L" DrawTextA";
    Log(L"draw hooks:" + patched);
}

LRESULT CALLBACK LabelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    WNDPROC orig = g->origProc;
    if (msg == WM_SETTEXT && lp) OnPromptSet((const wchar_t*)lp);
    if (msg == WM_PAINT) {
        HWND prev = t_painting;
        t_painting = hwnd;
        LRESULT r = CallWindowProcW(orig, hwnd, msg, wp, lp);
        t_painting = prev;
        return r;
    }
    if (msg == g_refreshMsg) {
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    }
    if (msg == WM_NCDESTROY) {
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)orig);
        Lock l;
        g->label = nullptr;
    }
    return CallWindowProcW(orig, hwnd, msg, wp, lp);
}

std::wstring ClassOf(HWND h) {
    wchar_t c[64] = {};
    GetClassNameW(h, c, _countof(c));
    return c;
}

// The prompt label is a panel whose text ends with '>' and whose sibling is
// the command line combo box. Class names differ between 64-bit TC (Lazarus)
// and 32-bit TC (Delphi).
BOOL CALLBACK FindLabelChild(HWND h, LPARAM lp) {
    std::wstring cls = ClassOf(h);
    if (cls != L"Window" && cls != L"TMyPanel") return TRUE;
    wchar_t text[1024] = {};
    GetWindowTextW(h, text, _countof(text));
    if (DirFromPrompt(text).empty()) return TRUE;
    HWND parent = GetParent(h);
    if (!parent || (!FindWindowExW(parent, nullptr, L"LCLComboBox", nullptr) &&
                    !FindWindowExW(parent, nullptr, L"TMyComboBox", nullptr)))
        return TRUE;
    *(HWND*)lp = h;
    return FALSE;
}

BOOL CALLBACK FindTcWindow(HWND h, LPARAM lp) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || ClassOf(h) != L"TTOTAL_CMD") return TRUE;
    EnumChildWindows(h, FindLabelChild, lp);
    return *(HWND*)lp == nullptr;
}

HWND EnsureHooked() {
    {
        Lock l;
        if (g->label && IsWindow(g->label)) return g->label;
        g->label = nullptr;
    }
    HWND label = nullptr;
    EnumWindows(FindTcWindow, (LPARAM)&label);
    if (!label) return nullptr;

    wchar_t text[1024] = {};
    GetWindowTextW(label, text, _countof(text));
    {
        Lock l;
        g->origProc = (WNDPROC)GetWindowLongPtrW(label, GWLP_WNDPROC);
        g->label = label;
    }
    SetWindowLongPtrW(label, GWLP_WNDPROC, (LONG_PTR)LabelProc);
    Log(L"hooked prompt label: " + std::wstring(text));
    static bool drawHooks = false;
    if (!drawHooks) {
        InstallDrawHooks();
        drawHooks = true;
    }
    OnPromptSet(text);
    return label;
}

// ---- Autoload. TC loads content plugins on demand; a color filter that
// references the Autoload field makes it load the plugin at startup.

std::wstring IniString(const wchar_t* ini, const wchar_t* section, const std::wstring& key) {
    wchar_t buf[1024] = {};
    GetPrivateProfileStringW(section, key.c_str(), L"", buf, _countof(buf), ini);
    return buf;
}

// True if a color filter refers to a search on totalcmd-git.Autoload;
// otherwise `freeIndex` is the first unused ColorFilterN.
bool AutoloadConfigured(const wchar_t* ini, int& freeIndex) {
    for (int i = 1;; i++) {
        std::wstring filter = IniString(ini, L"Colors", L"ColorFilter" + std::to_wstring(i));
        if (filter.empty()) {
            freeIndex = i;
            return false;
        }
        if (filter[0] == L'>' &&
            Lower(IniString(ini, L"searches", filter.substr(1) + L"_plugin")).find(L"totalcmd-git.autoload") !=
                std::wstring::npos)
            return true;
    }
}

// Offers to add the autoload color filter to wincmd.ini when it is missing,
// which is the case right after the plugin has been installed.
DWORD WINAPI OfferAutoload(LPVOID) {
    wchar_t ini[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"COMMANDER_INI", ini, MAX_PATH)) return 0;
    int index = 0;
    if (AutoloadConfigured(ini, index)) return 0;

    bool ru = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN;
    const wchar_t* ask =
        ru ? L"Чтобы totalcmd-git загружался при запуске Total Commander, нужно правило раскраски файлов, "
             L"которое ссылается на поле totalcmd-git.Autoload. Оно ничего не раскрашивает.\n\n"
             L"Добавить его в wincmd.ini?"
           : L"To load totalcmd-git when Total Commander starts, a file color filter referring to the "
             L"totalcmd-git.Autoload field is needed. It does not color any files.\n\n"
             L"Add it to wincmd.ini?";
    if (MessageBoxW(nullptr, ask, L"totalcmd-git", MB_YESNO | MB_ICONQUESTION | MB_TOPMOST | MB_SETFOREGROUND) != IDYES)
        return 0;

    const std::wstring search = L"totalcmd-git_autoload";
    auto put = [&](const wchar_t* section, const std::wstring& key, const wchar_t* value) {
        return WritePrivateProfileStringW(section, key.c_str(), value, ini);
    };
    std::wstring filter = L"ColorFilter" + std::to_wstring(index);
    bool ok = put(L"searches", search + L"_SearchFor", L"") && put(L"searches", search + L"_SearchIn", L"") &&
              put(L"searches", search + L"_SearchText", L"") &&
              put(L"searches", search + L"_SearchFlags", L"0|002002000020|||||||||0000|||") &&
              put(L"searches", search + L"_plugin", L"totalcmd-git.Autoload = 1") &&
              put(L"Colors", filter, (L">" + search).c_str()) && put(L"Colors", filter + L"Color", L"0");
    Log(std::wstring(L"autoload filter ") + (ok ? L"added as " + filter : L"could not be written") + L" in " + ini);
    MessageBoxW(nullptr,
                ok ? (ru ? L"Готово. Правило заработает после перезапуска Total Commander."
                         : L"Done. The filter takes effect after Total Commander is restarted.")
                   : (ru ? L"Не удалось записать wincmd.ini. Добавьте правило вручную, см. readme."
                         : L"Could not write wincmd.ini. Please add the filter manually, see the readme."),
                L"totalcmd-git", MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONWARNING) | MB_TOPMOST);
    return 0;
}

DWORD WINAPI Worker(LPVOID) {
    bool wasForeground = false, autoloadChecked = false;
    for (;;) {
        std::wstring dir;
        {
            Lock l;
            if (g->pending.empty()) SleepConditionVariableSRW(&g->wake, &g->lock, 1000, 0);
            dir.swap(g->pending);
        }
        HWND label = EnsureHooked();
        if (!label) continue;
        if (!autoloadChecked) {
            autoloadChecked = true;
            CloseHandle(CreateThread(nullptr, 0, OfferAutoload, nullptr, 0, nullptr));
        }

        bool reload = ConfigChanged();
        if (reload) {
            LoadConfig();
            Log(L"settings reloaded");
        }
        Config config = GetConfig();

        // Refresh when the user comes back to TC: a commit made elsewhere
        // would otherwise stay invisible until the next directory change.
        bool foreground = GetForegroundWindow() == GetAncestor(label, GA_ROOT);
        if ((reload || (foreground && !wasForeground && config.refreshOnActivate)) && dir.empty()) {
            Lock l;
            if (reload) g->cache.clear();
            dir = DirFromPrompt(g->rawText);
        }
        wasForeground = foreground;
        if (dir.empty()) continue;
        if (!config.enabled) {
            PostMessageW(label, g_refreshMsg, 0, 0);
            continue;
        }

        Segments deco = ComputeDecoration(config, dir);
        {
            Lock l;
            g->cache[dir] = deco;
            label = g->label;
        }
        if (label) PostMessageW(label, g_refreshMsg, 0, 0);
    }
}

void Start() {
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&once, [](PINIT_ONCE, PVOID, PVOID*) -> BOOL {
        // Never unload: the subclassed window keeps pointing at our code.
        HMODULE self = nullptr;
        BOOL pinned = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                         (LPCWSTR)&Start, &self);
        if (!pinned) Log(L"failed to pin module, err=" + std::to_wstring(GetLastError()));
        g = new State();
        g_refreshMsg = RegisterWindowMessageW(L"totalcmd-git.refresh");
        // totalcmd-git.ini next to the plugin if it is there (portable setups),
        // otherwise next to wincmd.ini: the plugin folder is often in
        // Program Files, which the user cannot write to.
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(self, path, MAX_PATH);
        g->iniPath = path;
        g->iniPath.resize(g->iniPath.find_last_of(L'\\') + 1);
        g->iniPath += L"totalcmd-git.ini";
        wchar_t tcIni[MAX_PATH] = {};
        if (!IsFile(g->iniPath) && GetEnvironmentVariableW(L"COMMANDER_INI", tcIni, MAX_PATH)) {
            std::wstring dir = tcIni;
            g->iniPath = dir.substr(0, dir.find_last_of(L'\\') + 1) + L"totalcmd-git.ini";
        }
        LoadConfig();
        CloseHandle(CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr));
        return TRUE;
    }, nullptr, nullptr);
}

}  // namespace

// ---- WDX exports. The single field exists only so that a color filter can
// reference it, which makes TC load the plugin at startup.

extern "C" int __stdcall ContentGetSupportedField(int fieldIndex, char* fieldName, char* units, int maxlen) {
    Start();
    if (fieldIndex != 0) return ft_nomorefields;
    strncpy_s(fieldName, maxlen, "Autoload", _TRUNCATE);
    units[0] = 0;
    return ft_boolean;
}

extern "C" int __stdcall ContentGetValueW(WCHAR*, int fieldIndex, int, void* fieldValue, int, int) {
    Start();
    if (fieldIndex != 0) return ft_nosuchfield;
    *(int*)fieldValue = 0;
    return ft_boolean;
}

extern "C" int __stdcall ContentGetValue(char*, int fieldIndex, int unitIndex, void* fieldValue, int maxlen,
                                         int flags) {
    return ContentGetValueW(nullptr, fieldIndex, unitIndex, fieldValue, maxlen, flags);
}

extern "C" void __stdcall ContentSetDefaultParams(ContentDefaultParamStruct*) { Start(); }

extern "C" void __stdcall ContentPluginUnloading() {}
