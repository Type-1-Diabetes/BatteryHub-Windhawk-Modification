// ==WindhawkMod==
// @id              batteryhub-pill-room
// @name            BatteryHub pill room
// @description     Widens a Taskbar Styler tray "pill" by exactly the width of the BatteryHub strip
// @version         1.1.0
// @author          BatteryHub Windhawk modification
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -lole32 -loleaut32 -lruntimeobject
// ==/WindhawkMod==

// Source code is published under The GNU General Public License v3.0.
//
// The taskbar lookup (TrayUI::StartTaskbar, the taskbar XamlRoot and the
// SystemTrayFrame search) is taken from "Taskbar tray auto-hide (show on hover)"
// by m417z (https://github.com/ramensoftware/windhawk-mods, GPL-3.0).

// ==WindhawkModReadme==
/*
# BatteryHub pill room

Companion mod for the BatteryHub Windhawk modification. With **Display settings
→ Inside the Windhawk tray pill** turned on, BatteryHub sits inside the rounded
tray "pill" that Windows 11 Taskbar Styler themes (for example Matter) draw.

A fixed padding in the Styler theme leaves either too little room (devices that
come and go, like headphones, end up outside the pill) or an empty gap. This mod
reads the room BatteryHub needs from
`HKEY_CURRENT_USER\Software\WirelessBattery\PillRoom` and adds it to the left
padding of the pill (`SystemTrayFrameGrid`) live, so the pill grows and shrinks
with the strip.

Remove any left padding you added for BatteryHub in the Styler settings: this
mod adds the room on top of the theme's own padding.

Only the main taskbar is changed, the one BatteryHub sits inside.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- extraRoom: 0
  $name: Extra room
  $description: >-
    Pixels added to the room BatteryHub asks for (negative values take some
    away), to fine-tune the spacing in the pill
- animationMs: 150
  $name: Animation duration
  $description: >-
    How long the pill takes to grow or shrink, in milliseconds. 0 changes it
    at once. Off as well when Windows' animation effects are turned off.
*/
// ==/WindhawkModSettings==

#include <windhawk_utils.h>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <functional>
#include <list>
#include <optional>

#undef GetCurrentTime

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.h>

using namespace winrt::Windows::UI::Xaml;

constexpr WCHAR kRoomKey[] = L"Software\\WirelessBattery";
constexpr WCHAR kRoomValue[] = L"PillRoom";

std::atomic<bool> g_unloading;
std::atomic<int> g_extraRoom;
std::atomic<int> g_animationMs;
std::atomic<int> g_room;  // what BatteryHub asks for, in 96-DPI pixels

HANDLE g_stopEvent;
HANDLE g_watchThread;

HWND FindCurrentProcessTaskbarWnd() {
    HWND hTaskbarWnd = nullptr;

    EnumWindows(
        [](HWND hWnd, LPARAM lParam) -> BOOL {
            DWORD dwProcessId;
            WCHAR className[32];
            if (GetWindowThreadProcessId(hWnd, &dwProcessId) &&
                dwProcessId == GetCurrentProcessId() &&
                GetClassName(hWnd, className, ARRAYSIZE(className)) &&
                _wcsicmp(className, L"Shell_TrayWnd") == 0) {
                *reinterpret_cast<HWND*>(lParam) = hWnd;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&hTaskbarWnd));

    return hTaskbarWnd;
}

int ReadRoom() {
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegGetValue(HKEY_CURRENT_USER, kRoomKey, kRoomValue, RRF_RT_REG_DWORD,
                    nullptr, &value, &size) != ERROR_SUCCESS) {
        return 0;
    }
    return std::clamp((int)value, 0, 1000);
}

FrameworkElement EnumChildElements(
    FrameworkElement element,
    std::function<bool(FrameworkElement)> enumCallback) {
    int childrenCount = Media::VisualTreeHelper::GetChildrenCount(element);

    for (int i = 0; i < childrenCount; i++) {
        auto child = Media::VisualTreeHelper::GetChild(element, i)
                         .try_as<FrameworkElement>();
        if (!child) {
            continue;
        }

        if (enumCallback(child)) {
            return child;
        }
    }

    return nullptr;
}

