#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <mmsystem.h>
#include <objbase.h>
#include <gdiplus.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>
#include <ctime>

#include "cloud_icon.h"
#include "wen_icon.h"

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "ole32.lib")

namespace {

constexpr wchar_t kClassName[] = L"SkysS2FishingMacroWindow";
constexpr UINT WM_TRACKER_UPDATE = WM_APP + 1;
constexpr UINT WM_CALIBRATION_DONE = WM_APP + 2;
constexpr UINT WM_KEYBIND_DONE = WM_APP + 3;
constexpr int ID_TOGGLE = 1001;
constexpr int ID_EXIT = 1002;
constexpr int ID_CALIBRATE = 1003;
constexpr int ID_BIND_TOGGLE = 1004;
constexpr int ID_TAB = 1005;
constexpr int ID_BIND_QUIT = 1006;
constexpr int ID_WAIT_FISH_EDIT = 1007;
constexpr int ID_CALIBRATE_BAR = 1008;
constexpr int ID_CALIBRATE_EXIT = 1009;
constexpr int ID_RESPAWN_CHECK = 1010;
constexpr int ID_RESPAWN_EDIT = 1011;
constexpr int ID_COLLECT_EDIT = 1012;
constexpr int ID_HOLD_EDIT = 1013;
constexpr int ID_BAIT_AMOUNT_EDIT = 1014;
constexpr int ID_BAIT_DELAY_EDIT = 1015;
constexpr int ID_DISCORD_URL_EDIT = 1016;
constexpr int ID_DISCORD_MIN_EDIT = 1017;
constexpr int ID_DISCORD_PING_EDIT = 1018;
constexpr int HOTKEY_TOGGLE = 1;
constexpr int HOTKEY_QUIT = 2;
constexpr int HOTKEY_BAIT = 4; // Auto Bait start/stop
// Shared by both "Calibrate Start Point" and the hotkey binder: whichever
// capture is in progress, Esc cancels it.
constexpr int HOTKEY_CANCEL_CAPTURE = 3;

// How often the tracker thread's telemetry actually repaints the HUD. The
// scan/control loop itself still runs at full speed (unrelated to this) -
// this only throttles how often the window redraws, which is what was
// causing the buttons to visibly flicker.
constexpr ULONGLONG kUiRefreshIntervalMs = 90;

enum class ControlState { Waiting, Holding, Releasing, Floating, Paused };

// The overall loop the tracker drives once a cycle is running:
//   Cast    -> a single click starts the next fishing minigame
//   Hook    -> existing bar-catching control, until the exit bar disappears
//   Collect -> a fixed pause after hooking ends
//   HoldKey -> holds the T key to collect/confirm, then loops back to Cast
enum class Phase { Cast, Hook, Collect, HoldKey, VerifyCollect, WaitForFish, Respawn, Reposition };

// Pause between the minigame ending and holding T to collect (user setting).
constexpr ULONGLONG kDefaultCollectDelayMs = 4000;
constexpr ULONGLONG kMaxCollectDelayMs = 30000;
// How long T is held to collect (user setting, "Hold T time").
constexpr ULONGLONG kDefaultHoldKeyMs = 4000;
constexpr ULONGLONG kMinHoldKeyMs = 1000;
constexpr ULONGLONG kMaxHoldKeyMs = 15000;
// Item-collection check: after releasing T, keep looking for the game's
// "<item> x1" message this long; without it the hold is retried this many
// times before the catch is counted as "failed to collect".
constexpr ULONGLONG kCollectVerifyMs = 500; // message always shows during the hold (measured)
constexpr int kCollectRetries = 2;
// The message must stay visible this long (filters one-frame flashes).
constexpr double kCollectConfirmMs = 150.0;
// Auto reposition (set respawn): every N catches the character is reset with
// Esc -> R -> Enter, which puts it back on its spawn point and undoes the
// small drift after each catch. After the keys we wait this long for the
// respawn to finish before casting again.
constexpr ULONGLONG kRespawnSettleMs = 6000;
constexpr int kDefaultRespawnEveryCatches = 10;
constexpr int kMinRespawnEveryCatches = 1;
constexpr int kMaxRespawnEveryCatches = 999;
// With auto reposition on, the character is also reset after this many casts
// in a row where the minigame never started (Wait for fish ran out without
// the Exit button appearing) - usually a sign the character drifted to a
// spot where casting no longer works.
constexpr int kMissedCastsBeforeRespawn = 2;
// Require both the exit bar and the bar's target zone to have been gone for
// this long before treating hooking as finished. This used to be 6 frames
// (~20 ms at 250-330 scans/sec), so any brief misread ended hooking and the
// tracker went on to hold T while the fish was still being reeled in.
constexpr ULONGLONG kHookGoneDebounceMs = 1200;
// If the exit bar never appears at all within this long after a Cast
// click, the click almost certainly didn't register (rather than the fish
// just taking a while to bite - the exit bar is tied to the minigame
// opening, not to a bite). Rather than sit there waiting on a minigame
// that never started, automatically go back and retry the cast. This is
// what actually eliminates occasional dropped clicks, rather than just
// tuning timing and hoping.
constexpr ULONGLONG kCastConfirmTimeoutMs = 3000;
// Failsafe against a cast click landing more than once (e.g. a double-fire
// at the OS/game level) and the minigame appearing to start, end, and
// restart in a fast little loop: once a click is actually confirmed
// successful - the exit bar has appeared, meaning the reel really is out
// in the water - hold off on firing another cast click for this long,
// no matter what brings the state machine back around to Phase::Cast.
constexpr ULONGLONG kPostCastConfirmCooldownMs = 10000;
// Delay between finishing the collection step and sending the next cast.
// This is intentionally user-configurable because different rods can have
// different reel/return timing. It prevents the next cast click from landing
// immediately and accidentally restarting the reel.
constexpr ULONGLONG kDefaultWaitForFishMs = 3000;
constexpr ULONGLONG kMinWaitForFishMs = 0;
constexpr ULONGLONG kMaxWaitForFishMs = 60000;
// A dragged calibration region smaller than this in either dimension is
// almost certainly an accidental click rather than a real drag, and is
// rejected so it doesn't silently leave a near-zero-size capture region.
constexpr int kMinRegionSize = 6;

struct Telemetry {
    bool enabled = false;
    bool minigame = false;
    bool targetFound = false;
    bool playerFound = false;
    bool mouseDown = false;
    bool captureReady = false; // false until both bar and exit regions are calibrated
    float targetY = 0.0f;
    float playerY = 0.0f;
    float error = 0.0f;
    float fps = 0.0f;
    float phaseElapsedMs = 0.0f;
    float castCooldownRemainingMs = 0.0f; // >0 while the post-confirm failsafe is holding off the next cast click
    float waitForFishRemainingMs = 0.0f;
    int catchesSinceRespawn = 0;
    int totalCatches = 0;
    int zonePct = -1; // share of this minigame's frames with the block inside the zone
    int failedCollects = 0;
    int collectAttempt = 0;       // 0-based try of the current collect
    int lastCollectDetected = -1; // -1 no check yet, 0 not found, 1 detected
    bool collectCheckReady = false;
    int rarityCounts[5] = {}; // indexed by Rarity (kRarityCount)
    int lastRarity = -1;
    int oreCount = 0;
    bool lastWasOre = false;
    double runningMs = 0.0; // time actually spent fishing this session (not paused)
    bool positionLost = false;   // walk back couldn't find the anchor spot again
    int positionOffset = -1;     // px from the anchor spot at the last check (-1 = not checked)
    bool learningWalk = false;   // learning the W/A/S/D steps right now
    int confidence = 0;
    ControlState state = ControlState::Paused;
    Phase phase = Phase::Cast;
};

std::atomic<bool> gQuit{false};
std::atomic<bool> gEnabled{false};
std::atomic<ULONGLONG> gWaitForFishMs{kDefaultWaitForFishMs};
std::atomic<bool> gAutoRespawn{false};
// Auto reposition mode: 0 = off, 1 = reset character (gamepass), 2 = walk
// back to the position anchor with W/A/S/D. gAutoRespawn mirrors mode 1.
std::atomic<int> gRepositionMode{0};
std::atomic<bool> gLearnWalkRequested{false}; // UI -> tracker: learn the W/A/S/D steps
// Setup tab toggle: write one CSV per minigame into logs\ for tuning.
std::atomic<bool> gMinigameLog{false};
std::atomic<ULONGLONG> gCollectDelayMs{kDefaultCollectDelayMs};
std::atomic<ULONGLONG> gHoldKeyMs{kDefaultHoldKeyMs};
// Pressed once, quickly, every time the macro is started: the correction
// key first, then the rod key (re-equips the rod).
std::atomic<WORD> gCorrectionKeyVk{static_cast<WORD>('1')};
std::atomic<WORD> gRodKeyVk{static_cast<WORD>('5')};
std::atomic<int> gRespawnEveryCatches{kDefaultRespawnEveryCatches};
std::mutex gTelemetryMutex;
Telemetry gTelemetry;

// Fish sessions: one line of numbers per session in
// SkysS2FishingMacro_sessions.txt next to the exe (saved on exit and every
// minute while running, so an accidental close or crash loses nothing).
struct SessionRecord {
    int id = 0;
    long long start = 0;     // unix time of the first Start
    long long end = 0;       // unix time of the last save
    long long runningMs = 0; // time actually fishing
    int catches = 0;
    int noItem = 0;
    int ore = 0;
    int rarity[5] = {};      // Impossible, Mythic, Legendary, Rare, Common
};
std::vector<SessionRecord> gSessions; // UI thread only, sorted by id
int gCurrentSessionId = 1;
long long gCurrentSessionStart = 0;   // 0 = not started fishing yet this run
int gRestorePromptId = -1;            // session offered for continuing at startup
int gSessionPage = 0;
int gSessionOpenId = -1;              // session shown in detail on the Sessions tab
bool gSessionDeleteArmed = false;     // "Delete" needs a second click
// Hand-off of a continued session's numbers to the tracker thread.
std::mutex gRestoreMutex;
bool gRestorePending = false;
SessionRecord gRestoreData;
HWND gWindow = nullptr;
HWND gToggleButton = nullptr;
HWND gExitButton = nullptr;
HWND gWaitFishEdit = nullptr;
HWND gRespawnEdit = nullptr;
HWND gCollectEdit = nullptr;
HWND gHoldKeyEdit = nullptr;
HWND gCalibrateButton = nullptr;
HWND gCalibrateBarButton = nullptr;
HWND gCalibrateExitButton = nullptr;
HWND gToggleBindButton = nullptr; // Keystrokes tab: rebind Start/Pause
HWND gExitBindButton = nullptr;   // Keystrokes tab: rebind Exit
HFONT gUiFont = nullptr;
HBRUSH gBackgroundBrush = nullptr;
int gActiveTab = 0; // 0 = Fishing, 1 = Settings, 2 = Hotkeys, 3 = Setup

// Which calibration is currently in progress, if any.
enum class CalibrationTarget { None, CastPoint, BarRegion, ExitRegion, CollectRegion, BaitPoint, AnchorRegion };
CalibrationTarget gCalibrationTarget = CalibrationTarget::None; // UI thread only
POINT gDragStart{};   // first corner of a region drag, set by the mouse hook
bool gDragging = false; // UI thread only; true between mousedown and mouseup

// Screen point the Cast phase clicks to start the next fishing minigame.
// Defaults to the primary screen's center; "Calibrate Start Point" lets the
// user click anywhere to set it instead.
std::mutex gCastPointMutex;
POINT gCastPoint{0, 0};
std::atomic<bool> gCalibrating{false};
HHOOK gMouseHook = nullptr;

// The top-level window the calibration click landed on. Used to force that
// window to the foreground for a moment before the Cast click and before
// holding the key, so both still land correctly even if the game isn't the
// active/focused window at the time.
std::atomic<HWND> gTargetWindow{nullptr};

// Fixed key the HoldKey phase holds down.
constexpr WORD kHoldKeyVk = static_cast<WORD>('T');

// The Start/Pause and Exit hotkeys, rebindable from the Keystrokes tab.
std::atomic<WORD> gToggleHotkeyVk{static_cast<WORD>(VK_F6)};
std::atomic<WORD> gQuitHotkeyVk{static_cast<WORD>(VK_F8)};
std::atomic<WORD> gBaitHotkeyVk{static_cast<WORD>(VK_F3)};
std::atomic<bool> gBindingHotkey{false};
int gBindingTarget = 0; // 0 = none, 1 = Start/Pause, 2 = Exit (UI thread only)
WORD gCapturedVk = 0;   // set by the hook, read once WM_KEYBIND_DONE arrives
HHOOK gKeyboardHook = nullptr;

// GDI+ is only used to decode/draw the two embedded LIVE/OFF fish-icon PNGs.
ULONG_PTR gGdiplusToken = 0;
Gdiplus::Bitmap* gCloudIcon = nullptr; // the logo (header, app icon, Credits, Discord screenshots)
Gdiplus::Bitmap* gWenIcon = nullptr; // Auto Bait wen calculator

Gdiplus::Bitmap* LoadPngFromMemory(const unsigned char* data, size_t size) {
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!hMem) return nullptr;
    void* mem = GlobalLock(hMem);
    if (!mem) {
        GlobalFree(hMem);
        return nullptr;
    }
    memcpy(mem, data, size);
    GlobalUnlock(hMem);

    IStream* stream = nullptr;
    if (CreateStreamOnHGlobal(hMem, TRUE, &stream) != S_OK) {
        // TRUE above means the stream owns hMem and frees it on release
        // even on this failure path, so no separate GlobalFree here.
        return nullptr;
    }
    Gdiplus::Bitmap* bitmap = Gdiplus::Bitmap::FromStream(stream);
    stream->Release();
    if (!bitmap) return nullptr;
    if (bitmap->GetLastStatus() != Gdiplus::Ok) {
        delete bitmap;
        return nullptr;
    }
    return bitmap;
}

POINT GetCastPoint() {
    std::lock_guard<std::mutex> lock(gCastPointMutex);
    return gCastPoint;
}

void SetCastPoint(POINT p) {
    std::lock_guard<std::mutex> lock(gCastPointMutex);
    gCastPoint = p;
}

// The fishing bar and exit-button regions, in absolute screen coordinates -
// calibrated live by dragging over them on whatever screen/resolution this
// particular user actually has, rather than assumed from any fixed
// reference resolution. {0,0,0,0} means "not calibrated yet".
std::mutex gBarRectMutex;
RECT gBarRect{0, 0, 0, 0};
std::mutex gExitRectMutex;
RECT gExitRect{0, 0, 0, 0};

RECT GetBarRect() {
    std::lock_guard<std::mutex> lock(gBarRectMutex);
    return gBarRect;
}

void SetBarRect(RECT r) {
    std::lock_guard<std::mutex> lock(gBarRectMutex);
    gBarRect = r;
}

RECT GetExitRect() {
    std::lock_guard<std::mutex> lock(gExitRectMutex);
    return gExitRect;
}

void SetExitRect(RECT r) {
    std::lock_guard<std::mutex> lock(gExitRectMutex);
    gExitRect = r;
}

// Where the game shows the "<item> x1" message after collecting.
std::mutex gCollectRectMutex;
RECT gCollectRect{0, 0, 0, 0};

// Auto bait buy: the 7 buttons clicked in order for one purchase of 99 bait.
constexpr int kBaitClicks = 7;
constexpr int kBaitPerCycle = 99;
const wchar_t* const kBaitClickNames[kBaitClicks] = {
    L"Fish head bait", L"Max", L"Buy the selection", L"Dialogue", L"Deal", L"Dialogue 2", L"Buy more"};
std::mutex gBaitPointsMutex;
POINT gBaitPoints[kBaitClicks] = {};
int gBaitCalibIndex = 0;                       // which point is being calibrated (UI thread)
std::atomic<int> gBaitAmount{kBaitPerCycle};   // always a multiple of 99
std::atomic<int> gBaitDelayMs{600};            // pause after each click
std::atomic<bool> gBaitRunning{false};
std::atomic<bool> gBaitStop{false};
std::atomic<int> gBaitCycle{0};                // 1-based progress, for the UI
std::atomic<int> gBaitClick{0};
std::atomic<ULONGLONG> gBaitStartMs{0};        // when the current purchase run started
// Time one click takes besides the click delay: focus check, the 12-step
// mouse glide (~135 ms), 80 ms settle and the 50 ms press. Only used for the
// estimate before starting; while buying the real measured pace is used.
constexpr int kBaitClickOverheadMs = 290;
std::atomic<bool> gBaitFocusLost{false};       // last run stopped: game not in front
HWND gBaitAmountEdit = nullptr;
HWND gDiscordUrlEdit = nullptr;
HWND gDiscordMinEdit = nullptr;
HWND gDiscordPingEdit = nullptr;
HFONT gSmallEditFont = nullptr;
bool gMenuOpen = false;        // burger menu (UI thread)
bool gResetArmed = false;      // "Reset stats" needs a second click
HWND gBaitDelayEdit = nullptr;

POINT GetBaitPoint(int i) {
    std::lock_guard<std::mutex> lock(gBaitPointsMutex);
    return gBaitPoints[i];
}

void SetBaitPoint(int i, POINT p) {
    std::lock_guard<std::mutex> lock(gBaitPointsMutex);
    gBaitPoints[i] = p;
}

bool BaitPointSet(int i) {
    const POINT p = GetBaitPoint(i);
    return p.x != 0 || p.y != 0;
}

RECT GetCollectRect() {
    std::lock_guard<std::mutex> lock(gCollectRectMutex);
    return gCollectRect;
}

void SetCollectRect(RECT r) {
    std::lock_guard<std::mutex> lock(gCollectRectMutex);
    gCollectRect = r;
}

bool RectCalibrated(const RECT& r) {
    return (r.right - r.left) >= kMinRegionSize && (r.bottom - r.top) >= kMinRegionSize;
}

// Human-readable name for a virtual-key code (e.g. "T", "Space", "F6").
std::wstring KeyDisplayName(WORD vk) {
    const UINT scan = MapVirtualKey(vk, MAPVK_VK_TO_VSC);
    const LONG lParam = static_cast<LONG>(scan) << 16;
    wchar_t buf[64] = L"";
    if (GetKeyNameTextW(lParam, buf, 64) == 0 || buf[0] == L'\0') {
        swprintf_s(buf, L"VK 0x%02X", vk);
    }
    return buf;
}

const wchar_t* StateText(ControlState state) {
    switch (state) {
        case ControlState::Waiting: return L"WAITING FOR FISH";
        case ControlState::Holding: return L"HOLDING / RISING";
        case ControlState::Releasing: return L"RELEASED / FALLING";
        case ControlState::Floating: return L"MICRO-PULSING";
        case ControlState::Paused: return L"PAUSED";
    }
    return L"UNKNOWN";
}

const wchar_t* PhaseText(Phase phase) {
    switch (phase) {
        case Phase::Cast: return L"CASTING";
        case Phase::Hook: return L"HOOKING";
        case Phase::Collect: return L"COLLECTING";
        case Phase::HoldKey: return L"HOLDING KEY";
        case Phase::VerifyCollect: return L"CHECKING ITEM";
        case Phase::WaitForFish: return L"WAIT FOR FISH";
        case Phase::Respawn: return L"RESPAWNING";
        case Phase::Reposition: return L"WALKING BACK";
    }
    return L"UNKNOWN";
}

void MouseButton(bool down) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    SendInput(1, &input, sizeof(input));
}

// Sleeps in short steps, bailing out early if the app is shutting down, so a
// long deliberate dwell (see CastClick below) doesn't delay Exit.
void InterruptibleSleep(ULONGLONG ms) {
    ULONGLONG remaining = ms;
    while (remaining > 0 && !gQuit.load()) {
        const ULONGLONG step = std::min<ULONGLONG>(remaining, 25);
        Sleep(static_cast<DWORD>(step));
        remaining -= step;
    }
}

// An explicit absolute MOUSEEVENTF_MOVE, on top of SetCursorPos: some games
// only react to a genuine mouse-move input event rather than the cursor
// simply teleporting, so CastClick sends both. Must be normalized against
// the FULL virtual desktop (all monitors, MOUSEEVENTF_VIRTUALDESK) rather
// than just the primary monitor - without that, this call was silently
// overriding SetCursorPos's correct placement with a wrong one whenever
// the calibrated point wasn't on the primary display, which is what was
// causing the click to land somewhere other than the actual start point.
void MoveMouseTo(int x, int y) {
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = std::max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN) - 1);
    const int vh = std::max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN) - 1);
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = static_cast<LONG>((x - vx) * 65535.0 / vw);
    input.mi.dy = static_cast<LONG>((y - vy) * 65535.0 / vh);
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    SendInput(1, &input, sizeof(input));
}

void MoveMouseBy(int dx, int dy) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = dx;
    input.mi.dy = dy;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &input, sizeof(input));
}

// Roblox tracks the mouse from real movement input, not from where the
// Windows cursor is: SetCursorPos alone (and an absolute move to the spot the
// cursor was already teleported to, i.e. a zero-length move) left the game
// aiming at wherever the user's mouse was before Start. So walk the cursor
// there in small relative steps like a real mouse, snap to the exact pixel
// (relative moves are subject to pointer acceleration), then finish with a
// 1-pixel wiggle so the game's last movement event is on the exact spot.
void GlideMouseTo(POINT p) {
    POINT cur{};
    GetCursorPos(&cur);
    constexpr int kSteps = 12;
    int sentX = 0, sentY = 0;
    for (int i = 1; i <= kSteps; ++i) {
        const int wantX = (p.x - cur.x) * i / kSteps;
        const int wantY = (p.y - cur.y) * i / kSteps;
        MoveMouseBy(wantX - sentX, wantY - sentY);
        sentX = wantX;
        sentY = wantY;
        Sleep(8);
    }
    SetCursorPos(p.x, p.y);
    Sleep(8);
    MoveMouseBy(2, 0);
    Sleep(15);
    MoveMouseBy(-2, 0);
    Sleep(15);
    SetCursorPos(p.x, p.y);
    MoveMouseTo(p.x, p.y);
}

void ClickAt(POINT p) {
    POINT previous{};
    GetCursorPos(&previous);
    SetCursorPos(p.x, p.y);
    MouseButton(true);
    Sleep(35);
    MouseButton(false);
    SetCursorPos(previous.x, previous.y);
}

// Brings a window to the foreground/front even though we're a different,
// background process - a plain SetForegroundWindow call is usually blocked
// by Windows unless the calling thread's input state is attached to the
// currently-foreground thread's, so we do that attach/detach dance around
// it. This is what lets the Cast click and the held key actually reach the
// game even when it isn't the window the user currently has focused.
//
// Only the current foreground thread is attached, and never a thread of this
// process: attaching the tracker thread to both our own HUD's UI thread and
// the game's thread chained their input queues together, so a moment where
// the game stalled could freeze the HUD - after a few hours Windows reported
// the app as not responding ("AppHang") and closed it.
void ForceForeground(HWND target) {
    if (!target || !IsWindow(target)) return;
    const HWND current = GetForegroundWindow();
    if (current == target) return;
    if (IsIconic(target)) ShowWindow(target, SW_RESTORE);
    if (SetForegroundWindow(target) && GetForegroundWindow() == target) return;

    const DWORD thisThread = GetCurrentThreadId();
    DWORD curPid = 0;
    const DWORD curThread = current ? GetWindowThreadProcessId(current, &curPid) : 0;
    const bool attached = curThread && curThread != thisThread &&
                          curPid != GetCurrentProcessId() &&
                          AttachThreadInput(thisThread, curThread, TRUE);
    SetForegroundWindow(target);
    BringWindowToTop(target);
    if (attached) AttachThreadInput(thisThread, curThread, FALSE);
}

// The game window used to be whatever top-level window sat under the cast
// point at startup/calibration - if another window (e.g. a chat or editor)
// covered that point, every click and key went there instead of the game.
// Now the Roblox window is looked up by its process, and input is only sent
// once it is confirmed to be the foreground window.
bool IsGameWindow(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd)) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    wchar_t path[MAX_PATH]{};
    DWORD size = MAX_PATH;
    const bool ok = QueryFullProcessImageNameW(process, 0, path, &size) != FALSE;
    CloseHandle(process);
    if (!ok) return false;
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    return _wcsicmp(name, L"RobloxPlayerBeta.exe") == 0;
}

BOOL CALLBACK FindGameWindowProc(HWND hwnd, LPARAM lParam) {
    if (GetWindow(hwnd, GW_OWNER) == nullptr && IsGameWindow(hwnd)) {
        *reinterpret_cast<HWND*>(lParam) = hwnd;
        return FALSE;
    }
    return TRUE;
}

HWND GameWindow() {
    HWND hwnd = gTargetWindow.load();
    if (IsGameWindow(hwnd)) return hwnd;
    hwnd = nullptr;
    EnumWindows(FindGameWindowProc, reinterpret_cast<LPARAM>(&hwnd));
    gTargetWindow.store(hwnd);
    return hwnd;
}

// Brings the game to the front; false if it isn't running or Windows refused
// to switch - callers then skip their click/keys instead of sending them to
// whatever window the user happens to be in.
bool FocusGame() {
    const HWND game = GameWindow();
    if (!game) return false;
    ForceForeground(game);
    for (int i = 0; i < 10 && GetForegroundWindow() != game; ++i) Sleep(20);
    return GetForegroundWindow() == game;
}

// The Cast phase's click specifically: firing a click the instant the
// cursor teleports (and right after force-focusing a window that may not
// have finished activating yet) was sometimes getting dropped by the game.
// This instead moves to the point, dwells there briefly so the game
// registers the hover/focus, performs a firm held click, then stays put
// there a while longer before restoring the cursor - yanking the cursor
// away immediately after mouse-up was apparently also causing missed
// clicks.
void CastClick(POINT p) {
    if (!FocusGame()) return; // never click into some other window

    GlideMouseTo(p);
    InterruptibleSleep(250);

    MouseButton(true);
    InterruptibleSleep(50);
    MouseButton(false);

    // The cursor deliberately stays on the cast point. The game aims the
    // throw where the cursor is when the cast animation finishes (after the
    // click), so restoring the user's previous cursor position here made
    // every cast land wherever the cursor happened to be when Start was
    // pressed instead of on the calibrated point.
    InterruptibleSleep(1000);
}

// Puts the cursor back on the cast point (only while the game is in front),
// so the minigame's clicks and the next throw aim at the calibrated spot
// even if the cursor was moved in the meantime.
void ParkCursorOnCastPoint(POINT p) {
    if (GetForegroundWindow() != GameWindow()) return;
    GlideMouseTo(p);
}

// Scan-code-based injection (KEYEVENTF_SCANCODE) rather than a bare virtual-
// key code: many games read keyboard state via DirectInput/raw input, which
// tracks scan codes and can miss a plain vk-code SendInput entirely - this
// is what was actually causing the T key to not register as held.
void KeyEvent(bool down, WORD vk) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = 0;
    input.ki.wScan = static_cast<WORD>(MapVirtualKey(vk, MAPVK_VK_TO_VSC));
    input.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP);
    SendInput(1, &input, sizeof(input));
}

void TapKey(WORD vk) {
    KeyEvent(true, vk);
    InterruptibleSleep(60);
    KeyEvent(false, vk);
}

// On Start: tap the correction key, then the rod key, quickly - only while
// the game is really in front so the keys never go to another window.
void EquipRodOnStart() {
    if (!FocusGame()) return;
    const HWND game = GameWindow();
    TapKey(gCorrectionKeyVk.load());
    InterruptibleSleep(80);
    if (GetForegroundWindow() != game) return;
    TapKey(gRodKeyVk.load());
    InterruptibleSleep(250);
}

// Roblox reset: Esc opens the menu, R picks "Reset Character", Enter confirms.
// Checks before every key that the game is still in front, so these keys can
// never end up typed into another window. Returns false if it had to stop.
bool ResetCharacter() {
    if (!FocusGame()) return false;
    const HWND game = GameWindow();
    InterruptibleSleep(150);
    const WORD keys[] = {VK_ESCAPE, static_cast<WORD>('R'), VK_RETURN};
    for (int i = 0; i < 3; ++i) {
        if (GetForegroundWindow() != game) {
            if (i > 0) {
                // Menu may be open; close it again once the game is back.
                if (FocusGame()) TapKey(VK_ESCAPE);
            }
            return false;
        }
        TapKey(keys[i]);
        if (i < 2) InterruptibleSleep(400);
    }
    return true;
}

struct CaptureSurface {
    HDC screen = nullptr;
    HDC memory = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ oldBitmap = nullptr;
    uint32_t* pixels = nullptr;
    int width = 0;
    int height = 0;

    bool Create(int w, int h) {
        Destroy();
        width = w;
        height = h;
        screen = GetDC(nullptr);
        memory = CreateCompatibleDC(screen);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = w;
        info.bmiHeader.biHeight = -h; // top-down pixels
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        bitmap = CreateDIBSection(memory, &info, DIB_RGB_COLORS,
                                  reinterpret_cast<void**>(&pixels), nullptr, 0);
        if (!screen || !memory || !bitmap || !pixels) {
            Destroy();
            return false;
        }
        oldBitmap = SelectObject(memory, bitmap);
        return true;
    }

    bool Grab(int x, int y) {
        return BitBlt(memory, 0, 0, width, height, screen, x, y,
                      SRCCOPY | CAPTUREBLT) != FALSE;
    }

    void Destroy() {
        if (memory && oldBitmap) SelectObject(memory, oldBitmap);
        if (bitmap) DeleteObject(bitmap);
        if (memory) DeleteDC(memory);
        if (screen) ReleaseDC(nullptr, screen);
        screen = nullptr;
        memory = nullptr;
        bitmap = nullptr;
        oldBitmap = nullptr;
        pixels = nullptr;
    }

    ~CaptureSurface() { Destroy(); }
};

inline void ReadRgb(uint32_t p, int& r, int& g, int& b) {
    b = static_cast<int>(p & 0xff);
    g = static_cast<int>((p >> 8) & 0xff);
    r = static_cast<int>((p >> 16) & 0xff);
}

// The game applies lighting/transparency, so these predicates deliberately
// recognize the displayed color family instead of requiring one exact RGB.
bool IsTargetGreen(uint32_t p) {
    int r, g, b;
    ReadRgb(p, r, g, b);
    const int maxOther = std::max(r, b);
    const bool dominantGreen = g >= 75 && g - maxOther >= 15 && g >= r * 11 / 10;
    const int dr = r - 60, dg = g - 200, db = b - 80;
    const bool nearReference = dr * dr + dg * dg + db * db <= 95 * 95;
    return dominantGreen || nearReference;
}

