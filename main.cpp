#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
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

#include "fish_icons.h"

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
constexpr int HOTKEY_TOGGLE = 1;
constexpr int HOTKEY_QUIT = 2;
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
enum class Phase { Cast, Hook, Collect, HoldKey, WaitForFish, Respawn };

constexpr ULONGLONG kCollectDelayMs = 4000;
constexpr ULONGLONG kHoldKeyDurationMs = 4000;
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
    int confidence = 0;
    ControlState state = ControlState::Paused;
    Phase phase = Phase::Cast;
};

std::atomic<bool> gQuit{false};
std::atomic<bool> gEnabled{false};
std::atomic<ULONGLONG> gWaitForFishMs{kDefaultWaitForFishMs};
std::atomic<bool> gAutoRespawn{false};
std::atomic<int> gRespawnEveryCatches{kDefaultRespawnEveryCatches};
std::mutex gTelemetryMutex;
Telemetry gTelemetry;
HWND gWindow = nullptr;
HWND gToggleButton = nullptr;
HWND gExitButton = nullptr;
HWND gWaitFishEdit = nullptr;
HWND gRespawnEdit = nullptr;
HWND gCalibrateButton = nullptr;
HWND gCalibrateBarButton = nullptr;
HWND gCalibrateExitButton = nullptr;
HWND gToggleBindButton = nullptr; // Keystrokes tab: rebind Start/Pause
HWND gExitBindButton = nullptr;   // Keystrokes tab: rebind Exit
HFONT gUiFont = nullptr;
HBRUSH gBackgroundBrush = nullptr;
int gActiveTab = 0; // 0 = Fishing, 1 = Keystrokes, 2 = Calibration

// Which calibration is currently in progress, if any.
enum class CalibrationTarget { None, CastPoint, BarRegion, ExitRegion };
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
std::atomic<bool> gBindingHotkey{false};
int gBindingTarget = 0; // 0 = none, 1 = Start/Pause, 2 = Exit (UI thread only)
WORD gCapturedVk = 0;   // set by the hook, read once WM_KEYBIND_DONE arrives
HHOOK gKeyboardHook = nullptr;

// GDI+ is only used to decode/draw the two embedded LIVE/OFF fish-icon PNGs.
ULONG_PTR gGdiplusToken = 0;
Gdiplus::Bitmap* gFishOnIcon = nullptr;
Gdiplus::Bitmap* gFishOffIcon = nullptr;

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
        case Phase::WaitForFish: return L"WAIT FOR FISH";
        case Phase::Respawn: return L"RESPAWNING";
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