FrameworkElement FindChildByName(FrameworkElement element, PCWSTR name) {
    return EnumChildElements(element, [name](FrameworkElement child) {
        return child.Name() == name;
    });
}

FrameworkElement FindChildByClassName(FrameworkElement element,
                                      PCWSTR className) {
    return EnumChildElements(element, [className](FrameworkElement child) {
        return winrt::get_class_name(child) == className;
    });
}

FrameworkElement FindDescendantByName(FrameworkElement element,
                                      PCWSTR name,
                                      int depth = 0) {
    if (depth > 12) {
        return nullptr;
    }

    FrameworkElement found = nullptr;
    EnumChildElements(element, [&](FrameworkElement child) {
        if (child.Name() == name) {
            found = child;
            return true;
        }
        found = FindDescendantByName(child, name, depth + 1);
        return found != nullptr;
    });
    return found;
}

// On newer builds, the XAML root content is the taskbar frame, and the tray
// frame is an item of its repeater.
FrameworkElement FindSystemTrayFrame(FrameworkElement xamlRootContent) {
    if (winrt::get_class_name(xamlRootContent) != L"Taskbar.TaskbarFrame") {
        return FindChildByClassName(xamlRootContent,
                                    L"SystemTray.SystemTrayFrame");
    }

    FrameworkElement child = xamlRootContent;
    if (!(child = FindChildByName(child, L"RootGrid")) ||
        !(child = FindChildByName(child, L"TaskbarFrameRepeater"))) {
        return nullptr;
    }

    FrameworkElement systemTrayFrame = nullptr;
    EnumChildElements(child, [&systemTrayFrame](FrameworkElement item) {
        if (winrt::get_class_name(item) != L"Taskbar.TaskbarExtensionElement") {
            return false;
        }

        auto extensionElement = item.try_as<Controls::ContentControl>();
        auto content =
            extensionElement
                ? extensionElement.Content().try_as<FrameworkElement>()
                : nullptr;
        if (!content ||
            winrt::get_class_name(content) != L"SystemTray.SystemTrayFrame") {
            return false;
        }

        systemTrayFrame = content;
        return true;
    });

    return systemTrayFrame;
}

// The element that draws the pill: a Grid on older builds, a StackPanel on
// newer ones. Both have a Padding property, under different owners.
DependencyProperty PaddingPropertyOf(FrameworkElement element) {
    if (element.try_as<Controls::Grid>()) {
        return Controls::Grid::PaddingProperty();
    }
    if (element.try_as<Controls::StackPanel>()) {
        return Controls::StackPanel::PaddingProperty();
    }
    if (element.try_as<Controls::Border>()) {
        return Controls::Border::PaddingProperty();
    }
    return nullptr;
}

// Only accessed from the taskbar thread.
struct PillTarget {
    winrt::weak_ref<FrameworkElement> element;
    DependencyProperty paddingProperty{nullptr};
    Thickness basePadding{};  // the padding without our room (the theme's)
    int64_t callbackToken = 0;
    bool settingPadding = false;
    // The room shown now, and the glide to a new room (frames on CompositionTarget::Rendering).
    double shownRoom = 0, fromRoom = 0, toRoom = 0;
    ULONGLONG glideStart = 0;
    winrt::event_token renderingToken{};
};
[[clang::no_destroy]] std::optional<PillTarget> g_target;

struct TrayAddedWait {
    winrt::weak_ref<FrameworkElement> taskbarFrame;
    FrameworkElement::LayoutUpdated_revoker layoutUpdatedRevoker;
};
[[clang::no_destroy]] std::optional<std::list<TrayAddedWait>> g_trayAddedWaits{
    std::in_place};

void SetPadding(PillTarget& target, FrameworkElement element, double room) {
    target.shownRoom = room;
    Thickness padding = target.basePadding;
    padding.Left = std::max(0.0, padding.Left + std::round(room));
    target.settingPadding = true;
    element.SetValue(target.paddingProperty, winrt::box_value(padding));
    target.settingPadding = false;
}

