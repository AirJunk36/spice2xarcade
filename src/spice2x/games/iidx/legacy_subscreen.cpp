#include "legacy_subscreen.h"
#include "legacy_panel.h"
#include "iidx.h"

#include <windows.h>
#include <windowsx.h>

#include <atomic>
#include <future>
#include <mutex>
#include <thread>

#include "build/resource.h"
#include "util/logging.h"
#include "util/resutils.h"

namespace games::iidx {
    namespace {
        std::array<std::atomic<uint16_t>, 2> keypad;
        std::array<std::atomic<int>, 5> slider {{-1, -1, -1, -1, -1}};

        // Keep the binary compatible with XP; touch entry points are optional.
        struct TouchInput {
            LONG x, y;
            HANDLE source;
            DWORD id, flags, mask, time;
            ULONG_PTR extra;
            DWORD width, height;
        };
        using RegisterTouch = BOOL (WINAPI *)(HWND, ULONG);
        using GetTouch = BOOL (WINAPI *)(HANDLE, UINT, TouchInput *, int);
        using CloseTouch = BOOL (WINAPI *)(HANDLE);

        struct MonitorSearch {
            std::string name;
            MONITORINFOEXA result {};
            bool found = false;
        };

        BOOL CALLBACK find_monitor(HMONITOR monitor, HDC, LPRECT, LPARAM context) {
            auto &search = *reinterpret_cast<MonitorSearch *>(context);
            MONITORINFOEXA info {};
            info.cbSize = sizeof(info);
            if (GetMonitorInfoA(monitor, &info) &&
                    (search.name.empty() ? !(info.dwFlags & MONITORINFOF_PRIMARY)
                        : search.name == info.szDevice)) {
                search.result = info;
                search.found = true;
                return FALSE;
            }
            return TRUE;
        }

        class Subscreen {
        public:
            std::mutex lifecycle;
            std::thread worker;
            std::atomic<HWND> window {nullptr};
            legacy_panel::State controls;
            HFONT font = nullptr;
            HFONT label_font = nullptr;
            HFONT led_font = nullptr;
            HANDLE font_resource = nullptr;
            HDC back_dc = nullptr;
            HBITMAP back_bitmap = nullptr;
            HGDIOBJ old_bitmap = nullptr;
            GetTouch get_touch = nullptr;
            CloseTouch close_touch = nullptr;
            int width = 0, height = 0;
            std::array<char, 9> ticker {};
            std::string monitor_name;

            ~Subscreen() { stop(); }

            void stop() {
                const auto hwnd = window.load();
                if (hwnd != nullptr) {
                    PostMessageW(hwnd, WM_CLOSE, 0, 0);
                }
                if (worker.joinable()) {
                    worker.join();
                }
            }

            void publish(HWND hwnd) {
                for (size_t i = 0; i < 2; i++) {
                    keypad[i] = controls.keys[i];
                }
                for (size_t i = 0; i < 5; i++) {
                    slider[i] = controls.sliders[i];
                }
                InvalidateRect(hwnd, nullptr, FALSE);
            }

            void point(HWND hwnd, uint32_t id, int x, int y, bool down, bool up) {
                RECT client {};
                GetClientRect(hwnd, &client);
                if (client.right > 0 && client.bottom > 0) {
                    controls.touch(id, x * legacy_panel::WIDTH / client.right,
                        y * legacy_panel::HEIGHT / client.bottom, down, up);
                    publish(hwnd);
                }
            }