// The target zone turns from green to a yellow-green "warning" color after
// the player block has been outside it too long. Sampled reference points:
// a bright gold border around RGB(215,196,5) and a dimmer, semi-transparent
// olive fill around RGB(62,68,28) - very different brightness, but both
// land in the same yellow-to-green hue band (~50-70 degrees), clearly
// separate from the normal target's pure green (~120-140 degrees). Hue is
// used instead of a fixed RGB distance so this still matches regardless of
// how bright/transparent the fill happens to render.
bool IsTargetWarning(uint32_t p) {
    int r, g, b;
    ReadRgb(p, r, g, b);
    const int cmax = std::max({r, g, b});
    const int cmin = std::min({r, g, b});
    const int delta = cmax - cmin;
    if (delta < 12 || cmax < 30) return false; // too gray or too dark to tell
    double hue;
    if (cmax == r) {
        hue = 60.0 * std::fmod((static_cast<double>(g - b) / delta), 6.0);
        if (hue < 0.0) hue += 360.0;
    } else if (cmax == g) {
        hue = 60.0 * ((static_cast<double>(b - r) / delta) + 2.0);
    } else {
        hue = 60.0 * ((static_cast<double>(r - g) / delta) + 4.0);
    }
    const double saturation = static_cast<double>(delta) / cmax;
    return hue >= 35.0 && hue <= 95.0 && saturation >= 0.35;
}

// The warning-state zone fill is a faded, translucent tint over the blue
// bar. Measured in-game: RGB(65,86,74) and RGB(80,104,110) - neither
// IsTargetGreen nor IsTargetWarning accepts these, so only the thin yellow
// border rows were detected (too few rows for StrongestRun) and the zone was
// lost entirely, releasing the mouse and dropping the player block.
// The bar is translucent, so bright sky/foam behind its top can show through
// as a pale grey-blue (e.g. RGB(116,139,154)); keeping g <= 125 and b - g <= 10
// excludes that, and FindBarObjects additionally only accepts tint rows that
// connect to a real green/yellow row (the zone's border).
bool IsTargetTint(uint32_t p) {
    int r, g, b;
    ReadRgb(p, r, g, b);
    return g >= 75 && g <= 125 && b - g <= 10 && r >= 40 && std::max({r, g, b}) < 165;
}

bool IsPlayerWhite(uint32_t p) {
    int r, g, b;
    ReadRgb(p, r, g, b);
    const int hi = std::max({r, g, b});
    const int lo = std::min({r, g, b});
    // Allows the green target to tint the white block when they overlap.
    return r >= 168 && g >= 178 && b >= 145 && hi - lo <= 92;
}

// Only the Exit button's saturated red (measured RGB(125-182, 32-45, 38-46)).
// The old test (r >= 2g) also matched the hotbar's brown horse icon
// (RGB(136,80,42)) and the red screen-edge vignette under the button, so the
// Exit button read as visible while no minigame was open.
bool IsExitRed(uint32_t p) {
    int r, g, b;
    ReadRgb(p, r, g, b);
    return r >= 110 && g <= 75 && b <= 75 && r * 2 >= g * 5 && r * 2 >= b * 5;
}

struct Detection {
    bool found = false;
    float center = 0.0f;
    int top = 0;
    int bottom = 0;
    int score = 0;
};

// With `anchor`, a run only counts if at least one of its rows also reaches
// minimumPerRow in `anchor` on its own.
Detection StrongestRun(const std::vector<int>& rowScore, int minimumPerRow,
                       int minimumRows, const std::vector<int>* anchor = nullptr) {
    Detection best{};
    int runTop = -1;
    int runScore = 0;
    bool runAnchored = false;
    for (int y = 0; y <= static_cast<int>(rowScore.size()); ++y) {
        const bool active = y < static_cast<int>(rowScore.size()) &&
                            rowScore[y] >= minimumPerRow;
        if (active) {
            if (runTop < 0) {
                runTop = y;
                runAnchored = false;
            }
            runScore += rowScore[y];
            if (!anchor || (*anchor)[y] >= minimumPerRow) runAnchored = true;
        } else if (runTop >= 0) {
            const int runBottom = y - 1;
            if (runBottom - runTop + 1 >= minimumRows && runAnchored &&
                runScore > best.score) {
                best.found = true;
                best.top = runTop;
                best.bottom = runBottom;
                best.center = (runTop + runBottom) * 0.5f;
                best.score = runScore;
            }
            runTop = -1;
            runScore = 0;
        }
    }
    return best;
}

bool ExitVisible(CaptureSurface& exitSurface, int x, int y) {
    if (!exitSurface.Grab(x, y)) return false;
    int red = 0;
    const int total = exitSurface.width * exitSurface.height;
    for (int i = 0; i < total; ++i) red += IsExitRed(exitSurface.pixels[i]);
    // The button fills most of a tight calibration box; stray red pixels
    // from the hotbar/vignette never come close to this.
    return red >= std::max(24, total / 6);
}

void FindBarObjects(const CaptureSurface& surface, Detection& target,
                    Detection& player) {
    std::vector<int> solid(surface.height, 0); // clear green/yellow only
    std::vector<int> green(surface.height, 0); // solid plus faded warning tint
    std::vector<int> white(surface.height, 0);
    for (int y = 0; y < surface.height; ++y) {
        for (int x = 0; x < surface.width; ++x) {
            const uint32_t p = surface.pixels[y * surface.width + x];
            const bool isSolid = IsTargetGreen(p) || IsTargetWarning(p);
            solid[y] += isSolid;
            green[y] += (isSolid || IsTargetTint(p));
            white[y] += IsPlayerWhite(p);
        }
    }
    target = StrongestRun(green, std::max(5, surface.width * 28 / 100), 5, &solid);
    player = StrongestRun(white, std::max(3, surface.width * 7 / 100), 4);
}

// Millisecond clock with sub-millisecond resolution. GetTickCount64 only
// advances in ~15.6 ms steps, so loop timing, velocities and the PWM phase
// computed from it were mostly 0 (clamped to 1 ms) with an occasional 16 ms
// jump - and the FPS shown in the HUD was wrong for the same reason.
double PreciseMs() {
    static const double ticksPerMs = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart) / 1000.0;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / ticksPerMs;
}

