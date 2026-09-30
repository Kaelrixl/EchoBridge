#pragma once

#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <functional>
#include <string>
#include <utility>
#include <vector>

// The C# edition uses a custom-painted ToolStripDropDownMenu. A native HMENU
// has its own non-client margins and cannot reproduce that layout reliably.
class TrayMenu {
public:
    enum class Kind { Status, Separator, Item, Devices };
    struct Entry {
        Kind kind = Kind::Item;
        std::wstring text;
        UINT command = 0;
        bool enabled = true;
        bool checkable = false;
        bool checked = false;
        std::vector<int> lights;
    };

    ~TrayMenu() {
        Close();
        if (font_) DeleteObject(font_);
        if (gdiplusToken_) Gdiplus::GdiplusShutdown(gdiplusToken_);
    }

    void Show(HINSTANCE instance, HWND owner, std::vector<Entry> main,
        std::vector<Entry> devices, std::function<void(UINT)> onCommand) {
        Close();
        instance_ = instance;
        owner_ = owner;
        main_ = std::move(main);
        devices_ = std::move(devices);
        onCommand_ = std::move(onCommand);
        selectedMain_ = -1;
        selectedDevice_ = -1;
        submenuOpen_ = false;
        if (!gdiplusToken_) {
            Gdiplus::GdiplusStartupInput input;
            Gdiplus::GdiplusStartup(&gdiplusToken_, &input, nullptr);
        }
        if (!font_) {
            HDC screen = GetDC(nullptr);
            int dpi = GetDeviceCaps(screen, LOGPIXELSY);
            ReleaseDC(nullptr, screen);
            font_ = CreateFontW(-MulDiv(9, dpi, 72), 0, 0, 0, FW_NORMAL,
                FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                L"Microsoft YaHei UI");
        }
        if (!RegisterWindowClass()) return;
        POINT cursor{};
        GetCursorPos(&cursor);
        MONITORINFO monitor{ sizeof(monitor) };
        GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &monitor);
        RECT work = monitor.rcWork;
        rootX_ = std::clamp(static_cast<int>(cursor.x), static_cast<int>(work.left),
            static_cast<int>(work.right) - rootWidth_);
        rootY_ = cursor.y - RootHeight();
        if (rootY_ < work.top) rootY_ = std::min(static_cast<int>(cursor.y),
            static_cast<int>(work.bottom) - RootHeight());
        rootY_ = std::clamp(rootY_, static_cast<int>(work.top),
            static_cast<int>(work.bottom) - RootHeight());
        submenuLeft_ = rootX_ + rootWidth_ + SubmenuWidth() > work.right;
        window_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
            L"EchoBridgeNativeTrayMenu", L"", WS_POPUP,
            rootX_, rootY_, rootWidth_, RootHeight(), owner_, nullptr,
            instance_, this);
        if (!window_) return;
        UpdateBounds();
        DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUNDSMALL;
        DwmSetWindowAttribute(window_, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
        COLORREF noSystemBorder = 0xFFFFFFFE;
        DwmSetWindowAttribute(window_, DWMWA_BORDER_COLOR, &noSystemBorder, sizeof(noSystemBorder));
        ShowWindow(window_, SW_SHOWNORMAL);
        SetForegroundWindow(window_);
        SetFocus(window_);
    }

    void Close() {
        if (window_) DestroyWindow(window_);
    }