bool AnimationsOn() {
    BOOL on = TRUE;
    return g_animationMs > 0 &&
           (!SystemParametersInfo(SPI_GETCLIENTAREAANIMATION, 0, &on, 0) || on);
}

void StopGlide(PillTarget& target) {
    if (target.renderingToken.value) {
        Media::CompositionTarget::Rendering(target.renderingToken);
        target.renderingToken = {};
    }
}

// Grows or shrinks the pill to `room` over the animation duration, one step per rendered frame.
void GlideTo(PillTarget& target, FrameworkElement element, int room) {
    if (!AnimationsOn() || std::abs(room - target.shownRoom) < 1) {
        StopGlide(target);
        SetPadding(target, element, room);
        return;
    }
    target.fromRoom = target.shownRoom;
    target.toRoom = room;
    target.glideStart = GetTickCount64();
    if (target.renderingToken.value) {
        return;  // already gliding: it continues towards the new room
    }
    target.renderingToken = Media::CompositionTarget::Rendering(
        [](winrt::Windows::Foundation::IInspectable const&,
           winrt::Windows::Foundation::IInspectable const&) {
            if (!g_target) {
                return;
            }
            auto element = g_target->element.get();
            if (!element || g_unloading) {
                StopGlide(*g_target);
                return;
            }
            double t = std::min(
                1.0, (GetTickCount64() - g_target->glideStart) /
                         (double)std::max(1, g_animationMs.load()));
            double e = 1 - (1 - t) * (1 - t) * (1 - t);  // ease-out
            SetPadding(*g_target, element,
                       g_target->fromRoom +
                           (g_target->toRoom - g_target->fromRoom) * e);
            if (t >= 1) {
                StopGlide(*g_target);
            }
        });
}

void ReleaseTarget(bool restore) {
    if (!g_target) {
        return;
    }

    StopGlide(*g_target);
    if (auto element = g_target->element.get()) {
        element.UnregisterPropertyChangedCallback(g_target->paddingProperty,
                                                  g_target->callbackToken);
        if (restore) {
            g_target->settingPadding = true;
            element.SetValue(g_target->paddingProperty,
                             winrt::box_value(g_target->basePadding));
        }
    }

    g_target.reset();
}

int CurrentRoom() {
    int room = g_room;
    return room > 0 ? std::max(0, room + g_extraRoom) : 0;
}

bool ApplyRoom(XamlRoot xamlRoot) {
    auto content = xamlRoot.Content().try_as<FrameworkElement>();
    auto trayFrame = content ? FindSystemTrayFrame(content) : nullptr;
    auto pill = trayFrame
                    ? FindDescendantByName(trayFrame, L"SystemTrayFrameGrid")
                    : nullptr;
    if (!pill) {
        return false;
    }

    if (!g_target || g_target->element.get() != pill) {
        ReleaseTarget(/*restore=*/true);

        auto paddingProperty = PaddingPropertyOf(pill);
        if (!paddingProperty) {
            Wh_Log(L"SystemTrayFrameGrid has no Padding property");
            return true;
        }

        g_target.emplace();
        g_target->element = pill;
        g_target->paddingProperty = paddingProperty;
        g_target->basePadding =
            winrt::unbox_value<Thickness>(pill.GetValue(paddingProperty));

        // Taskbar Styler sets the theme's padding when it (re)applies its
        // styles. Take that as the new base and add the room on top again.
        g_target->callbackToken = pill.RegisterPropertyChangedCallback(
            paddingProperty,
            [](DependencyObject const& sender, DependencyProperty const& dp) {
                if (!g_target || g_target->settingPadding || g_unloading) {
                    return;
                }
                auto element = sender.try_as<FrameworkElement>();
                if (!element || g_target->element.get() != element) {
                    return;
                }
                g_target->basePadding =
                    winrt::unbox_value<Thickness>(element.GetValue(dp));
                StopGlide(*g_target);
                SetPadding(*g_target, element, CurrentRoom());
            });
    }

    GlideTo(*g_target, pill, CurrentRoom());
    return true;
}