// Cheap fingerprint of the captured bar. The loop scans far faster than the
// game redraws (60-144 Hz), so most scans see an identical image; velocities
// must only be measured between frames that actually changed.
uint64_t FrameSignature(const CaptureSurface& surface) {
    uint64_t hash = 1469598103934665603ULL;
    const int total = surface.width * surface.height;
    for (int i = 0; i < total; i += 3) {
        hash ^= surface.pixels[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

// New bright pixels in the collect-message region compared to a snapshot
// taken just before T was pressed. The message's name can be any colour and
// the icon differs per item, but the text and icon are always much brighter
// than the deck/water behind them (text ~195-255 vs floor ~46-68), so
// "pixels that became bright" works for every item.
inline int Luma(uint32_t p) {
    int r, g, b;
    ReadRgb(p, r, g, b);
    return (r * 299 + g * 587 + b * 114) / 1000;
}

int CollectMessageScore(const CaptureSurface& surface, const std::vector<uint32_t>& baseline) {
    const int total = surface.width * surface.height;
    if (static_cast<int>(baseline.size()) != total) return 0;
    int score = 0;
    for (int i = 0; i < total; ++i) {
        const int now = Luma(surface.pixels[i]);
        if (now >= 170 && now - Luma(baseline[i]) >= 60) ++score;
    }
    return score;
}

// Rarity of a collected item, read from the thin coloured line along the top
// of the message's banner: red = Mythic, gold = Legendary, blue = Rare,
// grey = Common. The banner is translucent, so on screen that line is mixed
// with whatever is behind it (deck, water, sky, darker at night). The
// background is known from the pre-T snapshot, so it is subtracted out:
//   seen = a * line + (1 - a) * background  ->  line = (seen - (1 - a) * bg) / a
// a = 0.30 and the true line colours below were fitted on 69 labelled real
// collects (69/69 correct, also leave-one-out). A fixed deck/water split
// failed on a darker deck, which read as "water" and turned Rares into Mythic.
// IMPOSSIBLE (the rarest, e.g. Lost Shotgun) has a black line: measured
// (-2,-2,-5) after un-blending on the first one caught, 2026-09-29 09:32:39.
enum Rarity { kRarityImpossible = 0, kRarityMythic, kRarityLegendary, kRarityRare, kRarityCommon,
              kRarityCount };
const wchar_t* const kRarityNames[kRarityCount] = {L"Impossible", L"Mythic", L"Legendary", L"Rare", L"Common"};

constexpr double kBannerAlpha = 0.30;
const int kRarityLineColour[kRarityCount][3] = {
    {0, 0, 0},       // Impossible: black
    {115, -4, 2},    // Mythic: red
    {192, 152, 38},  // Legendary: gold
    {48, 137, 185},  // Rare: blue
    {160, 164, 145}, // Common: grey
};
// Real item banners score 18+ (median ~55 over 2746 captures). Things that
// are not item messages score lower: the character's lantern glare ~0 and
// the "Wall - Stop Climbing" prompt 11.7-12.6 (it was counted as Mythic/ORE).
constexpr double kMinBannerLineScore = 15.0;

// Median colour of one row, skipping the left quarter (item icon) and the
// white text. False if too few pixels remain.
bool RowMedian(const uint32_t* px, int w, int y, int out[3]) {
    std::vector<int> ch[3];
    for (int x = w / 4; x < w; ++x) {
        const uint32_t p = px[y * w + x];
        if (Luma(p) >= 170) continue;
        int r, g, b;
        ReadRgb(p, r, g, b);
        ch[0].push_back(r);
        ch[1].push_back(g);
        ch[2].push_back(b);
    }
    if (static_cast<int>(ch[0].size()) < w / 4) return false;
    for (int c = 0; c < 3; ++c) {
        auto mid = ch[c].begin() + ch[c].size() / 2;
        std::nth_element(ch[c].begin(), mid, ch[c].end());
        out[c] = *mid;
    }
    return true;
}

double ColourDistance(const int a[3], const int b[3]) {
    const double dr = a[0] - b[0], dg = a[1] - b[1], db = a[2] - b[2];
    return std::sqrt(dr * dr + dg * dg + db * db);
}

// How much row y stands out as a thin line: different in colour from the
// rows 2 above and below, which themselves look alike. Works for a bright
// grey line (Common) as well as a dark red one (Mythic).
double LineStrength(const uint32_t* px, int w, int h, int y, int colour[3]) {
    int up[3], down[3];
    if (!RowMedian(px, w, y, colour) || !RowMedian(px, w, std::max(0, y - 2), up) ||
        !RowMedian(px, w, std::min(h - 1, y + 2), down)) {
        return -1e9;
    }
    return (ColourDistance(colour, up) + ColourDistance(colour, down)) / 2.0 - ColourDistance(up, down) / 2.0;
}

// frame = the clearest capture of the message, baseline = same region just
// before T. Finds the banner's top line (a line that is new compared to the
// baseline), removes the background from its colour and returns the nearest
// rarity - or -1 if there is no banner line at all (not an item message).
int ClassifyRarity(const std::vector<uint32_t>& frame, const std::vector<uint32_t>& baseline, int w, int h) {
    if (w <= 0 || h <= 4 || static_cast<int>(frame.size()) != w * h ||
        static_cast<int>(baseline.size()) != w * h) {
        return -1;
    }
    double bestScore = -1e9;
    int bestY = -1;
    int bestColour[3] = {};
    for (int y = 2; y < h * 45 / 100; ++y) {
        int colour[3], unused[3];
        const double now = LineStrength(frame.data(), w, h, y, colour);
        const double before = LineStrength(baseline.data(), w, h, y, unused);
        const double score = now - std::max(0.0, before);
        if (score > bestScore) {
            bestScore = score;
            bestY = y;
            std::copy(colour, colour + 3, bestColour);
        }
    }
    int background[3];
    if (bestY < 0 || bestScore < kMinBannerLineScore ||
        !RowMedian(baseline.data(), w, bestY, background)) {
        return -1;
    }
    int line[3];
    for (int c = 0; c < 3; ++c) {
        line[c] = static_cast<int>(std::lround((bestColour[c] - (1.0 - kBannerAlpha) * background[c]) /
                                               kBannerAlpha));
    }
    int best = kRarityCommon;
    double bestDistance = 1e9;
    for (int rarity = 0; rarity < kRarityCount; ++rarity) {
        const double d = ColourDistance(line, kRarityLineColour[rarity]);
        if (d < bestDistance) {
            bestDistance = d;
            best = rarity;
        }
    }
    return best;
}

// ORE - the item everyone fishes for - is a Mythic with a very short name.
// Measured on real collects (1440p): "Ore" is 36 px wide, the shortest other
// item ("Coral") 56 px, other Mythics ("Lost Cape", "Lost Mask") ~101 px and
// "Refinement Ore" (Rare anyway) ~170 px. The limit sits in between and
// scales with the screen height, since Roblox scales its UI with the window.
int ItemNameWidth(const std::vector<uint32_t>& frame, int w, int h) {
    if (static_cast<int>(frame.size()) != w * h) return 0;
    int first = -1, last = -1;
    for (int x = w * 15 / 100; x < w; ++x) { // skip the item icon on the left
        for (int y = h * 20 / 100; y < h * 62 / 100; ++y) { // name line, above "x1"
            const uint32_t p = frame[y * w + x];
            int r, g, b;
            ReadRgb(p, r, g, b);
            if (Luma(p) >= 200 && std::max({r, g, b}) - std::min({r, g, b}) < 60) {
                if (first < 0) first = x;
                last = x;
                break;
            }
        }
    }
    return first < 0 ? 0 : last - first + 1;
}

bool IsOreMessage(const std::vector<uint32_t>& frame, int w, int h) {
    const double scale = GetSystemMetrics(SM_CYSCREEN) / 1440.0;
    const int width = ItemNameWidth(frame, w, h);
    return width >= 12 * scale && width <= 46 * scale;
}

// <exe dir>\logs (created if needed).
std::wstring LogsDir() {
    wchar_t modulePath[MAX_PATH];
    const DWORD len = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    std::wstring dir(modulePath, (len > 0 && len < MAX_PATH) ? len : 0);
    dir = dir.substr(0, dir.find_last_of(L"\\/") + 1) + L"logs";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

// Writes 32-bit top-down pixels as a .bmp (for the collect-message debug
// captures: the rarity colours over water need real samples to tune).
void SaveBmp(const std::wstring& path, const uint32_t* pixels, int w, int h) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return;
    BITMAPINFOHEADER bi{};
    bi.biSize = sizeof(bi);
    bi.biWidth = w;
    bi.biHeight = -h; // top-down, same order as CaptureSurface
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;
    bi.biSizeImage = static_cast<DWORD>(w) * h * 4;
    BITMAPFILEHEADER bf{};
    bf.bfType = 0x4D42; // "BM"
    bf.bfOffBits = sizeof(bf) + sizeof(bi);
    bf.bfSize = bf.bfOffBits + bi.biSizeImage;
    fwrite(&bf, sizeof(bf), 1, f);
    fwrite(&bi, sizeof(bi), 1, f);
    fwrite(pixels, 4, static_cast<size_t>(w) * h, f);
    fclose(f);
}

// One CSV per minigame (Setup tab -> Minigame log), for tuning the control.
struct MinigameLog {
    FILE* file = nullptr;
    double startMs = 0.0;

    void Open() {
        wchar_t modulePath[MAX_PATH];
        const DWORD len = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
        std::wstring dir(modulePath, (len > 0 && len < MAX_PATH) ? len : 0);
        dir = dir.substr(0, dir.find_last_of(L"\\/") + 1) + L"logs";
        CreateDirectoryW(dir.c_str(), nullptr);
        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t name[64];
        swprintf_s(name, L"\\minigame_%04u%02u%02u_%02u%02u%02u.csv",
                   st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        file = _wfopen((dir + name).c_str(), L"w");
        if (!file) return;
        setvbuf(file, nullptr, _IOFBF, 1 << 16);
        fprintf(file, "ms,exit,t_top,t_bottom,t_center,p_top,p_bottom,p_center,"
                      "t_vel,p_vel,error,duty,mouse\n");
        startMs = PreciseMs();
    }

    void Row(double nowMs, bool exitVisible, const Detection& t, const Detection& pl,
             float tVel, float pVel, float error, float duty, bool mouse) {
        if (!file) return;
        fprintf(file, "%.1f,%d,%d,%d,%.1f,%d,%d,%.1f,%.4f,%.4f,%.1f,%.2f,%d\n",
                nowMs - startMs, exitVisible ? 1 : 0,
                t.found ? t.top : -1, t.found ? t.bottom : -1, t.found ? t.center : -1.0f,
                pl.found ? pl.top : -1, pl.found ? pl.bottom : -1, pl.found ? pl.center : -1.0f,
                tVel, pVel, error, duty, mouse ? 1 : 0);
    }

    void Close(int frames, int inZone) {
        if (!file) return;
        fprintf(file, "# frames=%d in_zone=%d (%.1f%%) duration_ms=%.0f\n", frames, inZone,
                frames ? inZone * 100.0 / frames : 0.0, PreciseMs() - startMs);
        fclose(file);
        file = nullptr;
    }
};

// ---------------------------------------------------------------------------
// Discord notifications: the tracker only queues a job; a separate thread
// makes the screenshot PNG and sends it, so fishing never waits on it.
// ---------------------------------------------------------------------------
enum class DiscordEvent { Item, Ore, Failing, Test };
struct DiscordJob {
    DiscordEvent type = DiscordEvent::Test;
    int rarity = -1;
    int sessionId = 0;
    int catches = 0;
    int ore = 0;
    double runningMs = 0.0;
    std::wstring detail;             // e.g. why "failing to fish"
    std::vector<uint32_t> pixels;    // optional screenshot
    int w = 0, h = 0;
    bool itemCapture = false;        // small item-message capture (framed) vs full screen
};
std::mutex gDiscordMutex;
std::vector<DiscordJob> gDiscordQueue;
HANDLE gDiscordWake = nullptr;
std::mutex gDiscordUrlMutex;
std::wstring gDiscordUrl;
std::atomic<bool> gDiscordEnabled{false};
std::atomic<bool> gDiscordPing{false};
std::atomic<bool> gDiscordHideUrl{false};    // show the webhook link as dots
std::wstring gDiscordPingWho;               // "" / "everyone" = @everyone, digits = one user ID (gDiscordUrlMutex)
std::atomic<bool> gDiscordFailAlerts{true};
std::atomic<bool> gDiscordOre{true};
std::atomic<bool> gDiscordRarity[5] = {{true}, {true}, {true}, {false}, {false}}; // Impossible..Common
std::atomic<int> gDiscordNoCatchMin{10};
std::atomic<int> gDiscordLastStatus{0};      // 0 = nothing sent yet, HTTP status, -1 = failed
std::atomic<long long> gDiscordLastTime{0};

std::wstring GetDiscordUrl() {
    std::lock_guard<std::mutex> lock(gDiscordUrlMutex);
    return gDiscordUrl;
}

std::wstring GetDiscordPingWho() {
    std::lock_guard<std::mutex> lock(gDiscordUrlMutex);
    return gDiscordPingWho;
}

void EnqueueDiscord(DiscordJob&& job) {
    if (!gDiscordEnabled.load() || GetDiscordUrl().empty()) return;
    {
        std::lock_guard<std::mutex> lock(gDiscordMutex);
        if (gDiscordQueue.size() >= 8) return; // never pile up (e.g. no internet)
        gDiscordQueue.push_back(std::move(job));
    }
    if (gDiscordWake) SetEvent(gDiscordWake);
}

bool CaptureFullScreen(std::vector<uint32_t>& pixels, int& w, int& h) {
    w = GetSystemMetrics(SM_CXSCREEN);
    h = GetSystemMetrics(SM_CYSCREEN);
    CaptureSurface surface;
    if (!surface.Create(w, h) || !surface.Grab(0, 0)) return false;
    pixels.assign(surface.pixels, surface.pixels + static_cast<size_t>(w) * h);
    return true;
}

std::string ToUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &out[0], n, nullptr, nullptr);
    return out;
}

std::string JsonEscape(const std::string& in) {
    std::string out;
    for (const char c : in) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out;
}

bool PngEncoderClsid(CLSID& clsid) {
    UINT count = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&count, &size);
    if (size == 0) return false;
    std::vector<unsigned char> buffer(size);
    auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
    Gdiplus::GetImageEncoders(count, size, codecs);
    for (UINT i = 0; i < count; ++i) {
        if (wcscmp(codecs[i].MimeType, L"image/png") == 0) {
            clsid = codecs[i].Clsid;
            return true;
        }
    }
    return false;
}

// Screenshot -> PNG with the cloud logo in the top-left corner (below
// Roblox's own buttons). A small item-message capture gets a dark header
// band for the logo instead, so nothing of the message is covered.
bool MakeDiscordPng(const DiscordJob& job, std::vector<unsigned char>& png) {
    if (job.pixels.empty() || job.w <= 0 || job.h <= 0) return false;
    Gdiplus::Bitmap source(job.w, job.h, job.w * 4, PixelFormat32bppRGB,
                           reinterpret_cast<BYTE*>(const_cast<uint32_t*>(job.pixels.data())));
    const int scale = job.itemCapture ? 2 : 1;
    const int band = job.itemCapture ? 64 : 0;
    const bool ore = job.type == DiscordEvent::Ore;
    const int outW = job.w * scale, outH = job.h * scale + band;
    Gdiplus::Bitmap canvas(outW, outH, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics g(&canvas);
        g.SetInterpolationMode(job.itemCapture ? Gdiplus::InterpolationModeHighQualityBicubic
                                               : Gdiplus::InterpolationModeNearestNeighbor);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.Clear(ore ? Gdiplus::Color(255, 200, 28, 36) : Gdiplus::Color(255, 40, 41, 70)); // red band for ORE
        g.DrawImage(&source, Gdiplus::RectF(0, static_cast<float>(band), static_cast<float>(outW),
                                   static_cast<float>(job.h * scale)));
        if (gCloudIcon) {
            if (job.itemCapture) {
                g.DrawImage(gCloudIcon, Gdiplus::RectF(12, 8, 65, 48));
                Gdiplus::Font font(L"Segoe UI", 20.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
                Gdiplus::SolidBrush white(Gdiplus::Color(255, 240, 242, 255));
                g.DrawString(ore ? L"ORE caught!  \u00B7  Sky's S2 Fishing Macro" : L"Sky's S2 Fishing Macro", -1,
                             &font, Gdiplus::PointF(88, 18), &white);
            } else {
                const float cw = std::max(80.0f, outW * 0.06f);
                g.DrawImage(gCloudIcon, Gdiplus::RectF(outW * 0.01f, outH * 0.065f, cw, cw * 230.0f / 312.0f));
            }
        }
    }
    CLSID pngClsid;
    if (!PngEncoderClsid(pngClsid)) return false;
    IStream* stream = nullptr;
    if (CreateStreamOnHGlobal(nullptr, TRUE, &stream) != S_OK) return false;
    bool ok = canvas.Save(stream, &pngClsid, nullptr) == Gdiplus::Ok;
    if (ok) {
        HGLOBAL mem = nullptr;
        GetHGlobalFromStream(stream, &mem);
        STATSTG stat{};
        stream->Stat(&stat, STATFLAG_NONAME);
        const size_t bytes = static_cast<size_t>(stat.cbSize.QuadPart);
        const void* data = GlobalLock(mem);
        ok = data != nullptr;
        if (ok) png.assign(static_cast<const unsigned char*>(data), static_cast<const unsigned char*>(data) + bytes);
        GlobalUnlock(mem);
    }
    stream->Release();
    return ok;
}

// POSTs a webhook message (multipart when there is a picture). Returns the
// HTTP status, or -1 if it couldn't be sent.
int PostDiscord(const std::wstring& url, const std::string& json, const std::vector<unsigned char>& png) {
    wchar_t host[256] = {}, path[2048] = {};
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.lpszHostName = host;
    parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS) return -1;
    int status = -1;
    HINTERNET session = WinHttpOpen(L"SkysS2FishingMacro/3.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return -1;
    WinHttpSetTimeouts(session, 5000, 5000, 15000, 15000);
    HINTERNET connect = WinHttpConnect(session, host, parts.nPort, 0);
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"POST", path, nullptr, WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                                : nullptr;
    if (request) {
        const std::string boundary = "----SkysS2FishingMacroBoundary7d1f";
        std::string body = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"payload_json\"\r\n"
                           "Content-Type: application/json\r\n\r\n" + json + "\r\n";
        if (!png.empty()) {
            body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"files[0]\"; filename=\"shot.png\"\r\n"
                    "Content-Type: image/png\r\n\r\n";
            body.append(reinterpret_cast<const char*>(png.data()), png.size());
            body += "\r\n";
        }
        body += "--" + boundary + "--\r\n";
        const std::wstring header = L"Content-Type: multipart/form-data; boundary=" +
                                    std::wstring(boundary.begin(), boundary.end());
        if (WinHttpSendRequest(request, header.c_str(), static_cast<DWORD>(-1L), body.data(),
                               static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) &&
            WinHttpReceiveResponse(request, nullptr)) {
            DWORD code = 0, size = sizeof(code);
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX);
            status = static_cast<int>(code);
        }
        WinHttpCloseHandle(request);
    }
    if (connect) WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return status;
}

std::string DiscordJson(const DiscordJob& job, bool hasImage) {
    static const char* const rarityNames[5] = {"Impossible", "Mythic", "Legendary", "Rare", "Common"};
    static const int rarityColours[5] = {0x1B1B22, 0xF85252, 0xFABE32, 0x4696FF, 0xA5A8B9};
    std::string title, description;
    int colour = 0x2D6CFF;
    switch (job.type) {
        case DiscordEvent::Ore:
            // Red theme: an ORE must stand out immediately.
            title = "\xF0\x9F\x9F\xA5 ORE CAUGHT! \xE2\x9B\x8F\xEF\xB8\x8F";
            description = "\xF0\x9F\x94\xB4 Everyone's favourite drop just landed.";
            colour = 0xE02424;
            break;
        case DiscordEvent::Item:
            if (job.rarity >= 0 && job.rarity < 5) {
                title = std::string(rarityNames[job.rarity]) + " catch!";
                colour = rarityColours[job.rarity];
                if (job.rarity == 0) {
                    // Black theme for the rarest drop.
                    title = "\xE2\xAC\x9B IMPOSSIBLE CATCH! \xE2\xAC\x9B";
                    description = "\xF0\x9F\x96\xA4 The rarest drop there is.";
                    colour = 0x010101; // 0 would mean "no colour" to Discord
                }
            }
            break;
        case DiscordEvent::Failing:
            title = "\xE2\x9A\xA0\xEF\xB8\x8F Failing to fish - go check!";
            description = ToUtf8(job.detail);
            colour = 0xF59E0B;
            break;
        case DiscordEvent::Test:
            title = "\xE2\x9C\x85 Test message";
            description = "Discord notifications are working.";
            break;
    }
    char fields[512];
    const double hours = job.runningMs / 3600000.0;
    const long long minutes = static_cast<long long>(job.runningMs / 60000.0);
    snprintf(fields, sizeof(fields),
             "[{\"name\":\"Session\",\"value\":\"#%d\",\"inline\":true},"
             "{\"name\":\"Catches\",\"value\":\"%d\",\"inline\":true},"
             "{\"name\":\"ORE\",\"value\":\"%d (%.1f/h)\",\"inline\":true},"
             "{\"name\":\"Time fished\",\"value\":\"%lldh %02lldm\",\"inline\":true}]",
             job.sessionId, job.catches, job.ore, hours > 0.016 ? job.ore / hours : 0.0, minutes / 60, minutes % 60);
    // Ping: @everyone by default; digits = one user's ID (a real ping); any
    // other text is only shown, because webhooks can't ping by name.
    std::string content, mentions = "{\"parse\":[]}";
    if (gDiscordPing.load()) {
        std::string who = ToUtf8(GetDiscordPingWho());
        if (!who.empty() && who[0] == '@') who.erase(0, 1);
        const bool digits = !who.empty() && who.find_first_not_of("0123456789") == std::string::npos;
        if (who.empty() || who == "everyone") {
            content = "@everyone";
            mentions = "{\"parse\":[\"everyone\"]}";
        } else if (digits) {
            content = "<@" + who + ">";
            mentions = "{\"users\":[\"" + who + "\"]}";
        } else {
            content = "@" + JsonEscape(who);
        }
    }
    std::string json = "{\"username\":\"Sky's S2 Fishing Macro\",";
    json += "\"content\":\"" + content + "\",";
    json += "\"allowed_mentions\":" + mentions + ",";
    json += "\"embeds\":[{\"title\":\"" + JsonEscape(title) + "\",";
    if (!description.empty()) json += "\"description\":\"" + JsonEscape(description) + "\",";
    json += "\"color\":" + std::to_string(colour) + ",\"fields\":" + fields + ",";
    if (hasImage) json += "\"image\":{\"url\":\"attachment://shot.png\"},";
    json += "\"footer\":{\"text\":\"Sky's S2 Fishing Macro \xE2\x80\xA2 by Sky\"}}]}";
    return json;
}

DWORD WINAPI DiscordThread(void*) {
    while (!gQuit.load()) {
        WaitForSingleObject(gDiscordWake, 1000);
        for (;;) {
            DiscordJob job;
            {
                std::lock_guard<std::mutex> lock(gDiscordMutex);
                if (gDiscordQueue.empty()) break;
                job = std::move(gDiscordQueue.front());
                gDiscordQueue.erase(gDiscordQueue.begin());
            }
            if (gQuit.load()) return 0;
            std::vector<unsigned char> png;
            const bool hasImage = MakeDiscordPng(job, png);
            const int status = PostDiscord(GetDiscordUrl(), DiscordJson(job, hasImage), png);
            gDiscordLastStatus.store(status);
            gDiscordLastTime.store(static_cast<long long>(time(nullptr)));
            if (gWindow) PostMessage(gWindow, WM_TRACKER_UPDATE, 0, 0);
            Sleep(600); // stays well under Discord's webhook rate limit
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Walk back (auto reposition without the reset gamepass).
// The camera follows the character, so drift shows up as the whole scene
// shifting on screen. A "position anchor" (a static patch of the world,
// calibrated by the user) is found again with ZNCC on Sobel edges - edges
// don't change with day/night or shadows and ZNCC ignores brightness and
// contrast - and the measured shift is walked off with W/A/S/D using
// per-tap steps learned right after calibrating.
// ---------------------------------------------------------------------------
// px searched around the anchor spot. One 90 ms tap moved the view ~24 px in
// a live test, so the old 48 px was left behind after 2 taps while learning.
constexpr int kAnchorSearch = 96;
// Below this the anchor counts as not found. Measured: real matches 0.85-1.00
// (shifts, +-25% light, +-5% zoom), 5 deg rotated view 0.50, blocked 0.16.
constexpr double kAnchorMinScore = 0.6;
constexpr int kWalkTapMs = 90;             // one step = a 90 ms key tap
constexpr int kWalkOkPx = 5;               // close enough

struct WalkStep { float x = 0, y = 0; };   // scene shift in px per tap
std::mutex gAnchorMutex;
RECT gAnchorRect{0, 0, 0, 0};
std::vector<float> gAnchorEdges;           // edges of the anchor window at calibration
int gAnchorWinW = 0, gAnchorWinH = 0;      // anchor rect + kAnchorSearch on every side
WalkStep gWalkSteps[4];                    // W, A, S, D
bool gWalkLearned = false;
const WORD kWalkKeys[4] = {'W', 'A', 'S', 'D'};

RECT GetAnchorRect() {
    std::lock_guard<std::mutex> lock(gAnchorMutex);
    return gAnchorRect;
}

bool AnchorReady() {
    std::lock_guard<std::mutex> lock(gAnchorMutex);
    return !gAnchorEdges.empty();
}

bool WalkLearned() {
    std::lock_guard<std::mutex> lock(gAnchorMutex);
    return gWalkLearned && !gAnchorEdges.empty();
}

std::wstring AnchorImagePath() {
    wchar_t modulePath[MAX_PATH];
    const DWORD len = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    std::wstring dir(modulePath, (len > 0 && len < MAX_PATH) ? len : 0);
    return dir.substr(0, dir.find_last_of(L"\\/") + 1) + L"SkysS2FishingMacro_anchor.bmp";
}

bool LoadBmp32(const std::wstring& path, std::vector<uint32_t>& pixels, int& w, int& h) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    BITMAPFILEHEADER bf{};
    BITMAPINFOHEADER bi{};
    const bool ok = fread(&bf, sizeof(bf), 1, f) == 1 && fread(&bi, sizeof(bi), 1, f) == 1 &&
                    bf.bfType == 0x4D42 && bi.biBitCount == 32 && bi.biHeight < 0;
    if (ok) {
        w = bi.biWidth;
        h = -bi.biHeight;
        pixels.resize(static_cast<size_t>(w) * h);
        fseek(f, bf.bfOffBits, SEEK_SET);
        if (fread(pixels.data(), 4, pixels.size(), f) != pixels.size()) pixels.clear();
    }
    fclose(f);
    return ok && !pixels.empty();
}

// Sobel gradient magnitude of the grayscale image.
void EdgeMap(const uint32_t* px, int w, int h, std::vector<float>& out) {
    std::vector<float> g(static_cast<size_t>(w) * h);
    for (size_t i = 0; i < g.size(); ++i) g[i] = static_cast<float>(Luma(px[i]));
    out.assign(g.size(), 0.0f);
    for (int y = 1; y < h - 1; ++y) {
        for (int x = 1; x < w - 1; ++x) {
            const float* a = &g[(y - 1) * w + x];
            const float* b = &g[y * w + x];
            const float* c = &g[(y + 1) * w + x];
            const float gx = (a[1] + 2 * b[1] + c[1]) - (a[-1] + 2 * b[-1] + c[-1]);
            const float gy = (c[-1] + 2 * c[0] + c[1]) - (a[-1] + 2 * a[0] + a[1]);
            out[y * w + x] = std::fabs(gx) + std::fabs(gy);
        }
    }
    // Soften the edges (two 3x3 box passes): a slightly zoomed view after
    // drifting forward/back then still overlaps (sharp 1 px edges didn't at
    // +3% scale), and the coarse 4 px search step can't skip the peak.
    std::vector<float> tmp(out.size());
    for (int pass = 0; pass < 2; ++pass) {
        for (int y = 1; y < h - 1; ++y) {
            for (int x = 1; x < w - 1; ++x) {
                float sum = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    const float* row = &out[(y + dy) * w + x];
                    sum += row[-1] + row[0] + row[1];
                }
                tmp[y * w + x] = sum / 9.0f;
            }
        }
        out.swap(tmp);
    }
}

// Zero-mean normalized cross-correlation of the anchor (the centre of the
// reference window) against the current window shifted by (ox, oy),
// sampling every `step` pixels. -1..1, 1 = identical up to brightness/contrast.
double Zncc(const std::vector<float>& ref, const std::vector<float>& cur, int winW, int winH,
            int ox, int oy, int step) {
    const int tw = winW - 2 * kAnchorSearch, th = winH - 2 * kAnchorSearch;
    double st = 0, sv = 0, stt = 0, svv = 0, stv = 0;
    int n = 0;
    for (int y = 1; y < th - 1; y += step) {
        const float* t = &ref[(kAnchorSearch + y) * winW + kAnchorSearch];
        const float* v = &cur[(oy + y) * winW + ox];
        for (int x = 1; x < tw - 1; x += step) {
            st += t[x];
            sv += v[x];
            stt += t[x] * t[x];
            svv += v[x] * v[x];
            stv += t[x] * v[x];
            ++n;
        }
    }
    const double varT = n * stt - st * st, varV = n * svv - sv * sv;
    if (n == 0 || varT <= 1e-6 || varV <= 1e-6) return -1.0;
    return (n * stv - st * sv) / std::sqrt(varT * varV);
}

struct AnchorMatch {
    bool valid = false;
    int dx = 0, dy = 0;   // scene shift since calibration, in px
    double score = -1.0;
};

// Captures the anchor window now and finds the anchor in it: coarse (every
// 4 px, half the samples) over +-48 px, then +-3 px at full detail.
AnchorMatch MeasureAnchor() {
    AnchorMatch m;
    std::vector<float> ref;
    RECT r;
    int winW, winH;
    {
        std::lock_guard<std::mutex> lock(gAnchorMutex);
        if (gAnchorEdges.empty()) return m;
        ref = gAnchorEdges;
        r = gAnchorRect;
        winW = gAnchorWinW;
        winH = gAnchorWinH;
    }
    CaptureSurface surface;
    if (!surface.Create(winW, winH) || !surface.Grab(r.left - kAnchorSearch, r.top - kAnchorSearch)) return m;
    std::vector<float> cur;
    EdgeMap(surface.pixels, winW, winH, cur);
    int bestX = kAnchorSearch, bestY = kAnchorSearch;
    double best = -2.0;
    for (int oy = 0; oy <= 2 * kAnchorSearch; oy += 5) {
        for (int ox = 0; ox <= 2 * kAnchorSearch; ox += 5) {
            const double z = Zncc(ref, cur, winW, winH, ox, oy, 2);
            if (z > best) { best = z; bestX = ox; bestY = oy; }
        }
    }
    const int cx = bestX, cy = bestY;
    best = -2.0;
    for (int oy = std::max(0, cy - 3); oy <= std::min(2 * kAnchorSearch, cy + 3); ++oy) {
        for (int ox = std::max(0, cx - 3); ox <= std::min(2 * kAnchorSearch, cx + 3); ++ox) {
            const double z = Zncc(ref, cur, winW, winH, ox, oy, 1);
            if (z > best) { best = z; bestX = ox; bestY = oy; }
        }
    }
    m.valid = true;
    m.dx = bestX - kAnchorSearch;
    m.dy = bestY - kAnchorSearch;
    m.score = best;
    return m;
}

// Waits until the anchor window stops changing (animations, camera settling,
// render lag after a key tap): unchanged for `stableMs`, at most `maxMs`.
void WaitStableAnchor(int stableMs, int maxMs) {
    RECT r;
    int winW, winH;
    {
        std::lock_guard<std::mutex> lock(gAnchorMutex);
        r = gAnchorRect;
        winW = gAnchorWinW;
        winH = gAnchorWinH;
    }
    CaptureSurface surface;
    if (winW <= 0 || !surface.Create(winW, winH)) return;
    uint64_t last = 0;
    const ULONGLONG start = GetTickCount64();
    ULONGLONG sameSince = start;
    while (!gQuit.load() && GetTickCount64() - start < static_cast<ULONGLONG>(maxMs)) {
        if (surface.Grab(r.left - kAnchorSearch, r.top - kAnchorSearch)) {
            const uint64_t sig = FrameSignature(surface);
            if (sig != last) {
                last = sig;
                sameSince = GetTickCount64();
            } else if (GetTickCount64() - sameSince >= static_cast<ULONGLONG>(stableMs)) {
                return;
            }
        }
        Sleep(15);
    }
}

void TapWalkKey(int key, int count) {
    for (int i = 0; i < count && !gQuit.load(); ++i) {
        KeyEvent(true, kWalkKeys[key]);
        Sleep(kWalkTapMs);
        KeyEvent(false, kWalkKeys[key]);
        Sleep(60);
    }
}

// UI thread, right after the anchor box was dragged: store the reference.
// Why the last "learn steps" failed (shown on the Setup tab); UI reads it.
std::mutex gWalkStatusMutex;
std::wstring gWalkStatus;

void SetWalkStatus(const std::wstring& text) {
    std::lock_guard<std::mutex> lock(gWalkStatusMutex);
    gWalkStatus = text;
}

std::wstring GetWalkStatus() {
    std::lock_guard<std::mutex> lock(gWalkStatusMutex);
    return gWalkStatus;
}

constexpr int kMinAnchorW = 60, kMinAnchorH = 40; // smaller boxes don't match reliably

bool CaptureAnchorReference(const RECT& r) {
    if (r.right - r.left < kMinAnchorW || r.bottom - r.top < kMinAnchorH) {
        SetWalkStatus(L"box too small - drag at least 60x40 px");
        return false;
    }
    // In third person the camera looks at the character, so it sits around
    // the screen centre; an anchor there gets covered by the character, its
    // rod and the "Collect" prompt.
    const int cx = GetSystemMetrics(SM_CXSCREEN) / 2, cy = GetSystemMetrics(SM_CYSCREEN) / 2;
    const RECT character{cx - 160, cy - 260, cx + 160, cy + 200};
    RECT overlap;
    if (IntersectRect(&overlap, &r, &character)) {
        SetWalkStatus(L"too close to your character - pick a spot further away");
        return false;
    }
    const int winW = (r.right - r.left) + 2 * kAnchorSearch, winH = (r.bottom - r.top) + 2 * kAnchorSearch;
    CaptureSurface surface;
    if (!surface.Create(winW, winH) || !surface.Grab(r.left - kAnchorSearch, r.top - kAnchorSearch)) return false;
    std::vector<float> edges;
    EdgeMap(surface.pixels, winW, winH, edges);
    SaveBmp(AnchorImagePath(), surface.pixels, winW, winH);
    std::lock_guard<std::mutex> lock(gAnchorMutex);
    gAnchorRect = r;
    gAnchorEdges = std::move(edges);
    gAnchorWinW = winW;
    gAnchorWinH = winH;
    gWalkLearned = false;
    return true;
}

// Startup: load the saved reference image (the rect comes from the ini).
void LoadAnchorReference(const RECT& r) {
    std::vector<uint32_t> pixels;
    int w = 0, h = 0;
    if (!RectCalibrated(r) || !LoadBmp32(AnchorImagePath(), pixels, w, h)) return;
    if (w != (r.right - r.left) + 2 * kAnchorSearch || h != (r.bottom - r.top) + 2 * kAnchorSearch) return;
    std::vector<float> edges;
    EdgeMap(pixels.data(), w, h, edges);
    std::lock_guard<std::mutex> lock(gAnchorMutex);
    gAnchorRect = r;
    gAnchorEdges = std::move(edges);
    gAnchorWinW = w;
    gAnchorWinH = h;
}

// Walk-back log (Minigame log on): logs\walkback.csv, one line per step.
void WalkLog(const char* event, int round, const AnchorMatch& m, const char* action, const char* result) {
    if (!gMinigameLog.load()) return;
    FILE* f = _wfopen((LogsDir() + L"\\walkback.csv").c_str(), L"a");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    if (ftell(f) == 0) fprintf(f, "time,event,round,dx,dy,score,action,result\n");
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "%02u:%02u:%02u,%s,%d,%d,%d,%.2f,%s,%s\n", st.wHour, st.wMinute, st.wSecond, event, round,
            m.dx, m.dy, m.score, action, result);
    fclose(f);
}

// Learns how far one tap of W, A, S and D shifts the scene: 3 taps per key,
// waiting for a still frame after each, then the same taps back - and checks
// the result, because bad steps make it walk the wrong way:
//  - after walking back it must be (nearly) where it started,
//  - W/S and A/D must roughly cancel out (fails if the camera turns),
//  - every step must move the view visibly, and W and A must differ in direction.
// Nothing is saved unless all of that holds.
bool LearnWalkSteps() {
    if (!AnchorReady()) { SetWalkStatus(L"no anchor set"); return false; }
    if (!FocusGame()) { SetWalkStatus(L"Roblox not in front"); return false; }
    static const char* const keyNames[4] = {"W", "A", "S", "D"};
    WaitStableAnchor(300, 1500);
    WalkStep learned[4];
    for (int key = 0; key < 4; ++key) {
        const AnchorMatch start = MeasureAnchor();
        WalkLog("learn", key, start, keyNames[key], "start");
        if (!start.valid || start.score < kAnchorMinScore) {
            SetWalkStatus(L"anchor not found - pick a still spot");
            return false;
        }
        AnchorMatch before = start;
        float sx = 0, sy = 0;
        for (int i = 0; i < 2; ++i) {
            TapWalkKey(key, 1);
            WaitStableAnchor(150, 600);
            const AnchorMatch after = MeasureAnchor();
            WalkLog("learn", key, after, keyNames[key], "tap");
            if (!after.valid || after.score < kAnchorMinScore) {
                TapWalkKey((key + 2) % 4, i + 1);
                SetWalkStatus(L"anchor lost while stepping - bigger/other spot");
                return false;
            }
            sx += static_cast<float>(after.dx - before.dx);
            sy += static_cast<float>(after.dy - before.dy);
            before = after;
        }
        learned[key].x = sx / 2.0f;
        learned[key].y = sy / 2.0f;
        TapWalkKey((key + 2) % 4, 2); // W<->S, A<->D: walk back
        WaitStableAnchor(150, 600);
        const AnchorMatch back = MeasureAnchor();
        WalkLog("learn", key, back, keyNames[key], "back");
        const double residual = back.valid ? std::hypot(back.dx - start.dx, back.dy - start.dy) : 1e9;
        const double moved = 2.0 * std::hypot(learned[key].x, learned[key].y);
        if (back.score < kAnchorMinScore || residual > std::max(6.0, 0.35 * moved)) {
            SetWalkStatus(L"didn't end where it started - camera turning?");
            return false;
        }
    }
    auto len = [](const WalkStep& v) { return std::hypot(v.x, v.y); };
    for (int k = 0; k < 4; ++k) {
        if (len(learned[k]) < 2.0) { SetWalkStatus(L"steps too small - is the anchor far away?"); return false; }
    }
    for (int k = 0; k < 2; ++k) {             // W vs S, A vs D
        const WalkStep& a = learned[k];
        const WalkStep& b = learned[k + 2];
        if (std::hypot(a.x + b.x, a.y + b.y) > 0.5 * std::max(len(a), len(b))) {
            SetWalkStatus(k == 0 ? L"W and S don't cancel out - camera turning?"
                                 : L"A and D don't cancel out - camera turning?");
            return false;
        }
    }
    const double cosWA = (learned[0].x * learned[1].x + learned[0].y * learned[1].y) / (len(learned[0]) * len(learned[1]));
    if (std::fabs(cosWA) > 0.9) { SetWalkStatus(L"W and A move the same way - try again"); return false; }
    std::lock_guard<std::mutex> lock(gAnchorMutex);
    for (int k = 0; k < 4; ++k) gWalkSteps[k] = learned[k];
    gWalkLearned = true;
    SetWalkStatus(L"");
    return true;
}

struct WalkResult {
    bool lost = false;
    int offset = 0;        // px from the anchor spot at the end
    int taps = 0;
};

// Walks the drift off. The needed scene shift (-offset) is split over one
// forward/back and one left/right key by solving a 2x2 system with the
// learned steps (tries all four key pairs, keeps the one with positive
// amounts), taps 80% of it (inertia), lets it settle and measures again.
// Safety: drift per catch is small, so a big jump means something is in
// front of the anchor - don't walk at all; stop at once if a step makes it
// worse; at most 12 taps per catch.
constexpr double kMaxWalkOffset = 35.0;
constexpr int kMaxWalkTaps = 12;

WalkResult WalkBackToAnchor() {
    static const char* const keyNames[4] = {"W", "A", "S", "D"};
    WalkResult result;
    WalkStep steps[4];
    {
        std::lock_guard<std::mutex> lock(gAnchorMutex);
        for (int k = 0; k < 4; ++k) steps[k] = gWalkSteps[k];
    }
    WaitStableAnchor(300, 1500);       // let the catch/rod animation finish
    double prevDist = -1.0;
    for (int round = 0; round < 8; ++round) {
        const AnchorMatch m = MeasureAnchor();
        const double dist = m.valid ? std::hypot(m.dx, m.dy) : 1e9;
        result.offset = m.valid ? static_cast<int>(std::lround(dist)) : -1;
        if (!m.valid || m.score < kAnchorMinScore) {
            WalkLog("walk", round, m, "-", "anchor not found");
            result.lost = true;
            return result;
        }
        if (dist <= kWalkOkPx) {
            WalkLog("walk", round, m, "-", "ok");
            return result;
        }
        if (round == 0 && dist > kMaxWalkOffset) {
            WalkLog("walk", round, m, "-", "jump too big - not walking");
            result.lost = true;
            return result;
        }
        if (prevDist >= 0 && dist > prevDist * 1.15) {
            WalkLog("walk", round, m, "-", "got worse - stopped");
            result.lost = true;
            return result;
        }
        if (result.taps >= kMaxWalkTaps) {
            WalkLog("walk", round, m, "-", "tap limit");
            result.lost = true;
            return result;
        }
        if (!FocusGame()) return result; // not in front: try again after the next catch
        prevDist = dist;

        const double tx = -m.dx, ty = -m.dy;
        int bestKey1 = -1, bestKey2 = -1;
        double bestA = 0, bestB = 0;
        for (int fwd : {0, 2}) {           // W or S
            for (int side : {1, 3}) {      // A or D
                const double det = steps[fwd].x * steps[side].y - steps[fwd].y * steps[side].x;
                if (std::fabs(det) < 1e-3) continue;
                const double a = (tx * steps[side].y - ty * steps[side].x) / det;
                const double b = (steps[fwd].x * ty - steps[fwd].y * tx) / det;
                if (a >= -0.25 && b >= -0.25 && (bestKey1 < 0 || a + b < bestA + bestB)) {
                    bestKey1 = fwd; bestKey2 = side; bestA = std::max(0.0, a); bestB = std::max(0.0, b);
                }
            }
        }
        if (bestKey1 < 0) {
            WalkLog("walk", round, m, "-", "no key mix");
            result.lost = true;
            return result;
        }
        // Roblox characters start and stop almost instantly, so the distance
        // walked is ~proportional to how long the key is held: hold each key
        // for 80% of the needed amount (in ms) instead of whole 90 ms taps,
        // which were ~24 px each - too coarse to land within 5 px.
        auto holdMs = [](double steps) {
            const double ms = 0.8 * steps * kWalkTapMs;
            return ms < 12.0 ? 0 : static_cast<int>(std::lround(std::min(ms, 360.0)));
        };
        int msA = holdMs(bestA), msB = holdMs(bestB);
        if (msA == 0 && msB == 0) {        // tiny: nudge the bigger one
            if (bestA >= bestB) msA = 25; else msB = 25;
        }
        if (msA > 0 && msA < 25) msA = 25; // shorter presses may not register
        if (msB > 0 && msB < 25) msB = 25;
        char action[40];
        snprintf(action, sizeof(action), "%dms %s %dms %s", msA, keyNames[bestKey1], msB, keyNames[bestKey2]);
        WalkLog("walk", round, m, action, "walking");
        if (msA > 0) { KeyEvent(true, kWalkKeys[bestKey1]); Sleep(msA); KeyEvent(false, kWalkKeys[bestKey1]); Sleep(40); }
        if (msB > 0) { KeyEvent(true, kWalkKeys[bestKey2]); Sleep(msB); KeyEvent(false, kWalkKeys[bestKey2]); }
        result.taps += (msA + msB + kWalkTapMs - 1) / kWalkTapMs;
        WaitStableAnchor(150, 600);
    }
    result.lost = true;
    return result;
}

DWORD WINAPI TrackerThread(void*) {
    timeBeginPeriod(1);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    // Bar/exit regions are calibrated live (see the Calibration tab) rather
    // than assumed from any fixed reference resolution, so this works the
    // same regardless of the user's monitor/resolution/game window size.
    RECT bar = GetBarRect();
    RECT exitRect = GetExitRect();
    RECT collectRect = GetCollectRect();
    CaptureSurface barSurface;
    CaptureSurface exitSurface;
    CaptureSurface collectSurface;
    bool collectReady = RectCalibrated(collectRect) &&
        collectSurface.Create(collectRect.right - collectRect.left, collectRect.bottom - collectRect.top);
    bool barReady = RectCalibrated(bar) &&
        barSurface.Create(bar.right - bar.left, bar.bottom - bar.top);
    bool exitReady = RectCalibrated(exitRect) &&
        exitSurface.Create(exitRect.right - exitRect.left, exitRect.bottom - exitRect.top);

    bool mouseDown = false;
    bool hadPlayer = false;
    float previousPlayer = 0.0f;
    float playerVelocity = 0.0f;
    float previousTarget = 0.0f;
    float targetVelocity = 0.0f;
    double pwmPhaseMs = 0.0;
    double previousMs = PreciseMs();
    ULONGLONG lastUiTick = 0;
    double holdStartedMs = 0.0;
    // Frame-change tracking for velocity (see FrameSignature).
    uint64_t lastSignature = 0;
    double lastChangeMs = 0.0;
    // Per-minigame stats + optional CSV log.
    bool hookActive = false;
    int hookFrames = 0;
    int hookInZone = 0;
    MinigameLog minigameLog;
    int missingFrames = 0;
    float fpsAverage = 0.0f;

    auto setMouse = [&](bool down) {
        if (mouseDown != down) {
            MouseButton(down);
            mouseDown = down;
            if (down) holdStartedMs = PreciseMs();
        }
    };

    Phase phase = Phase::Cast;
    bool sawExitDuringHook = false;
    bool exitSeenWhileWaiting = false;
    ULONGLONG hookLastSeenAt = 0; // last time the exit bar or target zone was visible during this hook
    bool keyDown = false;
    ULONGLONG phaseChangedAt = GetTickCount64();
    // 0 means "no confirmed cast yet" - never blocks the very first cast of a run.
    ULONGLONG lastCastConfirmedAt = 0;
    int catchesSinceRespawn = 0;
    int missedCastsInARow = 0;
    int totalCatches = 0; // whole session, survives pausing
    bool wasRunning = false; // to press the equip keys once per Start
    // Item-collection check (see CollectMessageScore).
    int failedCollects = 0;
    int collectAttempt = 0;
    int lastCollectDetected = -1;
    std::vector<uint32_t> collectBaseline;
    bool collectSeen = false;
    double collectHitSinceMs = -1.0;
    // Collect timing (Minigame log): to tune collect delay / hold time from data.
    double minigameEndMs = 0.0;      // hook -> collect
    double firstTDownMs = 0.0;       // first T press of this collect
    double tDownMs = 0.0;            // T press of the current attempt
    double tUpMs = 0.0;              // T release of the current attempt
    double messageFirstMs = -1.0;    // first frame the message was visible (current attempt)
    int rarityCounts[kRarityCount] = {};
    int lastRarity = -1;
    int oreCount = 0;
    bool lastWasOre = false;
    double runningMs = 0.0;          // fishing time this session
    double lastLoopMs = PreciseMs();
    // "Failing to fish" alert: 3 failed tries in a row, or no catch for N
    // minutes of fishing. Sent once, then armed again by the next catch.
    int failStreak = 0;
    double runningAtLastCatch = 0.0;
    bool failAlertSent = false;
    // Walk back
    bool positionLost = false;
    int positionOffset = -1;
    int lostStreak = 0;
    int attemptRarity = kRarityCommon; // best classification during this attempt
    int attemptBestScore = -1;
    std::vector<uint32_t> attemptRarityFrame; // clearest frame of the message (rarity is read from it)
    // Snapshot of the collect-message region right before T goes down.
    auto takeCollectBaseline = [&]() {
        collectSeen = false;
        collectHitSinceMs = -1.0;
        attemptRarity = kRarityCommon;
        attemptBestScore = -1;
        attemptRarityFrame.clear();
        collectBaseline.clear();
        if (collectReady && collectSurface.Grab(collectRect.left, collectRect.top)) {
            collectBaseline.assign(collectSurface.pixels,
                                   collectSurface.pixels + collectSurface.width * collectSurface.height);
        }
    };
    // Looks for the item message; sets collectSeen once it has been visible
    // for kCollectConfirmMs.
    auto checkCollectMessage = [&](double nowMs) {
        // Keeps watching after the message is confirmed (T is held for the
        // full time anyway) so the rarity is read from its clearest frame.
        if (collectBaseline.empty()) return;
        if (!collectSurface.Grab(collectRect.left, collectRect.top)) return;
        const int area = collectSurface.width * collectSurface.height;
        const int needed = std::max(20, area / 50); // 2% of the region
        const int score = CollectMessageScore(collectSurface, collectBaseline);
        if (score >= needed) {
            if (collectHitSinceMs < 0.0) collectHitSinceMs = nowMs;
            if (messageFirstMs < 0.0) messageFirstMs = nowMs;
            if (nowMs - collectHitSinceMs >= kCollectConfirmMs) collectSeen = true;
            // The message fades in and out; the rarity line is clearest on the
            // frame with the most (fully opaque) text.
            if (score > attemptBestScore) {
                attemptBestScore = score;
                attemptRarityFrame.assign(collectSurface.pixels, collectSurface.pixels + area);
            }
        } else if (!collectSeen) {
            collectHitSinceMs = -1.0;
        }
    };
    // Minigame log on: saves the pre-T snapshot and a frame of the collect
    // region as logs\collect_<time>_<tag>_before/_message.bmp.
    auto saveCollectCapture = [&](const wchar_t* tag, const std::vector<uint32_t>& frame) {
        const int w = collectSurface.width, h = collectSurface.height;
        if (!gMinigameLog.load() || static_cast<int>(frame.size()) != w * h ||
            static_cast<int>(collectBaseline.size()) != w * h) {
            return;
        }
        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t name[128];
        swprintf_s(name, L"\\collect_%04u%02u%02u_%02u%02u%02u_%s",
                   st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, tag);
        const std::wstring base = LogsDir() + name;
        SaveBmp(base + L"_before.bmp", collectBaseline.data(), w, h);
        SaveBmp(base + L"_message.bmp", frame.data(), w, h);
    };


    while (!gQuit.load()) {
        const ULONGLONG now = GetTickCount64(); // coarse; fine for the phase timers
        const double nowMs = PreciseMs();
        const double dtMs = std::clamp(nowMs - previousMs, 0.05, 30.0);
        previousMs = nowMs;
        const float instantFps = static_cast<float>(1000.0 / dtMs);
        fpsAverage += (instantFps - fpsAverage) * 0.08f;

        // Pick up any (re)calibration of the bar/exit regions - recreate the
        // capture surface only when the size actually changed, since that's
        // the only part CaptureSurface::Create needs to redo; the position
        // is read fresh from `bar`/`exitRect` every Grab() call already.
        {
            const RECT freshBar = GetBarRect();
            if (freshBar.right - freshBar.left != bar.right - bar.left ||
                freshBar.bottom - freshBar.top != bar.bottom - bar.top) {
                barReady = RectCalibrated(freshBar) &&
                    barSurface.Create(freshBar.right - freshBar.left, freshBar.bottom - freshBar.top);
            } else {
                barReady = RectCalibrated(freshBar);
            }
            bar = freshBar;

            const RECT freshExit = GetExitRect();
            if (freshExit.right - freshExit.left != exitRect.right - exitRect.left ||
                freshExit.bottom - freshExit.top != exitRect.bottom - exitRect.top) {
                exitReady = RectCalibrated(freshExit) &&
                    exitSurface.Create(freshExit.right - freshExit.left, freshExit.bottom - freshExit.top);
            } else {
                exitReady = RectCalibrated(freshExit);
            }
            exitRect = freshExit;

            const RECT freshCollect = GetCollectRect();
            if (freshCollect.right - freshCollect.left != collectRect.right - collectRect.left ||
                freshCollect.bottom - freshCollect.top != collectRect.bottom - collectRect.top) {
                collectReady = RectCalibrated(freshCollect) &&
                    collectSurface.Create(freshCollect.right - freshCollect.left,
                                          freshCollect.bottom - freshCollect.top);
            } else {
                collectReady = RectCalibrated(freshCollect);
            }
            collectRect = freshCollect;
        }
        const bool captureReady = barReady && exitReady;

        // Continue a previous session: take over its numbers.
        {
            std::lock_guard<std::mutex> lock(gRestoreMutex);
            if (gRestorePending) {
                gRestorePending = false;
                totalCatches = gRestoreData.catches;
                failedCollects = gRestoreData.noItem;
                oreCount = gRestoreData.ore;
                for (int i = 0; i < kRarityCount; ++i) rarityCounts[i] = gRestoreData.rarity[i];
                runningMs = static_cast<double>(gRestoreData.runningMs);
                lastRarity = -1;
                lastWasOre = false;
                lastCollectDetected = -1;
                failStreak = 0;
                runningAtLastCatch = runningMs;
                failAlertSent = false;
            }
        }
        // Real elapsed time (not the clamped dtMs): casting/collecting can
        // block this loop for a second or more.
        if (gEnabled.load() && !gBaitRunning.load()) runningMs += nowMs - lastLoopMs;
        lastLoopMs = nowMs;
        if (gEnabled.load() && !failAlertSent && gDiscordFailAlerts.load() && gDiscordEnabled.load()) {
            const double noCatchMs = gDiscordNoCatchMin.load() * 60000.0;
            const bool streak = failStreak >= 3;
            if (streak || (noCatchMs > 0 && runningMs - runningAtLastCatch >= noCatchMs)) {
                failAlertSent = true;
                DiscordJob job;
                job.type = DiscordEvent::Failing;
                wchar_t why[128];
                if (streak) swprintf_s(why, L"%d tries in a row without a catch.", failStreak);
                else swprintf_s(why, L"No catch for %d minutes of fishing.", gDiscordNoCatchMin.load());
                job.detail = why;
                job.sessionId = gCurrentSessionId;
                job.catches = totalCatches;
                job.ore = oreCount;
                job.runningMs = runningMs;
                CaptureFullScreen(job.pixels, job.w, job.h);
                EnqueueDiscord(std::move(job));
            }
        }

        // Learn the W/A/S/D steps (asked by the Setup tab, only while paused).
        if (gLearnWalkRequested.exchange(false) && !gEnabled.load()) {
            {
                Telemetry busy;
                {
                    std::lock_guard<std::mutex> lock(gTelemetryMutex);
                    busy = gTelemetry;
                }
                busy.learningWalk = true;
                std::lock_guard<std::mutex> lock(gTelemetryMutex);
                gTelemetry = busy;
            }
            PostMessage(gWindow, WM_TRACKER_UPDATE, 0, 0);
            LearnWalkSteps();
            gLearnWalkRequested.store(false);
            PostMessage(gWindow, WM_CALIBRATION_DONE + 100, 0, 0); // save the steps (UI thread)
        }

        Telemetry current{};
        current.enabled = gEnabled.load();
        current.runningMs = runningMs;
        current.positionLost = positionLost;
        current.positionOffset = positionOffset;
        current.fps = fpsAverage;
        current.captureReady = captureReady;
        current.totalCatches = totalCatches;
        current.catchesSinceRespawn = catchesSinceRespawn; // also shown while paused
        current.failedCollects = failedCollects;
        current.collectAttempt = collectAttempt;
        current.lastCollectDetected = lastCollectDetected;
        current.collectCheckReady = collectReady;
        std::copy(rarityCounts, rarityCounts + kRarityCount, current.rarityCounts);
        current.lastRarity = lastRarity;
        current.oreCount = oreCount;
        current.lastWasOre = lastWasOre;
        current.state =current.enabled ? ControlState::Waiting : ControlState::Paused;

        if (!current.enabled || !captureReady) {
            // Disabled (or capture unavailable): release everything and reset
            // the cycle so the next Start always begins from a fresh cast.
            setMouse(false);
            if (keyDown) {
                KeyEvent(false, kHoldKeyVk);
                keyDown = false;
            }
            phase = Phase::Cast;
            sawExitDuringHook = false;
            exitSeenWhileWaiting = false;
            hookLastSeenAt = now;
            missingFrames = 0;
            hadPlayer = false;
            playerVelocity = targetVelocity = 0.0f;
            pwmPhaseMs = 0.0;
            phaseChangedAt = now;
            lastCastConfirmedAt = 0; // a fresh Start shouldn't inherit a cooldown from before the pause
            // catchesSinceRespawn is deliberately kept: the drift from those
            // catches is still there after a pause, so the reset stays due.
            missedCastsInARow = 0;
            wasRunning = false;
            if (hookActive) {
                minigameLog.Close(hookFrames, hookInZone);
                hookActive = false;
            }
            current.phase = phase;
            current.mouseDown = mouseDown;
            {
                std::lock_guard<std::mutex> lock(gTelemetryMutex);
                gTelemetry = current;
            }
            if (now - lastUiTick >= kUiRefreshIntervalMs) {
                lastUiTick = now;
                PostMessage(gWindow, WM_TRACKER_UPDATE, 0, 0);
            }
            Sleep(2);
            continue;
        }

        if (!wasRunning) {
            wasRunning = true;
            EquipRodOnStart();
            previousMs = PreciseMs();
        }

        current.phase = phase;
        current.catchesSinceRespawn = catchesSinceRespawn;

        if (phase == Phase::WaitForFish) {
            // Cast -> WaitForFish -> Hook. No second cast is possible here.
            // The configurable timer is a minimum safety window: we can see
            // the red Exit button while waiting, but we do not hand control
            // to the reel/bar logic until that window has elapsed.
            setMouse(false);
            const ULONGLONG waitMs = gWaitForFishMs.load();
            const ULONGLONG elapsed = now - phaseChangedAt;
            current.phaseElapsedMs = static_cast<float>(elapsed);

            const bool exitVisible = ExitVisible(exitSurface, exitRect.left, exitRect.top);
            if (exitVisible) exitSeenWhileWaiting = true;

            current.waitForFishRemainingMs = elapsed < waitMs
                ? static_cast<float>(waitMs - elapsed) : 0.0f;

            if (exitSeenWhileWaiting) {
                // The red Exit button is the real signal that the fishing
                // minigame has started. The configurable timer is ONLY a
                // safety net to prevent an immediate second cast; it must not
                // add unnecessary delay once the minigame is actually ready.
                // As soon as Exit is detected, end WaitForFish immediately.
                ParkCursorOnCastPoint(GetCastPoint());
                missedCastsInARow = 0;
                phase = Phase::Hook;
                current.phase = phase;
                sawExitDuringHook = true;
                hookLastSeenAt = now;
                missingFrames = 0;
                hadPlayer = false;
                playerVelocity = targetVelocity = 0.0f;
                pwmPhaseMs = 0.0;
                phaseChangedAt = now;
                lastCastConfirmedAt = now;
            } else if (elapsed >= waitMs) {
                // The safety window ran out and the exit bar never showed up
                // at all - the cast click almost certainly didn't register.
                // Go back and retry it rather than sit here forever waiting
                // on a minigame that never actually started.
                setMouse(false);
                exitSeenWhileWaiting = false;
                phase = Phase::Cast;
                ++missedCastsInARow;
                ++failStreak;
                if (gAutoRespawn.load() && missedCastsInARow >= kMissedCastsBeforeRespawn &&
                    ResetCharacter()) {
                    missedCastsInARow = 0;
                    catchesSinceRespawn = 0;
                    phase = Phase::Respawn;
                    lastCastConfirmedAt = 0;
                }
                current.phase = phase;
                phaseChangedAt = GetTickCount64();
            }
        } else if (phase == Phase::Cast) {
            const ULONGLONG sinceConfirmed = now - lastCastConfirmedAt;
            if (lastCastConfirmedAt != 0 && sinceConfirmed < kPostCastConfirmCooldownMs) {
                // Failsafe: a click was already confirmed successful recently
                // (the exit bar actually appeared, so the reel really did go
                // out) - whatever brought us back to Phase::Cast this soon
                // after that, hold off on firing another click rather than
                // risk a double-cast that starts/ends/restarts the minigame.
                // Just wait it out; nothing else to do here.
                setMouse(false);
                current.castCooldownRemainingMs =
                    static_cast<float>(kPostCastConfirmCooldownMs - sinceConfirmed);
            } else {
                // Guide the cursor to the point, dwell there, then click firmly -
                // the exit bar appearing (handled in the Hook phase below)
                // confirms the minigame actually started.
                setMouse(false);
                CastClick(GetCastPoint());
                // Casting is a one-way transition into WaitForFish. No
                // additional click is permitted until the red Exit button
                // proves that the fish/minigame has started.
                phase = Phase::WaitForFish;
                current.phase = phase;
                sawExitDuringHook = false;
                exitSeenWhileWaiting = false;
                hookLastSeenAt = now;
                missingFrames = 0;
                hadPlayer = false;
                playerVelocity = targetVelocity = 0.0f;
                pwmPhaseMs = 0.0;
                phaseChangedAt = now;
            }
        } else if (phase == Phase::Hook) {
            const bool exitVisible = ExitVisible(exitSurface, exitRect.left, exitRect.top);
            Detection target{}, player{};
            bool frameChanged = false;
            if (barSurface.Grab(bar.left, bar.top)) {
                FindBarObjects(barSurface, target, player);
                const uint64_t signature = FrameSignature(barSurface);
                frameChanged = signature != lastSignature;
                lastSignature = signature;
            }
            if (!hookActive) {
                hookActive = true;
                hookFrames = hookInZone = 0;
                if (gMinigameLog.load()) minigameLog.Open();
            }
            if (exitVisible) {
                if (!sawExitDuringHook) {
                    // First sighting of the exit bar this cycle: the click is
                    // now actually confirmed successful (the reel is out in
                    // the water), which starts the post-cast cooldown above.
                    lastCastConfirmedAt = now;
                }
                sawExitDuringHook = true;
                hookLastSeenAt = now;
            }
            // Green lily pads in the water can pass as the target zone while no
            // minigame is open, so the bar only counts once the white player
            // block has been seen alongside it.
            if (exitVisible || (target.found && player.found)) hookLastSeenAt = now;

            current.targetFound = target.found;
            current.playerFound = player.found;
            current.minigame = exitVisible && target.found;
            current.targetY = target.center;
            current.playerY = player.center;
            current.confidence = target.found
                ? std::clamp(target.score * 100 /
                             std::max(1, barSurface.width * (target.bottom - target.top + 1)), 0, 100)
                : 0;

            if (!exitVisible || !target.found || (!player.found && !hadPlayer)) {
                setMouse(false);
                missingFrames = 0;
                hadPlayer = false;
                playerVelocity = targetVelocity = 0.0f;
                pwmPhaseMs = 0.0;
            } else if (!player.found) {
                // Never continue a blind hold. A few missing frames are tolerated,
                // then recovery alternates short nudges rather than sticking at an edge.
                ++missingFrames;
                if (missingFrames <= 2) {
                    setMouse(mouseDown);
                } else {
                    const bool recoveryPulse = (static_cast<ULONGLONG>(nowMs / 18.0) % 3) != 2;
                    setMouse(recoveryPulse);
                    current.state = recoveryPulse ? ControlState::Holding : ControlState::Releasing;
                }
            } else {
                missingFrames = 0;
                // Velocities (px/ms) are only measured between frames the game
                // actually redrew, over the real time between them.
                if (!hadPlayer) {
                    previousPlayer = player.center;
                    previousTarget = target.center;
                    lastChangeMs = nowMs;
                    playerVelocity = targetVelocity = 0.0f;
                    hadPlayer = true;
                } else if (frameChanged) {
                    const float frameDt = static_cast<float>(std::clamp(nowMs - lastChangeMs, 4.0, 100.0));
                    const float rawPlayerVelocity = (player.center - previousPlayer) / frameDt;
                    const float rawTargetVelocity = (target.center - previousTarget) / frameDt;
                    playerVelocity += (rawPlayerVelocity - playerVelocity) * 0.5f;
                    targetVelocity += (rawTargetVelocity - targetVelocity) * 0.4f;
                    previousPlayer = player.center;
                    previousTarget = target.center;
                    lastChangeMs = nowMs;
                }

                // Look ahead roughly the click -> game -> screen latency (a few
                // frames), for both the zone and the block, so the block brakes
                // before it overshoots instead of reacting after the fact.
                constexpr float kPredictMs = 50.0f;
                const float desired = std::clamp(target.center + targetVelocity * kPredictMs,
                                                 static_cast<float>(target.top + 2),
                                                 static_cast<float>(target.bottom - 2));
                const float predictedPlayer = player.center + playerVelocity * kPredictMs;
                const float error = predictedPlayer - desired; // positive = block will be below target
                const float relativeVelocity = playerVelocity - targetVelocity;
                current.error = error;
                const float targetHalf = std::max(6.0f, (target.bottom - target.top + 1) * 0.5f);
                const float safeHalf = std::max(3.0f, targetHalf -
                    std::max(3.0f, (player.bottom - player.top + 1) * 0.45f));
                float duty = -1.0f; // -1 = not in the PWM band (logged)
                const bool nearTop = player.top <= 3;
                const bool nearBottom = player.bottom >= barSurface.height - 4;

                bool commandDown = false;
                if (nearTop) {
                    commandDown = false;
                    current.state = ControlState::Releasing;
                } else if (nearBottom) {
                    commandDown = true;
                    current.state = ControlState::Holding;
                } else if (error > safeHalf * 0.80f) {
                    commandDown = true;
                    current.state = ControlState::Holding;
                } else if (error < -safeHalf * 0.80f) {
                    commandDown = false;
                    current.state = ControlState::Releasing;
                } else {
                    // Binary PID converted to 18 ms PWM around a 0.52 hover duty.
                    // Gains are relative to the zone / bar size instead of raw
                    // pixels: 0.07 per pixel saturated after ~5 px on a 500 px
                    // bar, so this was effectively bang-bang, and its strength
                    // depended on the screen resolution.
                    const float errorNorm = error / safeHalf; // -0.8 .. 0.8 here
                    const float velocityNorm = relativeVelocity * 1000.0f /
                        static_cast<float>(std::max(1, barSurface.height)); // bar heights per second
                    duty = 0.52f + 0.30f * errorNorm + 0.06f * velocityNorm;
                    duty = std::clamp(duty, 0.08f, 0.92f);
                    constexpr double periodMs = 18.0;
                    pwmPhaseMs = std::fmod(pwmPhaseMs + dtMs, periodMs);
                    commandDown = pwmPhaseMs < periodMs * duty;
                    current.state = ControlState::Floating;
                }

                // Safety release: long rises get a tiny braking gap, preventing a
                // lost frame from pinning the block at the top.
                if (commandDown && mouseDown && nowMs - holdStartedMs >= 135.0) {
                    commandDown = false;
                    pwmPhaseMs = 16.0;
                }
                setMouse(commandDown);

                if (frameChanged) {
                    ++hookFrames;
                    if (player.center >= target.top && player.center <= target.bottom) ++hookInZone;
                    minigameLog.Row(nowMs, exitVisible, target, player, targetVelocity,
                                    playerVelocity, error, duty, commandDown);
                }
            }
            current.zonePct = hookFrames ? hookInZone * 100 / hookFrames : -1;

            if (sawExitDuringHook && now - hookLastSeenAt >= kHookGoneDebounceMs) {
                // The exit bar has reliably disappeared (not just one flickered
                // frame) - that marks the end of hooking; wait out the collect
                // delay before the hold-key press.
                setMouse(false);
                minigameLog.Close(hookFrames, hookInZone);
                hookActive = false;
                collectAttempt = 0;
                minigameEndMs = nowMs;
                phase = Phase::Collect;
                current.phase = phase;
                phaseChangedAt = now;
            }
        } else if (phase == Phase::Collect) {
            setMouse(false);
            current.phaseElapsedMs = static_cast<float>(now - phaseChangedAt);
            if (now - phaseChangedAt >= gCollectDelayMs.load()) {
                takeCollectBaseline();
                phase = Phase::HoldKey;
                current.phase = phase;
                phaseChangedAt = now;
                if (FocusGame()) {
                    KeyEvent(true, kHoldKeyVk);
                    keyDown = true;
                    tDownMs = firstTDownMs = PreciseMs();
                    messageFirstMs = -1.0;
                }
            }
        } else if (phase == Phase::HoldKey || phase == Phase::VerifyCollect) {
            current.phaseElapsedMs = static_cast<float>(now - phaseChangedAt);
            checkCollectMessage(nowMs);
            const bool checking = !collectBaseline.empty();

            // null = still busy; true/false = this minigame's collect result.
            int outcome = -1;
            if (phase == Phase::HoldKey && (collectSeen || now - phaseChangedAt >= gHoldKeyMs.load())) {
                // Release T as soon as the item message is confirmed: the
                // message means the pickup is done (it shows 2.20-2.33 s after
                // pressing T). Hold T time is the maximum when no message comes.
                if (keyDown) KeyEvent(false, kHoldKeyVk);
                keyDown = false;
                tUpMs = nowMs;
                if (!checking) outcome = 1;          // no collect region: old behaviour
                else if (collectSeen) outcome = 1;
                else {
                    phase = Phase::VerifyCollect;    // give the message a moment to appear
                    current.phase = phase;
                    phaseChangedAt = now;
                }
            } else if (phase == Phase::VerifyCollect) {
                if (collectSeen) {
                    outcome = 1;
                } else if (now - phaseChangedAt >= kCollectVerifyMs) {
                    // Keep a picture of every attempt without a message, so a
                    // missed item (e.g. an ORE) can be checked afterwards.
                    if (gMinigameLog.load() && collectSurface.Grab(collectRect.left, collectRect.top)) {
                        const std::vector<uint32_t> last(collectSurface.pixels,
                            collectSurface.pixels + collectSurface.width * collectSurface.height);
                        wchar_t tag[32];
                        swprintf_s(tag, L"NOITEM_try%d", collectAttempt + 1);
                        saveCollectCapture(tag, last);
                    }
                    if (collectAttempt < kCollectRetries) {
                        // The swinging fish probably cancelled the pickup -
                        // it is usually still hanging there, so hold T again.
                        ++collectAttempt;
                        takeCollectBaseline();
                        phase = Phase::HoldKey;
                        current.phase = phase;
                        phaseChangedAt = now;
                        if (FocusGame()) {
                            KeyEvent(true, kHoldKeyVk);
                            keyDown = true;
                            tDownMs = PreciseMs();
                            messageFirstMs = -1.0;
                        }
                    } else {
                        outcome = 0;
                    }
                }
            }

            if (outcome >= 0) {
                // Only a detected item message counts as a catch; the
                // reposition counter counts every minigame, since the drift
                // happens either way.
                if (outcome == 1 && checking) {
                    attemptRarity = ClassifyRarity(attemptRarityFrame, collectBaseline,
                                                   collectSurface.width, collectSurface.height);
                    if (attemptRarity < 0) {
                        // Bright pixels but no banner line: glare (e.g. the
                        // character's lantern), not an item message.
                        saveCollectCapture(L"NOITEM_glare", attemptRarityFrame);
                        outcome = 0;
                    }
                }
                if (outcome == 1) {
                    ++totalCatches;
                    if (checking) {
                        lastRarity = attemptRarity;
                        ++rarityCounts[attemptRarity];
                        lastWasOre = attemptRarity == kRarityMythic &&
                            IsOreMessage(attemptRarityFrame, collectSurface.width, collectSurface.height);
                        if (lastWasOre) ++oreCount;
                        saveCollectCapture(lastWasOre ? L"ORE" : kRarityNames[attemptRarity],
                                           attemptRarityFrame);
                        // Discord: ORE with its item-message capture, Impossible
                        // with a full screenshot, other ticked rarities as text.
                        const bool oreMessage = lastWasOre && gDiscordOre.load();
                        if (oreMessage || gDiscordRarity[attemptRarity].load()) {
                            DiscordJob job;
                            job.type = oreMessage ? DiscordEvent::Ore : DiscordEvent::Item;
                            job.rarity = attemptRarity;
                            job.sessionId = gCurrentSessionId;
                            job.catches = totalCatches;
                            job.ore = oreCount;
                            job.runningMs = runningMs;
                            if (oreMessage) {
                                job.pixels = attemptRarityFrame;
                                job.w = collectSurface.width;
                                job.h = collectSurface.height;
                                job.itemCapture = true;
                            } else if (attemptRarity == kRarityImpossible) {
                                CaptureFullScreen(job.pixels, job.w, job.h);
                            }
                            EnqueueDiscord(std::move(job));
                        }
                    }
                    failStreak = 0;
                    runningAtLastCatch = runningMs;
                    failAlertSent = false;
                }
                else {
                    ++failedCollects;
                    ++failStreak;
                    lastWasOre = false;
                }
                if (checking && gMinigameLog.load()) {
                    // One line per collect: when the item message showed up
                    // relative to the minigame end and to pressing/releasing T.
                    const std::wstring path = LogsDir() + L"\\collect_timing.csv";
                    FILE* f = _wfopen(path.c_str(), L"a");
                    if (f) {
                        fseek(f, 0, SEEK_END);
                        if (ftell(f) == 0) {
                            fprintf(f, "time,outcome,attempts,rarity,collect_delay_ms,hold_ms,"
                                       "tdown_after_minigame_ms,msg_after_tdown_ms,msg_after_tup_ms,"
                                       "collect_total_ms\n");
                        }
                        SYSTEMTIME st;
                        GetLocalTime(&st);
                        const bool seen = messageFirstMs >= 0.0;
                        fprintf(f, "%02u:%02u:%02u,%s,%d,%ls,%llu,%llu,%.0f,%.0f,%.0f,%.0f\n",
                                st.wHour, st.wMinute, st.wSecond, outcome == 1 ? "item" : "none",
                                collectAttempt + 1, outcome == 1 ? kRarityNames[attemptRarity] : L"-",
                                gCollectDelayMs.load(), gHoldKeyMs.load(), firstTDownMs - minigameEndMs,
                                seen ? messageFirstMs - tDownMs : -1.0, seen ? messageFirstMs - tUpMs : -1.0,
                                nowMs - minigameEndMs);
                        fclose(f);
                    }
                }
                if (checking) lastCollectDetected = outcome;
                ++catchesSinceRespawn;
                // Only after a real catch: after a failed collect the fish may
                // still be lying there, and walking would lose it.
                phase = (gRepositionMode.load() == 2 && WalkLearned() && outcome == 1) ? Phase::Reposition
                                                                                      : Phase::Cast;
                if (gAutoRespawn.load() &&
                    catchesSinceRespawn >= gRespawnEveryCatches.load() &&
                    ResetCharacter()) {
                    // (If the game couldn't be brought to the front, the
                    // count stays and the reset is retried after the next catch.)
                    catchesSinceRespawn = 0;
                    missedCastsInARow = 0;
                    phase = Phase::Respawn;
                    // The first cast after respawning must not be held back
                    // by the post-cast cooldown of the previous fish.
                    lastCastConfirmedAt = 0;
                }
                current.phase = phase;
                current.totalCatches = totalCatches;
                current.failedCollects = failedCollects;
                current.lastCollectDetected = lastCollectDetected;
                std::copy(rarityCounts, rarityCounts + kRarityCount, current.rarityCounts);
                current.lastRarity = lastRarity;
                current.oreCount = oreCount;
                current.lastWasOre = lastWasOre;
                current.catchesSinceRespawn = catchesSinceRespawn;
                phaseChangedAt = GetTickCount64();
            }
            current.collectAttempt = collectAttempt;
        } else if (phase == Phase::Reposition) {
            setMouse(false);
            current.phase = phase;
            {
                std::lock_guard<std::mutex> lock(gTelemetryMutex);
                gTelemetry = current;
            }
            PostMessage(gWindow, WM_TRACKER_UPDATE, 0, 0);
            const WalkResult walk = WalkBackToAnchor();
            positionOffset = walk.offset;
            positionLost = walk.lost;
            lostStreak = walk.lost ? lostStreak + 1 : 0;
            if (walk.lost && lostStreak == 3 && gDiscordFailAlerts.load() && gDiscordEnabled.load()) {
                DiscordJob job;
                job.type = DiscordEvent::Failing;
                wchar_t why[128];
                swprintf_s(why, L"Lost position: couldn't walk back to the anchor (%d px off).", walk.offset);
                job.detail = why;
                job.sessionId = gCurrentSessionId;
                job.catches = totalCatches;
                job.ore = oreCount;
                job.runningMs = runningMs;
                CaptureFullScreen(job.pixels, job.w, job.h);
                EnqueueDiscord(std::move(job));
            }
            current.positionLost = positionLost;
            current.positionOffset = positionOffset;
            phase = Phase::Cast;
            current.phase = phase;
            phaseChangedAt = GetTickCount64();
            previousMs = PreciseMs();
        } else if (phase == Phase::Respawn) {
            setMouse(false);
            current.phaseElapsedMs = static_cast<float>(now - phaseChangedAt);
            if (now - phaseChangedAt >= kRespawnSettleMs) {
                phase = Phase::Cast;
                current.phase = phase;
                phaseChangedAt = now;
            }
        }

        current.mouseDown = mouseDown;
        {
            std::lock_guard<std::mutex> lock(gTelemetryMutex);
            gTelemetry = current;
        }
        if (now - lastUiTick >= kUiRefreshIntervalMs) {
            lastUiTick = now;
            PostMessage(gWindow, WM_TRACKER_UPDATE, 0, 0);
        }
        Sleep(2); // approximately 250-330 observations/sec including capture time
    }

    setMouse(false);
    if (keyDown) KeyEvent(false, kHoldKeyVk);
    if (hookActive) minigameLog.Close(hookFrames, hookInZone);
    timeEndPeriod(1);
    return 0;
}

// ---------------------------------------------------------------------------
// Auto bait buy: clicks the 7 calibrated shop buttons in order, once per 99
// bait, on its own thread. Every click first makes sure Roblox is in front
// (same rule as fishing), and moves the mouse there for real so the game
// registers it. Fishing is paused while buying.
// ---------------------------------------------------------------------------
bool BaitWait(int ms) {
    for (int waited = 0; waited < ms; waited += 20) {
        if (gBaitStop.load() || gQuit.load()) return false;
        Sleep(20);
    }
    return !gBaitStop.load() && !gQuit.load();
}

DWORD WINAPI BaitThread(void*) {
    const int cycles = std::max(1, gBaitAmount.load() / kBaitPerCycle);
    gBaitFocusLost.store(false);
    for (int cycle = 1; cycle <= cycles && !gBaitStop.load(); ++cycle) {
        gBaitCycle.store(cycle);
        for (int i = 0; i < kBaitClicks && !gBaitStop.load(); ++i) {
            gBaitClick.store(i + 1);
            PostMessage(gWindow, WM_TRACKER_UPDATE, 0, 0);
            if (!FocusGame()) {
                gBaitFocusLost.store(true);
                gBaitStop.store(true);
                break;
            }
            GlideMouseTo(GetBaitPoint(i));
            if (!BaitWait(80)) break;
            MouseButton(true);
            Sleep(50);
            MouseButton(false);
            if (!BaitWait(gBaitDelayMs.load())) break;
        }
    }
    MouseButton(false);
    gBaitRunning.store(false);
    PostMessage(gWindow, WM_TRACKER_UPDATE, 0, 0);
    return 0;
}

bool AllBaitPointsSet() {
    for (int i = 0; i < kBaitClicks; ++i) {
        if (!BaitPointSet(i)) return false;
    }
    return true;
}

void StartBaitBuying() {
    if (gBaitRunning.load() || !AllBaitPointsSet()) return;
    gEnabled.store(false); // no fishing clicks in between the shop clicks
    MouseButton(false);
    gBaitStop.store(false);
    gBaitCycle.store(0);
    gBaitClick.store(0);
    gBaitStartMs.store(GetTickCount64());
    gBaitRunning.store(true);
    HANDLE thread = CreateThread(nullptr, 0, BaitThread, nullptr, 0, nullptr);
    if (thread) CloseHandle(thread);
    else gBaitRunning.store(false);
}

void StopBaitBuying() {
    gBaitStop.store(true);
}

// ---------------------------------------------------------------------------
// Fish sessions storage.
// ---------------------------------------------------------------------------
std::wstring SessionsPath() {
    wchar_t modulePath[MAX_PATH];
    const DWORD len = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    std::wstring dir(modulePath, (len > 0 && len < MAX_PATH) ? len : 0);
    return dir.substr(0, dir.find_last_of(L"\\/") + 1) + L"SkysS2FishingMacro_sessions.txt";
}

void LoadSessions() {
    gSessions.clear();
    FILE* f = _wfopen(SessionsPath().c_str(), L"r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        SessionRecord r;
        if (sscanf(line, "%d|%lld|%lld|%lld|%d|%d|%d|%d|%d|%d|%d|%d", &r.id, &r.start, &r.end, &r.runningMs,
                   &r.catches, &r.noItem, &r.ore, &r.rarity[0], &r.rarity[1], &r.rarity[2], &r.rarity[3],
                   &r.rarity[4]) == 12) {
            gSessions.push_back(r);
        }
    }
    fclose(f);
    std::sort(gSessions.begin(), gSessions.end(),
              [](const SessionRecord& a, const SessionRecord& b) { return a.id < b.id; });
}

void WriteSessions() {
    FILE* f = _wfopen(SessionsPath().c_str(), L"w");
    if (!f) return;
    fprintf(f, "# id|start|end|running_ms|catches|no_item|ore|impossible|mythic|legendary|rare|common\n");
    for (const SessionRecord& r : gSessions) {
        fprintf(f, "%d|%lld|%lld|%lld|%d|%d|%d|%d|%d|%d|%d|%d\n", r.id, r.start, r.end, r.runningMs, r.catches,
                r.noItem, r.ore, r.rarity[0], r.rarity[1], r.rarity[2], r.rarity[3], r.rarity[4]);
    }
    fclose(f);
}

SessionRecord* FindSession(int id) {
    for (SessionRecord& r : gSessions) {
        if (r.id == id) return &r;
    }
    return nullptr;
}

// Writes the running session into the list (insert or update). Nothing is
// saved until fishing was actually started and something happened.
void SaveCurrentSession() {
    Telemetry t;
    {
        std::lock_guard<std::mutex> lock(gTelemetryMutex);
        t = gTelemetry;
    }
    if (gCurrentSessionStart == 0) return;
    if (t.runningMs < 60000.0 && t.totalCatches == 0 && t.failedCollects == 0) return;
    SessionRecord r;
    r.id = gCurrentSessionId;
    r.start = gCurrentSessionStart;
    r.end = static_cast<long long>(time(nullptr));
    r.runningMs = static_cast<long long>(t.runningMs);
    r.catches = t.totalCatches;
    r.noItem = t.failedCollects;
    r.ore = t.oreCount;
    for (int i = 0; i < 5; ++i) r.rarity[i] = t.rarityCounts[i];
    SessionRecord* existing = FindSession(r.id);
    // A continued session that hasn't fished since keeps its old end time.
    if (existing && existing->runningMs == r.runningMs && existing->catches == r.catches &&
        existing->noItem == r.noItem) {
        return;
    }
    if (existing) *existing = r;
    else gSessions.push_back(r);
    std::sort(gSessions.begin(), gSessions.end(),
              [](const SessionRecord& a, const SessionRecord& b) { return a.id < b.id; });
    WriteSessions();
}

// Reset stats: the running session is saved to the Sessions list and a new,
// empty session starts (the tracker zeroes its counters).
void ResetStats() {
    SaveCurrentSession();
    int next = gCurrentSessionId;
    for (const SessionRecord& r : gSessions) next = std::max(next, r.id);
    gCurrentSessionId = next + 1;
    gCurrentSessionStart = gEnabled.load() ? static_cast<long long>(time(nullptr)) : 0;
    std::lock_guard<std::mutex> lock(gRestoreMutex);
    gRestoreData = SessionRecord{};
    gRestorePending = true;
}

// Continue a saved session: its numbers go back into the Fishing tab and it
// keeps its number; saving later updates that same entry.
void ContinueSession(int id) {
    const SessionRecord* r = FindSession(id);
    if (!r) return;
    {
        std::lock_guard<std::mutex> lock(gRestoreMutex);
        gRestoreData = *r;
        gRestorePending = true;
    }
    gCurrentSessionId = r->id;
    gCurrentSessionStart = r->start;
}

void DeleteSession(int id) {
    gSessions.erase(std::remove_if(gSessions.begin(), gSessions.end(),
                                   [id](const SessionRecord& r) { return r.id == id; }),
                    gSessions.end());
    WriteSessions();
    if (gRestorePromptId == id) gRestorePromptId = -1;
}

// "5h 12m" / "12m" / "45s"
std::wstring FormatHoursMinutes(long long ms) {
    const long long minutes = ms / 60000;
    wchar_t buf[32];
    if (minutes >= 60) swprintf_s(buf, L"%lldh %02lldm", minutes / 60, minutes % 60);
    else if (minutes > 0) swprintf_s(buf, L"%lldm", minutes);
    else swprintf_s(buf, L"%llds", ms / 1000);
    return buf;
}

std::wstring FormatDate(long long unixTime) {
    const time_t tt = static_cast<time_t>(unixTime);
    tm local{};
    localtime_s(&local, &tt);
    wchar_t buf[32];
    wcsftime(buf, 32, L"%d %b %Y", &local);
    return buf;
}

std::wstring FormatClock(long long unixTime) {
    const time_t tt = static_cast<time_t>(unixTime);
    tm local{};
    localtime_s(&local, &tt);
    wchar_t buf[16];
    wcsftime(buf, 16, L"%H:%M", &local);
    return buf;
}

// ---------------------------------------------------------------------------
// Sky's S2 Fishing Macro - custom-drawn UI.
// Everything below the title bar is painted with GDI+ (dark navy cards with a
// soft shadow, teal->blue gradient for anything active) and hit-tested by
// hand; only the two number inputs are real EDIT controls.
// ---------------------------------------------------------------------------
namespace ui {
const Gdiplus::Color kBg(255, 40, 41, 70);
const Gdiplus::Color kCard(255, 47, 48, 82);
const Gdiplus::Color kInset(255, 32, 33, 57);
const Gdiplus::Color kText(255, 240, 242, 255);
const Gdiplus::Color kSoft(255, 196, 199, 232);
const Gdiplus::Color kMuted(255, 138, 142, 184);
const Gdiplus::Color kGradA(255, 0, 229, 160);
const Gdiplus::Color kGradB(255, 45, 108, 255);
const Gdiplus::Color kWarn(255, 250, 204, 21);
const Gdiplus::Color kBad(255, 248, 113, 113);
constexpr COLORREF kBgRef = RGB(40, 41, 70);
constexpr COLORREF kInsetRef = RGB(32, 33, 57);
constexpr COLORREF kTextRef = RGB(240, 242, 255);
constexpr int kWidth = 380;
constexpr int kHeight = 612;
constexpr int kHeaderHeight = 64;
} // namespace ui

Gdiplus::Font* gFontTitle = nullptr;
Gdiplus::Font* gFontHead = nullptr;
Gdiplus::Font* gFontBig = nullptr;
Gdiplus::Font* gFontRow = nullptr;
Gdiplus::Font* gFontButton = nullptr;
Gdiplus::Font* gFontLabel = nullptr;
Gdiplus::Font* gFontBody = nullptr;
Gdiplus::Font* gFontSmall = nullptr;
Gdiplus::Font* gFontTab = nullptr;
HBRUSH gInputBrush = nullptr;

enum HitId {
    kHitNone = 0,
    kHitTab0 = 1, kHitTab1, kHitTab2, kHitTab3, kHitTab4, kHitTab5,
    kHitStart = 10, kHitExit, kHitClose, kHitRespawnToggle, kHitMinimize,
    kHitBindToggle = 20, kHitBindQuit, kHitBindCorrection, kHitBindRod, kHitBindBait,
    kHitCalCast = 30, kHitCalBar, kHitCalExit, kHitLogToggle, kHitCalCollect,
    kHitBaitStart = 40, kHitBaitPreset0, // presets: kHitBaitPreset0 + 0..4
    kHitBaitCal0 = 50,                   // calibration: kHitBaitCal0 + 0..6
    kHitSessRow0 = 60,                   // session list rows: kHitSessRow0 + 0..6
    kHitSessPrev = 70, kHitSessNext, kHitSessBack, kHitSessDelete, kHitRestoreYes, kHitRestoreNo,
    kHitMenu = 80, kHitMenuItem0,            // menu items: kHitMenuItem0 + 0..7
    kHitMenuBackdrop = 90, kHitResetStats, kHitDiscordToggle, kHitDiscordPing, kHitDiscordFail,
    kHitDiscordTest, kHitDiscordChip0 = 100, // rarity/ORE chips: kHitDiscordChip0 + 0..5
    kHitDiscordHide = 110,
    kHitRepoMode0 = 111, // reposition mode segments: kHitRepoMode0 + 0..2
    kHitCalAnchor = 115, kHitLearnWalk,
};
struct HitRegion {
    RECT rect;
    int id;
};
std::vector<HitRegion> gHits; // rebuilt on every paint (UI thread only)
int gHoverHit = kHitNone;
int gPressedHit = kHitNone;
bool gTrackingMouse = false;

using Gdiplus::RectF;

void AddHit(const RectF& r, int id) {
    gHits.push_back({RECT{static_cast<LONG>(r.X), static_cast<LONG>(r.Y),
                          static_cast<LONG>(r.X + r.Width), static_cast<LONG>(r.Y + r.Height)}, id});
}

int HitAt(POINT pt) {
    for (const HitRegion& h : gHits) {
        if (PtInRect(&h.rect, pt)) return h.id;
    }
    return kHitNone;
}

void AddRoundRect(Gdiplus::GraphicsPath& path, const RectF& r, float radius) {
    const float d = std::min(radius * 2.0f, std::min(r.Width, r.Height));
    if (d < 1.0f) {
        path.AddRectangle(r);
        return;
    }
    path.AddArc(r.X, r.Y, d, d, 180.0f, 90.0f);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270.0f, 90.0f);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0.0f, 90.0f);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