            static LRESULT CALLBACK proc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
                auto *self = reinterpret_cast<Subscreen *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
                if (message == WM_NCCREATE) {
                    self = static_cast<Subscreen *>(reinterpret_cast<CREATESTRUCTW *>(lp)->lpCreateParams);
                    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
                }
                if (self == nullptr) {
                    return DefWindowProcW(hwnd, message, wp, lp);
                }
                switch (message) {
                    case WM_MOUSEACTIVATE:
                        return MA_NOACTIVATE;
                    case WM_ERASEBKGND:
                        return 1;
                    case 0x02cc: // WM_TABLET_QUERYSYSTEMGESTURESTATUS
                        return 1; // Disable press-and-hold right click.
                    case WM_LBUTTONDOWN:
                    case WM_MOUSEMOVE:
                    case WM_LBUTTONUP: {
                        if ((GetMessageExtraInfo() & 0xffffff80) == 0xff515700) {
                            return 0; // Ignore mouse events synthesized from touch.
                        }
                        if (message == WM_LBUTTONDOWN) {
                            SetCapture(hwnd);
                        }
                        self->point(hwnd, UINT32_MAX, GET_X_LPARAM(lp), GET_Y_LPARAM(lp),
                            message == WM_LBUTTONDOWN, message == WM_LBUTTONUP);
                        if (message == WM_LBUTTONUP) {
                            ReleaseCapture();
                        }
                        return 0;
                    }
                    case 0x0240: { // WM_TOUCH
                        std::array<TouchInput, 32> points {};
                        const auto handle = reinterpret_cast<HANDLE>(lp);
                        const UINT count = LOWORD(wp);
                        if (self->get_touch && count <= points.size()
                                && self->get_touch(handle, count, points.data(), sizeof(TouchInput))) {
                            for (UINT i = 0; i < count; i++) {
                                const auto &touch = points[i];
                                POINT position {touch.x / 100, touch.y / 100};
                                ScreenToClient(hwnd, &position);
                                self->point(hwnd, touch.id, position.x, position.y,
                                    touch.flags & 2, touch.flags & 4);
                            }
                        }
                        if (self->close_touch) {
                            self->close_touch(handle);
                        }
                        return 0;
                    }
                    case WM_CANCELMODE:
                        self->controls.release();
                        ReleaseCapture();
                        self->publish(hwnd);
                        return 0;
                    case WM_CAPTURECHANGED:
                        self->controls.touch(UINT32_MAX, 0, 0, false, true);
                        self->publish(hwnd);
                        return 0;
                    case WM_TIMER: {
                        // A non-activating window may miss mouse-up outside its
                        // visible area. Release only the mouse contact, keeping
                        // independent touchscreen contacts intact.
                        if (GetCapture() == hwnd && !(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) {
                            self->controls.touch(UINT32_MAX, 0, 0, false, true);
                            ReleaseCapture();
                            self->publish(hwnd);
                        }
                        std::array<char, 9> current;
                        {
                            std::lock_guard<std::mutex> lock(IIDX_LED_TICKER_LOCK);
                            std::copy_n(IIDXIO_LED_TICKER, current.size(), current.begin());
                        }
                        if (self->ticker != current) {
                            self->ticker = current;
                            InvalidateRect(hwnd, nullptr, FALSE);
                        }
                        return 0;
                    }
                    case WM_PAINT:
                        self->paint(hwnd);
                        return 0;
                    case WM_DISPLAYCHANGE:
                    case 0x02e0: { // WM_DPICHANGED
                        MonitorSearch search {self->monitor_name};
                        EnumDisplayMonitors(nullptr, nullptr, find_monitor,
                            reinterpret_cast<LPARAM>(&search));
                        if (search.found) {
                            const auto rect = search.result.rcMonitor;
                            SetWindowPos(hwnd, nullptr, rect.left, rect.top,
                                rect.right - rect.left, rect.bottom - rect.top,
                                SWP_NOACTIVATE | SWP_NOZORDER);
                        } else {
                            DestroyWindow(hwnd);
                        }
                        return 0;
                    }
                    case WM_CLOSE:
                        DestroyWindow(hwnd);
                        return 0;
                    case WM_DESTROY:
                        self->controls.release();
                        self->publish(hwnd);
                        self->window = nullptr;
                        PostQuitMessage(0);
                        return 0;
                }
                return DefWindowProcW(hwnd, message, wp, lp);
            }