// On newer builds, the tray is an item of the taskbar frame repeater, which is
// added after the taskbar is started.
void ApplyRoomWhenTrayAdded(XamlRoot xamlRoot) {
    if (g_unloading || !g_trayAddedWaits) {
        return;
    }

    auto taskbarFrame = xamlRoot.Content().try_as<FrameworkElement>();
    if (!taskbarFrame) {
        return;
    }

    std::erase_if(*g_trayAddedWaits, [](const TrayAddedWait& wait) {
        return !wait.taskbarFrame.get();
    });

    if (std::any_of(g_trayAddedWaits->begin(), g_trayAddedWaits->end(),
                    [&](const TrayAddedWait& wait) {
                        return wait.taskbarFrame.get() == taskbarFrame;
                    })) {
        return;
    }

    auto it = g_trayAddedWaits->emplace(g_trayAddedWaits->end());
    it->taskbarFrame = taskbarFrame;
    it->layoutUpdatedRevoker = taskbarFrame.LayoutUpdated(
        winrt::auto_revoke,
        [it](winrt::Windows::Foundation::IInspectable const&,
             winrt::Windows::Foundation::IInspectable const&) {
            auto taskbarFrame = it->taskbarFrame.get();
            if (!taskbarFrame) {
                g_trayAddedWaits->erase(it);
                return;
            }

            try {
                if (ApplyRoom(taskbarFrame.XamlRoot())) {
                    g_trayAddedWaits->erase(it);
                }
            } catch (...) {
                Wh_Log(L"Error %08X", winrt::to_hresult());
                g_trayAddedWaits->erase(it);
            }
        });
}

void* CTaskBand_ITaskListWndSite_vftable;

using CTaskBand_GetTaskbarHost_t = void*(WINAPI*)(void* pThis, void** result);
CTaskBand_GetTaskbarHost_t CTaskBand_GetTaskbarHost_Original;

void* TaskbarHost_FrameHeight_Original;

using std__Ref_count_base__Decref_t = void(WINAPI*)(void* pThis);
std__Ref_count_base__Decref_t std__Ref_count_base__Decref_Original;

XamlRoot XamlRootFromTaskbarHostSharedPtr(void* taskbarHostSharedPtr[2]) {
    if (!taskbarHostSharedPtr[0] && !taskbarHostSharedPtr[1]) {
        return nullptr;
    }

    size_t taskbarElementIUnknownOffset = 0x10;

    {
        // 48:83EC 28 | sub rsp,28
        // 48:83C1 48 | add rcx,48
        const BYTE* b = (const BYTE*)TaskbarHost_FrameHeight_Original;
        if (b[0] == 0x48 && b[1] == 0x83 && b[2] == 0xEC && b[4] == 0x48 &&
            b[5] == 0x83 && b[6] == 0xC1 && b[7] <= 0x7F) {
            taskbarElementIUnknownOffset = b[7];
        } else {
            Wh_Log(L"Unsupported TaskbarHost::FrameHeight");
        }
    }

    auto* taskbarElementIUnknown =
        *(IUnknown**)((BYTE*)taskbarHostSharedPtr[0] +
                      taskbarElementIUnknownOffset);

    FrameworkElement taskbarElement = nullptr;
    taskbarElementIUnknown->QueryInterface(winrt::guid_of<FrameworkElement>(),
                                           winrt::put_abi(taskbarElement));

    auto result = taskbarElement ? taskbarElement.XamlRoot() : nullptr;

    std__Ref_count_base__Decref_Original(taskbarHostSharedPtr[1]);

    return result;
}

