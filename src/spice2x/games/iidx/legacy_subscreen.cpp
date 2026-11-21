#include "legacy_subscreen.h"

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <utility>

#include "util/logging.h"

namespace games::iidx {
    namespace {
        std::array<std::atomic<uint16_t>, 2> keypad {};
        std::array<std::atomic<int>, 5> sliders {{-1, -1, -1, -1, -1}};

#ifndef SPICE_XP
        using namespace std::chrono_literals;

        bool is_iidx_title(const char *name) {
            return std::strncmp(name, "beatmaniaIIDX", 13) == 0 ||
                std::strncmp(name, "beatmania IIDX", 14) == 0;
        }

        BOOL CALLBACK find_game_window(HWND window, LPARAM data) {
            DWORD pid = 0;
            GetWindowThreadProcessId(window, &pid);
            if (pid != GetCurrentProcessId() || !IsWindowVisible(window)) {
                return TRUE;
            }
            char title[256] {};
            GetWindowTextA(window, title, sizeof(title));
            if (std::strstr(title, " sub")) {
                return TRUE;
            }
            char name[256] {};
            GetClassNameA(window, name, sizeof(name));
            if (is_iidx_title(title) || is_iidx_title(name) || std::strncmp(name, "C02", 3) == 0) {
                *reinterpret_cast<HWND *>(data) = window;
                return FALSE;
            }
            return TRUE;
        }

        struct MonitorSearch {
            HMONITOR main;
            const std::string &requested;
            MONITORINFOEXA found {};
        };

        BOOL CALLBACK find_monitor(HMONITOR monitor, HDC, LPRECT, LPARAM data) {
            auto &search = *reinterpret_cast<MonitorSearch *>(data);
            MONITORINFOEXA info {};
            info.cbSize = sizeof(info);
            if (monitor != search.main && GetMonitorInfoA(monitor, &info) &&
                    (search.requested.empty() || search.requested == info.szDevice)) {
                search.found = info;
                return FALSE;
            }
            return TRUE;
        }

        class Subscreen {
            std::thread worker;
            std::mutex mutex;
            std::condition_variable wake;
            std::atomic_bool stopping = false;

            void run(const std::string &monitor) {
                log_info("iidx::subscreen", "Waiting for the IIDX game window and secondary display");
                auto next_warning = std::chrono::steady_clock::now() + 10s;
                while (!stopping) {
                    HWND main = nullptr;
                    EnumWindows(find_game_window, reinterpret_cast<LPARAM>(&main));
                    if (main) {
                        MonitorSearch search {MonitorFromWindow(main, MONITOR_DEFAULTTONEAREST), monitor};
                        EnumDisplayMonitors(nullptr, nullptr, find_monitor, reinterpret_cast<LPARAM>(&search));
                        if (search.found.cbSize) {
                            log_info("iidx::subscreen", "Opening built-in subscreen on {}",
                                search.found.szDevice);
                            if (!run_builtin_legacy_subscreen(main, search.found.rcMonitor,
                                    stopping, keypad, sliders)) {
                                return;
                            }
                            next_warning = std::chrono::steady_clock::now() + 10s;
                        }
                    }
                    if (std::chrono::steady_clock::now() >= next_warning) {
                        if (main) {
                            log_warning("iidx::subscreen", "Still waiting for a separate active monitor ({})",
                                monitor.empty() ? "automatic selection" : monitor);
                        } else {
                            log_warning("iidx::subscreen", "Still waiting for the visible IIDX game window");
                        }
                        next_warning = std::chrono::steady_clock::now() + 30s;
                    }
                    std::unique_lock lock(mutex);
                    wake.wait_for(lock, 250ms, [this] { return stopping.load(); });
                }
            }

        public:
            ~Subscreen() { stop(); }

            void start(std::string monitor) {
                if (!worker.joinable()) {
                    stopping = false;
                    worker = std::thread([this, monitor = std::move(monitor)] {
                        run(monitor);
                    });
                }
            }

            void stop() {
                if (worker.joinable()) {
                    {
                        std::lock_guard lock(mutex);
                        stopping = true;
                    }
                    wake.notify_one();
                    worker.join();
                }
            }
        };

        Subscreen &subscreen() {
            static Subscreen instance;
            return instance;
        }
#endif
    }

    bool legacy_subscreen_start([[maybe_unused]] const std::string &monitor) {
#ifndef SPICE_XP
        if (GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetPointerInfo")) {
            subscreen().start(monitor);
            return true;
        }
#endif
        log_warning("iidx::subscreen", "The legacy subscreen requires Windows 8 or newer");
        return false;
    }

    void legacy_subscreen_stop() {
#ifndef SPICE_XP
        subscreen().stop();
#endif
    }

    bool is_legacy_subscreen_window(HWND window) {
        char name[sizeof(LEGACY_SUBSCREEN_WINDOW_CLASS) + 1] {};
        return GetClassNameA(window, name, sizeof(name)) &&
            std::strcmp(name, LEGACY_SUBSCREEN_WINDOW_CLASS) == 0;
    }

    void keep_legacy_subscreen_topmost(HWND window, bool show) {
        if (!show && !IsWindowVisible(window)) {
            return;
        }
        constexpr UINT flags =
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS;
        if (!SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, flags | (show ? SWP_SHOWWINDOW : 0))
                && show) {
            ShowWindow(window, SW_SHOWNOACTIVATE);
        }
    }

    uint16_t legacy_subscreen_keypad(size_t player) {
        return player < keypad.size() ? keypad[player].load() : 0;
    }

    int legacy_subscreen_slider(size_t index) {
        return index < sliders.size() ? sliders[index].load() : -1;
    }
}