            void paint(HWND hwnd) {
                PAINTSTRUCT paint {};
                const auto dc = BeginPaint(hwnd, &paint);
                RECT client {};
                GetClientRect(hwnd, &client);
                if (back_dc == nullptr || width != client.right || height != client.bottom) {
                    clear_buffer();
                    width = client.right;
                    height = client.bottom;
                    back_dc = CreateCompatibleDC(dc);
                    if (back_dc != nullptr) {
                        back_bitmap = CreateCompatibleBitmap(dc,
                            std::max(1, width), std::max(1, height));
                        if (back_bitmap != nullptr) {
                            old_bitmap = SelectObject(back_dc, back_bitmap);
                        }
                    }
                }
                if (back_dc == nullptr || back_bitmap == nullptr) {
                    clear_buffer();
                    EndPaint(hwnd, &paint);
                    return;
                }
                SaveDC(back_dc);
                SetMapMode(back_dc, MM_ANISOTROPIC);
                SetWindowExtEx(back_dc, legacy_panel::WIDTH, legacy_panel::HEIGHT, nullptr);
                SetViewportExtEx(back_dc, width, height, nullptr);
                auto rectangle = [&](legacy_panel::Rect rect, COLORREF color) {
                    const RECT bounds {rect.left, rect.top, rect.right, rect.bottom};
                    SetDCBrushColor(back_dc, color);
                    FillRect(back_dc, &bounds, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
                };
                auto text = [&](legacy_panel::Rect rect, const char *value, int length,
                        COLORREF color, HFONT face) {
                    RECT bounds {rect.left, rect.top, rect.right, rect.bottom};
                    SelectObject(back_dc, face);
                    SetTextColor(back_dc, color);
                    DrawTextA(back_dc, value, length, &bounds,
                        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                };
                SetBkMode(back_dc, TRANSPARENT);
                rectangle({0, 0, 1280, 720}, RGB(12, 17, 25));
                text({55, 185, 375, 235}, "PLAYER 1", -1, RGB(220, 225, 235), font);
                text({905, 185, 1225, 235}, "PLAYER 2", -1, RGB(220, 225, 235), font);
                std::array<char, 9> display = ticker;
                for (auto &c : display) {
                    if (c == 'm') { c = '.'; }
                    if (c == '~') { c = '-'; }
                }
                text({110, 35, 1170, 160}, display.data(), display.size(), RGB(255, 60, 40), led_font);
                for (size_t i = 0; i < 26; i++) {
                    const bool pressed = controls.keys[i / 13] & (1u << legacy_panel::SCAN_CODES[i % 13]);
                    const auto rect = legacy_panel::bounds(i);
                    rectangle(rect, pressed ? RGB(35, 125, 190) : RGB(35, 46, 64));
                    text(rect, legacy_panel::LABELS[i % 13], -1, RGB(245, 245, 245), font);
                }
                for (size_t i = 0; i < 5; i++) {
                    auto rect = legacy_panel::bounds(26 + i);
                    rectangle(rect, RGB(35, 46, 64));
                    const int value = controls.sliders[i] < 0 ? 15 : controls.sliders[i];
                    const int y = rect.bottom - 1 - value * (rect.bottom - rect.top - 1) / 15;
                    rectangle({rect.left, y - 6, rect.right, y + 6}, RGB(65, 185, 225));
                    rect.top = 195;
                    rect.bottom = 245;
                    rect.left -= 10;
                    rect.right += 10;
                    text(rect, legacy_panel::SLIDER_NAMES[i], -1, RGB(220, 225, 235), label_font);
                }
                RestoreDC(back_dc, -1);
                BitBlt(dc, 0, 0, width, height, back_dc, 0, 0, SRCCOPY);
                EndPaint(hwnd, &paint);
            }

            void clear_buffer() {
                if (back_dc) {
                    if (old_bitmap != nullptr) {
                        SelectObject(back_dc, old_bitmap);
                    }
                    if (back_bitmap != nullptr) {
                        DeleteObject(back_bitmap);
                    }
                    DeleteDC(back_dc);
                }
                back_dc = nullptr;
                back_bitmap = nullptr;
                old_bitmap = nullptr;
            }

            void run(MONITORINFOEXA monitor, std::promise<bool> ready) {
                const auto instance = GetModuleHandleW(nullptr);
                WNDCLASSW klass {};
                klass.lpfnWndProc = proc;
                klass.hInstance = instance;
                klass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
                klass.lpszClassName = L"SpiceIIDXLegacySubscreen";
                RegisterClassW(&klass);
                controls = {};
                const auto user = GetModuleHandleW(L"user32.dll");
                const auto register_touch = reinterpret_cast<RegisterTouch>(
                    GetProcAddress(user, "RegisterTouchWindow"));
                get_touch = reinterpret_cast<GetTouch>(GetProcAddress(user, "GetTouchInputInfo"));
                close_touch = reinterpret_cast<CloseTouch>(GetProcAddress(user, "CloseTouchInputHandle"));
                monitor_name = monitor.szDevice;
                const auto rect = monitor.rcMonitor;
                const auto hwnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                    klass.lpszClassName, L"IIDX Legacy Subscreen", WS_POPUP,
                    rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                    nullptr, nullptr, instance, this);
                window = hwnd;
                if (hwnd == nullptr) {
                    UnregisterClassW(klass.lpszClassName, instance);
                    ready.set_value(false);
                    return;
                }
                DWORD bytes = 0, fonts = 0;
                const auto data = resutil::load_file(IDR_DSEGFONT, &bytes);
                if (data && bytes) {
                    font_resource = AddFontMemResourceEx(const_cast<char *>(data), bytes, nullptr, &fonts);
                }
                auto make_font = [](int height, const char *name) {
                    return CreateFontA(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        ANTIALIASED_QUALITY, DEFAULT_PITCH, name);
                };
                font = make_font(-27, "Segoe UI");
                label_font = make_font(-20, "Segoe UI");
                led_font = make_font(-75, "DSEG14 Classic");
                if (register_touch && get_touch && close_touch) {
                    register_touch(hwnd, 1);
                }
                SetTimer(hwnd, 1, 33, nullptr);
                ShowWindow(hwnd, SW_SHOWNOACTIVATE);
                ready.set_value(true);
                MSG message;
                while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
                clear_buffer();
                DeleteObject(font);
                DeleteObject(label_font);
                DeleteObject(led_font);
                if (font_resource) {
                    RemoveFontMemResourceEx(font_resource);
                    font_resource = nullptr;
                }
                for (auto &value : slider) { value = -1; }
                UnregisterClassW(klass.lpszClassName, instance);
            }
        };