void FillRound(Gdiplus::Graphics& g, const Gdiplus::Brush& brush, const RectF& r, float radius) {
    Gdiplus::GraphicsPath path;
    AddRoundRect(path, r, radius);
    g.FillPath(&brush, &path);
}

// Raised card: a few stacked translucent offsets make a soft drop shadow, a
// faint top-left highlight gives the "neumorphic" lift, then the card itself.
void DrawCard(Gdiplus::Graphics& g, const RectF& r, float radius = 14.0f) {
    for (int i = 0; i < 4; ++i) {
        Gdiplus::SolidBrush shadow(Gdiplus::Color(20, 8, 8, 22));
        FillRound(g, shadow, RectF(r.X + 1.0f + i, r.Y + 2.0f + i, r.Width, r.Height), radius + i);
    }
    Gdiplus::SolidBrush highlight(Gdiplus::Color(16, 255, 255, 255));
    FillRound(g, highlight, RectF(r.X - 1.0f, r.Y - 1.0f, r.Width, r.Height), radius);
    Gdiplus::SolidBrush fill(ui::kCard);
    FillRound(g, fill, r, radius);
}

void FillGradient(Gdiplus::Graphics& g, const RectF& r, float radius, bool vertical = false) {
    Gdiplus::LinearGradientBrush brush(RectF(r.X - 1.0f, r.Y - 1.0f, r.Width + 2.0f, r.Height + 2.0f),
                                       ui::kGradA, ui::kGradB,
                                       vertical ? Gdiplus::LinearGradientModeVertical
                                                : Gdiplus::LinearGradientModeHorizontal);
    FillRound(g, brush, r, radius);
}