DWORD WINAPI TrackerThread(void*) {
    timeBeginPeriod(1);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    // Bar/exit regions are calibrated live (see the Calibration tab) rather
    // than assumed from any fixed reference resolution, so this works the
    // same regardless of the user's monitor/resolution/game window size.
    RECT bar = GetBarRect();
    RECT exitRect = GetExitRect();
    CaptureSurface barSurface;
    CaptureSurface exitSurface;
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
    ULONGLONG previousTick = GetTickCount64();
    ULONGLONG lastUiTick = 0;
    ULONGLONG holdStarted = 0;
    int missingFrames = 0;
    float fpsAverage = 0.0f;

    auto setMouse = [&](bool down) {
        if (mouseDown != down) {
            MouseButton(down);
            mouseDown = down;
            if (down) holdStarted = GetTickCount64();
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

    while (!gQuit.load()) {
        const ULONGLONG now = GetTickCount64();
        const double dtMs = std::clamp<double>(now - previousTick, 1.0, 30.0);
        previousTick = now;
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
        }
        const bool captureReady = barReady && exitReady;

        Telemetry current{};
        current.enabled = gEnabled.load();
        current.fps = fpsAverage;
        current.captureReady = captureReady;
        current.totalCatches = totalCatches;
        current.state = current.enabled ? ControlState::Waiting : ControlState::Paused;

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
            catchesSinceRespawn = 0;
            missedCastsInARow = 0;
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
            if (barSurface.Grab(bar.left, bar.top)) FindBarObjects(barSurface, target, player);
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
                    const bool recoveryPulse = ((now / 18) % 3) != 2;
                    setMouse(recoveryPulse);
                    current.state = recoveryPulse ? ControlState::Holding : ControlState::Releasing;
                }
            } else {
                missingFrames = 0;
                const float alpha = 0.34f;
                const float rawPlayerVelocity = hadPlayer
                    ? (player.center - previousPlayer) / static_cast<float>(dtMs) : 0.0f;
                const float rawTargetVelocity = hadPlayer
                    ? (target.center - previousTarget) / static_cast<float>(dtMs) : 0.0f;
                playerVelocity += (rawPlayerVelocity - playerVelocity) * alpha;
                targetVelocity += (rawTargetVelocity - targetVelocity) * 0.28f;
                previousPlayer = player.center;
                previousTarget = target.center;
                hadPlayer = true;

                // Predict 14 ms ahead so the player block brakes before crossing a moving zone.
                const float desired = std::clamp(target.center + targetVelocity * 14.0f,
                                                 static_cast<float>(target.top + 2),
                                                 static_cast<float>(target.bottom - 2));
                const float error = player.center - desired; // positive = player is below target
                const float relativeVelocity = playerVelocity - targetVelocity;
                current.error = error;
                const float targetHalf = std::max(6.0f, (target.bottom - target.top + 1) * 0.5f);
                const float safeHalf = std::max(3.0f, targetHalf -
                    std::max(3.0f, (player.bottom - player.top + 1) * 0.45f));
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
                    // Binary PID converted to 18 ms PWM. The 0.52 hover term is
                    // continuously corrected by position and vertical momentum.
                    float duty = 0.52f + 0.070f * error + 0.095f * relativeVelocity;
                    if (error > 0.0f) duty += 0.035f;
                    if (error < 0.0f) duty -= 0.035f;
                    duty = std::clamp(duty, 0.08f, 0.92f);
                    constexpr double periodMs = 18.0;
                    pwmPhaseMs = std::fmod(pwmPhaseMs + dtMs, periodMs);
                    commandDown = pwmPhaseMs < periodMs * duty;
                    current.state = ControlState::Floating;
                }

                // Safety release: long rises get a tiny braking gap, preventing a
                // lost frame from pinning the block at the top.
                if (commandDown && mouseDown && now - holdStarted >= 135) {
                    commandDown = false;
                    pwmPhaseMs = 16.0;
                }
                setMouse(commandDown);
            }

            if (sawExitDuringHook && now - hookLastSeenAt >= kHookGoneDebounceMs) {
                // The exit bar has reliably disappeared (not just one flickered
                // frame) - that marks the end of hooking; wait out the collect
                // delay before the hold-key press.
                setMouse(false);
                phase = Phase::Collect;
                current.phase = phase;
                phaseChangedAt = now;
            }
        } else if (phase == Phase::Collect) {
            setMouse(false);
            current.phaseElapsedMs = static_cast<float>(now - phaseChangedAt);
            if (now - phaseChangedAt >= kCollectDelayMs) {
                phase = Phase::HoldKey;
                current.phase = phase;
                phaseChangedAt = now;
                if (FocusGame()) {
                    KeyEvent(true, kHoldKeyVk);
                    keyDown = true;
                }
            }
        } else if (phase == Phase::HoldKey) {
            current.phaseElapsedMs = static_cast<float>(now - phaseChangedAt);
            if (now - phaseChangedAt >= kHoldKeyDurationMs) {
                if (keyDown) KeyEvent(false, kHoldKeyVk);
                keyDown = false;
                // The game deliberately stays focused now. Handing focus back
                // to the user's previous window (usually this HUD, right after
                // pressing Start) every cycle was the riskiest cross-thread
                // focus switch, and the cursor stays on the cast point anyway.
                // Claiming is done - restart the whole cycle with a real
                // cast click. Going to WaitForFish here was the bug: nothing
                // in that phase ever fires a new cast, so if the exit bar
                // happened to still read as visible for a moment right after
                // claiming (a leftover from the previous minigame fading
                // out), it could fall straight through WaitForFish -> Hook
                // -> Collect -> HoldKey again without ever actually
                // re-casting, looping between Collect and HoldKey forever.
                phase = Phase::Cast;
                ++catchesSinceRespawn;
                ++totalCatches;
                current.totalCatches = totalCatches;
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
                current.catchesSinceRespawn = catchesSinceRespawn;
                phaseChangedAt = GetTickCount64();
            }
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
    timeEndPeriod(1);
    return 0;
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
constexpr int kHeight = 584;
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
HBRUSH gInputBrush = nullptr;

enum HitId {
    kHitNone = 0,
    kHitTab0 = 1, kHitTab1, kHitTab2,
    kHitStart = 10, kHitExit, kHitClose, kHitRespawnToggle,
    kHitBindToggle = 20, kHitBindQuit,
    kHitCalCast = 30, kHitCalBar, kHitCalExit,
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
    if (!t.captureReady) {
        headline = L"NOT CALIBRATED";
        headColor = ui::kBad;
        detail = L"Set the bar + Exit button on the Setup tab";
    } else {
        headline = !t.enabled ? L"PAUSED"
                 : (t.phase == Phase::Hook) ? StateText(t.state) : PhaseText(t.phase);
        if (t.phase == Phase::Hook && t.minigame) headColor = ui::kGradA;
        if (t.phase == Phase::Hook) {
            if (t.targetFound && t.playerFound) {
                swprintf_s(line, L"Target %.0f  ·  Player %.0f  ·  Error %+.0f px",
                           t.targetY, t.playerY, t.error);
            } else {
                swprintf_s(line, L"Target %s  ·  Player %s",
                           t.targetFound ? L"found" : L"---", t.playerFound ? L"found" : L"---");
            }
        } else if (t.phase == Phase::Collect) {
            swprintf_s(line, L"Waiting to collect  %.1fs / %.0fs",
                       t.phaseElapsedMs / 1000.0f, kCollectDelayMs / 1000.0f);
        } else if (t.phase == Phase::HoldKey) {
            swprintf_s(line, L"Holding T  %.1fs / %.0fs",
                       t.phaseElapsedMs / 1000.0f, kHoldKeyDurationMs / 1000.0f);
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

    const RectF pill(296, 140, 52, 22);
    if (t.enabled) {
        FillGradient(g, pill, 11.0f);
        TextCenter(g, L"LIVE", gFontLabel, ui::kText, pill);
    } else {
        FillInset(g, pill, 11.0f);
        TextCenter(g, L"OFF", gFontLabel, ui::kMuted, pill);
    }

    // --- Stat tiles --------------------------------------------------------
    DrawCard(g, RectF(18, 234, 166, 86));
    Text(g, L"CATCHES", gFontLabel, ui::kText, RectF(32, 246, 120, 16));
    Text(g, L"this session", gFontSmall, ui::kMuted, RectF(32, 262, 120, 14));
    swprintf_s(line, L"%d", t.totalCatches);
    Text(g, line, gFontBig, ui::kText, RectF(30, 280, 130, 32));
    FillGradient(g, RectF(164, 252, 4, 50), 2.0f, true);

    const RectF resetTile(196, 234, 166, 86);
    if (gAutoRespawn.load()) {
        DrawCard(g, resetTile);
        FillGradient(g, resetTile, 14.0f);
        Text(g, L"UNTIL RESET", gFontLabel, ui::kText, RectF(210, 246, 120, 16));
        Text(g, L"auto reposition", gFontSmall, Gdiplus::Color(220, 255, 255, 255),
             RectF(210, 262, 120, 14));
        swprintf_s(line, L"%d / %d", t.catchesSinceRespawn, gRespawnEveryCatches.load());
        Text(g, line, gFontBig, ui::kText, RectF(208, 280, 130, 32));
        Gdiplus::SolidBrush white(ui::kText);
        FillRound(g, white, RectF(342, 252, 4, 50), 2.0f);
    } else {
        DrawCard(g, resetTile);
        Text(g, L"UNTIL RESET", gFontLabel, ui::kMuted, RectF(210, 246, 120, 16));
        Text(g, L"auto reposition", gFontSmall, ui::kMuted, RectF(210, 262, 120, 14));
        Text(g, L"OFF", gFontBig, ui::kMuted, RectF(208, 280, 130, 32));
    }

    // --- Settings card -----------------------------------------------------
    DrawCard(g, RectF(18, 332, 344, 136));
    RowLabel(g, 340, L"Wait for fish", L"seconds before re-casting");
    FillInset(g, RectF(266, 345, 80, 30), 9.0f); // EDIT control sits on top
    Divider(g, 383);
    RowLabel(g, 386, L"Auto reposition", L"reset character (Esc · R · Enter)");
    {
        const RectF track(300, 394, 46, 24);
        const bool on = gAutoRespawn.load();
        if (on) FillGradient(g, track, 12.0f);
        else FillInset(g, track, 12.0f);
        Gdiplus::SolidBrush knob(on ? ui::kText : ui::kMuted);
        g.FillEllipse(&knob, RectF(on ? 325.0f : 303.0f, 397.0f, 18.0f, 18.0f));
        const RectF hit(262, 386, 94, 40);
        HoverOverlay(g, RectF(296, 390, 54, 32), 14.0f, kHitRespawnToggle);
        AddHit(hit, kHitRespawnToggle);
    }
    Divider(g, 428);
    RowLabel(g, 430, L"Reset every", L"catches (also after 2 missed casts)");
    FillInset(g, RectF(266, 434, 80, 30), 9.0f); // EDIT control sits on top

    // --- Buttons -------------------------------------------------------------
    std::wstring start = (t.enabled ? L"PAUSE   " : L"START   ") + HotkeyHint();
    GradientButton(g, RectF(18, 482, 212, 50), start.c_str(), kHitStart, 14.0f);
    std::wstring quit = L"EXIT   ( " + KeyDisplayName(gQuitHotkeyVk.load()) + L" )";
    const RectF exitR(242, 482, 120, 50);
    DrawCard(g, exitR);
    HoverOverlay(g, exitR, 14.0f, kHitExit);
    TextCenter(g, quit.c_str(), gFontButton, ui::kSoft, exitR);
    AddHit(exitR, kHitExit);

    // --- Footer --------------------------------------------------------------
    if (gCalibrating.load()) {
        TextCenter(g, L"Calibrating · see the Setup tab (Esc cancels)", gFontSmall, ui::kWarn,
                   RectF(18, 546, 344, 18));
    } else {
        const POINT cast = GetCastPoint();
        swprintf_s(line, L"Cast point (%ld, %ld)  ·  v2.0", cast.x, cast.y);
        TextCenter(g, line, gFontSmall, ui::kMuted, RectF(18, 546, 344, 18));
    }
}

void DrawKeyRow(Gdiplus::Graphics& g, float y, const wchar_t* title, const wchar_t* sub,
                const std::wstring& key, int id, bool binding) {
    DrawCard(g, RectF(18, y, 344, 70));
    RowLabel(g, y + 14, title, sub);
    const RectF chip(236, y + 17, 110, 36);
    if (binding) {
        GradientButton(g, chip, L"Press a key...", id);
    } else {
        InsetButton(g, chip, key.c_str(), id, id == kHitNone ? ui::kMuted : ui::kText);
    }
}

void DrawHotkeysTab(Gdiplus::Graphics& g) {
    DrawCard(g, RectF(18, 128, 344, 74));
    Text(g, L"HOTKEYS", gFontLabel, ui::kText, RectF(34, 140, 300, 16));
    Text(g, L"Click a key, then press the new key.", gFontSmall, ui::kMuted, RectF(34, 160, 310, 15));
    Text(g, L"Global: work in any window. Esc cancels.", gFontSmall, ui::kMuted,
         RectF(34, 176, 310, 15));

    const bool binding = gBindingHotkey.load();
    DrawKeyRow(g, 214, L"Start / Pause", L"global hotkey",
               KeyDisplayName(gToggleHotkeyVk.load()), kHitBindToggle, binding && gBindingTarget == 1);
    DrawKeyRow(g, 298, L"Safe exit", L"closes the macro",
               KeyDisplayName(gQuitHotkeyVk.load()), kHitBindQuit, binding && gBindingTarget == 2);
    DrawKeyRow(g, 382, L"Collect key", L"held 4s after each catch (fixed)", L"T", kHitNone, false);

    if (binding) {
        TextCenter(g, L"Waiting for a key press · Esc cancels", gFontSmall, ui::kWarn,
                   RectF(18, 466, 344, 18));
    }
}

void DrawCalibrationRow(Gdiplus::Graphics& g, float y, const wchar_t* title, const wchar_t* value,
                        bool isSet, int id, bool active) {
    DrawCard(g, RectF(18, y, 344, 70));
    RowLabel(g, y + 14, title, value, isSet ? ui::kMuted : ui::kBad);
    const RectF button(236, y + 17, 110, 36);
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
            ? L"CLICK the spot to cast at" : L"DRAG a box over the region";
        Text(g, banner, gFontBody, ui::kWarn, RectF(34, 158, 310, 18));
        Text(g, L"Esc cancels", gFontSmall, ui::kMuted, RectF(34, 177, 310, 15));
    } else {
        Text(g, L"Click Calibrate, then click or drag on screen.", gFontSmall, ui::kMuted,
             RectF(34, 160, 310, 15));
        Text(g, L"Everything is saved automatically.", gFontSmall, ui::kMuted, RectF(34, 176, 310, 15));
    }

    const POINT cast = GetCastPoint();
    swprintf_s(line, L"(%ld, %ld)", cast.x, cast.y);
    DrawCalibrationRow(g, 214, L"Cast point", line, true, kHitCalCast,
                       calibrating && gCalibrationTarget == CalibrationTarget::CastPoint);

    const RECT bar = GetBarRect();
    const bool barSet = RectCalibrated(bar);
    if (barSet) swprintf_s(line, L"%ldx%ld at (%ld, %ld)", bar.right - bar.left, bar.bottom - bar.top, bar.left, bar.top);
    else swprintf_s(line, L"NOT SET");
    DrawCalibrationRow(g, 298, L"Fishing bar", line, barSet, kHitCalBar,
                       calibrating && gCalibrationTarget == CalibrationTarget::BarRegion);

    const RECT exitR = GetExitRect();
    const bool exitSet = RectCalibrated(exitR);
    if (exitSet) swprintf_s(line, L"%ldx%ld at (%ld, %ld)", exitR.right - exitR.left, exitR.bottom - exitR.top, exitR.left, exitR.top);
    else swprintf_s(line, L"NOT SET");
    DrawCalibrationRow(g, 382, L"Exit button", line, exitSet, kHitCalExit,
                       calibrating && gCalibrationTarget == CalibrationTarget::ExitRegion);
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

        // Header: gradient logo with the fish icon, title, close button.
        FillGradient(g, RectF(18, 16, 40, 40), 20.0f);
        if (Gdiplus::Bitmap* icon = t.enabled ? gFishOnIcon : gFishOffIcon) {
            g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            g.DrawImage(icon, RectF(25, 23, 26, 26));
        }
        Text(g, L"Sky's S2 Fishing Macro", gFontTitle, ui::kText, RectF(68, 15, 250, 24));
        Text(g, L"Project Slayers 2  ·  auto fishing", gFontSmall, ui::kMuted, RectF(68, 38, 250, 16));
        const RectF closeR(static_cast<float>(client.right) - 46.0f, 22.0f, 28.0f, 28.0f);
        HoverOverlay(g, closeR, 9.0f, kHitClose);
        TextCenter(g, L"✕", gFontBody, gHoverHit == kHitClose ? ui::kText : ui::kMuted, closeR);
        AddHit(closeR, kHitClose);

        // Segmented tab bar.
        DrawCard(g, RectF(18, 72, 344, 42));
        const wchar_t* tabs[3] = {L"Fishing", L"Hotkeys", L"Setup"};
        for (int i = 0; i < 3; ++i) {
            const RectF seg(22.0f + i * 112.0f, 76.0f, 112.0f, 34.0f);
            const int id = kHitTab0 + i;
            if (gActiveTab == i) FillGradient(g, seg, 11.0f);
            else HoverOverlay(g, seg, 11.0f, id);
            TextCenter(g, tabs[i], gFontButton, gActiveTab == i ? ui::kText : ui::kMuted, seg);
            AddHit(seg, id);
        }

        if (gActiveTab == 0) DrawFishingTab(g, t);
        else if (gActiveTab == 1) DrawHotkeysTab(g);
        else DrawSetupTab(g);
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
}

void DestroyUiFonts() {
    for (Gdiplus::Font** f : {&gFontTitle, &gFontHead, &gFontBig, &gFontRow, &gFontButton,
                              &gFontLabel, &gFontBody, &gFontSmall}) {
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
    gEnabled.store(!gEnabled.load());
    if (!gEnabled.load()) MouseButton(false);
    UpdateButtonLabel();
    InvalidateRect(gWindow, nullptr, FALSE);
}

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
        gCalibrationTarget == CalibrationTarget::ExitRegion) {
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
                else SetExitRect(rect);
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
    const UINT waitSeconds = GetPrivateProfileIntW(L"Fishing", L"WaitForFishSeconds",
                                                    static_cast<UINT>(kDefaultWaitForFishMs / 1000),
                                                    path.c_str());
    const ULONGLONG waitMs = std::clamp<ULONGLONG>(
        static_cast<ULONGLONG>(waitSeconds) * 1000ULL,
        kMinWaitForFishMs, kMaxWaitForFishMs);
    gWaitForFishMs.store(waitMs);
    gAutoRespawn.store(GetPrivateProfileIntW(L"Fishing", L"AutoRespawn", 0, path.c_str()) != 0);
    gRespawnEveryCatches.store(std::clamp<int>(
        ReadIntSetting(L"Fishing", L"RespawnEveryCatches", kDefaultRespawnEveryCatches, path),
        kMinRespawnEveryCatches, kMaxRespawnEveryCatches));

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
}

void SaveSettings() {
    const std::wstring path = GetSettingsPath();
    wchar_t buf[16];
    swprintf_s(buf, L"%u", static_cast<unsigned>(gToggleHotkeyVk.load()));
    WritePrivateProfileStringW(L"Hotkeys", L"ToggleVk", buf, path.c_str());
    swprintf_s(buf, L"%u", static_cast<unsigned>(gQuitHotkeyVk.load()));
    WritePrivateProfileStringW(L"Hotkeys", L"QuitVk", buf, path.c_str());
    swprintf_s(buf, L"%u", static_cast<unsigned>(gWaitForFishMs.load() / 1000));
    WritePrivateProfileStringW(L"Fishing", L"WaitForFishSeconds", buf, path.c_str());
    WriteIntSetting(L"Fishing", L"AutoRespawn", gAutoRespawn.load() ? 1 : 0, path);
    WriteIntSetting(L"Fishing", L"RespawnEveryCatches", gRespawnEveryCatches.load(), path);

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
}

void ApplyToggleHotkey(WORD vk) {
    UnregisterHotKey(gWindow, HOTKEY_TOGGLE);
    RegisterHotKey(gWindow, HOTKEY_TOGGLE, MOD_NOREPEAT, vk);
    gToggleHotkeyVk.store(vk);
    UpdateButtonLabel();
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
    const int show = (tab == 0) ? SW_SHOW : SW_HIDE;
    if (gWaitFishEdit) ShowWindow(gWaitFishEdit, show);
    if (gRespawnEdit) ShowWindow(gRespawnEdit, show);
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
    switch (id) {
        case kHitTab0: case kHitTab1: case kHitTab2:
            SetActiveTab(id - kHitTab0);
            break;
        case kHitStart:
            ToggleTracker();
            break;
        case kHitExit:
        case kHitClose:
            PostMessage(hwnd, WM_CLOSE, 0, 0);
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
        case kHitCalExit:
            if (gCalibrating.load() && gCalibrationTarget == CalibrationTarget::ExitRegion) EndCalibration();
            else StartCalibration(CalibrationTarget::ExitRegion);
            break;
        default:
            break;
    }
    InvalidateRect(hwnd, nullptr, FALSE);
}

HWND CreateNumberEdit(HWND parent, int id, int x, int y) {
    HWND edit = CreateWindowEx(0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_AUTOHSCROLL | ES_CENTER,
        x, y, 68, 22, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
    SendMessage(edit, WM_SETFONT, reinterpret_cast<WPARAM>(gUiFont), TRUE);
    SendMessage(edit, EM_SETLIMITTEXT, 3, 0);
    return edit;
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE: {
            // Positioned over the inset boxes drawn in DrawFishingTab.
            gWaitFishEdit = CreateNumberEdit(hwnd, ID_WAIT_FISH_EDIT, 272, 349);
            gRespawnEdit = CreateNumberEdit(hwnd, ID_RESPAWN_EDIT, 272, 438);
            wchar_t buf[32];
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
            SaveSettings();
            if (justCalibrated == CalibrationTarget::CastPoint && FocusGame()) {
                ClickAt(GetCastPoint());
            }
            return 0;
        }
        case WM_KEYBIND_DONE: {
            const int target = gBindingTarget;
            const WORD vk = gCapturedVk;
            if (target == 1) ApplyToggleHotkey(vk);
            else if (target == 2) ApplyQuitHotkey(vk);
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
        case WM_CLOSE:
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
    gFishOnIcon = LoadPngFromMemory(kFishOnPng, kFishOnPngSize);
    gFishOffIcon = LoadPngFromMemory(kFishOffPng, kFishOffPngSize);
    CreateUiFonts();

    gBackgroundBrush = CreateSolidBrush(ui::kBgRef);
    gInputBrush = CreateSolidBrush(ui::kInsetRef);
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

    gWindow = CreateWindowEx(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        kClassName, L"Sky's S2 Fishing Macro", WS_POPUP | WS_CLIPCHILDREN,
        24, 80, ui::kWidth, ui::kHeight, nullptr, nullptr, instance, nullptr);
    if (!gWindow) return 1;

    // Whole-window alpha (the edits don't mix with per-pixel alpha) and
    // rounded corners.
    SetLayeredWindowAttributes(gWindow, 0, 247, LWA_ALPHA);
    SetWindowRgn(gWindow, CreateRoundRectRgn(0, 0, ui::kWidth + 1, ui::kHeight + 1, 26, 26), TRUE);

    SetActiveTab(0);

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
    ShowWindow(gWindow, showCommand);
    UpdateWindow(gWindow);

    HANDLE thread = CreateThread(nullptr, 0, TrackerThread, nullptr, 0, nullptr);
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
    UnregisterHotKey(gWindow, HOTKEY_QUIT);
    DeleteObject(gUiFont);
    DeleteObject(gBackgroundBrush);
    DeleteObject(gInputBrush);
    // GDI+ objects must be destroyed before GdiplusShutdown.
    DestroyUiFonts();
    delete gFishOnIcon;
    gFishOnIcon = nullptr;
    delete gFishOffIcon;
    gFishOffIcon = nullptr;
    Gdiplus::GdiplusShutdown(gGdiplusToken);
    return 0;
}