        Subscreen &subscreen() {
            static Subscreen value;
            return value;
        }


    }

    bool legacy_subscreen_start(const std::string &monitor) {
        auto &screen = subscreen();
        std::lock_guard<std::mutex> lock(screen.lifecycle);
        screen.stop();
        MonitorSearch search {monitor};
        EnumDisplayMonitors(nullptr, nullptr, find_monitor, reinterpret_cast<LPARAM>(&search));
        if (!search.found) {
            log_warning("iidx", "Legacy subscreen monitor not found; connect a second display or set iidxlegacymonitor");
            return false;
        }
        std::promise<bool> ready;
        auto result = ready.get_future();
        screen.worker = std::thread([&screen, info = search.result, ready = std::move(ready)]() mutable {
            screen.run(info, std::move(ready));
        });
        const bool started = result.get();
        if (started) {
            log_info("iidx", "Legacy subscreen opened on {}", search.result.szDevice);
        }
        return started;
    }

    void legacy_subscreen_stop() {
        auto &screen = subscreen();
        std::lock_guard<std::mutex> lock(screen.lifecycle);
        screen.stop();
    }

    uint16_t legacy_subscreen_keypad(size_t player) {
        return player < keypad.size() ? keypad[player].load() : 0;
    }

    int legacy_subscreen_slider(size_t index) {
        return index < slider.size() ? slider[index].load() : -1;
    }
}