void FillInset(Gdiplus::Graphics& g, const RectF& r, float radius) {
    Gdiplus::SolidBrush fill(ui::kInset);
    FillRound(g, fill, r, radius);
}

// Hover/pressed feedback for anything clickable.
void HoverOverlay(Gdiplus::Graphics& g, const RectF& r, float radius, int id) {
    if (id == kHitNone || (gHoverHit != id && gPressedHit != id)) return;
    Gdiplus::SolidBrush overlay(gPressedHit == id ? Gdiplus::Color(40, 0, 0, 0)
                                                  : Gdiplus::Color(22, 255, 255, 255));
    FillRound(g, overlay, r, radius);
}

void Text(Gdiplus::Graphics& g, const wchar_t* text, const Gdiplus::Font* font,
          const Gdiplus::Color& color, const RectF& r,
          Gdiplus::StringAlignment horizontal = Gdiplus::StringAlignmentNear,
          Gdiplus::StringAlignment vertical = Gdiplus::StringAlignmentCenter) {
    Gdiplus::StringFormat format;
    format.SetAlignment(horizontal);
    format.SetLineAlignment(vertical);
    format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
    format.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
    Gdiplus::SolidBrush brush(color);
    g.DrawString(text, -1, font, r, &format, &brush);
}

void TextCenter(Gdiplus::Graphics& g, const wchar_t* text, const Gdiplus::Font* font,
                const Gdiplus::Color& color, const RectF& r) {
    Text(g, text, font, color, r, Gdiplus::StringAlignmentCenter, Gdiplus::StringAlignmentCenter);
}

// A gradient pill button with centered label.
void GradientButton(Gdiplus::Graphics& g, const RectF& r, const wchar_t* label, int id,
                    float radius = 12.0f) {
    FillGradient(g, r, radius);
    HoverOverlay(g, r, radius, id);
    TextCenter(g, label, gFontButton, ui::kText, r);
    if (id != kHitNone) AddHit(r, id);
}

void InsetButton(Gdiplus::Graphics& g, const RectF& r, const wchar_t* label, int id,
                 const Gdiplus::Color& color, float radius = 12.0f) {
    FillInset(g, r, radius);
    HoverOverlay(g, r, radius, id);
    TextCenter(g, label, gFontButton, color, r);
    if (id != kHitNone) AddHit(r, id);
}

// Two-line row label (title + muted subtitle) used inside settings cards.
void RowLabel(Gdiplus::Graphics& g, float y, const wchar_t* title, const wchar_t* sub,
              const Gdiplus::Color& subColor = ui::kMuted) {
    Text(g, title, gFontRow, ui::kText, RectF(34.0f, y + 2.0f, 210.0f, 20.0f));
    Text(g, sub, gFontSmall, subColor, RectF(34.0f, y + 22.0f, 210.0f, 15.0f));
}

void Divider(Gdiplus::Graphics& g, float y) {
    Gdiplus::Pen pen(Gdiplus::Color(26, 255, 255, 255), 1.0f);
    g.DrawLine(&pen, 34.0f, y, 346.0f, y);
}

std::wstring HotkeyHint() {
    return L"( " + KeyDisplayName(gToggleHotkeyVk.load()) + L" )";
}

// A count that always fits its box: smaller font when needed, "12.3k" from
// 10,000 up (the Rare chip overflowed past 1,000).
void DrawCount(Gdiplus::Graphics& g, long long value, const RectF& r, const Gdiplus::Font* font,
               const Gdiplus::Color& color, Gdiplus::StringAlignment align = Gdiplus::StringAlignmentFar) {
    wchar_t text[32];
    if (value >= 10000) swprintf_s(text, L"%.1fk", value / 1000.0);
    else swprintf_s(text, L"%lld", value);
    // Typographic format: no extra padding around the text and never an
    // ellipsis - a count must always show all its digits ("1344", not "13...").
    Gdiplus::StringFormat format(Gdiplus::StringFormat::GenericTypographic());
    format.SetAlignment(align);
    format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap | Gdiplus::StringFormatFlagsNoClip);
    format.SetTrimming(Gdiplus::StringTrimmingNone);
    Gdiplus::RectF bounds;
    g.MeasureString(text, -1, font, Gdiplus::PointF(0, 0), &format, &bounds);
    const Gdiplus::Font* use = bounds.Width > r.Width ? gFontSmall : font;
    Gdiplus::SolidBrush brush(color);
    g.DrawString(text, -1, use, r, &format, &brush);
}

void DrawCalendarIcon(Gdiplus::Graphics& g, float x, float y, const Gdiplus::Color& c) {
    Gdiplus::Pen pen(c, 1.3f);
    Gdiplus::GraphicsPath path;
    AddRoundRect(path, RectF(x, y + 2, 12, 11), 2.5f);
    g.DrawPath(&pen, &path);
    Gdiplus::SolidBrush brush(c);
    g.FillRectangle(&brush, RectF(x, y + 2, 12, 3.5f));
    g.DrawLine(&pen, x + 3.5f, y, x + 3.5f, y + 3.5f);
    g.DrawLine(&pen, x + 8.5f, y, x + 8.5f, y + 3.5f);
}

void DrawClockIcon(Gdiplus::Graphics& g, float x, float y, const Gdiplus::Color& c) {
    Gdiplus::Pen pen(c, 1.3f);
    g.DrawEllipse(&pen, RectF(x, y + 1, 12, 12));
    g.DrawLine(&pen, x + 6, y + 7, x + 6, y + 3.5f);
    g.DrawLine(&pen, x + 6, y + 7, x + 9, y + 8.5f);
}

// ORE & rarity card, shared by the Fishing tab (live) and a saved session.
void DrawOreRarityCard(Gdiplus::Graphics& g, const Telemetry& t, float top, bool live) {
    wchar_t line[64];
    DrawCard(g, RectF(18, top, 344, 96));
    Text(g, L"ORE & RARITY", gFontLabel, ui::kText, RectF(34, top + 6, 180, 16));
    if (live) {
        if (!t.collectCheckReady) {
            Text(g, L"item check off", gFontSmall, ui::kWarn, RectF(200, top + 6, 146, 16),
                 Gdiplus::StringAlignmentFar);
        } else if (t.lastCollectDetected == 0) {
            Text(g, L"last: no item", gFontSmall, ui::kBad, RectF(200, top + 6, 146, 16),
                 Gdiplus::StringAlignmentFar);
        } else if (t.lastRarity >= 0) {
            swprintf_s(line, L"last: %s ✓", t.lastWasOre ? L"ORE" : kRarityNames[t.lastRarity]);
            Text(g, line, gFontSmall, ui::kSoft, RectF(200, top + 6, 146, 16), Gdiplus::StringAlignmentFar);
        }
    }
    {
        // ORE block (left): the item everyone fishes for, with ORE per hour.
        const RectF oreBox(34, top + 26, 82, 64);
        if (t.oreCount > 0 || (t.lastWasOre && t.lastCollectDetected == 1)) FillGradient(g, oreBox, 12.0f);
        else FillInset(g, oreBox, 12.0f);
        Text(g, L"ORE", gFontLabel, ui::kText, RectF(42, top + 31, 40, 16));
        if (t.runningMs >= 60000.0) {
            swprintf_s(line, L"%.1f/h", t.oreCount / (t.runningMs / 3600000.0));
            Text(g, line, gFontSmall, Gdiplus::Color(230, 255, 255, 255), RectF(70, top + 31, 42, 16),
                 Gdiplus::StringAlignmentFar);
        }
        DrawCount(g, t.oreCount, RectF(40, top + 48, 72, 34), gFontBig, ui::kText,
                  Gdiplus::StringAlignmentNear);
    }
    const Gdiplus::Color dots[kRarityCount] = {
        Gdiplus::Color(255, 20, 20, 26), Gdiplus::Color(255, 248, 82, 82),
        Gdiplus::Color(255, 250, 190, 50), Gdiplus::Color(255, 70, 150, 255),
        Gdiplus::Color(255, 165, 168, 185)};
    for (int i = 0; i < kRarityCount; ++i) {
        // Impossible gets the full top row, the others a 2x2 grid below it.
        const bool first = i == kRarityImpossible;
        const float cx = first ? 122.0f : 122.0f + ((i - 1) % 2) * 114.0f;
        const float cy = first ? top + 26 : top + 48 + ((i - 1) / 2) * 22.0f;
        const RectF chip(cx, cy, first ? 224.0f : 110.0f, 20.0f);
        FillInset(g, chip, 10.0f);
        if (live && t.lastRarity == i && t.lastCollectDetected == 1) {
            Gdiplus::Pen ring(first ? ui::kText : dots[i], 1.5f);
            Gdiplus::GraphicsPath path;
            AddRoundRect(path, chip, 10.0f);
            g.DrawPath(&ring, &path);
        }
        Gdiplus::SolidBrush dot(dots[i]);
        g.FillEllipse(&dot, RectF(cx + 9.0f, cy + 6.0f, 8.0f, 8.0f));
        if (first) {
            Gdiplus::Pen outline(ui::kMuted, 1.0f); // black dot needs an outline on the dark chip
            g.DrawEllipse(&outline, RectF(cx + 9.0f, cy + 6.0f, 8.0f, 8.0f));
        }
        // Name gets whatever the count leaves free; both drawn without padding.
        Gdiplus::StringFormat tight(Gdiplus::StringFormat::GenericTypographic());
        tight.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        tight.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
        tight.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
        wchar_t countText[32];
        swprintf_s(countText, L"%d", t.rarityCounts[i]);
        Gdiplus::RectF countBounds;
        g.MeasureString(countText, -1, gFontLabel, Gdiplus::PointF(0, 0), &tight, &countBounds);
        const float countWidth = std::min(countBounds.Width, 34.0f);
        Gdiplus::SolidBrush nameBrush((!live || t.collectCheckReady) ? ui::kSoft : ui::kMuted);
        g.DrawString(kRarityNames[i], -1, gFontSmall,
                     RectF(cx + 21.0f, cy, chip.Width - 21.0f - countWidth - 12.0f, 20.0f), &tight, &nameBrush);
        DrawCount(g, t.rarityCounts[i], RectF(cx + chip.Width - 8.0f - 34.0f, cy, 34.0f, 20.0f), gFontLabel, ui::kText);
    }
}

void DrawFishingTab(Gdiplus::Graphics& g, const Telemetry& t) {
    wchar_t line[160];

    // --- Status card -----------------------------------------------------
    DrawCard(g, RectF(18, 128, 344, 92));
    FillGradient(g, RectF(34, 146, 44, 44), 22.0f);
    {
        Gdiplus::SolidBrush white(ui::kText);
        if (t.enabled) {
            g.FillRectangle(&white, RectF(49, 158, 5, 20));
            g.FillRectangle(&white, RectF(58, 158, 5, 20));
        } else {
            const Gdiplus::PointF tri[3] = {{51, 157}, {51, 179}, {67, 168}};
            g.FillPolygon(&white, tri, 3);
        }
    }

    const wchar_t* headline = L"";
    Gdiplus::Color headColor = ui::kText;
    std::wstring detail;
    if (t.learningWalk) {
        headline = L"LEARNING STEPS";
        headColor = ui::kWarn;
        detail = L"Tapping W/A/S/D to learn the walk-back steps";
    } else if (t.captureReady && gRepositionMode.load() == 2 && t.positionLost) {
        headline = L"LOST POSITION";
        headColor = ui::kBad;
        wchar_t lost[96];
        if (t.positionOffset >= 0) swprintf_s(lost, L"Anchor %d px off · tries again after the next catch", t.positionOffset);
        else swprintf_s(lost, L"Anchor not visible · tries again after the next catch");
        detail = lost;
    } else if (!t.captureReady) {
        headline = L"NOT CALIBRATED";
        headColor = ui::kBad;
        detail = L"Set the bar + Exit button on the Setup tab";
    } else {
        headline = !t.enabled ? L"PAUSED"
                 : (t.phase == Phase::Hook) ? StateText(t.state) : PhaseText(t.phase);
        if (t.phase == Phase::Hook && t.minigame) headColor = ui::kGradA;
        if (t.phase == Phase::Hook) {
            if (t.targetFound && t.playerFound) {
                swprintf_s(line, L"In zone %d%%  \u00B7  Error %+.0f px", std::max(0, t.zonePct), t.error);
            } else {
                swprintf_s(line, L"Target %s  ·  Player %s",
                           t.targetFound ? L"found" : L"---", t.playerFound ? L"found" : L"---");
            }
        } else if (t.phase == Phase::Collect) {
            swprintf_s(line, L"Waiting to collect  %.1fs / %.1fs",
                       t.phaseElapsedMs / 1000.0f, gCollectDelayMs.load() / 1000.0f);
        } else if (t.phase == Phase::HoldKey) {
            if (t.collectCheckReady) {
                swprintf_s(line, L"Holding T  %.1fs / %.1fs  ·  try %d/%d",
                           t.phaseElapsedMs / 1000.0f, gHoldKeyMs.load() / 1000.0f,
                           t.collectAttempt + 1, kCollectRetries + 1);
            } else {
                swprintf_s(line, L"Holding T  %.1fs / %.1fs",
                           t.phaseElapsedMs / 1000.0f, gHoldKeyMs.load() / 1000.0f);
            }
        } else if (t.phase == Phase::VerifyCollect) {
            swprintf_s(line, L"Looking for the item message  ·  try %d/%d",
                       t.collectAttempt + 1, kCollectRetries + 1);
        } else if (t.phase == Phase::Respawn) {
            swprintf_s(line, L"Resetting character  %.1fs / %.0fs",
                       t.phaseElapsedMs / 1000.0f, kRespawnSettleMs / 1000.0f);
        } else if (t.phase == Phase::WaitForFish) {
            if (t.waitForFishRemainingMs > 0.0f) {
                swprintf_s(line, L"Waiting for a bite  ·  %.1fs left",
                           t.waitForFishRemainingMs / 1000.0f);
            } else {
                swprintf_s(line, L"Exit detected, starting reel");
            }
        } else if (t.castCooldownRemainingMs > 0.0f) {
            swprintf_s(line, L"Post-cast failsafe  ·  %.1fs", t.castCooldownRemainingMs / 1000.0f);
        } else {
            swprintf_s(line, t.enabled ? L"Casting..." : L"Paused  ·  press %s to start",
                       KeyDisplayName(gToggleHotkeyVk.load()).c_str());
        }
        detail = line;
    }
    Text(g, headline, gFontHead, headColor, RectF(92, 138, 196, 26));
    Text(g, detail.c_str(), gFontBody, ui::kSoft, RectF(92, 164, 258, 18));
    swprintf_s(line, L"Detection %d%%  ·  %.0f FPS  ·  Mouse %s",
               t.confidence, t.fps, t.mouseDown ? L"DOWN" : L"UP");
    Text(g, line, gFontSmall, ui::kMuted, RectF(92, 184, 258, 16));

    {
        // Reset stats (two clicks): saves this session and starts a new one.
        const RectF reset(238, 140, 52, 22);
        if (gResetArmed) {
            Gdiplus::SolidBrush red(Gdiplus::Color(255, 190, 60, 70));
            FillRound(g, red, reset, 11.0f);
            HoverOverlay(g, reset, 11.0f, kHitResetStats);
            TextCenter(g, L"Sure?", gFontLabel, ui::kText, reset);
            AddHit(reset, kHitResetStats);
        } else {
            InsetButton(g, reset, L"Reset", kHitResetStats, ui::kSoft, 11.0f);
        }
    }
    const RectF pill(296, 140, 52, 22);
    if (t.enabled) {
        FillGradient(g, pill, 11.0f);
        TextCenter(g, L"LIVE", gFontLabel, ui::kText, pill);
    } else {
        FillInset(g, pill, 11.0f);
        TextCenter(g, L"OFF", gFontLabel, ui::kMuted, pill);
    }

    // --- Stat tiles (2x2) --------------------------------------------------
    DrawCard(g, RectF(18, 234, 166, 76));
    Text(g, L"CATCHES", gFontLabel, ui::kText, RectF(32, 244, 120, 16));
    Text(g, L"item collected", gFontSmall, ui::kMuted, RectF(32, 259, 120, 14));
    DrawCount(g, t.totalCatches, RectF(30, 274, 130, 32), gFontBig, ui::kText, Gdiplus::StringAlignmentNear);
    FillGradient(g, RectF(164, 250, 4, 44), 2.0f, true);

    DrawCard(g, RectF(196, 234, 166, 76));
    // No item message can mean the pickup failed OR the game gave nothing
    // (catching nothing is possible) - the two look identical on screen.
    Text(g, L"NO ITEM", gFontLabel, ui::kText, RectF(210, 244, 130, 16));
    Text(g, L"failed / caught nothing", gFontSmall, ui::kMuted, RectF(210, 259, 140, 14));
    DrawCount(g, t.failedCollects, RectF(208, 274, 130, 32), gFontBig, t.failedCollects ? ui::kBad : ui::kText,
              Gdiplus::StringAlignmentNear);
    {
        Gdiplus::SolidBrush red(ui::kBad);
        FillRound(g, red, RectF(342, 250, 4, 44), 2.0f);
    }

    DrawCard(g, RectF(18, 322, 166, 76));
    Text(g, L"SUCCESS RATE", gFontLabel, ui::kText, RectF(32, 332, 130, 16));
    Text(g, L"collected / tries", gFontSmall, ui::kMuted, RectF(32, 347, 130, 14));
    const int attempts = t.totalCatches + t.failedCollects;
    if (attempts > 0) swprintf_s(line, L"%d%%", t.totalCatches * 100 / attempts);
    else swprintf_s(line, L"—");
    Text(g, line, gFontBig, ui::kText, RectF(30, 362, 130, 32));
    FillGradient(g, RectF(164, 338, 4, 44), 2.0f, true);

    const RectF resetTile(196, 322, 166, 76);
    DrawCard(g, resetTile);
    const int repoMode = gRepositionMode.load();
    if (repoMode == 2) {
        // Walk back: position status instead of the reset countdown.
        Text(g, L"POSITION", gFontLabel, ui::kText, RectF(210, 332, 120, 16));
        Text(g, L"walk back", gFontSmall, ui::kMuted, RectF(210, 347, 120, 14));
        Gdiplus::Color colour = ui::kText;
        if (!WalkLearned()) { swprintf_s(line, L"SET UP"); colour = ui::kWarn; }
        else if (t.positionLost) { swprintf_s(line, L"LOST"); colour = ui::kBad; }
        else if (t.positionOffset < 0) swprintf_s(line, L"—");
        else swprintf_s(line, L"OK");
        Text(g, line, gFontBig, colour, RectF(208, 362, 130, 32));
        FillGradient(g, RectF(342, 338, 4, 44), 2.0f, true);
    } else {
        const bool respawnOn = repoMode == 1;
        Text(g, L"UNTIL RESET", gFontLabel, respawnOn ? ui::kText : ui::kMuted, RectF(210, 332, 120, 16));
        Text(g, L"auto reposition", gFontSmall, ui::kMuted, RectF(210, 347, 120, 14));
        if (respawnOn) swprintf_s(line, L"%d / %d", t.catchesSinceRespawn, gRespawnEveryCatches.load());
        else swprintf_s(line, L"OFF");
        Text(g, line, gFontBig, respawnOn ? ui::kText : ui::kMuted, RectF(208, 362, 130, 32));
        if (respawnOn) FillGradient(g, RectF(342, 338, 4, 44), 2.0f, true);
    }

    DrawOreRarityCard(g, t, 410.0f, true);

    // --- Buttons -------------------------------------------------------------
    std::wstring start = (t.enabled ? L"PAUSE   " : L"START   ") + HotkeyHint();
    GradientButton(g, RectF(18, 514, 212, 50), start.c_str(), kHitStart, 14.0f);
    std::wstring quit = L"EXIT   ( " + KeyDisplayName(gQuitHotkeyVk.load()) + L" )";
    const RectF exitR(242, 514, 120, 50);
    DrawCard(g, exitR);
    HoverOverlay(g, exitR, 14.0f, kHitExit);
    TextCenter(g, quit.c_str(), gFontButton, ui::kSoft, exitR);
    AddHit(exitR, kHitExit);

    // --- Footer --------------------------------------------------------------
    if (gCalibrating.load()) {
        TextCenter(g, L"Calibrating · see the Setup tab (Esc cancels)", gFontSmall, ui::kWarn,
                   RectF(18, 578, 344, 18));
    } else {
        swprintf_s(line, L"Session #%d  ·  %ls fishing  ·  v3.0", gCurrentSessionId,
                   FormatHoursMinutes(static_cast<long long>(t.runningMs)).c_str());
        TextCenter(g, line, gFontSmall, ui::kMuted, RectF(18, 578, 344, 18));
    }
}

// Startup question: continue the last session? Drawn over the fishing tiles.
void DrawRestorePrompt(Gdiplus::Graphics& g) {
    const SessionRecord* r = FindSession(gRestorePromptId);
    if (!r) return;
    wchar_t line[160];
    const RectF card(26, 232, 328, 150);
    Gdiplus::SolidBrush dim(Gdiplus::Color(150, 20, 21, 40));
    g.FillRectangle(&dim, RectF(18, 226, 344, 280));
    DrawCard(g, card, 16.0f);
    Text(g, L"CONTINUE LAST SESSION?", gFontLabel, ui::kText, RectF(42, 244, 290, 16));
    swprintf_s(line, L"Session #%d", r->id);
    Text(g, line, gFontHead, ui::kText, RectF(42, 262, 290, 26));
    DrawCalendarIcon(g, 42, 294, ui::kMuted);
    Text(g, FormatDate(r->start).c_str(), gFontSmall, ui::kSoft, RectF(60, 292, 90, 16));
    DrawClockIcon(g, 150, 294, ui::kMuted);
    swprintf_s(line, L"%ls–%ls · %ls fishing", FormatClock(r->start).c_str(), FormatClock(r->end).c_str(),
               FormatHoursMinutes(r->runningMs).c_str());
    Text(g, line, gFontSmall, ui::kSoft, RectF(168, 292, 180, 16));
    swprintf_s(line, L"%d catches · %d ORE · %d no item", r->catches, r->ore, r->noItem);
    Text(g, line, gFontSmall, ui::kMuted, RectF(42, 312, 300, 16));
    GradientButton(g, RectF(42, 336, 140, 34), L"Continue", kHitRestoreYes, 12.0f);
    InsetButton(g, RectF(194, 336, 144, 34), L"New session", kHitRestoreNo, ui::kSoft, 12.0f);
}

// On/off switch drawn at the right of a settings row.
void DrawToggle(Gdiplus::Graphics& g, float rowY, bool on, int id) {
    const RectF track(300, rowY + 8, 46, 24);
    if (on) FillGradient(g, track, 12.0f);
    else FillInset(g, track, 12.0f);
    Gdiplus::SolidBrush knob(on ? ui::kText : ui::kMuted);
    g.FillEllipse(&knob, RectF(on ? 325.0f : 303.0f, rowY + 11, 18.0f, 18.0f));
    HoverOverlay(g, RectF(296, rowY + 4, 54, 32), 14.0f, id);
    AddHit(RectF(262, rowY, 94, 40), id);
}

// Settings tab. The EDIT controls (created in WM_CREATE) sit on the inset
// boxes of rows 0, 1, 2 and 4 - keep kSettingsRowY in sync with them.
constexpr float kSettingsTop = 134.0f;
constexpr float kSettingsRowH = 44.0f;
inline float SettingsRowY(int row) { return kSettingsTop + row * kSettingsRowH; }

void DrawSettingsTab(Gdiplus::Graphics& g) {
    DrawCard(g, RectF(18, 128, 344, 6 * kSettingsRowH + 10));
    const wchar_t* titles[6] = {L"Wait for fish", L"Collect delay", L"Hold T time",
                                L"Auto reposition", L"Reset every", L"Minigame log"};
    const wchar_t* subs[6] = {L"seconds before re-casting", L"seconds before holding T",
                              L"max; lets go when the item shows",
                              L"",
                              L"minigames (also after 2 missed casts)",
                              L"CSV + item captures in logs\\ (tuning)"};
    for (int row = 0; row < 6; ++row) {
        const float y = SettingsRowY(row);
        if (row > 0) Divider(g, y - 3.0f);
        RowLabel(g, y, titles[row], subs[row]);
        if (row == 3) {
            // Off / Reset (gamepass) / Walk back
            const int mode = gRepositionMode.load();
            const wchar_t* sub = mode == 1 ? L"reset character (Esc · R · Enter)"
                               : mode == 2 ? L"walks back with W/A/S/D" : L"character is not moved";
            Text(g, sub, gFontSmall, ui::kMuted, RectF(34.0f, y + 22.0f, 170.0f, 15.0f));
            const wchar_t* labels[3] = {L"Off", L"Reset", L"Walk"};
            FillInset(g, RectF(204, y + 6, 142, 28), 10.0f);
            for (int i = 0; i < 3; ++i) {
                const RectF seg(206.0f + i * 46.0f, y + 8, 46.0f, 24.0f);
                if (mode == i) FillGradient(g, seg, 9.0f);
                else HoverOverlay(g, seg, 9.0f, kHitRepoMode0 + i);
                TextCenter(g, labels[i], gFontLabel, mode == i ? ui::kText : ui::kMuted, seg);
                AddHit(seg, kHitRepoMode0 + i);
            }
        }
        else if (row == 5) DrawToggle(g, y, gMinigameLog.load(), kHitLogToggle);
        else FillInset(g, RectF(266, y + 5, 80, 30), 9.0f);
    }
}

void DrawKeyRow(Gdiplus::Graphics& g, float y, const wchar_t* title, const wchar_t* sub,
                const std::wstring& key, int id, bool binding) {
    DrawCard(g, RectF(18, y, 344, 52));
    RowLabel(g, y + 6, title, sub);
    const RectF chip(236, y + 8, 110, 36);
    if (binding) {
        GradientButton(g, chip, L"Press a key...", id);
    } else {
        InsetButton(g, chip, key.c_str(), id, id == kHitNone ? ui::kMuted : ui::kText);
    }
}