private:
    static constexpr int rootWidth_ = 148;
    static constexpr int rowHeight_ = 22;
    static constexpr int separatorHeight_ = 5;
    static constexpr int statusHeight_ = 28;
    static constexpr int overlap_ = 6;
    HINSTANCE instance_{};
    HWND owner_{};
    HWND window_{};
    HFONT font_{};
    ULONG_PTR gdiplusToken_{};
    std::vector<Entry> main_;
    std::vector<Entry> devices_;
    std::function<void(UINT)> onCommand_;
    int rootX_{};
    int rootY_{};
    int mainOriginX_{};
    int submenuOriginX_{};
    int submenuOriginY_{};
    int selectedMain_ = -1;
    int selectedDevice_ = -1;
    bool submenuOpen_ = false;
    bool submenuLeft_ = false;

    static int Height(Entry const& entry) {
        if (entry.kind == Kind::Status) return statusHeight_;
        if (entry.kind == Kind::Separator) return separatorHeight_;
        return rowHeight_;
    }

    int RootHeight() const {
        int height = 2;
        for (auto const& entry : main_) height += Height(entry);
        return height;
    }

    int SubmenuHeight() const { return 2 + static_cast<int>(devices_.size()) * rowHeight_; }

    int SubmenuWidth() const {
        int width = 148;
        HDC screen = GetDC(nullptr);
        HGDIOBJ old = font_ ? SelectObject(screen, font_) : nullptr;
        for (auto const& entry : devices_) {
            SIZE size{};
            GetTextExtentPoint32W(screen, entry.text.c_str(), static_cast<int>(entry.text.size()), &size);
            width = std::max(width, static_cast<int>(size.cx) + 48);
        }
        if (old) SelectObject(screen, old);
        ReleaseDC(nullptr, screen);
        return width;
    }

    int DevicesTop() const {
        int top = 1;
        for (auto const& entry : main_) {
            if (entry.kind == Kind::Devices) return top;
            top += Height(entry);
        }
        return 1;
    }

    bool RegisterWindowClass() {
        WNDCLASSEXW klass{ sizeof(klass) };
        klass.lpfnWndProc = WindowProc;
        klass.hInstance = instance_;
        klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        klass.lpszClassName = L"EchoBridgeNativeTrayMenu";
        return RegisterClassExW(&klass) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }

    void UpdateBounds() {
        if (!window_) return;
        int subWidth = SubmenuWidth();
        mainOriginX_ = submenuOpen_ && submenuLeft_ ? subWidth - overlap_ : 0;
        submenuOriginX_ = submenuLeft_ ? 0 : rootWidth_ - overlap_;
        submenuOriginY_ = DevicesTop();
        int x = rootX_ - mainOriginX_;
        int width = rootWidth_ + (submenuOpen_ ? subWidth - overlap_ : 0);
        int height = submenuOpen_ ? std::max(RootHeight(), submenuOriginY_ + SubmenuHeight()) : RootHeight();
        HRGN shape = CreateRoundRectRgn(mainOriginX_, 0, mainOriginX_ + rootWidth_, RootHeight(), 11, 11);
        if (submenuOpen_) {
            HRGN sub = CreateRoundRectRgn(submenuOriginX_, submenuOriginY_,
                submenuOriginX_ + subWidth, submenuOriginY_ + SubmenuHeight(), 11, 11);
            CombineRgn(shape, shape, sub, RGN_OR);
            DeleteObject(sub);
        }
        SetWindowPos(window_, HWND_TOPMOST, x, rootY_, width, height,
            SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        SetWindowRgn(window_, shape, TRUE); // Window owns the region after success.
        InvalidateRect(window_, nullptr, FALSE);
    }

    static void Fill(HDC dc, RECT area, COLORREF color) {
        HBRUSH brush = CreateSolidBrush(color);
        FillRect(dc, &area, brush);
        DeleteObject(brush);
    }

    static void RoundedPath(Gdiplus::GraphicsPath& path, float x, float y,
        float width, float height, float radius) {
        float diameter = radius * 2;
        path.AddArc(x, y, diameter, diameter, 180, 90);
        path.AddArc(x + width - diameter, y, diameter, diameter, 270, 90);
        path.AddArc(x + width - diameter, y + height - diameter, diameter, diameter, 0, 90);
        path.AddArc(x, y + height - diameter, diameter, diameter, 90, 90);
        path.CloseFigure();
    }

    static Gdiplus::Color Color(COLORREF color) {
        return Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color));
    }

    static void Outline(HDC dc, RECT area, COLORREF color, float radius) {
        Gdiplus::Graphics graphics(dc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::Pen pen(Color(color), 1.0f);
        Gdiplus::GraphicsPath path;
        RoundedPath(path, static_cast<float>(area.left), static_cast<float>(area.top),
            static_cast<float>(area.right - area.left - 1),
            static_cast<float>(area.bottom - area.top - 1), radius);
        graphics.DrawPath(&pen, &path);
    }

    void DrawCheck(HDC dc, int x, int y, bool checked) {
        Gdiplus::Graphics graphics(dc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::GraphicsPath path;
        RoundedPath(path, static_cast<float>(x), static_cast<float>(y), 12.0f, 12.0f, 2.0f);
        if (checked) {
            Gdiplus::SolidBrush brush(Color(RGB(76, 175, 80)));
            graphics.FillPath(&brush, &path);
            Gdiplus::Pen check(Gdiplus::Color(255, 255, 255, 255), 1.5f);
            graphics.DrawLine(&check, x + 3.2f, y + 6.0f, x + 5.2f, y + 8.0f);
            graphics.DrawLine(&check, x + 5.2f, y + 8.0f, x + 9.7f, y + 3.5f);
        } else {
            Gdiplus::Pen pen(Color(RGB(200, 200, 200)), 1.0f);
            graphics.DrawPath(&pen, &path);
        }
    }

    void DrawPanel(HDC dc, int x, int y, int width, std::vector<Entry> const& entries,
        int selected, bool root) {
        int height = root ? RootHeight() : SubmenuHeight();
        RECT panel{ x, y, x + width, y + height };
        Fill(dc, panel, RGB(250, 250, 250));
        int top = y + 1;
        HGDIOBJ oldFont = font_ ? SelectObject(dc, font_) : nullptr;
        SetBkMode(dc, TRANSPARENT);
        for (size_t i = 0; i < entries.size(); ++i) {
            Entry const& entry = entries[i];
            int row = Height(entry);
            if (entry.kind == Kind::Separator) {
                HPEN pen = CreatePen(PS_SOLID, 1, RGB(228, 228, 228));
                HGDIOBJ oldPen = SelectObject(dc, pen);
                MoveToEx(dc, x + 10, top + row / 2, nullptr);
                LineTo(dc, x + width - 10, top + row / 2);
                SelectObject(dc, oldPen);
                DeleteObject(pen);
            } else if (entry.kind == Kind::Status) {
                int dotX = x + (width - 88) / 2;
                Gdiplus::Graphics graphics(dc);
                graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                for (int dot = 0; dot < 4; ++dot) {
                    int state = dot < static_cast<int>(entry.lights.size()) ? entry.lights[dot] : 0;
                    COLORREF color = state == 2 ? RGB(76, 175, 80) : state == 1 ? RGB(33, 150, 243) : RGB(228, 228, 228);
                    Gdiplus::SolidBrush brush(Color(color));
                    graphics.FillEllipse(&brush, static_cast<float>(dotX),
                        static_cast<float>(top) + (row - 10) / 2.0f, 10.0f, 10.0f);
                    dotX += 26;
                }
            } else {
                if (static_cast<int>(i) == selected && entry.enabled) {
                    RECT highlight{ x + 1, top, x + width - 1, top + row };
                    Fill(dc, highlight, RGB(235, 240, 245));
                }
                if (entry.checkable) DrawCheck(dc, x + 10, top + (row - 12) / 2 - 2, entry.checked);
                SetTextColor(dc, entry.enabled ? RGB(45, 45, 45) : RGB(160, 160, 160));
                RECT textRect{ x + 34, top, x + width - 18, top + row };
                DrawTextW(dc, entry.text.c_str(), static_cast<int>(entry.text.size()),
                    &textRect, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_LEFT);
                if (entry.kind == Kind::Devices) {
                    RECT arrowRect{ x + width - 15, top, x + width - 2, top + row };
                    DrawTextW(dc, L"›", 1, &arrowRect,
                        DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_LEFT);
                }
            }
            top += row;
        }
        if (oldFont) SelectObject(dc, oldFont);
        Outline(dc, panel, RGB(218, 218, 218), 11);
    }

    void Paint() {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window_, &paint);
        RECT bounds{};
        GetClientRect(window_, &bounds);
        HDC buffer = CreateCompatibleDC(dc);
        HBITMAP bitmap = CreateCompatibleBitmap(dc, bounds.right, bounds.bottom);
        HGDIOBJ oldBitmap = SelectObject(buffer, bitmap);
        Fill(buffer, bounds, RGB(250, 250, 250));
        DrawPanel(buffer, mainOriginX_, 0, rootWidth_, main_, selectedMain_, true);
        if (submenuOpen_) DrawPanel(buffer, submenuOriginX_, submenuOriginY_,
            SubmenuWidth(), devices_, selectedDevice_, false);
        BitBlt(dc, 0, 0, bounds.right, bounds.bottom, buffer, 0, 0, SRCCOPY);
        SelectObject(buffer, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(buffer);
        EndPaint(window_, &paint);
    }

    int HitMain(int x, int y) const {
        if (x < mainOriginX_ || x >= mainOriginX_ + rootWidth_ || y < 1) return -1;
        int top = 1;
        for (size_t i = 0; i < main_.size(); ++i) {
            int bottom = top + Height(main_[i]);
            if (y >= top && y < bottom) return static_cast<int>(i);
            top = bottom;
        }
        return -1;
    }

    int HitDevice(int x, int y) const {
        if (!submenuOpen_ || x < submenuOriginX_ || x >= submenuOriginX_ + SubmenuWidth() ||
            y < submenuOriginY_ + 1) return -1;
        int index = (y - submenuOriginY_ - 1) / rowHeight_;
        return index < static_cast<int>(devices_.size()) ? index : -1;
    }

    void MouseMove(int x, int y) {
        int device = HitDevice(x, y);
        if (device >= 0) {
            if (selectedDevice_ != device) { selectedDevice_ = device; InvalidateRect(window_, nullptr, FALSE); }
            return;
        }
        int main = HitMain(x, y);
        if (main >= 0 && main_[main].kind != Kind::Separator && main_[main].kind != Kind::Status) {
            if (selectedMain_ != main || submenuOpen_ != (main_[main].kind == Kind::Devices)) {
                selectedMain_ = main;
                selectedDevice_ = -1;
                submenuOpen_ = main_[main].kind == Kind::Devices;
                UpdateBounds();
            }
        }
    }

    void Activate(Entry const& entry) {
        if (!entry.enabled || !entry.command) return;
        UINT command = entry.command;
        Close();
        if (onCommand_) onCommand_(command);
    }

    void KeyDown(WPARAM key) {
        if (key == VK_ESCAPE) { if (submenuOpen_) { submenuOpen_ = false; UpdateBounds(); } else Close(); return; }
        if (key == VK_LEFT && submenuOpen_) { submenuOpen_ = false; UpdateBounds(); return; }
        if (key == VK_RIGHT && selectedMain_ >= 0 && main_[selectedMain_].kind == Kind::Devices) {
            submenuOpen_ = true;
            selectedDevice_ = -1;
            for (size_t i = 0; i < devices_.size(); ++i)
                if (devices_[i].enabled) { selectedDevice_ = static_cast<int>(i); break; }
            UpdateBounds();
            return;
        }
        if (key == VK_RETURN) {
            if (submenuOpen_ && selectedDevice_ >= 0) Activate(devices_[selectedDevice_]);
            else if (selectedMain_ >= 0) Activate(main_[selectedMain_]);
            return;
        }
        if (key != VK_DOWN && key != VK_UP) return;
        if (submenuOpen_) {
            int direction = key == VK_DOWN ? 1 : -1;
            int count = static_cast<int>(devices_.size());
            int index = selectedDevice_;
            for (int step = 0; step < count; ++step) {
                index = (index + direction + count) % count;
                if (devices_[index].enabled) { selectedDevice_ = index; break; }
            }
        } else {
            int direction = key == VK_DOWN ? 1 : -1;
            int count = static_cast<int>(main_.size());
            int index = selectedMain_;
            for (int step = 0; step < count; ++step) {
                index = (index + direction + count) % count;
                if (main_[index].enabled &&
                    (main_[index].kind == Kind::Item || main_[index].kind == Kind::Devices)) {
                    selectedMain_ = index;
                    break;
                }
            }
        }
        InvalidateRect(window_, nullptr, FALSE);
    }

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        TrayMenu* self = reinterpret_cast<TrayMenu*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<TrayMenu*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            return TRUE;
        }
        if (!self) return DefWindowProcW(window, message, wparam, lparam);
        switch (message) {
        case WM_PAINT: self->Paint(); return 0;
        case WM_MOUSEMOVE: self->MouseMove(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)); return 0;
        case WM_LBUTTONUP: {
            int x = GET_X_LPARAM(lparam), y = GET_Y_LPARAM(lparam);
            int device = self->HitDevice(x, y);
            if (device >= 0) self->Activate(self->devices_[device]);
            else {
                int main = self->HitMain(x, y);
                if (main >= 0) self->Activate(self->main_[main]);
            }
            return 0;
        }
        case WM_RBUTTONUP: self->Close(); return 0;
        case WM_KEYDOWN: self->KeyDown(wparam); return 0;
        case WM_ACTIVATE:
            if (LOWORD(wparam) == WA_INACTIVE) self->Close();
            return 0;
        case WM_NCDESTROY:
            self->window_ = nullptr;
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            return DefWindowProcW(window, message, wparam, lparam);
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }
};