XamlRoot GetTaskbarXamlRoot(HWND hTaskbarWnd) {
    HWND hTaskSwWnd = (HWND)GetProp(hTaskbarWnd, L"TaskbandHWND");
    if (!hTaskSwWnd) {
        return nullptr;
    }

    void* taskBand = (void*)GetWindowLongPtr(hTaskSwWnd, 0);
    void* taskBandForTaskListWndSite = taskBand;
    for (int i = 0; *(void**)taskBandForTaskListWndSite !=
                    CTaskBand_ITaskListWndSite_vftable;
         i++) {
        if (i == 20) {
            return nullptr;
        }

        taskBandForTaskListWndSite = (void**)taskBandForTaskListWndSite + 1;
    }

    void* taskbarHostSharedPtr[2]{};
    CTaskBand_GetTaskbarHost_Original(taskBandForTaskListWndSite,
                                      taskbarHostSharedPtr);

    return XamlRootFromTaskbarHostSharedPtr(taskbarHostSharedPtr);
}

using RunFromWindowThreadProc_t = void(WINAPI*)(void* parameter);

bool RunFromWindowThread(HWND hWnd,
                         RunFromWindowThreadProc_t proc,
                         void* procParam) {
    static const UINT runFromWindowThreadRegisteredMsg =
        RegisterWindowMessage(L"Windhawk_RunFromWindowThread_" WH_MOD_ID);

    struct RUN_FROM_WINDOW_THREAD_PARAM {
        RunFromWindowThreadProc_t proc;
        void* procParam;
    };

    DWORD dwThreadId = GetWindowThreadProcessId(hWnd, nullptr);
    if (dwThreadId == 0) {
        return false;
    }

    if (dwThreadId == GetCurrentThreadId()) {
        proc(procParam);
        return true;
    }

    HHOOK hook = SetWindowsHookEx(
        WH_CALLWNDPROC,
        [](int nCode, WPARAM wParam, LPARAM lParam) -> LRESULT {
            if (nCode == HC_ACTION) {
                const CWPSTRUCT* cwp = (const CWPSTRUCT*)lParam;
                if (cwp->message == runFromWindowThreadRegisteredMsg) {
                    RUN_FROM_WINDOW_THREAD_PARAM* param =
                        (RUN_FROM_WINDOW_THREAD_PARAM*)cwp->lParam;
                    param->proc(param->procParam);
                }
            }

            return CallNextHookEx(nullptr, nCode, wParam, lParam);
        },
        nullptr, dwThreadId);
    if (!hook) {
        return false;
    }

    RUN_FROM_WINDOW_THREAD_PARAM param;
    param.proc = proc;
    param.procParam = procParam;
    SendMessage(hWnd, runFromWindowThreadRegisteredMsg, 0, (LPARAM)&param);

    UnhookWindowsHookEx(hook);

    return true;
}

void ApplyFromTaskbarThread() {
    if (g_unloading) {
        g_trayAddedWaits.reset();
        ReleaseTarget(/*restore=*/true);
        return;
    }

    HWND hTaskbarWnd = FindCurrentProcessTaskbarWnd();
    if (!hTaskbarWnd) {
        return;
    }

    try {
        XamlRoot xamlRoot = GetTaskbarXamlRoot(hTaskbarWnd);
        if (!xamlRoot) {
            Wh_Log(L"Getting XamlRoot failed");
            return;
        }

        if (!ApplyRoom(xamlRoot)) {
            ApplyRoomWhenTrayAdded(xamlRoot);
        }
    } catch (...) {
        Wh_Log(L"Error %08X", winrt::to_hresult());
    }
}

void Apply() {
    HWND hTaskbarWnd = FindCurrentProcessTaskbarWnd();
    if (hTaskbarWnd) {
        RunFromWindowThread(
            hTaskbarWnd, [](void*) -> void { ApplyFromTaskbarThread(); },
            nullptr);
    }
}

using TrayUI_StartTaskbar_t = void(WINAPI*)(void* pThis);
TrayUI_StartTaskbar_t TrayUI_StartTaskbar_Original;
void WINAPI TrayUI_StartTaskbar_Hook(void* pThis) {
    Wh_Log(L">");

    TrayUI_StartTaskbar_Original(pThis);

    ApplyFromTaskbarThread();
}