void DrawHotkeysTab(Gdiplus::Graphics& g) {
    DrawCard(g, RectF(18, 128, 344, 70));
    Text(g, L"HOTKEYS", gFontLabel, ui::kText, RectF(34, 138, 300, 16));
    Text(g, L"Click a key, then press the new key.", gFontSmall, ui::kMuted, RectF(34, 157, 310, 15));
    Text(g, L"Start, exit and bait keys work in any window. Esc cancels.", gFontSmall, ui::kMuted,
         RectF(34, 173, 310, 15));

    const bool binding = gBindingHotkey.load();
    DrawKeyRow(g, 208, L"Start / Pause", L"global hotkey",
               KeyDisplayName(gToggleHotkeyVk.load()), kHitBindToggle, binding && gBindingTarget == 1);
    DrawKeyRow(g, 266, L"Safe exit", L"closes the macro",
               KeyDisplayName(gQuitHotkeyVk.load()), kHitBindQuit, binding && gBindingTarget == 2);
    DrawKeyRow(g, 324, L"Auto bait buy", L"global: start / stop buying bait",
               KeyDisplayName(gBaitHotkeyVk.load()), kHitBindBait, binding && gBindingTarget == 5);
    DrawKeyRow(g, 382, L"Correction key", L"pressed first when starting",
               KeyDisplayName(gCorrectionKeyVk.load()), kHitBindCorrection, binding && gBindingTarget == 3);
    DrawKeyRow(g, 440, L"Rod key", L"pressed next to equip the rod",
               KeyDisplayName(gRodKeyVk.load()), kHitBindRod, binding && gBindingTarget == 4);
    DrawKeyRow(g, 498, L"Collect key", L"held after each catch (fixed)", L"T", kHitNone, false);

    if (binding) {
        TextCenter(g, L"Waiting for a key press \u00B7 Esc cancels", gFontSmall, ui::kWarn,
                   RectF(18, 560, 344, 18));
    }
}

void DrawCalibrationRow(Gdiplus::Graphics& g, float y, const wchar_t* title, const wchar_t* value,
                        bool isSet, int id, bool active) {
    DrawCard(g, RectF(18, y, 344, 60));
    RowLabel(g, y + 10, title, value, isSet ? ui::kMuted : ui::kBad);
    const RectF button(236, y + 12, 110, 36);
    if (active) InsetButton(g, button, L"Cancel", id, ui::kWarn);
    else GradientButton(g, button, L"Calibrate", id);
}

void DrawSetupTab(Gdiplus::Graphics& g) {
    wchar_t line[128];
    const bool calibrating = gCalibrating.load();
    DrawCard(g, RectF(18, 128, 344, 74));
    Text(g, L"CALIBRATION", gFontLabel, ui::kText, RectF(34, 140, 300, 16));
    if (calibrating) {
        const wchar_t* banner = (gCalibrationTarget == CalibrationTarget::CastPoint)
            ? L"CLICK the spot to cast at"
            : (gCalibrationTarget == CalibrationTarget::CollectRegion)
                ? L"DRAG a box where \"<item> x1\" appears"
            : (gCalibrationTarget == CalibrationTarget::AnchorRegion)
                ? L"DRAG a box over something that doesn't move"
                : L"DRAG a box over the region";
        Text(g, banner, gFontBody, ui::kWarn, RectF(34, 158, 310, 18));
        Text(g, L"Esc cancels", gFontSmall, ui::kMuted, RectF(34, 177, 310, 15));
    } else {
        Text(g, L"Click Calibrate, then click or drag on screen.", gFontSmall, ui::kMuted,
             RectF(34, 160, 310, 15));
        Text(g, L"Everything is saved automatically.", gFontSmall, ui::kMuted, RectF(34, 176, 310, 15));
    }

    const POINT cast = GetCastPoint();
    swprintf_s(line, L"(%ld, %ld)", cast.x, cast.y);
    DrawCalibrationRow(g, 210, L"Cast point", line, true, kHitCalCast,
                       calibrating && gCalibrationTarget == CalibrationTarget::CastPoint);

    const RECT bar = GetBarRect();
    const bool barSet = RectCalibrated(bar);
    if (barSet) swprintf_s(line, L"%ldx%ld at (%ld, %ld)", bar.right - bar.left, bar.bottom - bar.top, bar.left, bar.top);
    else swprintf_s(line, L"NOT SET");
    DrawCalibrationRow(g, 276, L"Fishing bar", line, barSet, kHitCalBar,
                       calibrating && gCalibrationTarget == CalibrationTarget::BarRegion);

    const RECT exitR = GetExitRect();
    const bool exitSet = RectCalibrated(exitR);
    if (exitSet) swprintf_s(line, L"%ldx%ld at (%ld, %ld)", exitR.right - exitR.left, exitR.bottom - exitR.top, exitR.left, exitR.top);
    else swprintf_s(line, L"NOT SET");
    DrawCalibrationRow(g, 342, L"Exit button", line, exitSet, kHitCalExit,
                       calibrating && gCalibrationTarget == CalibrationTarget::ExitRegion);

    const RECT collect = GetCollectRect();
    const bool collectSet = RectCalibrated(collect);
    if (collectSet) {
        swprintf_s(line, L"%ldx%ld at (%ld, %ld)", collect.right - collect.left,
                   collect.bottom - collect.top, collect.left, collect.top);
    } else {
        swprintf_s(line, L"NOT SET · box the item message");
    }
    DrawCalibrationRow(g, 408, L"Collect message", line, collectSet, kHitCalCollect,
                       calibrating && gCalibrationTarget == CalibrationTarget::CollectRegion);

    // Position anchor (walk back): calibrate + learn the W/A/S/D steps.
    {
        const float y = 474.0f;
        DrawCard(g, RectF(18, y, 344, 60));
        Telemetry t;
        {
            std::lock_guard<std::mutex> lock(gTelemetryMutex);
            t = gTelemetry;
        }
        const bool ready = AnchorReady();
        const bool learned = WalkLearned();
        if (t.learningWalk || gLearnWalkRequested.load()) swprintf_s(line, L"learning W/A/S/D steps...");
        else if (!ready) {
            const std::wstring why = GetWalkStatus();
            if (why.empty()) swprintf_s(line, L"NOT SET · box a spot that never moves");
            else swprintf_s(line, L"%ls", why.c_str());
        }
        else if (!learned) {
            const std::wstring why = GetWalkStatus();
            if (why.empty()) swprintf_s(line, L"steps not learned · click Learn");
            else swprintf_s(line, L"failed: %ls", why.c_str());
        }
        else {
            std::lock_guard<std::mutex> lock(gAnchorMutex);
            float avg = 0;
            for (const WalkStep& st : gWalkSteps) avg += std::hypot(st.x, st.y) / 4.0f;
            swprintf_s(line, L"ready · ~%.1f px per step", avg);
        }
        RowLabel(g, y + 10, L"Position anchor", line, learned ? ui::kMuted : (ready ? ui::kWarn : ui::kBad));
        const bool active = calibrating && gCalibrationTarget == CalibrationTarget::AnchorRegion;
        if (active) InsetButton(g, RectF(236, y + 12, 110, 36), L"Cancel", kHitCalAnchor, ui::kWarn);
        else {
            GradientButton(g, RectF(236, y + 12, 52, 36), L"Set", kHitCalAnchor);
            if (ready) InsetButton(g, RectF(294, y + 12, 52, 36), L"Learn", kHitLearnWalk, ui::kSoft);
        }
    }
}

// Auto bait buy page. The two EDIT controls sit on the inset boxes of the
// amount card (created in WM_CREATE at kBaitRowY + 9).
constexpr float kBaitAmountRowY = 206.0f;
constexpr float kBaitDelayRowY = 290.0f;
const int kBaitPresets[5] = {99, 198, 495, 990, 1980};

constexpr int kWenPerBait = 9;

// 27621 -> "27,621"
std::wstring WithThousands(long long value) {
    std::wstring digits = std::to_wstring(value);
    for (int i = static_cast<int>(digits.size()) - 3; i > 0; i -= 3) digits.insert(i, L",");
    return digits;
}

// 185000 -> "3:05", 4000000 -> "1:06:40"
std::wstring FormatDuration(ULONGLONG ms) {
    const ULONGLONG total = (ms + 999) / 1000;
    wchar_t buf[32];
    if (total >= 3600) swprintf_s(buf, L"%llu:%02llu:%02llu", total / 3600, (total / 60) % 60, total % 60);
    else swprintf_s(buf, L"%llu:%02llu", total / 60, total % 60);
    return buf;
}

// Time left for the bait purchase: before/at the start from the settings,
// after a few clicks from the actually measured pace.
ULONGLONG BaitTimeLeftMs(int cycles) {
    const int totalClicks = cycles * kBaitClicks;
    const ULONGLONG estimatePerClick = static_cast<ULONGLONG>(gBaitDelayMs.load() + kBaitClickOverheadMs);
    if (!gBaitRunning.load()) return totalClicks * estimatePerClick;
    const int done = std::max(0, (gBaitCycle.load() - 1) * kBaitClicks + gBaitClick.load() - 1);
    const ULONGLONG elapsed = GetTickCount64() - gBaitStartMs.load();
    const ULONGLONG perClick = done >= 3 ? elapsed / done : estimatePerClick;
    return static_cast<ULONGLONG>(std::max(0, totalClicks - done)) * perClick;
}

void DrawBaitTab(Gdiplus::Graphics& g) {
    wchar_t line[128];
    const bool running = gBaitRunning.load();
    const int cycles = std::max(1, gBaitAmount.load() / kBaitPerCycle);

    // --- Status -------------------------------------------------------------
    DrawCard(g, RectF(18, 128, 344, 62));
    Text(g, L"AUTO BAIT BUY", gFontLabel, ui::kText, RectF(34, 138, 300, 16));
    {
        // Wen calculator: every bait costs 9 wen.
        const long long cost = static_cast<long long>(cycles) * kBaitPerCycle * kWenPerBait;
        const std::wstring text = WithThousands(cost) + L" wen";
        Gdiplus::RectF bounds;
        g.MeasureString(text.c_str(), -1, gFontButton, Gdiplus::PointF(0, 0), &bounds);
        const float textX = 348.0f - bounds.Width;
        Text(g, text.c_str(), gFontButton, Gdiplus::Color(255, 214, 240, 70), RectF(textX, 134, bounds.Width + 2, 22));
        if (gWenIcon) {
            g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            g.DrawImage(gWenIcon, RectF(textX - 36.0f, 133.0f, 33.0f, 24.0f)); // icon is 85x62
        }
    }
    if (running) {
        swprintf_s(line, L"Buying %d/%d \u00B7 click %d/%d \u00B7 %ls left",
                   std::max(1, gBaitCycle.load()), cycles, std::max(1, gBaitClick.load()), kBaitClicks,
                   FormatDuration(BaitTimeLeftMs(cycles)).c_str());
        Text(g, line, gFontBody, ui::kGradA, RectF(34, 158, 312, 18));
    } else if (gCalibrating.load() && gCalibrationTarget == CalibrationTarget::BaitPoint) {
        swprintf_s(line, L"CLICK \"%s\" in the shop (Esc cancels)", kBaitClickNames[gBaitCalibIndex]);
        Text(g, line, gFontBody, ui::kWarn, RectF(34, 158, 312, 18));
    } else if (!AllBaitPointsSet()) {
        Text(g, L"Set all 7 click points below first", gFontBody, ui::kWarn, RectF(34, 158, 312, 18));
    } else if (gBaitFocusLost.load()) {
        Text(g, L"Stopped: Roblox was not in front", gFontBody, ui::kBad, RectF(34, 158, 312, 18));
    } else {
        swprintf_s(line, L"Ready \u00B7 %d\u00D799 = %d bait \u00B7 takes \u2248 %ls", cycles,
                   cycles * kBaitPerCycle, FormatDuration(BaitTimeLeftMs(cycles)).c_str());
        Text(g, line, gFontBody, ui::kSoft, RectF(34, 158, 312, 18));
    }

    // --- Amount / delay / presets -----------------------------------------------
    // Amount row with its presets right below it, then the click delay.
    DrawCard(g, RectF(18, 200, 344, 136));
    RowLabel(g, kBaitAmountRowY, L"Bait to buy", L"rounded up to 99 per purchase");
    FillInset(g, RectF(266, kBaitAmountRowY + 5, 80, 30), 9.0f);
    Divider(g, kBaitDelayRowY - 3);
    RowLabel(g, kBaitDelayRowY, L"Click delay", L"ms to wait after each click");
    FillInset(g, RectF(266, kBaitDelayRowY + 5, 80, 30), 9.0f);
    for (int i = 0; i < 5; ++i) {
        const RectF chip(34.0f + i * 63.0f, kBaitAmountRowY + 46.0f, 58.0f, 26.0f);
        swprintf_s(line, L"%d", kBaitPresets[i]);
        if (gBaitAmount.load() == kBaitPresets[i]) GradientButton(g, chip, line, kHitBaitPreset0 + i, 13.0f);
        else InsetButton(g, chip, line, kHitBaitPreset0 + i, ui::kSoft, 13.0f);
    }

    // --- Click points -------------------------------------------------------------
    DrawCard(g, RectF(18, 346, 344, 7 * 28 + 10));
    for (int i = 0; i < kBaitClicks; ++i) {
        const float y = 351.0f + i * 28.0f;
        const bool active = running && gBaitClick.load() == i + 1;
        swprintf_s(line, L"%d", i + 1);
        Text(g, line, gFontLabel, active ? ui::kGradA : ui::kMuted, RectF(32, y, 14, 26));
        Text(g, kBaitClickNames[i], gFontBody, active ? ui::kGradA : ui::kText, RectF(48, y, 140, 26));
        const POINT bp = GetBaitPoint(i);
        if (BaitPointSet(i)) swprintf_s(line, L"(%ld, %ld)", bp.x, bp.y);
        else swprintf_s(line, L"not set");
        Text(g, line, gFontSmall, BaitPointSet(i) ? ui::kMuted : ui::kBad, RectF(188, y, 100, 26));
        const RectF button(292, y + 2, 56, 22);
        const bool calibratingThis = gCalibrating.load() &&
            gCalibrationTarget == CalibrationTarget::BaitPoint && gBaitCalibIndex == i;
        if (calibratingThis) InsetButton(g, button, L"...", kHitBaitCal0 + i, ui::kWarn, 11.0f);
        else InsetButton(g, button, L"Set", kHitBaitCal0 + i, ui::kSoft, 11.0f);
    }

    // --- Start / stop -----------------------------------------------------------------
    const RectF startR(18, 562, 344, 42);
    if (running) {
        DrawCard(g, startR);
        HoverOverlay(g, startR, 14.0f, kHitBaitStart);
        const std::wstring stop = L"STOP BUYING   ( " + KeyDisplayName(gBaitHotkeyVk.load()) + L" )";
        TextCenter(g, stop.c_str(), gFontButton, ui::kBad, startR);
        AddHit(startR, kHitBaitStart);
    } else if (AllBaitPointsSet()) {
        swprintf_s(line, L"BUY %d BAIT   ( %s )", cycles * kBaitPerCycle,
                   KeyDisplayName(gBaitHotkeyVk.load()).c_str());
        GradientButton(g, startR, line, kHitBaitStart, 14.0f);
    } else {
        FillInset(g, startR, 14.0f);
        TextCenter(g, L"SET ALL 7 CLICK POINTS FIRST", gFontButton, ui::kMuted, startR);
    }
}

// Sessions tab: numbered list of saved sessions (newest first), or one
// session opened with all its stats.
constexpr int kSessionsPerPage = 7;

void DrawSessionDetail(Gdiplus::Graphics& g, const SessionRecord& r) {
    wchar_t line[128];
    DrawCard(g, RectF(18, 128, 344, 70));
    swprintf_s(line, L"#%d", r.id);
    const RectF badge(34, 142, 56, 42);
    FillGradient(g, badge, 12.0f);
    TextCenter(g, line, gFontHead, ui::kText, badge);
    DrawCalendarIcon(g, 104, 143, ui::kMuted);
    Text(g, FormatDate(r.start).c_str(), gFontBody, ui::kText, RectF(122, 140, 110, 18));
    DrawClockIcon(g, 104, 165, ui::kMuted);
    swprintf_s(line, L"%ls – %ls", FormatClock(r.start).c_str(), FormatClock(r.end).c_str());
    Text(g, line, gFontBody, ui::kText, RectF(122, 162, 110, 18));
    if (r.id == gCurrentSessionId) {
        const RectF pill(294, 140, 52, 20);
        FillGradient(g, pill, 10.0f);
        TextCenter(g, L"LIVE", gFontLabel, ui::kText, pill);
    }

    // Tiles: same numbers as the Fishing tab, with time fished instead of "until reset".
    const int attempts = r.catches + r.noItem;
    struct TileInfo { const wchar_t* title; const wchar_t* sub; } tiles[4] = {
        {L"CATCHES", L"item collected"}, {L"NO ITEM", L"failed / caught nothing"},
        {L"SUCCESS RATE", L"collected / tries"}, {L"TIME FISHED", L"macro running"}};
    for (int i = 0; i < 4; ++i) {
        const float x = (i % 2) ? 196.0f : 18.0f;
        const float y = (i / 2) ? 296.0f : 210.0f;
        DrawCard(g, RectF(x, y, 166, 76));
        Text(g, tiles[i].title, gFontLabel, ui::kText, RectF(x + 14, y + 10, 140, 16));
        Text(g, tiles[i].sub, gFontSmall, ui::kMuted, RectF(x + 14, y + 25, 140, 14));
        const RectF value(x + 12, y + 40, 140, 32);
        if (i == 0) DrawCount(g, r.catches, value, gFontBig, ui::kText, Gdiplus::StringAlignmentNear);
        else if (i == 1) DrawCount(g, r.noItem, value, gFontBig, r.noItem ? ui::kBad : ui::kText,
                                   Gdiplus::StringAlignmentNear);
        else if (i == 2) {
            if (attempts > 0) swprintf_s(line, L"%d%%", r.catches * 100 / attempts);
            else swprintf_s(line, L"—");
            Text(g, line, gFontBig, ui::kText, value);
        } else {
            Text(g, FormatHoursMinutes(r.runningMs).c_str(), gFontBig, ui::kText, value);
        }
    }

    Telemetry t;
    t.oreCount = r.ore;
    t.runningMs = static_cast<double>(r.runningMs);
    for (int i = 0; i < 5; ++i) t.rarityCounts[i] = r.rarity[i];
    DrawOreRarityCard(g, t, 384.0f, false);

    InsetButton(g, RectF(18, 494, 120, 40), L"‹  Back", kHitSessBack, ui::kSoft, 12.0f);
    const RectF del(150, 494, 212, 40);
    if (r.id == gCurrentSessionId) {
        FillInset(g, del, 12.0f);
        TextCenter(g, L"Current session", gFontButton, ui::kMuted, del);
    } else if (gSessionDeleteArmed) {
        Gdiplus::SolidBrush red(Gdiplus::Color(255, 190, 60, 70));
        FillRound(g, red, del, 12.0f);
        HoverOverlay(g, del, 12.0f, kHitSessDelete);
        TextCenter(g, L"Click again to delete", gFontButton, ui::kText, del);
        AddHit(del, kHitSessDelete);
    } else {
        InsetButton(g, del, L"Delete session", kHitSessDelete, ui::kBad, 12.0f);
    }
}

void DrawSessionsTab(Gdiplus::Graphics& g) {
    if (const SessionRecord* open = FindSession(gSessionOpenId)) {
        DrawSessionDetail(g, *open);
        return;
    }
    gSessionOpenId = -1;
    wchar_t line[128];
    DrawCard(g, RectF(18, 128, 344, 54));
    Text(g, L"FISH SESSIONS", gFontLabel, ui::kText, RectF(34, 136, 300, 16));
    swprintf_s(line, L"%d saved · saved on exit and every minute", static_cast<int>(gSessions.size()));
    Text(g, line, gFontSmall, ui::kMuted, RectF(34, 156, 310, 16));

    const int count = static_cast<int>(gSessions.size());
    if (count == 0) {
        TextCenter(g, L"No sessions yet – start fishing!", gFontBody, ui::kMuted, RectF(18, 250, 344, 30));
        return;
    }
    const int pages = (count + kSessionsPerPage - 1) / kSessionsPerPage;
    gSessionPage = std::clamp(gSessionPage, 0, pages - 1);
    for (int k = 0; k < kSessionsPerPage; ++k) {
        const int index = count - 1 - (gSessionPage * kSessionsPerPage + k); // newest first
        if (index < 0) break;
        const SessionRecord& r = gSessions[index];
        const float y = 192.0f + k * 50.0f;
        const RectF row(18, y, 344, 44);
        DrawCard(g, row, 12.0f);
        HoverOverlay(g, row, 12.0f, kHitSessRow0 + k);
        AddHit(row, kHitSessRow0 + k);
        swprintf_s(line, L"#%d", r.id);
        const RectF badge(28, y + 10, 44, 24);
        if (r.id == gCurrentSessionId) FillGradient(g, badge, 12.0f);
        else FillInset(g, badge, 12.0f);
        TextCenter(g, line, gFontLabel, ui::kText, badge);
        DrawCalendarIcon(g, 82, y + 8, ui::kMuted);
        Text(g, FormatDate(r.start).c_str(), gFontSmall, ui::kSoft, RectF(98, y + 5, 88, 16));
        DrawClockIcon(g, 82, y + 24, ui::kMuted);
        swprintf_s(line, L"%ls – %ls", FormatClock(r.start).c_str(), FormatClock(r.end).c_str());
        Text(g, line, gFontSmall, ui::kSoft, RectF(98, y + 22, 88, 16));
        swprintf_s(line, L"%d catches", r.catches);
        Text(g, line, gFontLabel, ui::kText, RectF(190, y + 5, 160, 16), Gdiplus::StringAlignmentFar);
        swprintf_s(line, L"%d ORE · %ls", r.ore, FormatHoursMinutes(r.runningMs).c_str());
        Text(g, line, gFontSmall, r.ore ? ui::kGradA : ui::kMuted, RectF(190, y + 22, 160, 16),
             Gdiplus::StringAlignmentFar);
    }
    if (pages > 1) {
        if (gSessionPage > 0) InsetButton(g, RectF(18, 548, 100, 32), L"‹ Newer", kHitSessPrev, ui::kSoft, 11.0f);
        swprintf_s(line, L"%d / %d", gSessionPage + 1, pages);
        TextCenter(g, line, gFontSmall, ui::kMuted, RectF(118, 548, 144, 32));
        if (gSessionPage < pages - 1) InsetButton(g, RectF(262, 548, 100, 32), L"Older ›", kHitSessNext, ui::kSoft, 11.0f);
    }
}

// Tick box drawn at the start of a chip.
void DrawCheckChip(Gdiplus::Graphics& g, const RectF& chip, const wchar_t* label, bool on,
                   const Gdiplus::Color& dot, int id) {
    FillInset(g, chip, 11.0f);
    HoverOverlay(g, chip, 11.0f, id);
    const RectF box(chip.X + 8, chip.Y + 5, 16, 16);
    if (on) {
        FillGradient(g, box, 4.0f);
        Gdiplus::Pen tick(ui::kText, 2.0f);
        g.DrawLine(&tick, box.X + 3.5f, box.Y + 8.5f, box.X + 6.8f, box.Y + 11.8f);
        g.DrawLine(&tick, box.X + 6.8f, box.Y + 11.8f, box.X + 12.8f, box.Y + 4.8f);
    } else {
        Gdiplus::Pen outline(ui::kMuted, 1.3f);
        Gdiplus::GraphicsPath path;
        AddRoundRect(path, box, 4.0f);
        g.DrawPath(&outline, &path);
    }
    Gdiplus::SolidBrush dotBrush(dot);
    g.FillEllipse(&dotBrush, RectF(chip.X + 32, chip.Y + 9, 8, 8));
    Text(g, label, gFontSmall, on ? ui::kText : ui::kMuted, RectF(chip.X + 46, chip.Y, chip.Width - 50, chip.Height));
    AddHit(chip, id);
}

// Discord page. Its two EDIT controls sit on the inset boxes (see WM_CREATE).
constexpr float kDiscordUrlY = 150.0f;
constexpr float kDiscordMinRowY = 410.0f;
constexpr float kDiscordPingRowY = 499.0f;

void DrawDiscordTab(Gdiplus::Graphics& g) {
    wchar_t line[128];
    DrawCard(g, RectF(18, 128, 344, 100));
    Text(g, L"DISCORD WEBHOOK", gFontLabel, ui::kText, RectF(34, 134, 200, 14));
    InsetButton(g, RectF(282, 131, 64, 17), gDiscordHideUrl.load() ? L"Show" : L"Hide", kHitDiscordHide,
                ui::kSoft, 8.0f);
    FillInset(g, RectF(34, kDiscordUrlY, 312, 30), 9.0f);
    RowLabel(g, 184, L"Send messages", L"paste your channel's webhook link above");
    DrawToggle(g, 184, gDiscordEnabled.load(), kHitDiscordToggle);

    DrawCard(g, RectF(18, 238, 344, 124));
    Text(g, L"NOTIFY ME FOR", gFontLabel, ui::kText, RectF(34, 246, 200, 14));
    const wchar_t* names[6] = {L"Impossible (+ pic)", L"Mythic", L"ORE (+ pic)", L"Legendary", L"Rare", L"Common"};
    const Gdiplus::Color dots[6] = {Gdiplus::Color(255, 20, 20, 26), Gdiplus::Color(255, 248, 82, 82),
                                    Gdiplus::Color(255, 224, 36, 36), Gdiplus::Color(255, 250, 190, 50),
                                    Gdiplus::Color(255, 70, 150, 255), Gdiplus::Color(255, 165, 168, 185)};
    const int rarityOf[6] = {kRarityImpossible, kRarityMythic, -1, kRarityLegendary, kRarityRare, kRarityCommon};
    for (int i = 0; i < 6; ++i) {
        const RectF chip(34.0f + (i % 2) * 158.0f, 266.0f + (i / 2) * 31.0f, 154.0f, 26.0f);
        const bool on = rarityOf[i] < 0 ? gDiscordOre.load() : gDiscordRarity[rarityOf[i]].load();
        DrawCheckChip(g, chip, names[i], on, dots[i], kHitDiscordChip0 + i);
    }

    DrawCard(g, RectF(18, 372, 344, 168));
    RowLabel(g, 376, L"Failing to fish alert", L"3 fails in a row, with a screenshot");
    DrawToggle(g, 376, gDiscordFailAlerts.load(), kHitDiscordFail);
    Divider(g, kDiscordMinRowY - 3);
    RowLabel(g, kDiscordMinRowY, L"No catch for", L"minutes of fishing = failing too (0 = off)");
    FillInset(g, RectF(266, kDiscordMinRowY + 5, 80, 30), 9.0f);
    Divider(g, 453);
    RowLabel(g, 456, L"Ping", L"in every message");
    DrawToggle(g, 456, gDiscordPing.load(), kHitDiscordPing);
    Divider(g, kDiscordPingRowY - 3);
    RowLabel(g, kDiscordPingRowY, L"Ping who", L"everyone, or your user ID");
    FillInset(g, RectF(206, kDiscordPingRowY + 5, 140, 30), 9.0f);

    GradientButton(g, RectF(18, 550, 150, 40), L"Send test", kHitDiscordTest, 12.0f);
    const int status = gDiscordLastStatus.load();
    if (GetDiscordUrl().empty()) {
        swprintf_s(line, L"No webhook link yet");
    } else if (status == 0) {
        swprintf_s(line, gDiscordEnabled.load() ? L"Ready" : L"Off");
    } else if (status >= 200 && status < 300) {
        const time_t tt = static_cast<time_t>(gDiscordLastTime.load());
        tm local{};
        localtime_s(&local, &tt);
        swprintf_s(line, L"Last message sent ✓ %02d:%02d", local.tm_hour, local.tm_min);
    } else if (status < 0) {
        swprintf_s(line, L"Could not reach Discord");
    } else {
        swprintf_s(line, L"Discord error %d (check the link)", status);
    }
    const bool bad = status < 0 || status >= 300;
    Text(g, line, gFontSmall, bad && status != 0 ? ui::kBad : ui::kSoft, RectF(178, 550, 184, 40));
}

