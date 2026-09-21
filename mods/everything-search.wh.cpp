// ==WindhawkMod==
// @id              everything-search
// @name            Everything results in Windows Search
// @description     Replaces the Windows 11 search results with two columns: your apps, and files from Everything.
// @version         0.1
// @author          bardelyne
// @github          https://github.com/bardelyne
// @include         SearchHost.exe
// @architecture    x86-64
// @license         GPL-3.0
// @compilerOptions -lole32 -loleaut32 -lruntimeobject
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Everything results in Windows Search

Windows 11 renders its search results as a web view talking to a remote
service. This replaces that surface with two columns drawn as native XAML:
your installed apps on the left, and files from
[Everything](https://www.voidtools.com/) on the right.

Everything is not bundled. Install it yourself and leave it running; this mod
talks to the IPC window it already exposes.

## What this half does

`SearchHost.exe` is a low-integrity AppContainer. It cannot reach Everything,
cannot read the shell's app list, and cannot open files. So it does not try:
this mod is only the renderer. A separate normal-integrity broker process
watches what you type, does the searching, and pushes rows down into this
panel, which is the one direction Windows permits.

**Without the broker running, nothing changes.** The stock results are left
alone until the first batch of rows actually arrives, so a missing or crashed
broker degrades to ordinary Windows search rather than to an empty panel.

## Caveats

- Windows updates `SearchHost` out of band and more often than most shell
  binaries. The element names this looks for may be renamed, in which case the
  mod does nothing and the stock results stay.
- Clicking a row asks the broker to open it, so launching is as fast as the
  broker polls -- tens of milliseconds, not instant.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- filesColumnPercent: 58
  $name: Width of the files column
  $description: >-
    Percentage of the results area given to files; the apps column takes the
    rest. Paths are long, so files usually want the larger share.
- showIcons: true
  $name: Show icons
  $description: >-
    Draw the icon the broker sends with each row. Turning this off saves the
    broker the work of fetching them.
- rowHeight: 40
  $name: Row height
  $description: Height of one result row, in pixels.
- hideHostSearchBox: false
  $name: Hide SearchHost's own search box (experimental)
  $description: >-
    Makes SearchHost's search box invisible, unclickable and disabled, while
    leaving it in the layout.

    Collapsing it outright also works, until something in SearchUx.UI reaches
    for it: the element is then present but unlaid-out and the host
    dereferences null. Disabling the inner RichEditBox is what actually keeps
    the keyboard in the Start menu, since an element that cannot take focus
    cannot receive keystrokes.

    Found by hand in UWPSpy: with that box hidden, typing in the Start menu
    stops handing off to SearchHost altogether. If that holds up, the search
    page never opens, SearchHost never runs a query, and Start is never
    cloaked -- which would make it possible to put the results in Start
    itself, a process that is not sandboxed and could reach Everything
    without a broker at all.

    This exists to measure that claim. Off by default.
- ownInputProbe: false
  $name: Own-input probe (experimental)
  $description: >-
    Adds a text box of our own to the search page, takes keyboard focus, and
    logs who the keystrokes actually reach.

    This answers one question: if we own the input, Microsoft's box never sees
    a keystroke, so it never completes what you type, never fires TextChanged
    twice per key, and never runs its own query. That would remove a whole
    class of problems rather than working around them. It is only worth
    building if focus can be taken and kept, which is what this measures.
- stripAutoComplete: false
  $name: Erase the box's guess as you type (experimental)
  $description: >-
    The search box guesses the rest of what you are typing -- its own
    behaviour, not the web results, and it stays on with every web-search
    policy disabled. The guess is never sent to the broker either way, so
    this only changes what you see.

    Off by default because it does not work cleanly: rewriting the text the
    control is in the middle of editing corrupted input on the fourth
    keystroke in testing, emptying the box. Turn it on only if you want to
    help work out why.
- suppressWebResults: false
  $name: Stop the web results engine from starting
  $description: >-
    Windows starts a WebView2 browser inside the search host to render its
    results and suggestions, and keeps it running whether or not you search.
    Measured on one machine: six processes and about 390 MB, spawned at
    startup. Turning off web search in Settings or by policy does not prevent
    it. This blocks it from launching at all.

    Only turn this on if you run the broker, because it also removes the
    stock results this mod would otherwise fall back to. It takes effect the
    next time the search host starts.
*/
// ==/WindhawkModSettings==

#include <initguid.h>  // must precede xamlom.h

#include <inspectable.h>
#include <xamlom.h>

// winbase.h defines GetCurrentTime as a macro, which collides with
// Windows.UI.Xaml.Media.Animation's method of the same name.
#pragma push_macro("GetCurrentTime")
#undef GetCurrentTime

#include <winrt/Windows.Foundation.h>
// Not just the .0.h forward declarations: Append/Size have deduced return
// types and must be defined before use.
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Media.Imaging.h>

#pragma pop_macro("GetCurrentTime")

#include <robuffer.h>
#include <roapi.h>
#include <windhawk_utils.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cwctype>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace wf = winrt::Windows::Foundation;
namespace wut = winrt::Windows::UI::Text;
namespace wux = winrt::Windows::UI::Xaml;
namespace wuxc = winrt::Windows::UI::Xaml::Controls;
namespace wuxm = winrt::Windows::UI::Xaml::Media;
namespace wuxmi = winrt::Windows::UI::Xaml::Media::Imaging;

static HRESULT InjectWindhawkTAP() noexcept;

// ===========================================================================
// Wire protocol
//
// Duplicated in broker/panel_protocol.h, because a Windhawk mod is a single
// translation unit and cannot include a project header. The version field
// exists so a drift between the two copies fails loudly on the first push
// instead of being read as garbage.
// ===========================================================================

namespace protocol {

inline constexpr DWORD kMagic = 0x564E5045;  // EPNV
inline constexpr DWORD kVersion = 1;

// The panel's listening window. A real top-level window, not HWND_MESSAGE:
// FindWindow cannot see message-only windows from another process, and the
// broker has to be able to find this one.
inline constexpr wchar_t kWindowClass[] = L"WindhawkEverythingSearchPanel";

// WM_COPYDATA dwData values. All of these travel normal -> low integrity,
// which UIPI permits; nothing travels the other way.
inline constexpr DWORD kMsgResults = 0x45565031;  // payload: Header + rows
inline constexpr DWORD kMsgPoll = 0x45565032;     // no payload
inline constexpr DWORD kMsgRelease = 0x45565033;  // no payload: hand search back

enum RowKind : DWORD { kRowApp = 0, kRowFile = 1 };
enum RowFlags : DWORD { kRowFolder = 1 };

#pragma pack(push, 1)
struct Header {
    DWORD magic;
    DWORD version;
    DWORD seq;  // bumped per push, so a click can be matched to what it hit
    DWORD appCount;
    DWORD fileCount;
    DWORD fileTotal;  // total Everything matches, not just those sent
    DWORD statusLen;  // characters, excluding the terminator
    // followed by the status string, then appCount + fileCount rows
};

struct RowHeader {
    DWORD kind;
    DWORD flags;
    DWORD titleLen;     // characters, excluding the terminator
    DWORD subtitleLen;  // characters, excluding the terminator
    DWORD iconSize;     // pixels per side; 0 when no icon was sent
    DWORD iconBytes;    // iconSize * iconSize * 4, BGRA, premultiplied
    // followed by title, subtitle, then the icon bytes
};
#pragma pack(pop)

// The panel answers by returning a value from its window procedure, because
// it cannot send a message upward. That value is truncated to 32 bits on the
// way back across the process boundary -- measured, not assumed: the panel
// returned 0x4000000100010002 and the broker received 0x00010002, exactly the
// low half. So the whole action has to fit in 32 bits.
inline constexpr DWORD kActionValid = 0x80000000u;

inline LRESULT EncodeAction(DWORD seq, DWORD kind, DWORD index) {
    DWORD packed = kActionValid | ((seq & 0x7FFF) << 16) |
                   ((kind & 0xF) << 12) | (index & 0xFFF);
    return static_cast<LRESULT>(packed);
}

}  // namespace protocol

namespace {

// ---------------------------------------------------------------------------
// Logging
//
// Writes land in this AppContainer's redirected temp, not the real one:
//   %LOCALAPPDATA%\Packages\MicrosoftWindows.Client.CBS_cw5n1h2txyewy\AC\Temp\
// ---------------------------------------------------------------------------

void Rec(const wchar_t* fmt, ...) {
    wchar_t body[2048] = {};
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(body, ARRAYSIZE(body), _TRUNCATE, fmt, args);
    va_end(args);

    wchar_t path[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, path);
    if (!n || n > MAX_PATH - 40) {
        return;
    }
    wcscat_s(path, MAX_PATH, L"everything-search.log");
    HANDLE h = CreateFileW(path, FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    static const wchar_t kCrLf[] = {13, 10, 0};
    wchar_t line[2200];
    int len = wsprintfW(line, L"%02d:%02d:%02d.%03d  %ls%ls", st.wHour,
                        st.wMinute, st.wSecond, st.wMilliseconds, body, kCrLf);
    DWORD written = 0;
    WriteFile(h, line, static_cast<DWORD>(len * sizeof(wchar_t)), &written,
              nullptr);
    CloseHandle(h);
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

struct Settings {
    int filesColumnPercent = 58;
    bool showIcons = true;
    int rowHeight = 40;
    bool stripAutoComplete = true;
    bool ownInputProbe = false;
    bool hideHostSearchBox = false;
    bool suppressWebResults = false;
};

Settings g_settings;

void LoadSettings() {
    g_settings.filesColumnPercent =
        static_cast<int>(Wh_GetIntSetting(L"filesColumnPercent"));
    if (g_settings.filesColumnPercent < 20 ||
        g_settings.filesColumnPercent > 80) {
        g_settings.filesColumnPercent = 58;
    }
    g_settings.showIcons = Wh_GetIntSetting(L"showIcons") != 0;
    g_settings.rowHeight = static_cast<int>(Wh_GetIntSetting(L"rowHeight"));
    if (g_settings.rowHeight < 24 || g_settings.rowHeight > 96) {
        g_settings.rowHeight = 40;
    }
    g_settings.stripAutoComplete = Wh_GetIntSetting(L"stripAutoComplete") != 0;
    g_settings.ownInputProbe = Wh_GetIntSetting(L"ownInputProbe") != 0;
    g_settings.hideHostSearchBox =
        Wh_GetIntSetting(L"hideHostSearchBox") != 0;
    g_settings.suppressWebResults =
        Wh_GetIntSetting(L"suppressWebResults") != 0;
}

// ---------------------------------------------------------------------------
// Keeping the web results engine from starting
//
// The search host launches a WebView2 browser to draw its results and its
// suggestions, and does it at startup rather than on the first search, so the
// cost is paid whether or not anyone searches. Every supported way of turning
// web search off leaves it running: on the machine this was written on,
// DisableSearchBoxSuggestions, DisableWebSearch, ConnectedSearchUseWeb,
// BingSearchEnabled and AllowCortana were all already set against it, and
// there were still six processes and 390 MB. The suggestions it serves are
// not only web ones either -- with Bing disabled it was still completing
// typed text to locally installed app names, which is actively wrong once
// this panel is answering instead.
//
// Killing it does not work: the host notices and respawns it within a second.
// It does survive losing it, though, which is what makes refusing the launch
// worth trying at all.
//
// CreateProcessW rather than the WebView2 entry point: that lives in
// EmbeddedBrowserWebView.dll, which is loaded lazily out of the Edge runtime
// folder and is not present yet when this mod initialises. kernel32 always
// is. Only the browser process is launched by the host -- the other five are
// its own children -- so refusing this one call is enough.

std::atomic<int> g_blockedLaunches{0};

using CreateProcessW_t = decltype(&CreateProcessW);
CreateProcessW_t CreateProcessW_Original;

bool NamesWebView(LPCWSTR text) {
    if (!text) {
        return false;
    }
    std::wstring lower(text);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return lower.find(L"msedgewebview2.exe") != std::wstring::npos;
}

BOOL WINAPI CreateProcessW_Hook(LPCWSTR applicationName, LPWSTR commandLine,
                                LPSECURITY_ATTRIBUTES processAttributes,
                                LPSECURITY_ATTRIBUTES threadAttributes,
                                BOOL inheritHandles, DWORD creationFlags,
                                LPVOID environment, LPCWSTR currentDirectory,
                                LPSTARTUPINFOW startupInfo,
                                LPPROCESS_INFORMATION processInformation) {
    if (g_settings.suppressWebResults &&
        (NamesWebView(applicationName) || NamesWebView(commandLine))) {
        int n = ++g_blockedLaunches;
        // Only the first few, and then powers of two. If the host reacts to a
        // refused launch by retrying forever this will say so without the log
        // itself becoming the problem.
        if (n <= 3 || (n & (n - 1)) == 0) {
            Rec(L"refused to launch the web view (attempt %d)", n);
        }
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    return CreateProcessW_Original(applicationName, commandLine,
                                   processAttributes, threadAttributes,
                                   inheritHandles, creationFlags, environment,
                                   currentDirectory, startupInfo,
                                   processInformation);
}

// ---------------------------------------------------------------------------
// Parsed rows
// ---------------------------------------------------------------------------

struct Row {
    DWORD kind = protocol::kRowApp;
    DWORD flags = 0;
    std::wstring title;
    std::wstring subtitle;
    DWORD iconSize = 0;
    std::vector<BYTE> icon;  // BGRA, premultiplied
};

struct Batch {
    DWORD seq = 0;
    DWORD fileTotal = 0;
    std::wstring status;
    std::vector<Row> apps;
    std::vector<Row> files;
};

// A bounds-checked walk over the payload. The broker is a separate process,
// so its buffer is untrusted input as far as this parser is concerned: every
// read is checked and any failure discards the whole batch rather than
// rendering half of it.
class Cursor {
   public:
    Cursor(const BYTE* base, size_t size) : base_(base), size_(size) {}

    bool ok() const { return ok_; }

    template <typename T>
    bool Read(T* out) {
        if (!ok_ || pos_ + sizeof(T) > size_) {
            ok_ = false;
            return false;
        }
        memcpy(out, base_ + pos_, sizeof(T));
        pos_ += sizeof(T);
        return true;
    }

    bool ReadString(DWORD chars, std::wstring* out) {
        if (chars > (1u << 16)) {
            ok_ = false;
            return false;
        }
        size_t bytes = (static_cast<size_t>(chars) + 1) * sizeof(wchar_t);
        if (!ok_ || pos_ + bytes > size_) {
            ok_ = false;
            return false;
        }
        out->assign(reinterpret_cast<const wchar_t*>(base_ + pos_), chars);
        pos_ += bytes;
        return true;
    }

    bool ReadBytes(DWORD count, std::vector<BYTE>* out) {
        if (!ok_ || pos_ + count > size_) {
            ok_ = false;
            return false;
        }
        out->assign(base_ + pos_, base_ + pos_ + count);
        pos_ += count;
        return true;
    }

   private:
    const BYTE* base_;
    size_t size_;
    size_t pos_ = 0;
    bool ok_ = true;
};

bool ParseBatch(const void* data, DWORD size, Batch* out) {
    if (!data || size < sizeof(protocol::Header)) {
        return false;
    }
    Cursor c(static_cast<const BYTE*>(data), size);

    protocol::Header h{};
    if (!c.Read(&h)) {
        return false;
    }
    if (h.magic != protocol::kMagic) {
        Rec(L"push rejected: bad magic %08X", h.magic);
        return false;
    }
    if (h.version != protocol::kVersion) {
        Rec(L"push rejected: protocol version %lu, this panel speaks %lu",
            h.version, protocol::kVersion);
        return false;
    }
    // A broker that asks for thousands of rows is either broken or not the
    // broker; either way the panel is not going to build that many elements.
    if (h.appCount > 64 || h.fileCount > 64) {
        Rec(L"push rejected: %lu apps / %lu files is out of range", h.appCount,
            h.fileCount);
        return false;
    }

    out->seq = h.seq;
    out->fileTotal = h.fileTotal;
    if (!c.ReadString(h.statusLen, &out->status)) {
        return false;
    }

    for (DWORD i = 0; i < h.appCount + h.fileCount; i++) {
        protocol::RowHeader rh{};
        if (!c.Read(&rh)) {
            return false;
        }
        Row row;
        row.kind = rh.kind;
        row.flags = rh.flags;
        if (!c.ReadString(rh.titleLen, &row.title) ||
            !c.ReadString(rh.subtitleLen, &row.subtitle)) {
            return false;
        }
        if (rh.iconSize) {
            // The declared pixel count and the declared byte count have to
            // agree, or the copy into the bitmap would run off the end.
            if (rh.iconSize > 256 ||
                rh.iconBytes != rh.iconSize * rh.iconSize * 4) {
                Rec(L"push rejected: icon %lux%lu does not match %lu bytes",
                    rh.iconSize, rh.iconSize, rh.iconBytes);
                return false;
            }
            if (!c.ReadBytes(rh.iconBytes, &row.icon)) {
                return false;
            }
            row.iconSize = rh.iconSize;
        }
        if (row.kind == protocol::kRowFile) {
            out->files.push_back(std::move(row));
        } else {
            out->apps.push_back(std::move(row));
        }
    }
    return c.ok();
}

// ---------------------------------------------------------------------------
// Shared state
//
// Everything below runs on the XAML thread. The receiving window is created
// on that thread too, so WM_COPYDATA is dispatched there and the tree can be
// touched directly, with no marshalling.
// ---------------------------------------------------------------------------

std::atomic<DWORD> g_xamlThreadId{0};

// Strong refs, not weak: the tree has to be put back on unload, and a weak
// ref can already be dead by then.
[[clang::no_destroy]] wux::FrameworkElement g_webHost{nullptr};
[[clang::no_destroy]] wuxc::Panel g_parentPanel{nullptr};
[[clang::no_destroy]] wuxc::Grid g_panel{nullptr};
[[clang::no_destroy]] wuxc::StackPanel g_appsList{nullptr};
[[clang::no_destroy]] wuxc::StackPanel g_filesList{nullptr};
[[clang::no_destroy]] wuxc::TextBlock g_appsHeader{nullptr};
[[clang::no_destroy]] wuxc::TextBlock g_filesHeader{nullptr};
[[clang::no_destroy]] wuxc::TextBlock g_status{nullptr};

[[clang::no_destroy]] wuxc::RichEditBox g_queryBox{nullptr};
[[clang::no_destroy]] wuxc::RichEditBox::TextChanged_revoker g_queryChanged;
[[clang::no_destroy]] std::wstring g_lastBoxText;
[[clang::no_destroy]] std::wstring g_lastPublished;

bool g_built = false;     // the panel exists in the tree
bool g_takenOver = false; // the web view is currently collapsed
HWND g_listener = nullptr;
ATOM g_listenerClass = 0;
LRESULT g_pendingAction = 0;

// ---------------------------------------------------------------------------
// Tree helpers
// ---------------------------------------------------------------------------

HMODULE GetCurrentModuleHandle() {
    HMODULE module;
    if (!GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           L"", &module)) {
        return nullptr;
    }
    return module;
}

// Every RichSearchBoxControl in the tree, not just the first.
//
// Matching by type and taking the first hit reported "already collapsed" on
// every page while the box stayed visible on screen -- which is what happens
// when there is more than one instance and the first one found is not the one
// being used. Doing it by hand in UWPSpy worked because the element being
// right-clicked is the visible one.
void CollectSearchBoxControls(wux::DependencyObject const& root, int maxDepth,
                              std::vector<wux::FrameworkElement>& out) {
    if (maxDepth < 0) {
        return;
    }
    try {
        if (std::wstring_view{winrt::get_class_name(root)} ==
            L"Cortana.UI.Views.RichSearchBoxControl") {
            if (auto fe = root.try_as<wux::FrameworkElement>()) {
                out.push_back(fe);
            }
        }
    } catch (...) {
    }
    int count = wuxm::VisualTreeHelper::GetChildrenCount(root);
    for (int i = 0; i < count; ++i) {
        CollectSearchBoxControls(wuxm::VisualTreeHelper::GetChild(root, i),
                                 maxDepth - 1, out);
    }
}

// Finds a descendant by its runtime class name.
//
// Needed because the thing that has to be collapsed is identified by type
// (Cortana.UI.Views.RichSearchBoxControl) rather than by x:Name, and the only
// search that existed here matched names.
wux::DependencyObject FindDescendantByType(wux::DependencyObject const& root,
                                           std::wstring_view type,
                                           int maxDepth) {
    if (maxDepth < 0) {
        return nullptr;
    }
    try {
        if (std::wstring_view{winrt::get_class_name(root)} == type) {
            return root;
        }
    } catch (...) {
    }
    int count = wuxm::VisualTreeHelper::GetChildrenCount(root);
    for (int i = 0; i < count; ++i) {
        if (auto found = FindDescendantByType(
                wuxm::VisualTreeHelper::GetChild(root, i), type, maxDepth - 1)) {
            return found;
        }
    }
    return nullptr;
}

wux::DependencyObject FindDescendantByName(wux::DependencyObject const& root,
                                           std::wstring_view name,
                                           int maxDepth) {
    if (maxDepth < 0) {
        return nullptr;
    }
    if (auto fe = root.try_as<wux::FrameworkElement>()) {
        if (std::wstring_view{fe.Name()} == name) {
            return root;
        }
    }
    int count = wuxm::VisualTreeHelper::GetChildrenCount(root);
    for (int i = 0; i < count; ++i) {
        auto child = wuxm::VisualTreeHelper::GetChild(root, i);
        if (auto found = FindDescendantByName(child, name, maxDepth - 1)) {
            return found;
        }
    }
    return nullptr;
}

// Type plus x:Name, for log lines that have to be matched against what a
// tree inspector shows.
std::wstring ElementLabel(wux::DependencyObject const& obj) {
    std::wstring label;
    try {
        label = winrt::get_class_name(obj);
    } catch (...) {
        label = L"<unknown>";
    }
    if (auto fe = obj.try_as<wux::FrameworkElement>()) {
        std::wstring name{fe.Name()};
        if (!name.empty()) {
            label += L"#" + name;
        }
    }
    return label;
}

// Writes the whole subtree to the log once per process. Diagnostic: the
// element that holds the query is not a TextBox and is not named anywhere
// documented, and the anchor this mod inserts against moves when the web
// view is suppressed. Both are questions about what is actually there.
void DumpTree(wux::DependencyObject const& root, int depth, int maxDepth) {
    if (depth > maxDepth) {
        return;
    }
    std::wstring indent(static_cast<size_t>(depth) * 2, L' ');
    double w = 0, h = 0;
    if (auto fe = root.try_as<wux::FrameworkElement>()) {
        w = fe.ActualWidth();
        h = fe.ActualHeight();
    }
    Rec(L"%ls%ls  [%.0fx%.0f]", indent.c_str(), ElementLabel(root).c_str(), w, h);

    int count = wuxm::VisualTreeHelper::GetChildrenCount(root);
    for (int i = 0; i < count; ++i) {
        DumpTree(wuxm::VisualTreeHelper::GetChild(root, i), depth + 1, maxDepth);
    }
}

// The search box, found by type.
//
// It is a RichEditBox, not a TextBox: the concrete type is
// Cortana.UI.Views.CortanaRichSearchBox and its DefaultStyleKey is
// Windows.UI.Xaml.Controls.RichEditBox. Looking for a TextBox found nothing
// at all, which is why the query was never published. Searched by type
// rather than by name because the name is undocumented and is one more thing
// to break on a servicing update.
wuxc::RichEditBox FindQueryBox(wux::DependencyObject const& root, int maxDepth) {
    if (maxDepth < 0) {
        return nullptr;
    }
    if (auto box = root.try_as<wuxc::RichEditBox>()) {
        return box;
    }
    int count = wuxm::VisualTreeHelper::GetChildrenCount(root);
    for (int i = 0; i < count; ++i) {
        auto child = wuxm::VisualTreeHelper::GetChild(root, i);
        if (auto found = FindQueryBox(child, maxDepth - 1)) {
            return found;
        }
    }
    return nullptr;
}

// A rich edit document always ends in a paragraph mark that is not part of
// what anyone typed.
std::wstring DocumentText(wuxc::RichEditBox const& box) {
    winrt::hstring raw;
    box.Document().GetText(wut::TextGetOptions::None, raw);
    std::wstring text{raw};
    while (!text.empty() && (text.back() == 13 || text.back() == 10)) {
        text.pop_back();
    }
    return text;
}

// Telling a keystroke apart from a completion.
//
// The box raises TextChanged twice for every key: once with what was typed,
// and again 60-100 ms later with its own guess appended. Measured:
//
//   'n'   -> 'nVIDIA App'   (97 ms)
//   'no'  -> 'node.js'      (65 ms)
//   'not' -> 'notepad'      (63 ms)
//
// The guess is not a selection -- the control marks the completed run by
// colour, which is what its AutoCompletedForeground brush is for -- so there
// is nothing in the text itself that says "this part was guessed".
//
// Key events looked like the answer and are not available: a RichEditBox
// marks them handled as it consumes them, so a plain KeyDown handler never
// runs, and AddHandler with handledEventsToo wants an IInspectable, which a
// WinRT delegate is not.
//
// What is left is the shape of the change. A person adds one character at a
// time or deletes; a completion appends several at once while keeping the
// prefix. That misreads a one-character completion as typing, which is
// harmless, and a multi-character paste as a completion, which costs one
// keystroke of staleness.
bool g_strippingCompletion = false;
[[clang::no_destroy]] std::wstring g_typed;

enum class Change { Typed, Completion };

Change Classify(const std::wstring& text, const std::wstring& previousBox,
                const std::wstring& typed) {
    if (text.size() < previousBox.size()) {
        return Change::Typed;  // a deletion is always a person
    }
    if (text.size() == previousBox.size() + 1 &&
        text.compare(0, previousBox.size(), previousBox) == 0) {
        return Change::Typed;  // one more character on the end
    }
    if (text.size() > typed.size() &&
        text.compare(0, typed.size(), typed) == 0) {
        return Change::Completion;  // the guess, extending what was typed
    }
    return Change::Typed;
}

// Puts the box back to what was actually typed.
void StripCompletion(wuxc::RichEditBox const& box, const std::wstring& typed) {
    // Never strip to nothing. If the classification is ever wrong the cost
    // must be a stale guess left on screen, never a search box that deletes
    // what is typed into it -- which is exactly what the previous attempt
    // did.
    if (g_strippingCompletion || typed.empty()) {
        return;
    }
    g_strippingCompletion = true;
    try {
        auto document = box.Document();
        document.SetText(wut::TextSetOptions::None, winrt::hstring{typed});
        auto caret = static_cast<int32_t>(typed.size());
        document.Selection().SetRange(caret, caret);
    } catch (...) {
        // Cosmetic. The query that was published is already the right one.
    }
    g_strippingCompletion = false;
}

// The panel cannot send the query anywhere: messages from here to a normal
// integrity process are dropped. So it puts the query in its own window
// title instead. Window text is stored by the window manager and can be read
// across processes with GetWindowText, which is a read rather than a message
// and is not what UIPI restricts. FindWindow still finds the window because
// the broker matches on class and ignores the title.
void PublishQuery(const std::wstring& query) {
    if (!g_listener) {
        return;
    }
    SetWindowTextW(g_listener, query.c_str());
}

bool XamlWindowExists() {
    bool found = false;
    EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid != GetCurrentProcessId()) {
                return TRUE;
            }
            wchar_t className[128] = {};
            GetClassNameW(hwnd, className, ARRAYSIZE(className));
            if (wcsstr(className, L"Windows.UI.Core.CoreWindow")) {
                *reinterpret_cast<bool*>(param) = true;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&found));
    return found;
}

// ---------------------------------------------------------------------------
// Building the panel
// ---------------------------------------------------------------------------

wuxm::SolidColorBrush Transparent() {
    return wuxm::SolidColorBrush(winrt::Windows::UI::Colors::Transparent());
}

wuxc::TextBlock MakeHeader(std::wstring_view text) {
    wuxc::TextBlock tb;
    tb.Text(winrt::hstring{text});
    tb.FontSize(12);
    tb.FontWeight(wut::FontWeights::SemiBold());
    // No explicit Foreground anywhere in this panel: inheriting the search
    // page's own foreground is what keeps it correct in both themes, and in
    // whatever accent or high-contrast scheme the user has set. Hierarchy is
    // expressed with opacity instead.
    tb.Opacity(0.65);
    tb.Margin(wux::ThicknessHelper::FromLengths(12, 4, 12, 6));
    return tb;
}

// One result row. A Button with a transparent background draws nothing until
// it is hovered, and then picks up the standard hover fill from its own
// template -- which is both free and automatically right for the theme.
wuxc::Button MakeRow(const Row& row, DWORD seq, DWORD index) {
    wuxc::Button btn;
    btn.Background(Transparent());
    btn.BorderThickness(wux::ThicknessHelper::FromLengths(0, 0, 0, 0));
    btn.Padding(wux::ThicknessHelper::FromLengths(8, 0, 8, 0));
    btn.HorizontalAlignment(wux::HorizontalAlignment::Stretch);
    btn.HorizontalContentAlignment(wux::HorizontalAlignment::Stretch);
    btn.VerticalContentAlignment(wux::VerticalAlignment::Center);
    btn.Height(static_cast<double>(g_settings.rowHeight));
    btn.UseSystemFocusVisuals(true);

    wuxc::Grid content;
    wuxc::ColumnDefinition iconCol, textCol;
    iconCol.Width(wux::GridLengthHelper::FromValueAndType(
        0, wux::GridUnitType::Auto));
    textCol.Width(wux::GridLengthHelper::FromValueAndType(
        1, wux::GridUnitType::Star));
    content.ColumnDefinitions().Append(iconCol);
    content.ColumnDefinitions().Append(textCol);

    if (g_settings.showIcons && row.iconSize && !row.icon.empty()) {
        try {
            wuxmi::WriteableBitmap bmp(static_cast<int32_t>(row.iconSize),
                                       static_cast<int32_t>(row.iconSize));
            auto buffer = bmp.PixelBuffer();
            auto access =
                buffer.as<::Windows::Storage::Streams::IBufferByteAccess>();
            BYTE* pixels = nullptr;
            if (SUCCEEDED(access->Buffer(&pixels)) && pixels &&
                buffer.Capacity() >= row.icon.size()) {
                memcpy(pixels, row.icon.data(), row.icon.size());
                bmp.Invalidate();

                wuxc::Image img;
                img.Source(bmp);
                double side = g_settings.rowHeight * 0.55;
                img.Width(side);
                img.Height(side);
                img.Margin(wux::ThicknessHelper::FromLengths(0, 0, 10, 0));
                img.VerticalAlignment(wux::VerticalAlignment::Center);
                wuxc::Grid::SetColumn(img, 0);
                content.Children().Append(img);
            }
        } catch (...) {
            // A row without its icon is still a usable row.
            Rec(L"icon failed for row %lu: %08X", index,
                static_cast<unsigned>(winrt::to_hresult()));
        }
    }

    wuxc::StackPanel text;
    text.VerticalAlignment(wux::VerticalAlignment::Center);
    wuxc::Grid::SetColumn(text, 1);

    wuxc::TextBlock title;
    title.Text(winrt::hstring{row.title});
    title.FontSize(14);
    title.TextTrimming(wux::TextTrimming::CharacterEllipsis);
    title.TextWrapping(wux::TextWrapping::NoWrap);
    text.Children().Append(title);

    if (!row.subtitle.empty()) {
        wuxc::TextBlock sub;
        sub.Text(winrt::hstring{row.subtitle});
        sub.FontSize(11);
        sub.Opacity(0.55);
        sub.TextTrimming(wux::TextTrimming::CharacterEllipsis);
        sub.TextWrapping(wux::TextWrapping::NoWrap);
        text.Children().Append(sub);
    }
    content.Children().Append(text);
    btn.Content(content);

    // The panel cannot launch anything itself -- no shell namespace, no
    // CreateProcess. It records what was clicked and the broker collects it.
    DWORD kind = row.kind;
    btn.Click([seq, kind, index](wf::IInspectable const&,
                                 wux::RoutedEventArgs const&) {
        g_pendingAction = protocol::EncodeAction(seq, kind, index);
        Rec(L"click: seq=%lu kind=%lu index=%lu", seq, kind, index);
    });

    wuxc::ToolTipService::SetToolTip(
        btn, winrt::box_value(winrt::hstring{
                 row.subtitle.empty() ? row.title
                                      : row.subtitle + L"\\" + row.title}));
    return btn;
}

wuxc::Grid MakeColumn(wuxc::TextBlock& header, wuxc::StackPanel& list,
                      std::wstring_view title) {
    wuxc::Grid column;
    wuxc::RowDefinition headerRow, bodyRow;
    headerRow.Height(
        wux::GridLengthHelper::FromValueAndType(0, wux::GridUnitType::Auto));
    bodyRow.Height(
        wux::GridLengthHelper::FromValueAndType(1, wux::GridUnitType::Star));
    column.RowDefinitions().Append(headerRow);
    column.RowDefinitions().Append(bodyRow);

    header = MakeHeader(title);
    wuxc::Grid::SetRow(header, 0);
    column.Children().Append(header);

    wuxc::ScrollViewer scroller;
    scroller.VerticalScrollBarVisibility(wuxc::ScrollBarVisibility::Auto);
    scroller.HorizontalScrollBarVisibility(wuxc::ScrollBarVisibility::Disabled);
    wuxc::Grid::SetRow(scroller, 1);

    list = wuxc::StackPanel();
    scroller.Content(list);
    column.Children().Append(scroller);
    return column;
}

// Builds the panel and puts it in the tree, collapsed. Nothing about the
// stock results is touched here -- see TakeOver.
bool BuildPanel(wuxc::Panel const& parent) {
    wuxc::Grid root;
    root.Name(L"WindhawkEverythingSearchPanel");
    root.HorizontalAlignment(wux::HorizontalAlignment::Stretch);
    root.VerticalAlignment(wux::VerticalAlignment::Stretch);
    root.Visibility(wux::Visibility::Collapsed);

    wuxc::ColumnDefinition appsCol, filesCol;
    appsCol.Width(wux::GridLengthHelper::FromValueAndType(
        100 - g_settings.filesColumnPercent, wux::GridUnitType::Star));
    filesCol.Width(wux::GridLengthHelper::FromValueAndType(
        g_settings.filesColumnPercent, wux::GridUnitType::Star));
    root.ColumnDefinitions().Append(appsCol);
    root.ColumnDefinitions().Append(filesCol);

    auto apps = MakeColumn(g_appsHeader, g_appsList, L"Apps");
    wuxc::Grid::SetColumn(apps, 0);
    root.Children().Append(apps);

    auto files = MakeColumn(g_filesHeader, g_filesList, L"Files");
    wuxc::Grid::SetColumn(files, 1);
    root.Children().Append(files);

    // Shown instead of the columns when there is nothing to show.
    g_status = wuxc::TextBlock();
    g_status.FontSize(13);
    g_status.Opacity(0.6);
    g_status.HorizontalAlignment(wux::HorizontalAlignment::Center);
    g_status.VerticalAlignment(wux::VerticalAlignment::Center);
    g_status.Visibility(wux::Visibility::Collapsed);
    wuxc::Grid::SetColumn(g_status, 0);
    wuxc::Grid::SetColumnSpan(g_status, 2);
    root.Children().Append(g_status);

    parent.Children().Append(root);
    g_panel = root;
    g_parentPanel = parent;
    g_built = true;
    return true;
}

// Collapse the stock results and show ours. Deliberately deferred until the
// first batch arrives: if the broker is not running, search is left exactly
// as Windows shipped it.
void TakeOver() {
    if (g_takenOver || !g_webHost || !g_panel) {
        return;
    }
    g_webHost.Visibility(wux::Visibility::Collapsed);
    g_panel.Visibility(wux::Visibility::Visible);
    g_takenOver = true;
    Rec(L"took over the results surface");
}

void HandBack() {
    if (!g_takenOver) {
        return;
    }
    if (g_webHost) {
        g_webHost.Visibility(wux::Visibility::Visible);
    }
    if (g_panel) {
        g_panel.Visibility(wux::Visibility::Collapsed);
    }
    g_takenOver = false;
    Rec(L"handed the results surface back");
}

void Fill(wuxc::StackPanel const& list, const std::vector<Row>& rows,
          DWORD seq) {
    list.Children().Clear();
    for (DWORD i = 0; i < rows.size(); i++) {
        list.Children().Append(MakeRow(rows[i], seq, i));
    }
}

void Apply(const Batch& batch) {
    if (!g_built) {
        return;
    }

    Fill(g_appsList, batch.apps, batch.seq);
    Fill(g_filesList, batch.files, batch.seq);

    wchar_t appsTitle[64];
    swprintf(appsTitle, 64, L"Apps");
    g_appsHeader.Text(appsTitle);

    wchar_t filesTitle[96];
    if (batch.fileTotal > batch.files.size()) {
        swprintf(filesTitle, 96, L"Files  -  %zu of %lu", batch.files.size(),
                 batch.fileTotal);
    } else {
        swprintf(filesTitle, 96, L"Files");
    }
    g_filesHeader.Text(filesTitle);

    bool empty = batch.apps.empty() && batch.files.empty();
    g_status.Text(winrt::hstring{batch.status});
    g_status.Visibility(empty && !batch.status.empty()
                            ? wux::Visibility::Visible
                            : wux::Visibility::Collapsed);
    auto columns = empty ? wux::Visibility::Collapsed : wux::Visibility::Visible;
    // Children 0 and 1 are the two columns; 2 is the status line.
    if (g_panel.Children().Size() >= 2) {
        g_panel.Children().GetAt(0).as<wux::UIElement>().Visibility(columns);
        g_panel.Children().GetAt(1).as<wux::UIElement>().Visibility(columns);
    }

    TakeOver();
}

// ---------------------------------------------------------------------------
// The listening window
// ---------------------------------------------------------------------------

// Sent by Wh_ModUninit when it finds itself on the wrong thread. The
// listener window belongs to the XAML thread, so handling a message here is a
// synchronous hop onto it -- which is the only thread allowed to touch the
// tree, and the only one allowed to destroy this window.
constexpr UINT WM_PANEL_TEARDOWN = WM_APP + 1;

void RemoveInjection();

LRESULT CALLBACK ListenerProc(HWND hwnd, UINT msg, WPARAM wParam,
                              LPARAM lParam) {
    if (msg == WM_PANEL_TEARDOWN) {
        RemoveInjection();
        return 0;
    }
    if (msg != WM_COPYDATA) {
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    auto* cds = reinterpret_cast<COPYDATASTRUCT*>(lParam);
    if (!cds) {
        return 0;
    }

    // Whatever the message was, the reply carries any click the panel is
    // holding, and clears it so it is delivered exactly once.
    LRESULT action = g_pendingAction;
    if (action) {
        Rec(L"handing back action %08X", (unsigned)action);
    }

    try {
        switch (cds->dwData) {
            case protocol::kMsgResults: {
                Batch batch;
                if (ParseBatch(cds->lpData, cds->cbData, &batch)) {
                    Apply(batch);
                } else {
                    Rec(L"discarded a malformed push of %lu bytes", cds->cbData);
                }
                break;
            }
            case protocol::kMsgRelease:
                HandBack();
                break;
            case protocol::kMsgPoll:
                break;
            default:
                return 0;  // not ours
        }
    } catch (...) {
        Rec(L"handler threw: %08X", static_cast<unsigned>(winrt::to_hresult()));
    }

    g_pendingAction = 0;
    return action;
}

// Created on the XAML thread on purpose. That thread already pumps messages,
// so WM_COPYDATA is dispatched there and the tree can be updated in the
// handler with no marshalling and no chance of touching XAML from elsewhere.
bool CreateListener() {
    if (g_listener) {
        return true;
    }
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ListenerProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = protocol::kWindowClass;
    g_listenerClass = RegisterClassExW(&wc);
    if (!g_listenerClass && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        Rec(L"RegisterClassEx failed: %lu", GetLastError());
        return false;
    }
    g_listener =
        CreateWindowExW(0, protocol::kWindowClass, protocol::kWindowClass,
                        WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance,
                        nullptr);
    if (!g_listener) {
        Rec(L"CreateWindowEx failed: %lu", GetLastError());
        return false;
    }
    Rec(L"listening on hwnd=%p (thread %lu)", g_listener,
        GetCurrentThreadId());
    return true;
}

void DestroyListener() {
    if (g_listener) {
        DestroyWindow(g_listener);
        g_listener = nullptr;
    }
    if (g_listenerClass) {
        UnregisterClassW(protocol::kWindowClass, GetModuleHandleW(nullptr));
        g_listenerClass = 0;
    }
}

// ---------------------------------------------------------------------------
// Own-input probe
//
// The box on the search page is Microsoft's, and it behaves in ways we spend
// effort undoing: it completes what you type, it raises TextChanged twice per
// key, and when its completion happens to be right the remaining keystrokes
// change nothing at all -- so typing "notepad" published "note" and the broker
// searched for a prefix of what was asked for.
//
// None of that needs working around if the keystrokes never reach that box.
// This puts a box of ours on the page and reports, in order: whether focus can
// be taken, whether it is kept, and what each box contains as keys arrive. If
// ours fills while Microsoft's stays empty, owning the input is viable.
[[clang::no_destroy]] wuxc::TextBox g_probeBox{nullptr};
[[clang::no_destroy]] wuxc::TextBox::TextChanged_revoker g_probeChanged;
[[clang::no_destroy]] wux::UIElement::LostFocus_revoker g_probeLost;
[[clang::no_destroy]] wux::UIElement::GotFocus_revoker g_probeGot;
[[clang::no_destroy]] wux::DispatcherTimer g_probeTimer{nullptr};

std::wstring FocusedElementLabel() {
    try {
        auto focused = wux::Input::FocusManager::GetFocusedElement();
        if (!focused) {
            return L"(nothing)";
        }
        if (auto dobj = focused.try_as<wux::DependencyObject>()) {
            return ElementLabel(dobj);
        }
        return std::wstring{winrt::get_class_name(focused)};
    } catch (...) {
        return L"(threw)";
    }
}

// Why this runs at page-layout time rather than at panel-build time.
//
// The panel waits for the web view host to have a size, and that does not
// happen until a search is actually under way. Measured in one run: the page
// was laid out at :01.2, the host got a size at :05.2, and the first keystroke
// arrived at about :05.6. Attaching at :05.2 meant the first character always
// went to Microsoft's box, which then completed it and ran its own query.
//
// The page itself is ready four seconds earlier, which is the whole margin we
// need: attach there, take focus, and its box never sees a character at all.
// An empty box is the thing we actually want -- with nothing in it there is no
// query to run and nothing to complete, and none of that required finding or
// hooking their search code.
std::atomic<bool> g_ownInputAttached{false};
std::atomic<int> g_refocusCount{0};

void AttachOwnInputProbe(wuxc::Panel const& parent) try {
    wuxc::TextBox box;
    box.Name(L"WindhawkOwnInputProbe");
    box.PlaceholderText(L"Windhawk probe -- type here");
    box.Height(34);
    box.Margin(wux::ThicknessHelper::FromLengths(12, 6, 12, 6));
    parent.Children().InsertAt(0, box);
    g_probeBox = box;

    g_probeChanged = box.TextChanged(
        winrt::auto_revoke,
        [](wf::IInspectable const& sender, wuxc::TextChangedEventArgs const&) {
            try {
                auto ours = sender.as<wuxc::TextBox>();
                std::wstring mine{ours.Text()};
                std::wstring theirs =
                    g_queryBox ? DocumentText(g_queryBox) : L"(no box)";
                // The comparison is the whole point: if theirs is empty while
                // mine is not, the host saw none of this.
                Rec(L"probe: ours='%ls'  microsoft's='%ls'", mine.c_str(),
                    theirs.c_str());
            } catch (...) {
            }
        });

    g_probeGot = box.GotFocus(
        winrt::auto_revoke,
        [](wf::IInspectable const&, wux::RoutedEventArgs const&) {
            Rec(L"probe: our box GOT focus");
        });
    g_probeLost = box.LostFocus(
        winrt::auto_revoke,
        [](wf::IInspectable const&, wux::RoutedEventArgs const&) {
            Rec(L"probe: our box LOST focus, now on %ls",
                FocusedElementLabel().c_str());
        });

    bool took = box.Focus(wux::FocusState::Programmatic);
    Rec(L"probe: box added; Focus() -> %d; focus is on %ls", took ? 1 : 0,
        FocusedElementLabel().c_str());

    // The host focuses its own box when the page is shown, which can happen
    // after this runs. Watching for a few seconds distinguishes "we never got
    // focus" from "we got it and it was taken back", which need different
    // answers.
    auto timer = wux::DispatcherTimer();
    timer.Interval(std::chrono::milliseconds(500));
    timer.Tick([timer, ticks = std::make_shared<int>(0)](
                   wf::IInspectable const&, wf::IInspectable const&) {
        if (++*ticks > 40) {
            timer.Stop();
            return;
        }
        std::wstring who = FocusedElementLabel();
        bool ours = who.find(L"WindhawkOwnInputProbe") != std::wstring::npos;
        if (!ours && g_probeBox && g_refocusCount.load() < 10) {
            int n = ++g_refocusCount;
            bool took = g_probeBox.Focus(wux::FocusState::Programmatic);
            Rec(L"probe: t+%dms focus was on %ls -- retook it (%d) -> %d",
                *ticks * 500, who.c_str(), n, took ? 1 : 0);
            return;
        }
        Rec(L"probe: t+%dms focus=%ls", *ticks * 500, who.c_str());
    });
    timer.Start();
    g_probeTimer = timer;
} catch (...) {
    Rec(L"probe: failed %08X", static_cast<unsigned>(winrt::to_hresult()));
}

// Collapses Cortana's box on a search page, and keeps it collapsed.
//
// Found by hand in UWPSpy first: with that box hidden, typing in the Start
// menu stops handing off to SearchHost. Measured afterwards -- Start kept the
// keyboard through every keystroke of a whole cycle.
//
// Keeping it collapsed is the hard part, and two attempts got it wrong.
// Collapsing once per session missed every rebuilt page. Collapsing once per
// page then revoking looked right but was not: the host rebuilds the page
// repeatedly ("TaskbarSearchPage added" six times in one session), the search
// finds the previous, already-collapsed box, sees nothing to do and gives up,
// and the new page's visible box is never touched. The handoff then happens a
// couple of seconds later, which is exactly what the trace showed.
//
// So this does not revoke on success. It holds a weak reference to whichever
// box it last collapsed and re-checks it each layout pass, which is cheap; it
// only walks the tree again when that reference is dead or still visible.
void HideHostSearchBoxWhenReady(wux::FrameworkElement const& page) {
    auto pageRef = page;
    auto token = std::make_shared<wux::FrameworkElement::LayoutUpdated_revoker>();
    auto known = std::make_shared<winrt::weak_ref<wux::FrameworkElement>>();
    auto passes = std::make_shared<int>(0);

    *token = page.LayoutUpdated(
        winrt::auto_revoke,
        [pageRef, token, known, passes](wf::IInspectable const&,
                                        wf::IInspectable const&) {
            try {
                // Stop once the page itself is gone, so a rebuilt page does
                // not leave a handler running against a dead tree forever.
                if (!pageRef.Parent() && pageRef.ActualWidth() == 0) {
                    if (++*passes > 200) {
                        token->revoke();
                    }
                    return;
                }
                *passes = 0;


                // The whole control, not the RichEditBox inside it:
                // collapsing just the inner box leaves the control visible and
                // clickable, and clicking it took SearchHost down with
                // 0xc0000005 inside SearchUx.UI.dll -- its own code, driving a
                // text box that was no longer laid out.
                //
                // And all of them, not the first: there is more than one
                // instance, and collapsing the first found reported success
                // while the visible one carried on working.
                std::vector<wux::FrameworkElement> controls;
                CollectSearchBoxControls(pageRef, 20, controls);

                static bool listed = false;
                if (!listed && !controls.empty()) {
                    listed = true;
                    Rec(L"found %zu RichSearchBoxControl instance(s):",
                        controls.size());
                    for (auto const& c : controls) {
                        Rec(L"  %ls  %.0fx%.0f  visibility=%d",
                            ElementLabel(c).c_str(), c.ActualWidth(),
                            c.ActualHeight(),
                            static_cast<int>(c.Visibility()));
                    }
                }

                // Made inert, not collapsed.
                //
                // Collapsing works right up until something in SearchUx.UI
                // reaches for the element: it is then in the tree but has no
                // layout, and the host dereferences null. That crash arrived
                // twice, from two different elements, and the second time it
                // was a click on our own box in Start -- because clicking
                // there activates SearchHost, which then goes looking for its
                // search box.
                //
                // So leave both elements laid out, where the host can find and
                // measure them, and take away only what we actually need gone:
                //   - Opacity(0)            nothing to see
                //   - IsHitTestVisible      nothing to click
                //   - IsEnabled on the box  nothing to type into
                //
                // The last is the one that matters. The inner RichEditBox is
                // what carries input, and an element that cannot take focus
                // cannot receive keystrokes -- which is what kept the keyboard
                // in Start when the box was collapsed, without removing the
                // element the host expects to exist.
                int changed = 0;

                if (auto inner = FindQueryBox(pageRef, 20)) {
                    g_queryBox = inner;
                    if (inner.IsEnabled() || inner.Opacity() != 0.0) {
                        inner.IsEnabled(false);
                        inner.IsTabStop(false);
                        inner.Opacity(0.0);
                        inner.IsHitTestVisible(false);
                        ++changed;
                        Rec(L"input made inert: %ls",
                            ElementLabel(inner).c_str());
                    }
                }

                for (auto const& c : controls) {
                    if (c.Opacity() != 0.0) {
                        c.Opacity(0.0);
                        c.IsHitTestVisible(false);
                        ++changed;
                        Rec(L"chrome made inert: %ls (%.0fx%.0f)",
                            ElementLabel(c).c_str(), c.ActualWidth(),
                            c.ActualHeight());
                    }
                }

                if (changed == 0) {
                    return;  // nothing to do this pass
                }
                if (!controls.empty()) {
                    *known = winrt::make_weak(controls.front());
                }
            } catch (...) {
                token->revoke();
            }
        });
}

void AttachQueryBox(wux::FrameworkElement const& page) {
    auto box = FindQueryBox(page, 20);
    if (!box) {
        Rec(L"no TextBox on the search page; the query cannot be read");
        return;
    }
    g_queryBox = box;
    Rec(L"query box found: %ls", ElementLabel(box).c_str());

    g_queryChanged = box.TextChanged(
        winrt::auto_revoke,
        [](wf::IInspectable const& sender, wux::RoutedEventArgs const&) {
            try {
                auto box = sender.try_as<wuxc::RichEditBox>();
                if (!box || g_strippingCompletion) {
                    return;
                }
                std::wstring text = DocumentText(box);
                Change change = Classify(text, g_lastBoxText, g_typed);
                g_lastBoxText = text;

                if (change == Change::Completion) {
                    if (g_settings.stripAutoComplete) {
                        StripCompletion(box, g_typed);
                        g_lastBoxText = g_typed;
                    }
                    return;  // never publish a guess
                }

                g_typed = text;
                if (text != g_lastPublished) {
                    g_lastPublished = text;
                    PublishQuery(text);
                }
            } catch (...) {
                // A failure here must not take the search box down with it.
            }
        });

    // Publish whatever is already in the box, so a broker that starts late
    // does not sit waiting for the next keystroke.
    g_typed = DocumentText(box);
    g_lastBoxText = g_typed;
    g_lastPublished = g_typed;
    PublishQuery(g_lastPublished);
}

void RemoveInjection() {
    try {
        HandBack();
        DestroyListener();
        if (g_panel && g_parentPanel) {
            uint32_t index = 0;
            if (g_parentPanel.Children().IndexOf(g_panel, index)) {
                g_parentPanel.Children().RemoveAt(index);
                Rec(L"removed the panel");
            }
        }
    } catch (...) {
        Rec(L"teardown threw: %08X", static_cast<unsigned>(winrt::to_hresult()));
    }
    g_panel = nullptr;
    g_parentPanel = nullptr;
    g_appsList = nullptr;
    g_filesList = nullptr;
    g_appsHeader = nullptr;
    g_filesHeader = nullptr;
    g_status = nullptr;
    g_webHost = nullptr;
    g_queryChanged.revoke();
    g_queryBox = nullptr;
    if (g_probeTimer) {
        g_probeTimer.Stop();
        g_probeTimer = nullptr;
    }
    g_probeChanged.revoke();
    g_probeGot.revoke();
    g_probeLost.revoke();
    g_probeBox = nullptr;
    g_ownInputAttached.store(false);
    g_refocusCount.store(0);
    g_typed.clear();
    g_lastBoxText.clear();
    g_lastPublished.clear();
    g_built = false;
    g_takenOver = false;
}

}  // namespace

// ===========================================================================
// XAML diagnostics plumbing (adapted from m417z's Notification Center Styler)
// ===========================================================================

class VisualTreeWatcher
    : public winrt::implements<VisualTreeWatcher, IVisualTreeServiceCallback2,
                               winrt::non_agile> {
   public:
    explicit VisualTreeWatcher(winrt::com_ptr<IUnknown> site)
        : m_XamlDiagnostics(site.as<IXamlDiagnostics>()) {
        Wh_Log(L"Constructing VisualTreeWatcher");

        // Calling AdviseVisualTreeChange on this thread can hang the app in
        // Advising::RunOnUIThread; doing it from a fresh thread avoids that.
        HANDLE thread = CreateThread(
            nullptr, 0,
            [](LPVOID param) -> DWORD {
                auto* watcher = reinterpret_cast<VisualTreeWatcher*>(param);
                auto service =
                    watcher->m_XamlDiagnostics.as<IVisualTreeService3>();
                HRESULT hr = service->AdviseVisualTreeChange(watcher);
                watcher->Release();
                if (FAILED(hr)) {
                    Wh_Log(L"AdviseVisualTreeChange failed: %08X", hr);
                }
                return 0;
            },
            this, 0, nullptr);
        if (thread) {
            AddRef();
            CloseHandle(thread);
        }
    }

    VisualTreeWatcher(const VisualTreeWatcher&) = delete;
    VisualTreeWatcher& operator=(const VisualTreeWatcher&) = delete;

    void UnadviseVisualTreeChange() {
        HRESULT hr = m_XamlDiagnostics.as<IVisualTreeService3>()
                         ->UnadviseVisualTreeChange(this);
        if (FAILED(hr)) {
            Wh_Log(L"UnadviseVisualTreeChange failed: %08X", hr);
        }
    }

   private:
    HRESULT STDMETHODCALLTYPE
    OnVisualTreeChange(ParentChildRelation, VisualElement element,
                       VisualMutationType mutationType) override try {
        if (mutationType != Add || !element.Type) {
            return S_OK;
        }
        if (wcscmp(element.Type, L"Cortana.UI.Views.TaskbarSearchPage") != 0) {
            return S_OK;
        }
        // Hiding the box has to happen for *every* page, including the ones
        // built after the panel exists -- unlike the panel itself, which is
        // built once. So this runs before the g_built check rather than after
        // it.
        if (g_settings.hideHostSearchBox) {
            static int pages = 0;
            Rec(L"page #%d seen; arming the hide", ++pages);
            try {
                wf::IInspectable pageObj;
                if (SUCCEEDED(m_XamlDiagnostics->GetIInspectableFromHandle(
                        element.Handle,
                        reinterpret_cast<::IInspectable**>(
                            winrt::put_abi(pageObj))))) {
                    if (auto pageFe = pageObj.try_as<wux::FrameworkElement>()) {
                        HideHostSearchBoxWhenReady(pageFe);
                    }
                }
            } catch (...) {
            }
        }

        if (g_built) {
            return S_OK;
        }
        Rec(L"TaskbarSearchPage added");

        wf::IInspectable obj;
        winrt::check_hresult(m_XamlDiagnostics->GetIInspectableFromHandle(
            element.Handle,
            reinterpret_cast<::IInspectable**>(winrt::put_abi(obj))));
        auto page = obj.try_as<wux::FrameworkElement>();
        if (!page) {
            return S_OK;
        }
        g_xamlThreadId.store(GetCurrentThreadId());

        // Do not measure or resize here. The page exists but has not been
        // laid out: the web view reports 0x0 and the window is still at a
        // default size. Wait for a layout pass where the sizes are real.
        auto pageRef = page;
        auto token =
            std::make_shared<wux::FrameworkElement::LayoutUpdated_revoker>();
        *token = page.LayoutUpdated(
            winrt::auto_revoke,
            [pageRef, token](wf::IInspectable const&, wf::IInspectable const&) {
                if (g_built) {
                    return;
                }
                if (pageRef.ActualWidth() < 100) {
                    return;  // page itself not laid out yet
                }
                static bool dumped = false;
                if (!dumped) {
                    dumped = true;
                    Rec(L"--- tree dump begins (page %.0fx%.0f) ---",
                        pageRef.ActualWidth(), pageRef.ActualHeight());
                    DumpTree(pageRef, 0, 25);
                    Rec(L"--- tree dump ends ---");
                }
                // Our own input box goes in here, at the first laid-out
                // frame of the page. Everything below this waits for the web
                // view host, which is far too late to own the first keystroke.
                if (g_settings.ownInputProbe &&
                    !g_ownInputAttached.exchange(true)) {
                    auto rootObj =
                        FindDescendantByName(pageRef, L"RootGrid", 20);
                    auto rootPanel =
                        rootObj ? rootObj.try_as<wuxc::Panel>() : nullptr;
                    if (!rootPanel) {
                        Rec(L"probe: no RootGrid panel to attach to");
                    } else {
                        // Microsoft's box is found here too, rather than at
                        // panel-build time, so the log can show what it holds
                        // from the very first keystroke onwards.
                        if (!g_queryBox) {
                            if (auto theirs = FindQueryBox(pageRef, 20)) {
                                g_queryBox = theirs;
                            }
                        }
                        Rec(L"probe: attaching at page layout (page %.0fx%.0f)",
                            pageRef.ActualWidth(), pageRef.ActualHeight());
                        AttachOwnInputProbe(rootPanel);
                    }
                }

                // Path confirmed with UWPSpy:
                //   RootGrid > QueryFormulationRoot > QueryFormulation > Grid
                //     > HostedWebView2Control > WebViewGrid > WebView2
                // Hiding the host takes the whole results surface out of the
                // layout, so nothing is asked to reflow into a narrower space.
                // Two variants exist and which one the page builds is not
                // stable: one run had
                // HostedWebView2Control#QueryFormulationHostedWebView2 over a
                // WebView2Standalone.Controls.WebView2, the next had
                // HostedWebViewControl#QueryFormulationHostedWebView over a
                // plain Windows.UI.Xaml.Controls.WebView. Anchoring on one
                // name meant the panel silently never built on the other.
                auto hostObj = FindDescendantByName(
                    pageRef, L"QueryFormulationHostedWebView2", 20);
                if (!hostObj) {
                    hostObj = FindDescendantByName(
                        pageRef, L"QueryFormulationHostedWebView", 20);
                }
                auto host =
                    hostObj ? hostObj.try_as<wux::FrameworkElement>() : nullptr;
                static bool complainedMissing = false;
                if (!host) {
                    if (!complainedMissing) {
                        complainedMissing = true;
                        Rec(L"anchor: no QueryFormulationHostedWebView[2] in the "
                            L"tree");
                    }
                    return;
                }
                auto parent = wuxm::VisualTreeHelper::GetParent(host)
                                  .try_as<wuxc::Panel>();
                auto parentFe = parent.try_as<wux::FrameworkElement>();
                if (!parent || !parentFe) {
                    Rec(L"FAIL: the web view host's parent is not a Panel");
                    token->revoke();
                    return;
                }
                // Wait on the parent's size, not the host's. When the web
                // view is suppressed the host never gets one -- there is no
                // browser to size it -- and keying off it meant the panel was
                // never built in exactly the configuration it is needed for.
                static bool complainedSize = false;
                if (parentFe.ActualWidth() < 100) {
                    if (!complainedSize) {
                        complainedSize = true;
                        Rec(L"anchor: %ls is %.0fx%.0f, host %.0fx%.0f -- "
                            L"waiting for a size",
                            ElementLabel(parentFe).c_str(),
                            parentFe.ActualWidth(), parentFe.ActualHeight(),
                            host.ActualWidth(), host.ActualHeight());
                    }
                    return;
                }

                try {
                    g_webHost = host;
                    if (BuildPanel(parent) && CreateListener()) {
                        AttachQueryBox(pageRef);
                        // The probe box is attached earlier now, at the
                        // page's first layout pass -- see above.
                        Rec(L"ready: panel built (host %.0fx%.0f, anchor "
                            L"%.0fx%.0f)",
                            host.ActualWidth(), host.ActualHeight(),
                            parentFe.ActualWidth(), parentFe.ActualHeight());
                    }
                } catch (...) {
                    Rec(L"FAIL during build: %08X",
                        static_cast<unsigned>(winrt::to_hresult()));
                }
                token->revoke();
            });

        return S_OK;
    } catch (...) {
        Wh_Log(L"OnVisualTreeChange error: %08X", winrt::to_hresult());
        return S_OK;  // never fail the shell's callback
    }

    HRESULT STDMETHODCALLTYPE OnElementStateChanged(InstanceHandle,
                                                    VisualElementState,
                                                    LPCWSTR) noexcept override {
        return S_OK;
    }

    winrt::com_ptr<IXamlDiagnostics> m_XamlDiagnostics = nullptr;
};

namespace {
[[clang::no_destroy]] winrt::com_ptr<VisualTreeWatcher> g_visualTreeWatcher;
}

// {C85D8CC7-5463-40E8-A432-F5916B6427E5}
static constexpr CLSID CLSID_WindhawkTAP = {
    0xc85d8cc7,
    0x5463,
    0x40e8,
    {0xa4, 0x32, 0xf5, 0x91, 0x6b, 0x64, 0x27, 0xe5}};

class WindhawkTAP : public winrt::implements<WindhawkTAP, IObjectWithSite,
                                             winrt::non_agile> {
   public:
    HRESULT STDMETHODCALLTYPE SetSite(IUnknown* pUnkSite) override try {
        if (g_visualTreeWatcher) {
            g_visualTreeWatcher->UnadviseVisualTreeChange();
            g_visualTreeWatcher = nullptr;
        }

        site.copy_from(pUnkSite);

        if (site) {
            // Balance the refcount taken by InitializeXamlDiagnosticsEx.
            FreeLibrary(GetCurrentModuleHandle());
            g_visualTreeWatcher = winrt::make_self<VisualTreeWatcher>(site);
        }

        return S_OK;
    } catch (...) {
        HRESULT hr = winrt::to_hresult();
        Wh_Log(L"SetSite error: %08X", hr);
        return hr;
    }

    HRESULT STDMETHODCALLTYPE GetSite(REFIID riid,
                                      void** ppvSite) noexcept override {
        return site.as(riid, ppvSite);
    }

   private:
    winrt::com_ptr<IUnknown> site;
};

template <class T>
struct SimpleFactory
    : winrt::implements<SimpleFactory<T>, IClassFactory, winrt::non_agile> {
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* pUnkOuter, REFIID riid,
                                             void** ppvObject) override try {
        if (pUnkOuter) {
            return CLASS_E_NOAGGREGATION;
        }
        *ppvObject = nullptr;
        return winrt::make<T>().as(riid, ppvObject);
    } catch (...) {
        return winrt::to_hresult();
    }

    HRESULT STDMETHODCALLTYPE LockServer(BOOL) noexcept override {
        return S_OK;
    }
};

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdll-attribute-on-redeclaration"

__declspec(dllexport) _Use_decl_annotations_ STDAPI
    DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) try {
    if (rclsid == CLSID_WindhawkTAP) {
        *ppv = nullptr;
        return winrt::make<SimpleFactory<WindhawkTAP>>().as(riid, ppv);
    }
    return CLASS_E_CLASSNOTAVAILABLE;
} catch (...) {
    return winrt::to_hresult();
}

