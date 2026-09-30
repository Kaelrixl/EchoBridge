#include <windows.h>
#include <shellapi.h>
#include <dwmapi.h>
#include "TrayMenu.h"
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
    TrayMenu trayMenu_;

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

    void ShowMenu() {
        menuIds_.clear();
        std::vector<std::wstring> ids;
        for (auto const& [id, ignored] : devices_) ids.push_back(id);
        std::sort(ids.begin(), ids.end(), [this](auto const& a, auto const& b) { return DisplayName(a) < DisplayName(b); });
        // Connected devices remain visible even if the watcher temporarily removes them.
        for (auto const& [id, ignored] : connections_)
            if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
        std::vector<TrayMenu::Entry> deviceEntries;
        for (auto const& id : ids) {
            UINT index = ID_DEVICE_BASE + static_cast<UINT>(menuIds_.size());
            if (index > 60000) break;
            menuIds_.push_back(id);
            bool connected = connections_.contains(id);
            bool enabled = connected || connections_.size() + pending_.size() < MAX_CONNECTIONS;
            deviceEntries.push_back({ TrayMenu::Kind::Item, DisplayName(id), index,
                enabled, true, connected });
        }
        if (deviceEntries.empty()) deviceEntries.push_back({ TrayMenu::Kind::Item,
            L"未发现 A2DP 设备", 0, false });
        TrayMenu::Entry status{ TrayMenu::Kind::Status };
        for (auto const& [id, ignored] : connections_) status.lights.push_back(2);
        for (auto const& [id, ignored] : pending_) status.lights.push_back(1);
        for (auto const& [id, ignored] : devices_)
            if (status.lights.size() < MAX_CONNECTIONS && !connections_.contains(id) && !pending_.contains(id))
                status.lights.push_back(1);
        bool startup = StartupEnabled();
        std::vector<TrayMenu::Entry> mainEntries;
        mainEntries.push_back(std::move(status));
        mainEntries.push_back({ TrayMenu::Kind::Separator });
        mainEntries.push_back({ TrayMenu::Kind::Devices, L"设备" });
        mainEntries.push_back({ TrayMenu::Kind::Item, L"刷新设备", ID_REFRESH });
        mainEntries.push_back({ TrayMenu::Kind::Item, L"断开全部", ID_DISCONNECT_ALL });
        mainEntries.push_back({ TrayMenu::Kind::Separator });
        mainEntries.push_back({ TrayMenu::Kind::Item, L"启动自动重连", ID_RECONNECT,
            true, true, settings_.reconnect });
        mainEntries.push_back({ TrayMenu::Kind::Item, L"开机自启动", ID_STARTUP,
            true, true, startup });
        mainEntries.push_back({ TrayMenu::Kind::Item, L"蓝牙设置", ID_BLUETOOTH });
        mainEntries.push_back({ TrayMenu::Kind::Separator });
        mainEntries.push_back({ TrayMenu::Kind::Item, L"退出", ID_EXIT });
        trayMenu_.Show(instance_, window_, std::move(mainEntries), std::move(deviceEntries),
            [this](UINT command) { Command(command); });
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
        trayMenu_.Close();
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