// Waits for BatteryHub to change the room it needs, and applies it.
DWORD WINAPI WatchThread(LPVOID) {
    HKEY key = nullptr;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, kRoomKey, 0, nullptr, 0,
                       KEY_NOTIFY | KEY_QUERY_VALUE, nullptr, &key,
                       nullptr) != ERROR_SUCCESS) {
        Wh_Log(L"Can't open the BatteryHub settings key");
        return 0;
    }

    HANDLE changed = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    while (changed) {
        if (RegNotifyChangeKeyValue(key, FALSE, REG_NOTIFY_CHANGE_LAST_SET,
                                    changed, TRUE) != ERROR_SUCCESS) {
            break;
        }

        HANDLE handles[] = {g_stopEvent, changed};
        if (WaitForMultipleObjects(ARRAYSIZE(handles), handles, FALSE,
                                   INFINITE) != WAIT_OBJECT_0 + 1) {
            break;  // stopping
        }

        int room = ReadRoom();
        if (room != g_room.exchange(room)) {
            Wh_Log(L"Room: %d", room);
            Apply();
        }
    }

    if (changed) {
        CloseHandle(changed);
    }
    RegCloseKey(key);
    return 0;
}

bool HookTaskbarDllSymbols() {
    HMODULE module =
        LoadLibraryEx(L"taskbar.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) {
        Wh_Log(L"Failed to load taskbar.dll");
        return false;
    }

    WindhawkUtils::SYMBOL_HOOK taskbarDllHooks[] = {
        {
            {LR"(const CTaskBand::`vftable'{for `ITaskListWndSite'})"},
            &CTaskBand_ITaskListWndSite_vftable,
        },
        {
            {LR"(public: virtual class std::shared_ptr<class TaskbarHost> __cdecl CTaskBand::GetTaskbarHost(void)const )"},
            &CTaskBand_GetTaskbarHost_Original,
        },
        {
            {LR"(public: int __cdecl TaskbarHost::FrameHeight(void)const )"},
            &TaskbarHost_FrameHeight_Original,
        },
        {
            {LR"(public: void __cdecl std::_Ref_count_base::_Decref(void))"},
            &std__Ref_count_base__Decref_Original,
        },
        {
            {LR"(public: virtual void __cdecl TrayUI::StartTaskbar(void))"},
            &TrayUI_StartTaskbar_Original,
            TrayUI_StartTaskbar_Hook,
        },
    };

    if (!HookSymbols(module, taskbarDllHooks, ARRAYSIZE(taskbarDllHooks))) {
        Wh_Log(L"HookSymbols failed");
        return false;
    }

    return true;
}

void LoadSettings() {
    g_extraRoom = std::clamp(Wh_GetIntSetting(L"extraRoom"), -200, 200);
    g_animationMs = std::clamp(Wh_GetIntSetting(L"animationMs"), 0, 1000);
}

BOOL Wh_ModInit() {
    Wh_Log(L">");

    LoadSettings();
    g_room = ReadRoom();

    if (!HookTaskbarDllSymbols()) {
        return FALSE;
    }

    return TRUE;
}

void Wh_ModAfterInit() {
    Wh_Log(L">");

    Apply();

    g_stopEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    if (g_stopEvent) {
        g_watchThread =
            CreateThread(nullptr, 0, WatchThread, nullptr, 0, nullptr);
    }
}

void Wh_ModBeforeUninit() {
    Wh_Log(L">");

    if (g_stopEvent) {
        SetEvent(g_stopEvent);
    }
    if (g_watchThread) {
        WaitForSingleObject(g_watchThread, INFINITE);
        CloseHandle(g_watchThread);
        g_watchThread = nullptr;
    }

    g_unloading = true;
    Apply();  // gives the pill its theme padding back
}

void Wh_ModUninit() {
    Wh_Log(L">");

    if (g_stopEvent) {
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
    }
}

void Wh_ModSettingsChanged() {
    Wh_Log(L">");

    LoadSettings();
    Apply();
}