__declspec(dllexport) _Use_decl_annotations_ STDAPI DllCanUnloadNow() {
    return winrt::get_module_lock() ? S_FALSE : S_OK;
}

#pragma clang diagnostic pop

using PFN_INITIALIZE_XAML_DIAGNOSTICS_EX =
    decltype(&InitializeXamlDiagnosticsEx);

static HRESULT InjectWindhawkTAP() noexcept {
    HMODULE module = GetCurrentModuleHandle();
    if (!module) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    WCHAR location[MAX_PATH];
    switch (GetModuleFileName(module, location, ARRAYSIZE(location))) {
        case 0:
        case ARRAYSIZE(location):
            return HRESULT_FROM_WIN32(GetLastError());
    }

    const HMODULE wuxDll = LoadLibraryEx(L"Windows.UI.Xaml.dll", nullptr,
                                         LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!wuxDll) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    const auto ixde = reinterpret_cast<PFN_INITIALIZE_XAML_DIAGNOSTICS_EX>(
        GetProcAddress(wuxDll, "InitializeXamlDiagnosticsEx"));
    if (!ixde) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    // There is no way to know which diagnostics slot is free, so walk the
    // connection names until one takes. ERROR_NOT_FOUND means "that name is
    // not available", i.e. keep going -- which is the entire reason the loop
    // exists. Anything else, success or a real error, ends it.
    HRESULT hr = E_FAIL;
    for (int i = 0; i < 64; i++) {
        WCHAR connectionName[256];
        wsprintf(connectionName, L"VisualDiagConnection%d", i + 1);

        hr = ixde(connectionName, GetCurrentProcessId(), L"", location,
                  CLSID_WindhawkTAP, nullptr);
        if (hr != HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) {
            break;
        }
    }

    return hr;
}

