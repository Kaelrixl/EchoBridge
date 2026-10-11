#pragma once

#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <shellscalingapi.h>
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
        if (!RegisterWindowClass()) return;
        POINT cursor{};
        GetCursorPos(&cursor);
        MONITORINFO monitor{ sizeof(monitor) };
        HMONITOR display = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
        GetMonitorInfoW(display, &monitor);
        UINT monitorDpiX = 96, monitorDpiY = 96;
        if (FAILED(GetDpiForMonitor(display, MDT_EFFECTIVE_DPI,
                &monitorDpiX, &monitorDpiY))) {
            monitorDpiX = 96;
        }
        window_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED,
            L"EchoBridgeNativeTrayMenu", L"", WS_POPUP,
            cursor.x, cursor.y, 1, 1, owner_, nullptr,
            instance_, this);
        if (!window_) return;
        dpi_ = monitorDpiX;
        rootWidth_ = Scale(148);
        rowHeight_ = Scale(22);
        separatorHeight_ = Scale(5);
        statusHeight_ = Scale(28);
        overlap_ = Scale(6);
        if (fontDpi_ != dpi_) {
            if (font_) DeleteObject(font_);
            font_ = CreateFontW(-MulDiv(9, dpi_, 72), 0, 0, 0, FW_NORMAL,
                FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                L"Microsoft YaHei UI");
            fontDpi_ = dpi_;
        }
        RECT work = monitor.rcWork;
        rootX_ = std::clamp(static_cast<int>(cursor.x), static_cast<int>(work.left),
            static_cast<int>(work.right) - rootWidth_);
        rootY_ = cursor.y - RootHeight();
        if (rootY_ < work.top) rootY_ = std::min(static_cast<int>(cursor.y),
            static_cast<int>(work.bottom) - RootHeight());
        rootY_ = std::clamp(rootY_, static_cast<int>(work.top),
            static_cast<int>(work.bottom) - RootHeight());
        submenuLeft_ = rootX_ + rootWidth_ + SubmenuWidth() > work.right;
        DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUNDSMALL;
        DwmSetWindowAttribute(window_, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
        COLORREF noSystemBorder = 0xFFFFFFFE;
        DwmSetWindowAttribute(window_, DWMWA_BORDER_COLOR, &noSystemBorder, sizeof(noSystemBorder));
        RenderMain();
        ShowWindow(window_, SW_SHOWNORMAL);
        SetForegroundWindow(window_);
        SetFocus(window_);
    }

    void Close() {
        if (submenuWindow_) DestroyWindow(submenuWindow_);
        if (window_) DestroyWindow(window_);
    }

