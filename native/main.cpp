#include <windows.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Media.Audio.h>
#include <winrt/Windows.Data.Json.h>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace winrt;
using namespace Windows::Devices::Enumeration;
using namespace Windows::Media::Audio;
using namespace Windows::Data::Json;

namespace {
constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT WM_DEVICE = WM_APP + 2;
constexpr UINT WM_CONNECT = WM_APP + 3;
constexpr UINT WM_STATE = WM_APP + 4;
constexpr UINT ID_DEVICE_BASE = 1000;
constexpr UINT ID_REFRESH = 10;
constexpr UINT ID_DISCONNECT_ALL = 11;
constexpr UINT ID_RECONNECT = 12;
constexpr UINT ID_STARTUP = 13;
constexpr UINT ID_BLUETOOTH = 14;
constexpr UINT ID_EXIT = 15;
constexpr size_t MAX_CONNECTIONS = 4;

struct DeviceEvent {
    unsigned generation;
    enum class Kind { Added, Updated, Removed, Completed } kind;
    std::wstring id;
    std::wstring name;
};

struct ConnectEvent {
    std::wstring id;
    unsigned long long serial{};
    AudioPlaybackConnection connection{ nullptr };
    std::wstring error;
};

struct StateEvent {
    std::wstring id;
    AudioPlaybackConnectionState state;
};

struct Settings {
    bool reconnect = false;
    bool startup = false;
    std::vector<std::wstring> lastIds;
};

struct MenuEntry {
    std::wstring text;
    bool checkable = false;
    bool checked = false;
    bool status = false;
    bool submenu = false;
    std::vector<int> lights;
};

std::filesystem::path ExePath() {
    std::wstring buffer(32768, L'\0');
    DWORD count = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    buffer.resize(count);
    return buffer;
}

std::filesystem::path SettingsPath() { return ExePath().parent_path() / L"EchoBridge.json"; }

Settings LoadSettings() {
    Settings settings;
    try {
        std::ifstream file(SettingsPath(), std::ios::binary);
        if (!file) return settings;
        std::string json((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        auto object = JsonObject::Parse(to_hstring(json));
        settings.reconnect = object.GetNamedBoolean(L"ReconnectOnStart", false);
        settings.startup = object.GetNamedBoolean(L"StartWithWindows", false);
        if (object.HasKey(L"LastDeviceIds")) {
            for (auto const& item : object.GetNamedArray(L"LastDeviceIds")) {
                if (item.ValueType() == JsonValueType::String)
                    settings.lastIds.emplace_back(item.GetString().c_str());
            }
        }
    } catch (...) { /* A damaged or unreadable file falls back to defaults. */ }
    return settings;
}

void SaveSettings(Settings const& settings) {
    try {
        JsonObject object;
        object.Insert(L"ReconnectOnStart", JsonValue::CreateBooleanValue(settings.reconnect));
        object.Insert(L"StartWithWindows", JsonValue::CreateBooleanValue(settings.startup));
        JsonArray ids;
        for (auto const& id : settings.lastIds)
            ids.Append(JsonValue::CreateStringValue(hstring{ id }));
        object.Insert(L"LastDeviceIds", ids);
        auto utf8 = to_string(object.Stringify());
        std::ofstream file(SettingsPath(), std::ios::binary | std::ios::trunc);
        file.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    } catch (...) { /* Keep the tray usable if the EXE directory is read-only. */ }
}

bool StartupEnabled() {
    HKEY key{};
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;
    wchar_t value[32768]{};
    DWORD bytes = sizeof(value);
    DWORD type{};
    LONG result = RegQueryValueExW(key, L"EchoBridge.Native", nullptr, &type, reinterpret_cast<BYTE*>(value), &bytes);
    RegCloseKey(key);
    return result == ERROR_SUCCESS && type == REG_SZ && std::wstring(value).find(ExePath().wstring()) != std::wstring::npos;
}

bool SetStartup(bool enabled) {
    HKEY key{};
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return false;
    LONG result;
    if (enabled) {
        std::wstring value = L"\"" + ExePath().wstring() + L"\"";
        result = RegSetValueExW(key, L"EchoBridge.Native", 0, REG_SZ,
            reinterpret_cast<BYTE const*>(value.c_str()), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    } else {
        result = RegDeleteValueW(key, L"EchoBridge.Native");
        if (result == ERROR_FILE_NOT_FOUND) result = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

template<typename T> void PostOwned(HWND window, UINT message, T* data) {
    if (!PostMessageW(window, message, 0, reinterpret_cast<LPARAM>(data))) delete data;
}

class App {
public:
    int Run(HINSTANCE instance) {
        instance_ = instance;
        settings_ = LoadSettings();
        WNDCLASSEXW klass{ sizeof(klass) };
        klass.lpfnWndProc = WindowProc;
        klass.hInstance = instance;
        klass.lpszClassName = L"EchoBridgeNativeWindow";
        if (!RegisterClassExW(&klass)) return 1;
        window_ = CreateWindowExW(0, klass.lpszClassName, L"EchoBridge", WS_OVERLAPPED, 0, 0, 0, 0,
            nullptr, nullptr, instance, this);
        if (!window_) return 1;
        callbackWindow_->store(window_);
        menuFont_ = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");

        NOTIFYICONDATAW icon{ sizeof(icon) };
        icon.hWnd = window_;
        icon.uID = 1;
        icon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        icon.uCallbackMessage = WM_TRAY;
        icon.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
        wcscpy_s(icon.szTip, L"EchoBridge");
        Shell_NotifyIconW(NIM_ADD, &icon);
        icon.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &icon);

        if (settings_.startup && !StartupEnabled()) SetStartup(true);
        StartWatcher();
        if (settings_.reconnect && !settings_.lastIds.empty()) {
            restoreIds_ = settings_.lastIds;
            Connect(restoreIds_.front());
        }

        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return static_cast<int>(message.wParam);
    }

private:
    HINSTANCE instance_{};
    HWND window_{};
    std::shared_ptr<std::atomic<HWND>> callbackWindow_ = std::make_shared<std::atomic<HWND>>(nullptr);
    Settings settings_;
    DeviceWatcher watcher_{ nullptr };
    unsigned generation_ = 0;
    std::map<std::wstring, std::wstring> devices_;
    std::map<std::wstring, AudioPlaybackConnection> connections_;
    std::map<std::wstring, unsigned long long> pending_;
    unsigned long long nextSerial_ = 0;
    std::vector<std::thread> workers_;
    std::vector<std::wstring> restoreIds_;
    size_t restoreIndex_ = 0;
    std::wstring status_ = L"未连接";
    std::vector<std::wstring> menuIds_;
    std::vector<std::unique_ptr<MenuEntry>> menuEntries_;
    HFONT menuFont_{};

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        App* self = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            return TRUE;
        }
        if (!self) return DefWindowProcW(window, message, wparam, lparam);
        return self->Handle(window, message, wparam, lparam);
    }

    LRESULT Handle(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_TRAY:
            if (LOWORD(lparam) == WM_CONTEXTMENU || LOWORD(lparam) == WM_RBUTTONUP) ShowMenu();
            return 0;
        case WM_DEVICE: {
            std::unique_ptr<DeviceEvent> event(reinterpret_cast<DeviceEvent*>(lparam));
            if (event->generation != generation_) return 0;
            if (event->kind == DeviceEvent::Kind::Added) devices_[event->id] = event->name;
            else if (event->kind == DeviceEvent::Kind::Removed) devices_.erase(event->id);
            else status_ = connections_.empty() ? L"未连接" : L"已连接";
            UpdateTip();
            return 0;
        }
        case WM_CONNECT: {
            std::unique_ptr<ConnectEvent> event(reinterpret_cast<ConnectEvent*>(lparam));
            auto pending = pending_.find(event->id);
            if (pending == pending_.end() || pending->second != event->serial) {
                if (event->connection) { try { event->connection.Close(); } catch (...) {} }
                return 0;
            }
            pending_.erase(pending);
            if (event->connection) {
                connections_.emplace(event->id, event->connection);
                status_ = L"已连接 " + DisplayName(event->id);
            } else status_ = event->error.empty() ? L"连接失败" : event->error;
            Save();
            UpdateTip();
            if (restoreIndex_ < restoreIds_.size() && event->id == restoreIds_[restoreIndex_]) {
                ++restoreIndex_;
                if (restoreIndex_ < restoreIds_.size()) SetTimer(window_, 1, 1500, nullptr);
            }
            return 0;
        }
        case WM_STATE: {
            std::unique_ptr<StateEvent> event(reinterpret_cast<StateEvent*>(lparam));
            if (connections_.contains(event->id) && event->state == AudioPlaybackConnectionState::Opened) {
                status_ = L"已连接";
                UpdateTip();
            }
            return 0;
        }
        case WM_TIMER:
            if (wparam == 1) {
                KillTimer(window_, 1);
                if (restoreIndex_ < restoreIds_.size()) Connect(restoreIds_[restoreIndex_]);
            }
            return 0;
        case WM_COMMAND:
            Command(LOWORD(wparam));
            return 0;
        case WM_MEASUREITEM: {
            auto* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lparam);
            if (measure->CtlType == ODT_MENU) {
                auto* entry = reinterpret_cast<MenuEntry*>(measure->itemData);
                SIZE textSize{};
                HDC dc = GetDC(window);
                HGDIOBJ oldFont = menuFont_ ? SelectObject(dc, menuFont_) : nullptr;
                GetTextExtentPoint32W(dc, entry->text.c_str(), static_cast<int>(entry->text.size()), &textSize);
                if (oldFont) SelectObject(dc, oldFont);
                ReleaseDC(window, dc);
                measure->itemWidth = static_cast<UINT>(std::clamp(static_cast<int>(textSize.cx) +
                    (entry->submenu ? 48 : 36), 115, 310));
                measure->itemHeight = entry->status ? 31 : 24;
                return TRUE;
            }
            break;
        }
        case WM_DRAWITEM: {
            auto* draw = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
            if (draw->CtlType == ODT_MENU) { DrawMenuItem(*draw); return TRUE; }
            break;
        }
        case WM_DESTROY:
            Shutdown();
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    std::wstring DisplayName(std::wstring const& id) const {
        auto found = devices_.find(id);
        return found == devices_.end() || found->second.empty() ? id : found->second;
    }

    void UpdateTip() {
        NOTIFYICONDATAW icon{ sizeof(icon) };
        icon.hWnd = window_;
        icon.uID = 1;
        icon.uFlags = NIF_TIP;
        wcsncpy_s(icon.szTip, status_.c_str(), _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &icon);
    }

    void StartWatcher() {
        try {
            unsigned current = ++generation_;
            status_ = L"正在扫描设备";
            UpdateTip();
            watcher_ = DeviceInformation::CreateWatcher(AudioPlaybackConnection::GetDeviceSelector());
            auto callbackWindow = callbackWindow_;
            watcher_.Added([callbackWindow, current](DeviceWatcher const&, DeviceInformation const& info) {
                PostOwned(callbackWindow->load(), WM_DEVICE, new DeviceEvent{ current, DeviceEvent::Kind::Added, info.Id().c_str(), info.Name().c_str() });
            });
            watcher_.Updated([callbackWindow, current](DeviceWatcher const&, DeviceInformationUpdate const& info) {
                PostOwned(callbackWindow->load(), WM_DEVICE, new DeviceEvent{ current, DeviceEvent::Kind::Updated, info.Id().c_str(), L"" });
            });
            watcher_.Removed([callbackWindow, current](DeviceWatcher const&, DeviceInformationUpdate const& info) {
                PostOwned(callbackWindow->load(), WM_DEVICE, new DeviceEvent{ current, DeviceEvent::Kind::Removed, info.Id().c_str(), L"" });
            });
            watcher_.EnumerationCompleted([callbackWindow, current](DeviceWatcher const&, winrt::Windows::Foundation::IInspectable const&) {
                PostOwned(callbackWindow->load(), WM_DEVICE, new DeviceEvent{ current, DeviceEvent::Kind::Completed, L"", L"" });
            });
            watcher_.Start();
        } catch (...) {
            watcher_ = nullptr;
            status_ = L"扫描设备失败";
            UpdateTip();
        }
    }

    void Refresh() {
        ++generation_;
        if (watcher_) {
            try { watcher_.Stop(); } catch (...) {}
            watcher_ = nullptr;
        }
        devices_.clear();
        StartWatcher();
    }

    void Connect(std::wstring const& id) {
        if (connections_.contains(id) || pending_.contains(id)) return;
        if (connections_.size() + pending_.size() >= MAX_CONNECTIONS) {
            status_ = L"最多同时连接 4 台设备";
            UpdateTip();
            return;
        }
        auto serial = ++nextSerial_;
        pending_.emplace(id, serial);
        status_ = L"正在连接 " + DisplayName(id);
        UpdateTip();
        auto callbackWindow = callbackWindow_;
        workers_.emplace_back([id, serial, callbackWindow] {
            auto event = std::make_unique<ConnectEvent>();
            event->id = id;
            event->serial = serial;
            try {
                init_apartment(apartment_type::multi_threaded);
                auto connection = AudioPlaybackConnection::TryCreateFromId(hstring{ id });
                if (!connection) event->error = L"系统无法创建音频连接";
                else {
                    connection.StateChanged([callbackWindow, id](AudioPlaybackConnection const& sender, winrt::Windows::Foundation::IInspectable const&) {
                        try {
                            PostOwned(callbackWindow->load(), WM_STATE, new StateEvent{ id, sender.State() });
                        } catch (...) {}
                    });
                    connection.StartAsync().get();
                    auto result = connection.OpenAsync().get();
                    if (result.Status() == AudioPlaybackConnectionOpenResultStatus::Success)
                        event->connection = connection;
                    else if (result.Status() == AudioPlaybackConnectionOpenResultStatus::RequestTimedOut)
                        event->error = L"连接请求超时";
                    else if (result.Status() == AudioPlaybackConnectionOpenResultStatus::DeniedBySystem)
                        event->error = L"系统拒绝连接";
                    else event->error = L"连接失败";
                }
            } catch (...) { event->error = L"连接失败"; }
            PostOwned(callbackWindow->load(), WM_CONNECT, event.release());
        });
    }

    void Disconnect(std::wstring const& id) {
        pending_.erase(id);
        auto found = connections_.find(id);
        if (found != connections_.end()) {
            try { found->second.Close(); } catch (...) {}
            connections_.erase(found);
        }
        status_ = connections_.empty() ? L"未连接" : L"已连接";
        UpdateTip();
    }

    void DisconnectAll() {
        KillTimer(window_, 1);
        restoreIds_.clear();
        auto ids = connections_;
        for (auto const& [id, ignored] : ids) Disconnect(id);
        pending_.clear();
        Save();
    }

    void Save() {
        settings_.lastIds.clear();
        for (auto const& [id, ignored] : connections_) settings_.lastIds.push_back(id);
        SaveSettings(settings_);
    }

    void AddMenuItem(HMENU menu, UINT flags, UINT_PTR id, std::wstring text,
        bool checkable = false, bool checked = false, bool status = false, bool submenu = false) {
        auto entry = std::make_unique<MenuEntry>();
        entry->text = std::move(text);
        entry->checkable = checkable;
        entry->checked = checked;
        entry->status = status;
        entry->submenu = submenu;
        if (status) {
            for (auto const& [deviceId, ignored] : connections_) entry->lights.push_back(2);
            for (auto const& [deviceId, ignored] : pending_) entry->lights.push_back(1);
            for (auto const& [deviceId, ignored] : devices_)
                if (entry->lights.size() < MAX_CONNECTIONS && !connections_.contains(deviceId) && !pending_.contains(deviceId))
                    entry->lights.push_back(1);
        }
        auto* pointer = entry.get();
        menuEntries_.push_back(std::move(entry));
        AppendMenuW(menu, flags | MF_OWNERDRAW, id, reinterpret_cast<LPCWSTR>(pointer));
    }

    void DrawMenuItem(DRAWITEMSTRUCT const& draw) {
        auto* entry = reinterpret_cast<MenuEntry*>(draw.itemData);
        RECT rect = draw.rcItem;
        bool selected = (draw.itemState & ODS_SELECTED) != 0;
        bool disabled = (draw.itemState & ODS_DISABLED) != 0;
        HBRUSH background = CreateSolidBrush(selected && !disabled ? RGB(235, 240, 245) : RGB(250, 250, 250));
        FillRect(draw.hDC, &rect, background);
        DeleteObject(background);
        SetBkMode(draw.hDC, TRANSPARENT);
        HGDIOBJ oldFont = menuFont_ ? SelectObject(draw.hDC, menuFont_) : nullptr;
        if (entry->status) {
            int diameter = 10;
            int spacing = 16;
            int width = diameter * 4 + spacing * 3;
            int x = rect.left + (rect.right - rect.left - width) / 2;
            int y = rect.top + (rect.bottom - rect.top - diameter) / 2;
            for (size_t i = 0; i < MAX_CONNECTIONS; ++i) {
                int state = i < entry->lights.size() ? entry->lights[i] : 0;
                COLORREF color = state == 2 ? RGB(76, 175, 80) : state == 1 ? RGB(33, 150, 243) : RGB(228, 228, 228);
                HBRUSH brush = CreateSolidBrush(color);
                HGDIOBJ oldBrush = SelectObject(draw.hDC, brush);
                HGDIOBJ oldPen = SelectObject(draw.hDC, GetStockObject(NULL_PEN));
                Ellipse(draw.hDC, x, y, x + diameter, y + diameter);
                SelectObject(draw.hDC, oldPen);
                SelectObject(draw.hDC, oldBrush);
                DeleteObject(brush);
                x += diameter + spacing;
            }
            if (oldFont) SelectObject(draw.hDC, oldFont);
            return;
        }
        if (entry->checkable) {
            RECT box{ rect.left + 10, rect.top + 6, rect.left + 22, rect.top + 18 };
            HBRUSH brush = CreateSolidBrush(entry->checked ? RGB(76, 175, 80) : RGB(250, 250, 250));
            HGDIOBJ oldBrush = SelectObject(draw.hDC, brush);
            HPEN pen = CreatePen(PS_SOLID, 1, entry->checked ? RGB(76, 175, 80) : RGB(200, 200, 200));
            HGDIOBJ oldPen = SelectObject(draw.hDC, pen);
            RoundRect(draw.hDC, box.left, box.top, box.right, box.bottom, 4, 4);
            SelectObject(draw.hDC, oldPen);
            SelectObject(draw.hDC, oldBrush);
            DeleteObject(pen);
            DeleteObject(brush);
            if (entry->checked) {
                HPEN check = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
                oldPen = SelectObject(draw.hDC, check);
                MoveToEx(draw.hDC, box.left + 2, box.top + 6, nullptr);
                LineTo(draw.hDC, box.left + 5, box.top + 9);
                LineTo(draw.hDC, box.left + 10, box.top + 3);
                SelectObject(draw.hDC, oldPen);
                DeleteObject(check);
            }
        }
        SetTextColor(draw.hDC, disabled ? RGB(160, 160, 160) : RGB(45, 45, 45));
        int textY = rect.top + (rect.bottom - rect.top - 15) / 2;
        TextOutW(draw.hDC, rect.left + 32, textY, entry->text.c_str(), static_cast<int>(entry->text.size()));
        if (entry->submenu) {
            TextOutW(draw.hDC, rect.right - 16, textY, L"›", 1);
        }
        if (oldFont) SelectObject(draw.hDC, oldFont);
    }

    void ShowMenu() {
        HMENU menu = CreatePopupMenu();
        HMENU devices = CreatePopupMenu();
        menuIds_.clear();
        menuEntries_.clear();
        std::vector<std::wstring> ids;
        for (auto const& [id, ignored] : devices_) ids.push_back(id);
        std::sort(ids.begin(), ids.end(), [this](auto const& a, auto const& b) { return DisplayName(a) < DisplayName(b); });
        // Connected devices remain visible even if the watcher temporarily removes them.
        for (auto const& [id, ignored] : connections_)
            if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
        for (auto const& id : ids) {
            UINT index = ID_DEVICE_BASE + static_cast<UINT>(menuIds_.size());
            if (index > 60000) break;
            menuIds_.push_back(id);
            bool connected = connections_.contains(id);
            UINT flags = (connected ? MF_CHECKED : 0) |
                (!connected && connections_.size() + pending_.size() >= MAX_CONNECTIONS ? MF_GRAYED : 0);
            AddMenuItem(devices, flags, index, DisplayName(id), true, connected);
        }
        if (menuIds_.empty()) AppendMenuW(devices, MF_STRING | MF_GRAYED, 0, L"未发现 A2DP 设备");
        AddMenuItem(menu, MF_GRAYED, 0, L"", false, false, true);
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AddMenuItem(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(devices), L"设备", false, false, false, true);
        AddMenuItem(menu, 0, ID_REFRESH, L"刷新设备");
        AddMenuItem(menu, 0, ID_DISCONNECT_ALL, L"断开全部");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AddMenuItem(menu, settings_.reconnect ? MF_CHECKED : 0, ID_RECONNECT, L"启动自动重连", true, settings_.reconnect);
        bool startup = StartupEnabled();
        AddMenuItem(menu, startup ? MF_CHECKED : 0, ID_STARTUP, L"开机自启动", true, startup);
        AddMenuItem(menu, 0, ID_BLUETOOTH, L"蓝牙设置");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AddMenuItem(menu, 0, ID_EXIT, L"退出");
        POINT point{};
        GetCursorPos(&point);
        SetForegroundWindow(window_);
        UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN,
            point.x, point.y, 0, window_, nullptr);
        DestroyMenu(menu);
        menuEntries_.clear();
        PostMessageW(window_, WM_NULL, 0, 0);
        if (command) Command(command);
    }

    void Command(UINT command) {
        if (command >= ID_DEVICE_BASE && command - ID_DEVICE_BASE < menuIds_.size()) {
            auto id = menuIds_[command - ID_DEVICE_BASE];
            if (connections_.contains(id)) Disconnect(id);
            else Connect(id);
            Save();
            return;
        }
        switch (command) {
        case ID_REFRESH: Refresh(); break;
        case ID_DISCONNECT_ALL: DisconnectAll(); break;
        case ID_RECONNECT: settings_.reconnect = !settings_.reconnect; Save(); break;
        case ID_STARTUP:
            if (SetStartup(!StartupEnabled())) { settings_.startup = StartupEnabled(); Save(); }
            break;
        case ID_BLUETOOTH: ShellExecuteW(window_, L"open", L"ms-settings:bluetooth", nullptr, nullptr, SW_SHOWNORMAL); break;
        case ID_EXIT: Save(); DestroyWindow(window_); break;
        }
    }

    void Shutdown() {
        callbackWindow_->store(nullptr);
        KillTimer(window_, 1);
        ++generation_;
        if (watcher_) { try { watcher_.Stop(); } catch (...) {} watcher_ = nullptr; }
        for (auto& worker : workers_) if (worker.joinable()) worker.join();
        for (auto const& [id, connection] : connections_) { try { connection.Close(); } catch (...) {} }
        connections_.clear();
        NOTIFYICONDATAW icon{ sizeof(icon) };
        icon.hWnd = window_;
        icon.uID = 1;
        Shell_NotifyIconW(NIM_DELETE, &icon);
        if (menuFont_) { DeleteObject(menuFont_); menuFont_ = nullptr; }
        MSG message{};
        while (PeekMessageW(&message, window_, WM_DEVICE, WM_STATE, PM_REMOVE)) {
            if (message.message == WM_DEVICE) delete reinterpret_cast<DeviceEvent*>(message.lParam);
            else if (message.message == WM_CONNECT) delete reinterpret_cast<ConnectEvent*>(message.lParam);
            else if (message.message == WM_STATE) delete reinterpret_cast<StateEvent*>(message.lParam);
        }
    }
};
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    try {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        HANDLE mutex = CreateMutexW(nullptr, TRUE, L"EchoBridge.Native.SingleInstance");
        if (!mutex) return 1;
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            MessageBoxW(nullptr, L"EchoBridge 已在运行。", L"EchoBridge", MB_OK | MB_ICONINFORMATION);
            CloseHandle(mutex);
            return 0;
        }
        App app;
        int result = app.Run(instance);
        CloseHandle(mutex);
        return result;
    } catch (...) {
        MessageBoxW(nullptr, L"EchoBridge 启动失败。", L"EchoBridge", MB_OK | MB_ICONERROR);
        return 1;
    }
}