// ===========================================================================
// Mod entry points
// ===========================================================================

namespace {

// Retry until the XAML runtime exists. Calling InjectWindhawkTAP before then
// returns ERROR_NOT_FOUND, which is what happens whenever the host has just
// restarted -- exactly the case here, since installing the mod restarts it.
[[clang::no_destroy]] std::thread g_tapThread;
std::atomic<bool> g_tapQuit{false};

}  // namespace

BOOL Wh_ModInit() {
    Wh_Log(L">");
    LoadSettings();

    // Installed unconditionally so that toggling the setting does not need a
    // reinstall; the hook checks the setting on every call and is a string
    // compare when it is off.
    if (!WindhawkUtils::SetFunctionHook(&CreateProcessW,
                                            CreateProcessW_Hook,
                                            &CreateProcessW_Original)) {
        Rec(L"could not hook CreateProcessW; the web view cannot be stopped");
    }
    // Nothing else here on purpose. Wh_ModInit runs before the host finishes
    // starting, and the first COM activation in a process fixes its security
    // settings process-wide -- doing that from here makes the host's own
    // CoInitializeSecurity fail with RPC_E_TOO_LATE and fast-fail.
    return TRUE;
}

void Wh_ModAfterInit() {
    Wh_Log(L">");
    Rec(L"=== everything-search attached, pid=%lu ===", GetCurrentProcessId());
    g_tapQuit.store(false);
    g_tapThread = std::thread([] {
        try {
            for (int attempt = 0; attempt < 120 && !g_tapQuit.load();
                 attempt++) {
                if (XamlWindowExists()) {
                    HRESULT hr = InjectWindhawkTAP();
                    Rec(L"InjectWindhawkTAP (attempt %d) -> %08X", attempt + 1,
                        static_cast<unsigned>(hr));
                    if (SUCCEEDED(hr)) {
                        return;
                    }
                }
                Sleep(500);
            }
            Rec(L"gave up waiting for XAML");
        } catch (...) {
            Rec(L"TAP thread threw");
        }
    });
}