private:
    int rootWidth_ = 148;
    int rowHeight_ = 22;
    int separatorHeight_ = 5;
    int statusHeight_ = 28;
    int overlap_ = 6;
    UINT dpi_ = 96;
    UINT fontDpi_ = 0;
    HINSTANCE instance_{};
    HWND owner_{};
    HWND window_{};
    HWND submenuWindow_{};
    HFONT font_{};
    ULONG_PTR gdiplusToken_{};
    std::vector<Entry> main_;
    std::vector<Entry> devices_;
    std::function<void(UINT)> onCommand_;
    int rootX_{};
    int rootY_{};
    int selectedMain_ = -1;
    int selectedDevice_ = -1;
    bool submenuOpen_ = false;
    bool submenuLeft_ = false;

    int Scale(int value) const { return MulDiv(value, static_cast<int>(dpi_), 96); }
    float Scale(float value) const { return value * static_cast<float>(dpi_) / 96.0f; }

    int Height(Entry const& entry) const {
        if (entry.kind == Kind::Status) return statusHeight_;
        if (entry.kind == Kind::Separator) return separatorHeight_;
        return rowHeight_;
    }

    int RootHeight() const {
        int height = Scale(2);
        for (auto const& entry : main_) height += Height(entry);
        return height;
    }

    int SubmenuHeight() const { return Scale(2) + static_cast<int>(devices_.size()) * rowHeight_; }

    int SubmenuWidth() const {
        int width = Scale(148);
        HDC screen = GetDC(nullptr);
        HGDIOBJ old = font_ ? SelectObject(screen, font_) : nullptr;
        for (auto const& entry : devices_) {
            SIZE size{};
            GetTextExtentPoint32W(screen, entry.text.c_str(), static_cast<int>(entry.text.size()), &size);
            width = std::max(width, static_cast<int>(size.cx) + Scale(48));
        }
        if (old) SelectObject(screen, old);
        ReleaseDC(nullptr, screen);
        return width;
    }

    int DevicesTop() const {
        int top = Scale(1);
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
        if (!RegisterClassExW(&klass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
        klass.lpszClassName = L"EchoBridgeNativeTraySubmenu";
        return RegisterClassExW(&klass) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }

    void UpdateSubmenu() {
        RenderMain();
        if (!submenuOpen_) {
            if (submenuWindow_) DestroyWindow(submenuWindow_);
            return;
        }
        if (!submenuWindow_) {
            submenuWindow_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST |
                    WS_EX_LAYERED | WS_EX_NOACTIVATE,
                L"EchoBridgeNativeTraySubmenu", L"", WS_POPUP,
                0, 0, SubmenuWidth(), SubmenuHeight(), window_, nullptr,
                instance_, this);
            if (!submenuWindow_) return;
            DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUNDSMALL;
            DwmSetWindowAttribute(submenuWindow_, DWMWA_WINDOW_CORNER_PREFERENCE,
                &corner, sizeof(corner));
            COLORREF noSystemBorder = 0xFFFFFFFE;
            DwmSetWindowAttribute(submenuWindow_, DWMWA_BORDER_COLOR,
                &noSystemBorder, sizeof(noSystemBorder));
            RenderSubmenu();
            ShowWindow(submenuWindow_, SW_SHOWNOACTIVATE);
        } else {
            RenderSubmenu();
        }
    }

    static float RoundedCoverage(float px, float py, float x, float y,
        float width, float height, float radius) {
        if (px < x || py < y || px >= x + width || py >= y + height) return 0.0f;
        float cx = std::clamp(px, x + radius, x + width - radius);
        float cy = std::clamp(py, y + radius, y + height - radius);
        float dx = px - cx, dy = py - cy;
        return dx * dx + dy * dy <= radius * radius ? 1.0f : 0.0f;
    }

    void ApplyRoundedAlpha(void* pixels, int width, int height) const {
        auto* argb = static_cast<UINT32*>(pixels);
        constexpr int samples = 8;
        const float radius = Scale(7.0f);
        for (int py = 0; py < height; ++py) {
            for (int px = 0; px < width; ++px) {
                int covered = 0;
                for (int sy = 0; sy < samples; ++sy) {
                    for (int sx = 0; sx < samples; ++sx) {
                        float fx = px + (sx + 0.5f) / samples;
                        float fy = py + (sy + 0.5f) / samples;
                        covered += RoundedCoverage(fx, fy, 0.0f, 0.0f,
                            static_cast<float>(width), static_cast<float>(height), radius) > 0.0f;
                    }
                }
                UINT32 alpha = static_cast<UINT32>(covered * 255 / (samples * samples));
                UINT32 color = argb[py * width + px];
                UINT32 blue = (color & 0xff) * alpha / 255;
                UINT32 green = ((color >> 8) & 0xff) * alpha / 255;
                UINT32 red = ((color >> 16) & 0xff) * alpha / 255;
                argb[py * width + px] = (alpha << 24) | (red << 16) | (green << 8) | blue;
            }
        }
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
        float size = Scale(12.0f);
        RoundedPath(path, static_cast<float>(x), static_cast<float>(y),
            size, size, Scale(2.0f));
        if (checked) {
            Gdiplus::SolidBrush brush(Color(RGB(76, 175, 80)));
            graphics.FillPath(&brush, &path);
            Gdiplus::Pen check(Gdiplus::Color(255, 255, 255, 255), Scale(1.5f));
            graphics.DrawLine(&check, x + Scale(3.2f), y + Scale(6.0f),
                x + Scale(5.2f), y + Scale(8.0f));
            graphics.DrawLine(&check, x + Scale(5.2f), y + Scale(8.0f),
                x + Scale(9.7f), y + Scale(3.5f));
        } else {
            Gdiplus::Pen pen(Color(RGB(200, 200, 200)), Scale(1.0f));
            graphics.DrawPath(&pen, &path);
        }
    }

    void DrawArrow(HDC dc, int x, int y) {
        Gdiplus::Graphics graphics(dc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::Pen pen(Color(RGB(55, 55, 55)), Scale(1.2f));
        pen.SetLineJoin(Gdiplus::LineJoinRound);
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);
        float right = static_cast<float>(x) + Scale(2.0f);
        float left = static_cast<float>(x) - Scale(2.0f);
        float center = static_cast<float>(y);
        graphics.DrawLine(&pen, left, center - Scale(3.0f), right, center);
        graphics.DrawLine(&pen, right, center, left, center + Scale(3.0f));
    }

    void DrawPanel(HDC dc, int x, int y, int width, std::vector<Entry> const& entries,
        int selected, bool root) {
        int height = root ? RootHeight() : SubmenuHeight();
        RECT panel{ x, y, x + width, y + height };
        Fill(dc, panel, RGB(250, 250, 250));
        int top = y + Scale(1);
        HGDIOBJ oldFont = font_ ? SelectObject(dc, font_) : nullptr;
        // ClearType needs an opaque target whose background matches the row.
        SetBkMode(dc, OPAQUE);
        for (size_t i = 0; i < entries.size(); ++i) {
            Entry const& entry = entries[i];
            int row = Height(entry);
            if (entry.kind == Kind::Separator) {
                HPEN pen = CreatePen(PS_SOLID, 1, RGB(228, 228, 228));
                HGDIOBJ oldPen = SelectObject(dc, pen);
                MoveToEx(dc, x + Scale(10), top + row / 2, nullptr);
                LineTo(dc, x + width - Scale(10), top + row / 2);
                SelectObject(dc, oldPen);
                DeleteObject(pen);
            } else if (entry.kind == Kind::Status) {
                float diameter = Scale(10.0f);
                float spacing = Scale(16.0f);
                float dotX = x + (width - 4 * diameter - 3 * spacing) / 2.0f;
                Gdiplus::Graphics graphics(dc);
                graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                for (int dot = 0; dot < 4; ++dot) {
                    int state = dot < static_cast<int>(entry.lights.size()) ? entry.lights[dot] : 0;
                    COLORREF color = state == 2 ? RGB(76, 175, 80) : state == 1 ? RGB(33, 150, 243) : RGB(228, 228, 228);
                    Gdiplus::SolidBrush brush(Color(color));
                    graphics.FillEllipse(&brush, dotX,
                        static_cast<float>(top) + (row - diameter) / 2.0f,
                        diameter, diameter);
                    dotX += diameter + spacing;
                }
            } else {
                if (static_cast<int>(i) == selected && entry.enabled) {
                    RECT highlight{ x + Scale(1), top, x + width - Scale(1), top + row };
                    Fill(dc, highlight, RGB(235, 240, 245));
                }
                if (entry.checkable) DrawCheck(dc, x + Scale(10),
                    top + (row - Scale(12)) / 2 - Scale(2), entry.checked);
                SetBkColor(dc, static_cast<int>(i) == selected && entry.enabled
                    ? RGB(235, 240, 245) : RGB(250, 250, 250));
                SetTextColor(dc, entry.enabled ? RGB(45, 45, 45) : RGB(160, 160, 160));
                RECT textRect{ x + Scale(34), top, x + width - Scale(18), top + row };
                DrawTextW(dc, entry.text.c_str(), static_cast<int>(entry.text.size()),
                    &textRect, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_LEFT);
                if (entry.kind == Kind::Devices) {
                    DrawArrow(dc, x + width - Scale(22), top + row / 2);
                }
            }
            top += row;
        }
        if (oldFont) SelectObject(dc, oldFont);
        Outline(dc, panel, RGB(218, 218, 218), Scale(7.0f));
    }

    void RenderPanel(HWND target, int x, int y, int width, int height,
        std::vector<Entry> const& entries, int selected, bool root) {
        if (!target) return;
        RECT bounds{ 0, 0, width, height };
        HDC screen = GetDC(nullptr);
        HDC buffer = CreateCompatibleDC(screen);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* pixels = nullptr;
        HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!buffer || !bitmap || !pixels) {
            if (bitmap) DeleteObject(bitmap);
            if (buffer) DeleteDC(buffer);
            ReleaseDC(nullptr, screen);
            return;
        }
        HGDIOBJ oldBitmap = SelectObject(buffer, bitmap);
        Fill(buffer, bounds, RGB(250, 250, 250));
        DrawPanel(buffer, 0, 0, width, entries, selected, root);
        ApplyRoundedAlpha(pixels, width, height);
        POINT destination{ x, y };
        POINT source{ 0, 0 };
        SIZE size{ width, height };
        BLENDFUNCTION blend{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        UpdateLayeredWindow(target, screen, &destination, &size, buffer, &source,
            0, &blend, ULW_ALPHA);
        SelectObject(buffer, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(buffer);
        ReleaseDC(nullptr, screen);
    }

    void RenderMain() {
        RenderPanel(window_, rootX_, rootY_, rootWidth_, RootHeight(),
            main_, selectedMain_, true);
    }

    void RenderSubmenu() {
        int width = SubmenuWidth();
        int x = submenuLeft_ ? rootX_ - width + overlap_ : rootX_ + rootWidth_ - overlap_;
        RenderPanel(submenuWindow_, x, rootY_ + DevicesTop(), width, SubmenuHeight(),
            devices_, selectedDevice_, false);
    }

    int HitMain(int x, int y) const {
        if (x < 0 || x >= rootWidth_ || y < Scale(1)) return -1;
        int top = Scale(1);
        for (size_t i = 0; i < main_.size(); ++i) {
            int bottom = top + Height(main_[i]);
            if (y >= top && y < bottom) return static_cast<int>(i);
            top = bottom;
        }
        return -1;
    }

    int HitDevice(int x, int y) const {
        if (!submenuOpen_ || x < 0 || x >= SubmenuWidth() || y < Scale(1)) return -1;
        int index = (y - Scale(1)) / rowHeight_;
        return index < static_cast<int>(devices_.size()) ? index : -1;
    }

    void MouseMove(HWND source, int x, int y) {
        if (source == submenuWindow_) {
            int device = HitDevice(x, y);
            if (device >= 0 && selectedDevice_ != device) {
                selectedDevice_ = device;
                RenderSubmenu();
            }
            return;
        }
        int main = HitMain(x, y);
        if (main >= 0 && main_[main].kind != Kind::Separator && main_[main].kind != Kind::Status) {
            if (selectedMain_ != main || submenuOpen_ != (main_[main].kind == Kind::Devices)) {
                selectedMain_ = main;
                selectedDevice_ = -1;
                submenuOpen_ = main_[main].kind == Kind::Devices;
                UpdateSubmenu();
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
        if (key == VK_ESCAPE) { if (submenuOpen_) { submenuOpen_ = false; UpdateSubmenu(); } else Close(); return; }
        if (key == VK_LEFT && submenuOpen_) { submenuOpen_ = false; UpdateSubmenu(); return; }
        if (key == VK_RIGHT && selectedMain_ >= 0 && main_[selectedMain_].kind == Kind::Devices) {
            submenuOpen_ = true;
            selectedDevice_ = -1;
            for (size_t i = 0; i < devices_.size(); ++i)
                if (devices_[i].enabled) { selectedDevice_ = static_cast<int>(i); break; }
            UpdateSubmenu();
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
        if (submenuOpen_) RenderSubmenu();
        else RenderMain();
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
        case WM_PAINT: ValidateRect(window, nullptr); return 0;
        case WM_MOUSEMOVE:
            self->MouseMove(window, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            return 0;
        case WM_LBUTTONUP: {
            int x = GET_X_LPARAM(lparam), y = GET_Y_LPARAM(lparam);
            if (window == self->submenuWindow_) {
                int device = self->HitDevice(x, y);
                if (device >= 0) self->Activate(self->devices_[device]);
            } else {
                int main = self->HitMain(x, y);
                if (main >= 0) self->Activate(self->main_[main]);
            }
            return 0;
        }
        case WM_RBUTTONUP: self->Close(); return 0;
        case WM_KEYDOWN: self->KeyDown(wparam); return 0;
        case WM_ACTIVATE:
            if (window == self->window_ && LOWORD(wparam) == WA_INACTIVE) self->Close();
            return 0;
        case WM_NCDESTROY:
            if (window == self->submenuWindow_) self->submenuWindow_ = nullptr;
            else if (window == self->window_) self->window_ = nullptr;
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            return DefWindowProcW(window, message, wparam, lparam);
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }
};