// Credits page.
void DrawCreditsTab(Gdiplus::Graphics& g) {
    DrawCard(g, RectF(18, 128, 344, 460));
    if (gCloudIcon) {
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.DrawImage(gCloudIcon, RectF(80, 150, 220, 162)); // 312x230 logo
    }
    TextCenter(g, L"Sky's S2 Fishing Macro", gFontHead, ui::kText, RectF(18, 324, 344, 26));
    TextCenter(g, L"MADE BY", gFontLabel, ui::kMuted, RectF(18, 364, 344, 16));
    {
        // "Sky" in the gradient, with a small trademark sign.
        Gdiplus::Font big(L"Segoe UI", 54.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::StringFormat format;
        format.SetAlignment(Gdiplus::StringAlignmentCenter);
        format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        const RectF area(18, 378, 344, 72);
        Gdiplus::LinearGradientBrush brush(RectF(130, 380, 120, 70), ui::kGradA, ui::kGradB,
                                           Gdiplus::LinearGradientModeHorizontal);
        g.DrawString(L"Sky", -1, &big, area, &format, &brush);
        Gdiplus::RectF bounds;
        g.MeasureString(L"Sky", -1, &big, area, &format, &bounds);
        Text(g, L"™", gFontButton, ui::kSoft, RectF(bounds.X + bounds.Width - 4, 390, 24, 18));
    }
    Divider(g, 462);
    TextCenter(g, L"Version 3.0  ·  Project Slayers 2 auto fishing", gFontSmall, ui::kMuted,
               RectF(18, 470, 344, 18));
    TextCenter(g, L"Special thanks: midnytejay (original Fishing Hyper Tracker)", gFontSmall, ui::kMuted,
               RectF(18, 492, 344, 18));
    TextCenter(g, L"© 2026 Sky", gFontLabel, ui::kSoft, RectF(18, 548, 344, 20));
}

// Burger menu: pages in menu order -> page index used by gActiveTab.
const wchar_t* const kPageNames[8] = {L"Fishing", L"Settings", L"Hotkeys", L"Setup",
                                      L"Auto Bait", L"Sessions", L"Discord Webhook", L"Credits"};
const int kMenuOrder[8] = {0, 5, 1, 2, 3, 4, 6, 7};

void DrawMenu(Gdiplus::Graphics& g) {
    Gdiplus::SolidBrush dim(Gdiplus::Color(140, 18, 19, 36));
    g.FillRectangle(&dim, RectF(0, 114, static_cast<float>(ui::kWidth), static_cast<float>(ui::kHeight) - 114));
    const RectF card(22, 118, 210, 8 * 38 + 10);
    DrawCard(g, card, 14.0f);
    std::vector<HitRegion> hits;
    for (int k = 0; k < 8; ++k) {
        const int page = kMenuOrder[k];
        const RectF item(28, 123.0f + k * 38.0f, 198, 34);
        const int id = kHitMenuItem0 + k;
        if (gActiveTab == page) FillGradient(g, item, 10.0f);
        else HoverOverlay(g, item, 10.0f, id);
        Text(g, kPageNames[page], gFontButton, gActiveTab == page ? ui::kText : ui::kSoft,
             RectF(item.X + 14, item.Y, item.Width - 20, item.Height));
        hits.push_back({RECT{static_cast<LONG>(item.X), static_cast<LONG>(item.Y),
                             static_cast<LONG>(item.X + item.Width), static_cast<LONG>(item.Y + item.Height)}, id});
    }
    // Menu items and the burger come first, then a backdrop that closes the
    // menu; the page underneath gets no clicks while the menu is open.
    for (const HitRegion& h : gHits) {
        if (h.id == kHitMenu || h.id == kHitClose || h.id == kHitMinimize) hits.push_back(h);
    }
    hits.push_back({RECT{0, 72, ui::kWidth, ui::kHeight}, kHitMenuBackdrop});
    gHits = hits;
}

void PaintHud(HWND hwnd) {
    PAINTSTRUCT ps{};
    HDC windowDc = BeginPaint(hwnd, &ps);
    RECT client{};
    GetClientRect(hwnd, &client);

    // Double-buffered to avoid flicker: draw off-screen, blit once.
    HDC dc = CreateCompatibleDC(windowDc);
    HBITMAP memBitmap = CreateCompatibleBitmap(windowDc, client.right, client.bottom);
    HGDIOBJ oldBitmap = SelectObject(dc, memBitmap);

    Telemetry t;
    {
        std::lock_guard<std::mutex> lock(gTelemetryMutex);
        t = gTelemetry;
    }

    gHits.clear();
    {
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
        g.Clear(ui::kBg);

        // Header: cloud logo, title, minimize/close.
        if (gCloudIcon) {
            g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            g.DrawImage(gCloudIcon, RectF(14, 18, 48, 35)); // 312x230 logo
        }
        Text(g, L"Sky's S2 Fishing Macro", gFontTitle, ui::kText, RectF(68, 15, 250, 24));
        Text(g, L"Project Slayers 2  ·  by Sky", gFontSmall, ui::kMuted, RectF(68, 38, 250, 16));
        const RectF closeR(static_cast<float>(client.right) - 46.0f, 22.0f, 28.0f, 28.0f);
        HoverOverlay(g, closeR, 9.0f, kHitClose);
        TextCenter(g, L"✕", gFontBody, gHoverHit == kHitClose ? ui::kText : ui::kMuted, closeR);
        AddHit(closeR, kHitClose);
        const RectF minR(closeR.X - 34.0f, closeR.Y, 28.0f, 28.0f);
        HoverOverlay(g, minR, 9.0f, kHitMinimize);
        {
            Gdiplus::Pen pen(gHoverHit == kHitMinimize ? ui::kText : ui::kMuted, 1.6f);
            g.DrawLine(&pen, minR.X + 9.0f, minR.Y + 14.5f, minR.X + 19.0f, minR.Y + 14.5f);
        }
        AddHit(minR, kHitMinimize);

        // Burger menu bar: the button plus the name of the current page.
        DrawCard(g, RectF(18, 72, 344, 42));
        {
            const RectF burger(22, 76, 42, 34);
            if (gMenuOpen) FillGradient(g, burger, 10.0f);
            else HoverOverlay(g, burger, 10.0f, kHitMenu);
            Gdiplus::Pen bars(ui::kText, 2.2f);
            bars.SetStartCap(Gdiplus::LineCapRound);
            bars.SetEndCap(Gdiplus::LineCapRound);
            for (int i = 0; i < 3; ++i) {
                const float y = 86.0f + i * 7.0f;
                g.DrawLine(&bars, 34.0f, y, 52.0f, y);
            }
            AddHit(burger, kHitMenu);
            Text(g, kPageNames[std::clamp(gActiveTab, 0, 7)], gFontButton, ui::kText, RectF(74, 76, 200, 34));
        }

        if (gActiveTab == 0) DrawFishingTab(g, t);
        else if (gActiveTab == 1) DrawSettingsTab(g);
        else if (gActiveTab == 2) DrawHotkeysTab(g);
        else if (gActiveTab == 3) DrawSetupTab(g);
        else if (gActiveTab == 4) DrawBaitTab(g);
        else if (gActiveTab == 5) DrawSessionsTab(g);
        else if (gActiveTab == 6) DrawDiscordTab(g);
        else DrawCreditsTab(g);
        if (gActiveTab == 0 && gRestorePromptId >= 0) DrawRestorePrompt(g);
        if (gMenuOpen) DrawMenu(g);
    }

    BitBlt(windowDc, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBitmap);
    DeleteObject(memBitmap);
    DeleteDC(dc);
    EndPaint(hwnd, &ps);
}

void CreateUiFonts() {
    gFontTitle = new Gdiplus::Font(L"Segoe UI", 18.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    gFontHead = new Gdiplus::Font(L"Segoe UI", 19.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    gFontBig = new Gdiplus::Font(L"Segoe UI", 26.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    gFontRow = new Gdiplus::Font(L"Segoe UI Semibold", 14.0f, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    gFontButton = new Gdiplus::Font(L"Segoe UI", 13.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    gFontLabel = new Gdiplus::Font(L"Segoe UI", 11.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    gFontBody = new Gdiplus::Font(L"Segoe UI", 13.0f, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    gFontSmall = new Gdiplus::Font(L"Segoe UI", 11.0f, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    gFontTab = new Gdiplus::Font(L"Segoe UI", 11.5f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
}

void DestroyUiFonts() {
    for (Gdiplus::Font** f : {&gFontTitle, &gFontHead, &gFontBig, &gFontRow, &gFontButton,
                              &gFontLabel, &gFontBody, &gFontSmall, &gFontTab}) {
        delete *f;
        *f = nullptr;
    }
}

void UpdateButtonLabel() {
    if (gToggleButton) {
        const std::wstring key = KeyDisplayName(gToggleHotkeyVk.load());
        const std::wstring label = (gEnabled.load() ? L"Pause  (" : L"Start  (") + key + L")";
        SetWindowText(gToggleButton, label.c_str());
    }
}

void UpdateExitButtonLabel() {
    if (gExitButton) {
        const std::wstring label = L"Exit  (" + KeyDisplayName(gQuitHotkeyVk.load()) + L")";
        SetWindowText(gExitButton, label.c_str());
    }
}

void ToggleTracker() {
    if (gBaitRunning.load()) {
        StopBaitBuying();
        return;
    }
    gEnabled.store(!gEnabled.load());
    if (!gEnabled.load()) MouseButton(false);
    if (gEnabled.load()) {
        gRestorePromptId = -1;
        if (gCurrentSessionStart == 0) gCurrentSessionStart = static_cast<long long>(time(nullptr));
    }
    UpdateButtonLabel();
    InvalidateRect(gWindow, nullptr, FALSE);
}

RECT gPendingAnchor{0, 0, 0, 0}; // anchor box just dragged (reference captured in WindowProc)

// While calibrating, a low-level mouse hook swallows left-button input and
// reports it back to the window (via WM_CALIBRATION_DONE) instead of
// letting it reach whatever is underneath. The Cast point is a single
// click; the bar/exit regions are a click-drag (mousedown = one corner,
// mouseup = the opposite corner). Actually finishing calibration
// (unhooking, restoring button labels) happens in WindowProc, not here,
// since a low-level hook procedure should stay minimal and return quickly.
LRESULT CALLBACK CalibrationMouseProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code != HC_ACTION || (wParam != WM_LBUTTONDOWN && wParam != WM_LBUTTONUP)) {
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }
    const auto* info = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
    // Ignore our own SendInput-generated clicks (e.g. the Cast phase's
    // click firing mid-calibration) - only real, physical input counts.
    if (info->flags & LLMHF_INJECTED) {
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }
    const POINT pt{info->pt.x, info->pt.y};

    if (gCalibrationTarget == CalibrationTarget::BaitPoint) {
        if (wParam != WM_LBUTTONDOWN) return CallNextHookEx(nullptr, code, wParam, lParam);
        SetBaitPoint(gBaitCalibIndex, pt);
        PostMessage(gWindow, WM_CALIBRATION_DONE, 0, 0);
        return 1; // swallow: calibrating must not already press the shop button
    }

    if (gCalibrationTarget == CalibrationTarget::CastPoint) {
        if (wParam != WM_LBUTTONDOWN) return CallNextHookEx(nullptr, code, wParam, lParam);
        SetCastPoint(pt);
        // Remember which top-level window is under that point, so Cast
        // clicks and key holds can force it to the foreground later.
        if (HWND under = WindowFromPoint(pt)) {
            gTargetWindow.store(GetAncestor(under, GA_ROOT));
        }
        PostMessage(gWindow, WM_CALIBRATION_DONE, 0, 0);
        return 1;
    }

    if (gCalibrationTarget == CalibrationTarget::BarRegion ||
        gCalibrationTarget == CalibrationTarget::ExitRegion ||
        gCalibrationTarget == CalibrationTarget::CollectRegion ||
        gCalibrationTarget == CalibrationTarget::AnchorRegion) {
        if (wParam == WM_LBUTTONDOWN) {
            gDragStart = pt;
            gDragging = true;
            return 1; // Swallow so the drag doesn't also click the game.
        }
        if (wParam == WM_LBUTTONUP && gDragging) {
            gDragging = false;
            const RECT rect{std::min(gDragStart.x, pt.x), std::min(gDragStart.y, pt.y),
                             std::max(gDragStart.x, pt.x), std::max(gDragStart.y, pt.y)};
            if (RectCalibrated(rect)) {
                if (gCalibrationTarget == CalibrationTarget::BarRegion) SetBarRect(rect);
                else if (gCalibrationTarget == CalibrationTarget::ExitRegion) SetExitRect(rect);
                else if (gCalibrationTarget == CalibrationTarget::AnchorRegion) gPendingAnchor = rect;
                else SetCollectRect(rect);
                PostMessage(gWindow, WM_CALIBRATION_DONE, 0, 0);
            }
            // Too small to be a real drag (likely an accidental click) -
            // swallow it and silently let the user try the drag again;
            // calibration mode stays active either way.
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

void StartCalibration(CalibrationTarget target) {
    if (gCalibrationTarget != CalibrationTarget::None || gBindingHotkey.load()) return;
    gMouseHook = SetWindowsHookEx(WH_MOUSE_LL, CalibrationMouseProc,
                                  GetModuleHandle(nullptr), 0);
    if (!gMouseHook) return;
    gCalibrationTarget = target;
    gDragging = false;
    gCalibrating.store(true);
    RegisterHotKey(gWindow, HOTKEY_CANCEL_CAPTURE, MOD_NOREPEAT, VK_ESCAPE);
    if (target == CalibrationTarget::CastPoint && gCalibrateButton) {
        SetWindowText(gCalibrateButton, L"Click anywhere...  (Esc cancels)");
    } else if (target == CalibrationTarget::BarRegion && gCalibrateBarButton) {
        SetWindowText(gCalibrateBarButton, L"Drag over the bar...  (Esc cancels)");
    } else if (target == CalibrationTarget::ExitRegion && gCalibrateExitButton) {
        SetWindowText(gCalibrateExitButton, L"Drag over Exit...  (Esc cancels)");
    }
    InvalidateRect(gWindow, nullptr, FALSE);
}

void EndCalibration() {
    if (gMouseHook) {
        UnhookWindowsHookEx(gMouseHook);
        gMouseHook = nullptr;
    }
    UnregisterHotKey(gWindow, HOTKEY_CANCEL_CAPTURE);
    gCalibrationTarget = CalibrationTarget::None;
    gDragging = false;
    gCalibrating.store(false);
    if (gCalibrateButton) SetWindowText(gCalibrateButton, L"Calibrate Cast Point");
    if (gCalibrateBarButton) SetWindowText(gCalibrateBarButton, L"Calibrate Fishing Bar");
    if (gCalibrateExitButton) SetWindowText(gCalibrateExitButton, L"Calibrate Exit Button");
    InvalidateRect(gWindow, nullptr, FALSE);
}

void UpdateHotkeyButtonLabels() {
    if (gToggleBindButton) {
        const std::wstring label = L"Start/Pause: " + KeyDisplayName(gToggleHotkeyVk.load()) + L"  (click to change)";
        SetWindowText(gToggleBindButton, label.c_str());
    }
    if (gExitBindButton) {
        const std::wstring label = L"Exit: " + KeyDisplayName(gQuitHotkeyVk.load()) + L"  (click to change)";
        SetWindowText(gExitBindButton, label.c_str());
    }
}

// Settings persist to a small INI file next to the exe, so rebound
// Start/Pause and Exit hotkeys survive closing and reopening the program.
std::wstring GetSettingsPath() {
    wchar_t modulePath[MAX_PATH];
    const DWORD len = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    const std::wstring module(modulePath, (len > 0 && len < MAX_PATH) ? len : 0);
    const size_t slash = module.find_last_of(L"\\/");
    const std::wstring dir = (slash == std::wstring::npos) ? L"" : module.substr(0, slash + 1);
    const std::wstring path = dir + L"SkysS2FishingMacro.ini";
    // Carry over settings/calibration from the old Fishing Hyper Tracker name.
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        CopyFileW((dir + L"FishingHyperTracker.ini").c_str(), path.c_str(), TRUE);
    }
    return path;
}

// GetPrivateProfileInt is documented/typed as unsigned, which doesn't
// suit screen coordinates (can be negative on multi-monitor setups with a
// display above/left of the primary) - read/write coordinates as signed
// text instead.
constexpr int kNoSavedValue = -2147483647;

int ReadIntSetting(const wchar_t* section, const wchar_t* key, int def, const std::wstring& path) {
    wchar_t buf[32]{};
    GetPrivateProfileStringW(section, key, L"", buf, 32, path.c_str());
    if (buf[0] == L'\0') return def;
    wchar_t* end = nullptr;
    const long value = wcstol(buf, &end, 10);
    if (end == buf) return def;
    return static_cast<int>(value);
}

void WriteIntSetting(const wchar_t* section, const wchar_t* key, int value, const std::wstring& path) {
    wchar_t buf[16];
    swprintf_s(buf, L"%d", value);
    WritePrivateProfileStringW(section, key, buf, path.c_str());
}

void LoadSettings() {
    const std::wstring path = GetSettingsPath();
    const UINT toggleVk = GetPrivateProfileIntW(L"Hotkeys", L"ToggleVk",
                                                static_cast<UINT>(VK_F6), path.c_str());
    const UINT quitVk = GetPrivateProfileIntW(L"Hotkeys", L"QuitVk",
                                              static_cast<UINT>(VK_F8), path.c_str());
    if (toggleVk > 0 && toggleVk <= 0xFF) gToggleHotkeyVk.store(static_cast<WORD>(toggleVk));
    if (quitVk > 0 && quitVk <= 0xFF) gQuitHotkeyVk.store(static_cast<WORD>(quitVk));
    const UINT baitVk = GetPrivateProfileIntW(L"Hotkeys", L"BaitVk", static_cast<UINT>(VK_F3), path.c_str());
    if (baitVk > 0 && baitVk <= 0xFF) gBaitHotkeyVk.store(static_cast<WORD>(baitVk));
    const UINT waitSeconds = GetPrivateProfileIntW(L"Fishing", L"WaitForFishSeconds",
                                                    static_cast<UINT>(kDefaultWaitForFishMs / 1000),
                                                    path.c_str());
    const ULONGLONG waitMs = std::clamp<ULONGLONG>(
        static_cast<ULONGLONG>(waitSeconds) * 1000ULL,
        kMinWaitForFishMs, kMaxWaitForFishMs);
    gWaitForFishMs.store(waitMs);
    gAutoRespawn.store(GetPrivateProfileIntW(L"Fishing", L"AutoRespawn", 0, path.c_str()) != 0);
    gRepositionMode.store(std::clamp(ReadIntSetting(L"Reposition", L"Mode", gAutoRespawn.load() ? 1 : 0, path), 0, 2));
    gAutoRespawn.store(gRepositionMode.load() == 1);
    {
        const RECT anchor{ReadIntSetting(L"Reposition", L"AnchorLeft", 0, path),
                          ReadIntSetting(L"Reposition", L"AnchorTop", 0, path),
                          ReadIntSetting(L"Reposition", L"AnchorRight", 0, path),
                          ReadIntSetting(L"Reposition", L"AnchorBottom", 0, path)};
        LoadAnchorReference(anchor);
        const wchar_t* names[4] = {L"W", L"A", L"S", L"D"};
        bool learned = ReadIntSetting(L"Reposition", L"Learned", 0, path) != 0;
        std::lock_guard<std::mutex> lock(gAnchorMutex);
        for (int k = 0; k < 4; ++k) {
            wchar_t kx[16], ky[16];
            swprintf_s(kx, L"%lsx100", names[k]);
            swprintf_s(ky, L"%lsy100", names[k]);
            gWalkSteps[k].x = ReadIntSetting(L"Reposition", kx, 0, path) / 100.0f;
            gWalkSteps[k].y = ReadIntSetting(L"Reposition", ky, 0, path) / 100.0f;
        }
        gWalkLearned = learned && !gAnchorEdges.empty();
    }
    gRespawnEveryCatches.store(std::clamp<int>(
        ReadIntSetting(L"Fishing", L"RespawnEveryCatches", kDefaultRespawnEveryCatches, path),
        kMinRespawnEveryCatches, kMaxRespawnEveryCatches));
    {
        // Stored in ms now (0.1 s steps); older ini files only have whole seconds.
        const int seconds = ReadIntSetting(L"Fishing", L"CollectDelaySeconds",
                                           static_cast<int>(kDefaultCollectDelayMs / 1000), path);
        const int ms = ReadIntSetting(L"Fishing", L"CollectDelayMs", seconds * 1000, path);
        gCollectDelayMs.store(std::min<ULONGLONG>(static_cast<ULONGLONG>(std::max(0, ms)), kMaxCollectDelayMs));
    }
    gMinigameLog.store(GetPrivateProfileIntW(L"Debug", L"MinigameLog", 0, path.c_str()) != 0);
    {
        const int seconds = ReadIntSetting(L"Fishing", L"HoldKeySeconds",
                                           static_cast<int>(kDefaultHoldKeyMs / 1000), path);
        const int ms = ReadIntSetting(L"Fishing", L"HoldKeyMs", seconds * 1000, path);
        gHoldKeyMs.store(std::clamp<ULONGLONG>(static_cast<ULONGLONG>(std::max(0, ms)), kMinHoldKeyMs, kMaxHoldKeyMs));
    }
    {
        const int amount = ReadIntSetting(L"Bait", L"Amount", kBaitPerCycle, path);
        gBaitAmount.store(std::clamp((amount + kBaitPerCycle - 1) / kBaitPerCycle, 1, 999) * kBaitPerCycle);
        gBaitDelayMs.store(std::clamp(ReadIntSetting(L"Bait", L"DelayMs", 600, path), 100, 5000));
        for (int i = 0; i < kBaitClicks; ++i) {
            wchar_t kx[16], ky[16];
            swprintf_s(kx, L"P%dX", i + 1);
            swprintf_s(ky, L"P%dY", i + 1);
            SetBaitPoint(i, POINT{ReadIntSetting(L"Bait", kx, 0, path), ReadIntSetting(L"Bait", ky, 0, path)});
        }
    }
    {
        wchar_t url[1024] = {};
        GetPrivateProfileStringW(L"Discord", L"WebhookUrl", L"", url, 1024, path.c_str());
        {
            std::lock_guard<std::mutex> lock(gDiscordUrlMutex);
            gDiscordUrl = url;
        }
        gDiscordEnabled.store(ReadIntSetting(L"Discord", L"Enabled", 0, path) != 0);
        gDiscordPing.store(ReadIntSetting(L"Discord", L"Ping", 0, path) != 0);
        gDiscordHideUrl.store(ReadIntSetting(L"Discord", L"HideUrl", 0, path) != 0);
        wchar_t who[128] = {};
        GetPrivateProfileStringW(L"Discord", L"PingWho", L"everyone", who, 128, path.c_str());
        {
            std::lock_guard<std::mutex> lock(gDiscordUrlMutex);
            gDiscordPingWho = who;
        }
        gDiscordFailAlerts.store(ReadIntSetting(L"Discord", L"FailAlerts", 1, path) != 0);
        gDiscordOre.store(ReadIntSetting(L"Discord", L"Ore", 1, path) != 0);
        gDiscordNoCatchMin.store(std::clamp(ReadIntSetting(L"Discord", L"NoCatchMinutes", 10, path), 0, 999));
        const int defaults[5] = {1, 1, 1, 0, 0};
        for (int i = 0; i < 5; ++i) {
            wchar_t key[16];
            swprintf_s(key, L"Rarity%d", i);
            gDiscordRarity[i].store(ReadIntSetting(L"Discord", key, defaults[i], path) != 0);
        }
    }
    const UINT correctionVk = GetPrivateProfileIntW(L"Keys", L"CorrectionVk", '1', path.c_str());
    const UINT rodVk = GetPrivateProfileIntW(L"Keys", L"RodVk", '5', path.c_str());
    if (correctionVk > 0 && correctionVk <= 0xFF) gCorrectionKeyVk.store(static_cast<WORD>(correctionVk));
    if (rodVk > 0 && rodVk <= 0xFF) gRodKeyVk.store(static_cast<WORD>(rodVk));

    // Calibration - each user's own cast point/bar/exit regions, saved so
    // they don't have to recalibrate on every single launch.
    const int castX = ReadIntSetting(L"Calibration", L"CastX", kNoSavedValue, path);
    const int castY = ReadIntSetting(L"Calibration", L"CastY", kNoSavedValue, path);
    if (castX != kNoSavedValue && castY != kNoSavedValue) {
        SetCastPoint(POINT{castX, castY});
        if (HWND under = WindowFromPoint(POINT{castX, castY})) {
            gTargetWindow.store(GetAncestor(under, GA_ROOT));
        }
    }

    const RECT bar{ReadIntSetting(L"Calibration", L"BarLeft", 0, path),
                    ReadIntSetting(L"Calibration", L"BarTop", 0, path),
                    ReadIntSetting(L"Calibration", L"BarRight", 0, path),
                    ReadIntSetting(L"Calibration", L"BarBottom", 0, path)};
    if (RectCalibrated(bar)) SetBarRect(bar);

    const RECT exit{ReadIntSetting(L"Calibration", L"ExitLeft", 0, path),
                     ReadIntSetting(L"Calibration", L"ExitTop", 0, path),
                     ReadIntSetting(L"Calibration", L"ExitRight", 0, path),
                     ReadIntSetting(L"Calibration", L"ExitBottom", 0, path)};
    if (RectCalibrated(exit)) SetExitRect(exit);

    const RECT collect{ReadIntSetting(L"Calibration", L"CollectLeft", 0, path),
                       ReadIntSetting(L"Calibration", L"CollectTop", 0, path),
                       ReadIntSetting(L"Calibration", L"CollectRight", 0, path),
                       ReadIntSetting(L"Calibration", L"CollectBottom", 0, path)};
    if (RectCalibrated(collect)) SetCollectRect(collect);
}

void SaveSettings() {
    const std::wstring path = GetSettingsPath();
    wchar_t buf[16];
    swprintf_s(buf, L"%u", static_cast<unsigned>(gToggleHotkeyVk.load()));
    WritePrivateProfileStringW(L"Hotkeys", L"ToggleVk", buf, path.c_str());
    swprintf_s(buf, L"%u", static_cast<unsigned>(gQuitHotkeyVk.load()));
    WritePrivateProfileStringW(L"Hotkeys", L"QuitVk", buf, path.c_str());
    swprintf_s(buf, L"%u", static_cast<unsigned>(gBaitHotkeyVk.load()));
    WritePrivateProfileStringW(L"Hotkeys", L"BaitVk", buf, path.c_str());
    swprintf_s(buf, L"%u", static_cast<unsigned>(gWaitForFishMs.load() / 1000));
    WritePrivateProfileStringW(L"Fishing", L"WaitForFishSeconds", buf, path.c_str());
    WriteIntSetting(L"Fishing", L"AutoRespawn", gAutoRespawn.load() ? 1 : 0, path);
    WriteIntSetting(L"Reposition", L"Mode", gRepositionMode.load(), path);
    {
        const RECT anchor = GetAnchorRect();
        WriteIntSetting(L"Reposition", L"AnchorLeft", anchor.left, path);
        WriteIntSetting(L"Reposition", L"AnchorTop", anchor.top, path);
        WriteIntSetting(L"Reposition", L"AnchorRight", anchor.right, path);
        WriteIntSetting(L"Reposition", L"AnchorBottom", anchor.bottom, path);
        const wchar_t* names[4] = {L"W", L"A", L"S", L"D"};
        std::lock_guard<std::mutex> lock(gAnchorMutex);
        WriteIntSetting(L"Reposition", L"Learned", gWalkLearned ? 1 : 0, path);
        for (int k = 0; k < 4; ++k) {
            wchar_t kx[16], ky[16];
            swprintf_s(kx, L"%lsx100", names[k]);
            swprintf_s(ky, L"%lsy100", names[k]);
            WriteIntSetting(L"Reposition", kx, static_cast<int>(std::lround(gWalkSteps[k].x * 100)), path);
            WriteIntSetting(L"Reposition", ky, static_cast<int>(std::lround(gWalkSteps[k].y * 100)), path);
        }
    }
    WriteIntSetting(L"Fishing", L"RespawnEveryCatches", gRespawnEveryCatches.load(), path);
    WriteIntSetting(L"Fishing", L"CollectDelayMs", static_cast<int>(gCollectDelayMs.load()), path);
    WriteIntSetting(L"Fishing", L"HoldKeyMs", static_cast<int>(gHoldKeyMs.load()), path);
    WriteIntSetting(L"Bait", L"Amount", gBaitAmount.load(), path);
    WriteIntSetting(L"Bait", L"DelayMs", gBaitDelayMs.load(), path);
    for (int i = 0; i < kBaitClicks; ++i) {
        wchar_t kx[16], ky[16];
        swprintf_s(kx, L"P%dX", i + 1);
        swprintf_s(ky, L"P%dY", i + 1);
        const POINT bp = GetBaitPoint(i);
        WriteIntSetting(L"Bait", kx, bp.x, path);
        WriteIntSetting(L"Bait", ky, bp.y, path);
    }
    WriteIntSetting(L"Debug", L"MinigameLog", gMinigameLog.load() ? 1 : 0, path);
    WritePrivateProfileStringW(L"Discord", L"WebhookUrl", GetDiscordUrl().c_str(), path.c_str());
    WriteIntSetting(L"Discord", L"Enabled", gDiscordEnabled.load() ? 1 : 0, path);
    WriteIntSetting(L"Discord", L"Ping", gDiscordPing.load() ? 1 : 0, path);
    WriteIntSetting(L"Discord", L"HideUrl", gDiscordHideUrl.load() ? 1 : 0, path);
    WritePrivateProfileStringW(L"Discord", L"PingWho", GetDiscordPingWho().c_str(), path.c_str());
    WriteIntSetting(L"Discord", L"FailAlerts", gDiscordFailAlerts.load() ? 1 : 0, path);
    WriteIntSetting(L"Discord", L"Ore", gDiscordOre.load() ? 1 : 0, path);
    WriteIntSetting(L"Discord", L"NoCatchMinutes", gDiscordNoCatchMin.load(), path);
    for (int i = 0; i < 5; ++i) {
        wchar_t key[16];
        swprintf_s(key, L"Rarity%d", i);
        WriteIntSetting(L"Discord", key, gDiscordRarity[i].load() ? 1 : 0, path);
    }
    WriteIntSetting(L"Keys", L"CorrectionVk", gCorrectionKeyVk.load(), path);
    WriteIntSetting(L"Keys", L"RodVk", gRodKeyVk.load(), path);

    const POINT cast = GetCastPoint();
    WriteIntSetting(L"Calibration", L"CastX", cast.x, path);
    WriteIntSetting(L"Calibration", L"CastY", cast.y, path);

    const RECT bar = GetBarRect();
    WriteIntSetting(L"Calibration", L"BarLeft", bar.left, path);
    WriteIntSetting(L"Calibration", L"BarTop", bar.top, path);
    WriteIntSetting(L"Calibration", L"BarRight", bar.right, path);
    WriteIntSetting(L"Calibration", L"BarBottom", bar.bottom, path);

    const RECT exit = GetExitRect();
    WriteIntSetting(L"Calibration", L"ExitLeft", exit.left, path);
    WriteIntSetting(L"Calibration", L"ExitTop", exit.top, path);
    WriteIntSetting(L"Calibration", L"ExitRight", exit.right, path);
    WriteIntSetting(L"Calibration", L"ExitBottom", exit.bottom, path);

    const RECT collect = GetCollectRect();
    WriteIntSetting(L"Calibration", L"CollectLeft", collect.left, path);
    WriteIntSetting(L"Calibration", L"CollectTop", collect.top, path);
    WriteIntSetting(L"Calibration", L"CollectRight", collect.right, path);
    WriteIntSetting(L"Calibration", L"CollectBottom", collect.bottom, path);
}

void ApplyToggleHotkey(WORD vk) {
    UnregisterHotKey(gWindow, HOTKEY_TOGGLE);
    RegisterHotKey(gWindow, HOTKEY_TOGGLE, MOD_NOREPEAT, vk);
    gToggleHotkeyVk.store(vk);
    UpdateButtonLabel();
    SaveSettings();
}

void ApplyBaitHotkey(WORD vk) {
    UnregisterHotKey(gWindow, HOTKEY_BAIT);
    RegisterHotKey(gWindow, HOTKEY_BAIT, MOD_NOREPEAT, vk);
    gBaitHotkeyVk.store(vk);
    SaveSettings();
}

void ApplyQuitHotkey(WORD vk) {
    UnregisterHotKey(gWindow, HOTKEY_QUIT);
    RegisterHotKey(gWindow, HOTKEY_QUIT, MOD_NOREPEAT, vk);
    gQuitHotkeyVk.store(vk);
    UpdateExitButtonLabel();
    SaveSettings();
}

// Same idea as CalibrationMouseProc, but for keyboard: swallows the next
// real keypress and reports it back (via WM_KEYBIND_DONE) to be bound as
// whichever hotkey is currently being rebound, instead of letting it reach
// whatever window currently has focus.
LRESULT CALLBACK HotkeyBindProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && wParam == WM_KEYDOWN) {
        const auto* info = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        if (!(info->flags & LLKHF_INJECTED) && info->vkCode != VK_ESCAPE) {
            gCapturedVk = static_cast<WORD>(info->vkCode);
            PostMessage(gWindow, WM_KEYBIND_DONE, 0, 0);
            return 1; // Swallow it so it doesn't also type into whatever's focused.
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

void StartHotkeyBind(int target) {
    if (gBindingHotkey.load() || gCalibrating.load()) return;
    gKeyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, HotkeyBindProc,
                                     GetModuleHandle(nullptr), 0);
    if (!gKeyboardHook) return;
    gBindingTarget = target;
    gBindingHotkey.store(true);
    RegisterHotKey(gWindow, HOTKEY_CANCEL_CAPTURE, MOD_NOREPEAT, VK_ESCAPE);
    HWND button = (target == 1) ? gToggleBindButton : gExitBindButton;
    if (button) SetWindowText(button, L"Press a key...  (Esc cancels)");
    InvalidateRect(gWindow, nullptr, FALSE);
}

void EndHotkeyBind() {
    if (gKeyboardHook) {
        UnhookWindowsHookEx(gKeyboardHook);
        gKeyboardHook = nullptr;
    }
    UnregisterHotKey(gWindow, HOTKEY_CANCEL_CAPTURE);
    gBindingHotkey.store(false);
    gBindingTarget = 0;
    UpdateHotkeyButtonLabels();
    InvalidateRect(gWindow, nullptr, FALSE);
}

void SetActiveTab(int tab) {
    gActiveTab = tab;
    // Edit controls are real child windows: hide them all while the menu is
    // open so they don't show through it.
    const int show = (tab == 1 && !gMenuOpen) ? SW_SHOW : SW_HIDE;
    const int showBait = (tab == 4 && !gMenuOpen) ? SW_SHOW : SW_HIDE;
    const int showDiscord = (tab == 6 && !gMenuOpen) ? SW_SHOW : SW_HIDE;
    if (gDiscordUrlEdit) ShowWindow(gDiscordUrlEdit, showDiscord);
    if (gDiscordMinEdit) ShowWindow(gDiscordMinEdit, showDiscord);
    if (gDiscordPingEdit) ShowWindow(gDiscordPingEdit, showDiscord);
    if (gBaitAmountEdit) ShowWindow(gBaitAmountEdit, showBait);
    if (gBaitDelayEdit) ShowWindow(gBaitDelayEdit, showBait);
    if (gHoldKeyEdit) ShowWindow(gHoldKeyEdit, show);
    if (gWaitFishEdit) ShowWindow(gWaitFishEdit, show);
    if (gRespawnEdit) ShowWindow(gRespawnEdit, show);
    if (gCollectEdit) ShowWindow(gCollectEdit, show);
    InvalidateRect(gWindow, nullptr, FALSE);
}

ULONGLONG ReadWaitForFishEdit() {
    if (!gWaitFishEdit) return gWaitForFishMs.load();
    wchar_t buf[32]{};
    GetWindowTextW(gWaitFishEdit, buf, 32);
    wchar_t* end = nullptr;
    const unsigned long value = wcstoul(buf, &end, 10);
    if (end == buf) return gWaitForFishMs.load();
    return std::clamp<ULONGLONG>(static_cast<ULONGLONG>(value) * 1000ULL,
                                  kMinWaitForFishMs, kMaxWaitForFishMs);
}

void UpdateWaitForFishSetting() {
    const ULONGLONG ms = ReadWaitForFishEdit();
    gWaitForFishMs.store(ms);
    wchar_t buf[32];
    swprintf_s(buf, L"%u", static_cast<unsigned>(ms / 1000));
    SetWindowTextW(gWaitFishEdit, buf);
    SaveSettings();
}

// "2.6" (or "2,6") seconds -> 2600 ms; false if the text isn't a number.
bool ReadSecondsEdit(HWND edit, ULONGLONG& ms) {
    wchar_t buf[32]{};
    GetWindowTextW(edit, buf, 32);
    for (wchar_t* c = buf; *c; ++c) {
        if (*c == L',') *c = L'.';
    }
    wchar_t* end = nullptr;
    const double seconds = wcstod(buf, &end);
    if (end == buf || seconds < 0.0) return false;
    ms = static_cast<ULONGLONG>(std::lround(std::min(seconds, 999.0) * 10.0)) * 100ULL; // 0.1 s steps
    return true;
}

void ShowSecondsEdit(HWND edit, ULONGLONG ms) {
    wchar_t buf[32];
    swprintf_s(buf, L"%.1f", ms / 1000.0);
    SetWindowTextW(edit, buf);
}

void UpdateCollectDelaySetting() {
    ULONGLONG ms = 0;
    if (ReadSecondsEdit(gCollectEdit, ms)) gCollectDelayMs.store(std::min(ms, kMaxCollectDelayMs));
    ShowSecondsEdit(gCollectEdit, gCollectDelayMs.load());
    SaveSettings();
}

void UpdateHoldKeySetting() {
    ULONGLONG ms = 0;
    if (ReadSecondsEdit(gHoldKeyEdit, ms)) gHoldKeyMs.store(std::clamp(ms, kMinHoldKeyMs, kMaxHoldKeyMs));
    ShowSecondsEdit(gHoldKeyEdit, gHoldKeyMs.load());
    SaveSettings();
}

// Typed bait amount is rounded up to whole purchases of 99.
void SetBaitAmount(int amount) {
    gBaitAmount.store(std::clamp((amount + kBaitPerCycle - 1) / kBaitPerCycle, 1, 999) * kBaitPerCycle);
    wchar_t buf[32];
    swprintf_s(buf, L"%d", gBaitAmount.load());
    if (gBaitAmountEdit) SetWindowTextW(gBaitAmountEdit, buf);
    SaveSettings();
}

void UpdateBaitAmountSetting() {
    wchar_t buf[32]{};
    GetWindowTextW(gBaitAmountEdit, buf, 32);
    wchar_t* end = nullptr;
    const unsigned long value = wcstoul(buf, &end, 10);
    SetBaitAmount(end != buf ? static_cast<int>(std::min<unsigned long>(value, 99000)) : gBaitAmount.load());
}

void UpdateBaitDelaySetting() {
    wchar_t buf[32]{};
    GetWindowTextW(gBaitDelayEdit, buf, 32);
    wchar_t* end = nullptr;
    const unsigned long value = wcstoul(buf, &end, 10);
    if (end != buf) gBaitDelayMs.store(std::clamp(static_cast<int>(std::min<unsigned long>(value, 99999)), 100, 5000));
    swprintf_s(buf, L"%d", gBaitDelayMs.load());
    SetWindowTextW(gBaitDelayEdit, buf);
    SaveSettings();
}

void UpdateRespawnEverySetting() {
    wchar_t buf[32]{};
    GetWindowTextW(gRespawnEdit, buf, 32);
    wchar_t* end = nullptr;
    const unsigned long value = wcstoul(buf, &end, 10);
    if (end != buf) {
        gRespawnEveryCatches.store(static_cast<int>(std::clamp<unsigned long>(
            value, kMinRespawnEveryCatches, kMaxRespawnEveryCatches)));
    }
    swprintf_s(buf, L"%d", gRespawnEveryCatches.load());
    SetWindowTextW(gRespawnEdit, buf);
    SaveSettings();
}

void HandleHit(HWND hwnd, int id) {
    if (id != kHitResetStats) gResetArmed = false;
    switch (id) {
        case kHitMenu:
            gMenuOpen = !gMenuOpen;
            SetActiveTab(gActiveTab);
            break;
        case kHitMenuBackdrop:
            gMenuOpen = false;
            SetActiveTab(gActiveTab);
            break;
        case kHitMenuItem0: case kHitMenuItem0 + 1: case kHitMenuItem0 + 2: case kHitMenuItem0 + 3:
        case kHitMenuItem0 + 4: case kHitMenuItem0 + 5: case kHitMenuItem0 + 6: case kHitMenuItem0 + 7: {
            const int page = kMenuOrder[id - kHitMenuItem0];
            gMenuOpen = false;
            gSessionDeleteArmed = false;
            if (page == 5 && gActiveTab == 5) gSessionOpenId = -1; // Sessions again = back to the list
            SetActiveTab(page);
            break;
        }
        case kHitResetStats:
            if (gResetArmed) {
                ResetStats();
                gResetArmed = false;
            } else {
                gResetArmed = true;
            }
            break;
        case kHitDiscordToggle:
            gDiscordEnabled.store(!gDiscordEnabled.load());
            SaveSettings();
            break;
        case kHitDiscordHide:
            gDiscordHideUrl.store(!gDiscordHideUrl.load());
            SendMessage(gDiscordUrlEdit, EM_SETPASSWORDCHAR, gDiscordHideUrl.load() ? 0x25CF : 0, 0);
            InvalidateRect(gDiscordUrlEdit, nullptr, TRUE);
            SaveSettings();
            break;
        case kHitDiscordPing:
            gDiscordPing.store(!gDiscordPing.load());
            SaveSettings();
            break;
        case kHitDiscordFail:
            gDiscordFailAlerts.store(!gDiscordFailAlerts.load());
            SaveSettings();
            break;
        case kHitDiscordChip0: case kHitDiscordChip0 + 1: case kHitDiscordChip0 + 2:
        case kHitDiscordChip0 + 3: case kHitDiscordChip0 + 4: case kHitDiscordChip0 + 5: {
            const int rarityOf[6] = {kRarityImpossible, kRarityMythic, -1, kRarityLegendary, kRarityRare, kRarityCommon};
            const int r = rarityOf[id - kHitDiscordChip0];
            if (r < 0) gDiscordOre.store(!gDiscordOre.load());
            else gDiscordRarity[r].store(!gDiscordRarity[r].load());
            SaveSettings();
            break;
        }
        case kHitDiscordTest: {
            SetFocus(hwnd); // commit a link that is still being typed
            DiscordJob job;
            job.type = DiscordEvent::Test;
            Telemetry t;
            {
                std::lock_guard<std::mutex> lock(gTelemetryMutex);
                t = gTelemetry;
            }
            job.sessionId = gCurrentSessionId;
            job.catches = t.totalCatches;
            job.ore = t.oreCount;
            job.runningMs = t.runningMs;
            CaptureFullScreen(job.pixels, job.w, job.h);
            const bool wasEnabled = gDiscordEnabled.exchange(true); // a test always sends
            EnqueueDiscord(std::move(job));
            gDiscordEnabled.store(wasEnabled);
            break;
        }
        case kHitTab0: case kHitTab1: case kHitTab2: case kHitTab3: case kHitTab4: case kHitTab5:
            gSessionDeleteArmed = false;
            if (id == kHitTab5 && gActiveTab == 5) gSessionOpenId = -1; // tab click again = back to list
            SetActiveTab(id - kHitTab0);
            break;
        case kHitStart:
            ToggleTracker();
            break;
        case kHitMinimize:
            ShowWindow(hwnd, SW_MINIMIZE);
            break;
        case kHitBindCorrection:
            if (gBindingHotkey.load() && gBindingTarget == 3) EndHotkeyBind();
            else StartHotkeyBind(3);
            break;
        case kHitBindBait:
            if (gBindingHotkey.load() && gBindingTarget == 5) EndHotkeyBind();
            else StartHotkeyBind(5);
            break;
        case kHitBindRod:
            if (gBindingHotkey.load() && gBindingTarget == 4) EndHotkeyBind();
            else StartHotkeyBind(4);
            break;
        case kHitExit:
        case kHitClose:
            PostMessage(hwnd, WM_CLOSE, 0, 0);
            break;
        case kHitLogToggle:
            gMinigameLog.store(!gMinigameLog.load());
            SaveSettings();
            break;
        case kHitRepoMode0: case kHitRepoMode0 + 1: case kHitRepoMode0 + 2:
            gRepositionMode.store(id - kHitRepoMode0);
            gAutoRespawn.store(gRepositionMode.load() == 1);
            SaveSettings();
            break;
        case kHitCalAnchor:
            if (gCalibrating.load() && gCalibrationTarget == CalibrationTarget::AnchorRegion) EndCalibration();
            else StartCalibration(CalibrationTarget::AnchorRegion);
            break;
        case kHitLearnWalk:
            if (AnchorReady()) {
                gEnabled.store(false);
                gLearnWalkRequested.store(true);
            }
            break;
        case kHitRespawnToggle:
            gAutoRespawn.store(!gAutoRespawn.load());
            SaveSettings();
            break;
        case kHitBindToggle:
            if (gBindingHotkey.load() && gBindingTarget == 1) EndHotkeyBind();
            else StartHotkeyBind(1);
            break;
        case kHitBindQuit:
            if (gBindingHotkey.load() && gBindingTarget == 2) EndHotkeyBind();
            else StartHotkeyBind(2);
            break;
        case kHitCalCast:
            if (gCalibrating.load() && gCalibrationTarget == CalibrationTarget::CastPoint) EndCalibration();
            else StartCalibration(CalibrationTarget::CastPoint);
            break;
        case kHitCalBar:
            if (gCalibrating.load() && gCalibrationTarget == CalibrationTarget::BarRegion) EndCalibration();
            else StartCalibration(CalibrationTarget::BarRegion);
            break;
        case kHitSessRow0: case kHitSessRow0 + 1: case kHitSessRow0 + 2: case kHitSessRow0 + 3:
        case kHitSessRow0 + 4: case kHitSessRow0 + 5: case kHitSessRow0 + 6: {
            const int index = static_cast<int>(gSessions.size()) - 1 -
                (gSessionPage * kSessionsPerPage + (id - kHitSessRow0));
            if (index >= 0 && index < static_cast<int>(gSessions.size())) gSessionOpenId = gSessions[index].id;
            gSessionDeleteArmed = false;
            break;
        }
        case kHitSessPrev:
            --gSessionPage;
            break;
        case kHitSessNext:
            ++gSessionPage;
            break;
        case kHitSessBack:
            gSessionOpenId = -1;
            gSessionDeleteArmed = false;
            break;
        case kHitSessDelete:
            if (!gSessionDeleteArmed) {
                gSessionDeleteArmed = true;
            } else {
                DeleteSession(gSessionOpenId);
                gSessionOpenId = -1;
                gSessionDeleteArmed = false;
            }
            break;
        case kHitRestoreYes:
            ContinueSession(gRestorePromptId);
            gRestorePromptId = -1;
            break;
        case kHitRestoreNo:
            gRestorePromptId = -1;
            break;
        case kHitBaitStart:
            if (gBaitRunning.load()) StopBaitBuying();
            else StartBaitBuying();
            break;
        case kHitBaitPreset0: case kHitBaitPreset0 + 1: case kHitBaitPreset0 + 2:
        case kHitBaitPreset0 + 3: case kHitBaitPreset0 + 4:
            SetBaitAmount(kBaitPresets[id - kHitBaitPreset0]);
            break;
        case kHitBaitCal0: case kHitBaitCal0 + 1: case kHitBaitCal0 + 2: case kHitBaitCal0 + 3:
        case kHitBaitCal0 + 4: case kHitBaitCal0 + 5: case kHitBaitCal0 + 6:
            if (gCalibrating.load() && gCalibrationTarget == CalibrationTarget::BaitPoint &&
                gBaitCalibIndex == id - kHitBaitCal0) {
                EndCalibration();
            } else if (!gCalibrating.load() && !gBaitRunning.load()) {
                gBaitCalibIndex = id - kHitBaitCal0;
                StartCalibration(CalibrationTarget::BaitPoint);
            }
            break;
        case kHitCalCollect:
            if (gCalibrating.load() && gCalibrationTarget == CalibrationTarget::CollectRegion) EndCalibration();
            else StartCalibration(CalibrationTarget::CollectRegion);
            break;
        case kHitCalExit:
            if (gCalibrating.load() && gCalibrationTarget == CalibrationTarget::ExitRegion) EndCalibration();
            else StartCalibration(CalibrationTarget::ExitRegion);
            break;
        default:
            break;
    }
    InvalidateRect(hwnd, nullptr, FALSE);
}

HWND CreateNumberEdit(HWND parent, int id, int x, int y, bool decimal = false) {
    HWND edit = CreateWindowEx(0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | (decimal ? 0 : ES_NUMBER) | ES_AUTOHSCROLL | ES_CENTER,
        x, y, 68, 22, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
    SendMessage(edit, WM_SETFONT, reinterpret_cast<WPARAM>(gUiFont), TRUE);
    SendMessage(edit, EM_SETLIMITTEXT, 3, 0);
    return edit;
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE: {
            // Positioned over the inset boxes drawn in DrawSettingsTab.
            const auto editY = [](int row) { return static_cast<int>(SettingsRowY(row)) + 9; };
            gWaitFishEdit = CreateNumberEdit(hwnd, ID_WAIT_FISH_EDIT, 272, editY(0));
            gCollectEdit = CreateNumberEdit(hwnd, ID_COLLECT_EDIT, 272, editY(1), true);
            gHoldKeyEdit = CreateNumberEdit(hwnd, ID_HOLD_EDIT, 272, editY(2), true);
            SendMessage(gCollectEdit, EM_SETLIMITTEXT, 4, 0);
            SendMessage(gHoldKeyEdit, EM_SETLIMITTEXT, 4, 0);
            gRespawnEdit = CreateNumberEdit(hwnd, ID_RESPAWN_EDIT, 272, editY(4));
            gBaitAmountEdit = CreateNumberEdit(hwnd, ID_BAIT_AMOUNT_EDIT, 272, static_cast<int>(kBaitAmountRowY) + 9);
            // Discord: webhook link (masked - it works like a password) and minutes.
            gDiscordUrlEdit = CreateWindowEx(0, L"EDIT", L"",
                WS_CHILD | ES_AUTOHSCROLL | ES_PASSWORD, 42, static_cast<int>(kDiscordUrlY) + 7, 296, 18, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_DISCORD_URL_EDIT)), nullptr, nullptr);
            SendMessage(gDiscordUrlEdit, WM_SETFONT, reinterpret_cast<WPARAM>(gSmallEditFont), TRUE);
            SendMessage(gDiscordUrlEdit, EM_SETLIMITTEXT, 1000, 0);
            SetWindowTextW(gDiscordUrlEdit, GetDiscordUrl().c_str());
            // Visible by default; the Hide button turns it into dots.
            SendMessage(gDiscordUrlEdit, EM_SETPASSWORDCHAR, gDiscordHideUrl.load() ? 0x25CF : 0, 0);
            gDiscordPingEdit = CreateWindowEx(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL,
                214, static_cast<int>(kDiscordPingRowY) + 12, 124, 18, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_DISCORD_PING_EDIT)), nullptr, nullptr);
            SendMessage(gDiscordPingEdit, WM_SETFONT, reinterpret_cast<WPARAM>(gSmallEditFont), TRUE);
            SendMessage(gDiscordPingEdit, EM_SETLIMITTEXT, 64, 0);
            SetWindowTextW(gDiscordPingEdit, GetDiscordPingWho().c_str());
            gDiscordMinEdit = CreateNumberEdit(hwnd, ID_DISCORD_MIN_EDIT, 272, static_cast<int>(kDiscordMinRowY) + 9);
            {
                wchar_t minutes[16];
                swprintf_s(minutes, L"%d", gDiscordNoCatchMin.load());
                SetWindowTextW(gDiscordMinEdit, minutes);
            }
            gBaitDelayEdit = CreateNumberEdit(hwnd, ID_BAIT_DELAY_EDIT, 272, static_cast<int>(kBaitDelayRowY) + 9);
            SendMessage(gBaitAmountEdit, EM_SETLIMITTEXT, 5, 0);
            SendMessage(gBaitDelayEdit, EM_SETLIMITTEXT, 4, 0);
            wchar_t buf[32];
            swprintf_s(buf, L"%d", gBaitAmount.load());
            SetWindowTextW(gBaitAmountEdit, buf);
            swprintf_s(buf, L"%d", gBaitDelayMs.load());
            SetWindowTextW(gBaitDelayEdit, buf);
            ShowSecondsEdit(gHoldKeyEdit, gHoldKeyMs.load());
            ShowSecondsEdit(gCollectEdit, gCollectDelayMs.load());
            swprintf_s(buf, L"%u", static_cast<unsigned>(gWaitForFishMs.load() / 1000));
            SetWindowTextW(gWaitFishEdit, buf);
            swprintf_s(buf, L"%d", gRespawnEveryCatches.load());
            SetWindowTextW(gRespawnEdit, buf);
            return 0;
        }
        case WM_CTLCOLOREDIT: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetTextColor(hdc, ui::kTextRef);
            SetBkColor(hdc, ui::kInsetRef);
            return reinterpret_cast<LRESULT>(gInputBrush);
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == ID_WAIT_FISH_EDIT && HIWORD(wParam) == EN_KILLFOCUS) {
                UpdateWaitForFishSetting();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            if (LOWORD(wParam) == ID_DISCORD_URL_EDIT && HIWORD(wParam) == EN_KILLFOCUS) {
                wchar_t url[1024] = {};
                GetWindowTextW(gDiscordUrlEdit, url, 1024);
                std::wstring text(url);
                while (!text.empty() && iswspace(text.back())) text.pop_back();
                while (!text.empty() && iswspace(text.front())) text.erase(text.begin());
                {
                    std::lock_guard<std::mutex> lock(gDiscordUrlMutex);
                    gDiscordUrl = text;
                }
                SaveSettings();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            if (LOWORD(wParam) == ID_DISCORD_PING_EDIT && HIWORD(wParam) == EN_KILLFOCUS) {
                wchar_t who[128] = {};
                GetWindowTextW(gDiscordPingEdit, who, 128);
                std::wstring text(who);
                while (!text.empty() && iswspace(text.back())) text.pop_back();
                while (!text.empty() && iswspace(text.front())) text.erase(text.begin());
                if (text.empty()) text = L"everyone";
                {
                    std::lock_guard<std::mutex> lock(gDiscordUrlMutex);
                    gDiscordPingWho = text;
                }
                SetWindowTextW(gDiscordPingEdit, text.c_str());
                SaveSettings();
            }
            if (LOWORD(wParam) == ID_DISCORD_MIN_EDIT && HIWORD(wParam) == EN_KILLFOCUS) {
                wchar_t buf[16] = {};
                GetWindowTextW(gDiscordMinEdit, buf, 16);
                gDiscordNoCatchMin.store(std::clamp(_wtoi(buf), 0, 999));
                swprintf_s(buf, L"%d", gDiscordNoCatchMin.load());
                SetWindowTextW(gDiscordMinEdit, buf);
                SaveSettings();
            }
            if (LOWORD(wParam) == ID_BAIT_AMOUNT_EDIT && HIWORD(wParam) == EN_KILLFOCUS) {
                UpdateBaitAmountSetting();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            if (LOWORD(wParam) == ID_BAIT_DELAY_EDIT && HIWORD(wParam) == EN_KILLFOCUS) {
                UpdateBaitDelaySetting();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            if (LOWORD(wParam) == ID_HOLD_EDIT && HIWORD(wParam) == EN_KILLFOCUS) {
                UpdateHoldKeySetting();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            if (LOWORD(wParam) == ID_COLLECT_EDIT && HIWORD(wParam) == EN_KILLFOCUS) {
                UpdateCollectDelaySetting();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            if (LOWORD(wParam) == ID_RESPAWN_EDIT && HIWORD(wParam) == EN_KILLFOCUS) {
                UpdateRespawnEverySetting();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONDOWN: {
            SetFocus(hwnd); // commits a number being typed (EN_KILLFOCUS)
            const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            gPressedHit = HitAt(pt);
            if (gPressedHit != kHitNone) {
                SetCapture(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            if (gPressedHit == kHitNone) return 0;
            const int pressed = gPressedHit;
            gPressedHit = kHitNone;
            ReleaseCapture();
            const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (HitAt(pt) == pressed) HandleHit(hwnd, pressed);
            else InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (!gTrackingMouse) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                gTrackingMouse = TrackMouseEvent(&tme) != FALSE;
            }
            const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const int hover = HitAt(pt);
            if (hover != gHoverHit) {
                gHoverHit = hover;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            gTrackingMouse = false;
            if (gHoverHit != kHitNone) {
                gHoverHit = kHitNone;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT && gHoverHit != kHitNone) {
                SetCursor(LoadCursor(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        case WM_ERASEBKGND:
            return 1; // PaintHud covers everything
        case WM_HOTKEY:
            if (wParam == HOTKEY_TOGGLE) ToggleTracker();
            if (wParam == HOTKEY_QUIT) PostMessage(hwnd, WM_CLOSE, 0, 0);
            if (wParam == HOTKEY_BAIT) {
                if (gBaitRunning.load()) StopBaitBuying();
                else StartBaitBuying();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            if (wParam == HOTKEY_CANCEL_CAPTURE) {
                if (gCalibrating.load()) EndCalibration();
                if (gBindingHotkey.load()) EndHotkeyBind();
            }
            return 0;
        case WM_CALIBRATION_DONE: {
            // Only the Cast Point gets a confirmation click - clicking after
            // calibrating the bar/exit regions would be an unwanted extra cast.
            const CalibrationTarget justCalibrated = gCalibrationTarget;
            EndCalibration();
            if (justCalibrated == CalibrationTarget::AnchorRegion) {
                // Store the reference image, then learn the W/A/S/D steps.
                Sleep(150); // let the selection overlay/cursor settle
                if (CaptureAnchorReference(gPendingAnchor)) {
                    gEnabled.store(false);
                    gLearnWalkRequested.store(true);
                }
            }
            SaveSettings();
            if (justCalibrated == CalibrationTarget::CastPoint && FocusGame()) {
                ClickAt(GetCastPoint());
            }
            return 0;
        }
        case WM_CALIBRATION_DONE + 100: // walk steps learned (tracker thread)
            SaveSettings();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_KEYBIND_DONE: {
            const int target = gBindingTarget;
            const WORD vk = gCapturedVk;
            if (target == 1) ApplyToggleHotkey(vk);
            else if (target == 2) ApplyQuitHotkey(vk);
            else if (target == 3) { gCorrectionKeyVk.store(vk); SaveSettings(); }
            else if (target == 4) { gRodKeyVk.store(vk); SaveSettings(); }
            else if (target == 5) ApplyBaitHotkey(vk);
            EndHotkeyBind();
            return 0;
        }
        case WM_TRACKER_UPDATE:
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_PAINT:
            PaintHud(hwnd);
            return 0;
        case WM_NCHITTEST: {
            // The header (outside its buttons) drags the window.
            const LRESULT result = DefWindowProc(hwnd, message, wParam, lParam);
            if (result == HTCLIENT) {
                POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                ScreenToClient(hwnd, &pt);
                if (pt.y < ui::kHeaderHeight && HitAt(pt) == kHitNone) return HTCAPTION;
            }
            return result;
        }
        case WM_TIMER:
            if (wParam == 1) SaveCurrentSession(); // autosave every minute
            return 0;
        case WM_ENDSESSION:
            if (wParam) SaveCurrentSession();      // Windows shutting down / logging off
            return 0;
        case WM_CLOSE:
            SaveCurrentSession();
            StopBaitBuying();
            gEnabled.store(false);
            gQuit.store(true);
            MouseButton(false);
            KeyEvent(false, kHoldKeyVk);
            if (gCalibrating.load()) EndCalibration();
            if (gBindingHotkey.load()) EndHotkeyBind();
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(hwnd, message, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    SetProcessDPIAware();
    LoadSettings();

    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    Gdiplus::GdiplusStartup(&gGdiplusToken, &gdiplusStartupInput, nullptr);
    gCloudIcon = LoadPngFromMemory(kCloudPng, kCloudPngSize);
    gWenIcon = LoadPngFromMemory(kWenPng, kWenPngSize);
    CreateUiFonts();

    gBackgroundBrush = CreateSolidBrush(ui::kBgRef);
    gInputBrush = CreateSolidBrush(ui::kInsetRef);
    gSmallEditFont = CreateFont(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    gUiFont = CreateFont(-17, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

    WNDCLASSEX wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = gBackgroundBrush;
    wc.lpszClassName = kClassName;
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassEx(&wc);

    // No WS_EX_TOOLWINDOW: the window needs a taskbar button so it can be
    // minimized and restored.
    gWindow = CreateWindowEx(WS_EX_TOPMOST | WS_EX_LAYERED,
        kClassName, L"Sky's S2 Fishing Macro", WS_POPUP | WS_CLIPCHILDREN | WS_MINIMIZEBOX | WS_SYSMENU,
        24, 80, ui::kWidth, ui::kHeight, nullptr, nullptr, instance, nullptr);
    if (!gWindow) return 1;

    // Whole-window alpha (the edits don't mix with per-pixel alpha) and
    // rounded corners.
    SetLayeredWindowAttributes(gWindow, 0, 247, LWA_ALPHA);
    SetWindowRgn(gWindow, CreateRoundRectRgn(0, 0, ui::kWidth + 1, ui::kHeight + 1, 26, 26), TRUE);

    HICON appIcon = nullptr;
    if (gCloudIcon) {
        // Square app icon with the cloud centred (the logo itself is 312x230).
        Gdiplus::Bitmap square(256, 256, PixelFormat32bppARGB);
        {
            Gdiplus::Graphics ig(&square);
            ig.Clear(Gdiplus::Color(0, 0, 0, 0));
            ig.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            ig.DrawImage(gCloudIcon, Gdiplus::RectF(0, 34, 256, 189));
        }
        square.GetHICON(&appIcon);
    }
    if (appIcon) {
        SendMessage(gWindow, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(appIcon));
        SendMessage(gWindow, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(appIcon));
    }

    SetActiveTab(0);

    // Sessions: next number after the saved ones, and offer the last one.
    LoadSessions();
    if (!gSessions.empty()) {
        gCurrentSessionId = gSessions.back().id + 1;
        gRestorePromptId = gSessions.back().id;
    }
    SetTimer(gWindow, 1, 60000, nullptr);

    // Default the cast point to screen center until the user calibrates it -
    // but don't clobber one already loaded from settings above.
    {
        const POINT loaded = GetCastPoint();
        if (loaded.x == 0 && loaded.y == 0) {
            SetCastPoint(POINT{GetSystemMetrics(SM_CXSCREEN) / 2, GetSystemMetrics(SM_CYSCREEN) / 2});
        }
    }

    RegisterHotKey(gWindow, HOTKEY_TOGGLE, MOD_NOREPEAT, gToggleHotkeyVk.load());
    RegisterHotKey(gWindow, HOTKEY_QUIT, MOD_NOREPEAT, gQuitHotkeyVk.load());
    RegisterHotKey(gWindow, HOTKEY_BAIT, MOD_NOREPEAT, gBaitHotkeyVk.load());
    ShowWindow(gWindow, showCommand);
    UpdateWindow(gWindow);

    HANDLE thread = CreateThread(nullptr, 0, TrackerThread, nullptr, 0, nullptr);
    gDiscordWake = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (HANDLE discord = CreateThread(nullptr, 0, DiscordThread, nullptr, 0, nullptr)) CloseHandle(discord);
    MSG msg{};
    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    gQuit.store(true);
    MouseButton(false);
    if (gCalibrating.load()) EndCalibration();
    if (gBindingHotkey.load()) EndHotkeyBind();
    if (thread) {
        WaitForSingleObject(thread, 2000);
        CloseHandle(thread);
    }
    UnregisterHotKey(gWindow, HOTKEY_TOGGLE);
    UnregisterHotKey(gWindow, HOTKEY_BAIT);
    UnregisterHotKey(gWindow, HOTKEY_QUIT);
    DeleteObject(gUiFont);
    DeleteObject(gSmallEditFont);
    DeleteObject(gBackgroundBrush);
    DeleteObject(gInputBrush);
    if (appIcon) DestroyIcon(appIcon);
    // GDI+ objects must be destroyed before GdiplusShutdown.
    DestroyUiFonts();
    delete gCloudIcon;
    gCloudIcon = nullptr;
    delete gWenIcon;
    gWenIcon = nullptr;
    Gdiplus::GdiplusShutdown(gGdiplusToken);
    return 0;
}