BOOL Wh_ModSettingsChanged(BOOL* bReload) {
    Wh_Log(L">");
    // Column widths and row heights are baked into elements that already
    // exist, so the mod is reloaded rather than trying to mutate them.
    *bReload = TRUE;
    return TRUE;
}

void Wh_ModUninit() {
    Wh_Log(L">");
    g_tapQuit.store(true);
    if (g_tapThread.joinable()) {
        g_tapThread.join();
    }
    if (g_visualTreeWatcher) {
        g_visualTreeWatcher->UnadviseVisualTreeChange();
        g_visualTreeWatcher = nullptr;
    }
    // Must happen on the thread that owns the tree, and before we return --
    // Windhawk frees this image the moment we do.
    DWORD xamlThread = g_xamlThreadId.load();
    if (xamlThread == GetCurrentThreadId()) {
        RemoveInjection();
    } else if (g_listener) {
        // Bounded, because a hung XAML thread must not hang the unload: if
        // the hop does not complete we are no worse off than before, and
        // leaving the panel behind beats deadlocking the shell.
        DWORD_PTR result = 0;
        LRESULT ok = SendMessageTimeoutW(g_listener, WM_PANEL_TEARDOWN, 0, 0,
                                         SMTO_ABORTIFHUNG, 2000, &result);
        Rec(L"teardown marshalled to the XAML thread: %ls",
            ok ? L"done" : L"timed out, tree left alone");
    } else if (xamlThread) {
        Rec(L"uninit on the wrong thread and no listener; tree left alone");
    }
    Rec(L"=== everything-search detached ===");
}
