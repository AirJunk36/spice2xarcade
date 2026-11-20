#include "legacy_subscreen.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>

#include "misc/eamuse.h"
#include "util/detour.h"
#include "util/logging.h"

namespace games::iidx {
    namespace {
        std::array<std::atomic<uint16_t>, 2> keypad {};
        std::array<std::atomic<int>, 5> sliders {{-1, -1, -1, -1, -1}};

#ifndef SPICE_XP
        using namespace std::chrono_literals;

        constexpr char ORIGINAL_WINDOW_CLASS[] = "BM2DX_SUBMON_TOUCH";
        constexpr uint64_t COIN_BIT = 1ull << 29;
        std::atomic<HWND> game_window {nullptr};
        std::atomic<HMONITOR> display_override {nullptr};
        std::atomic<HHOOK> pointer_hook {nullptr};

        BOOL CALLBACK find_original_window_callback(HWND window, LPARAM data) {
            DWORD pid = 0;
            GetWindowThreadProcessId(window, &pid);
            char name[sizeof(ORIGINAL_WINDOW_CLASS) + 1] {};
            if (pid == GetCurrentProcessId() && GetClassNameA(window, name, sizeof(name)) &&
                    std::strcmp(name, ORIGINAL_WINDOW_CLASS) == 0) {
                *reinterpret_cast<HWND *>(data) = window;
                return FALSE;
            }
            return TRUE;
        }

        HWND find_original_window() {
            HWND result = nullptr;
            EnumWindows(find_original_window_callback, reinterpret_cast<LPARAM>(&result));
            return result;
        }

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
            HMONITOR handle = nullptr;
            MONITORINFOEXA found {};
        };

        BOOL CALLBACK find_monitor(HMONITOR monitor, HDC, LPRECT, LPARAM data) {
            auto &search = *reinterpret_cast<MonitorSearch *>(data);
            MONITORINFOEXA info {};
            info.cbSize = sizeof(info);
            if (monitor != search.main && GetMonitorInfoA(monitor, &info) &&
                    (search.requested.empty() || search.requested == info.szDevice)) {
                search.handle = monitor;
                search.found = info;
                return FALSE;
            }
            return TRUE;
        }

        struct MonitorCallback {
            MONITORENUMPROC callback;
            LPARAM data;
        };

        BOOL CALLBACK selected_monitor(HMONITOR monitor, HDC dc, LPRECT rect, LPARAM data) {
            const auto &next = *reinterpret_cast<MonitorCallback *>(data);
            return monitor != display_override.load() || next.callback(monitor, dc, rect, next.data);
        }

        BOOL WINAPI enum_subscreen_monitor(HDC dc, LPCRECT clip, MONITORENUMPROC callback, LPARAM data) {
            MonitorCallback next {callback, data};
            return EnumDisplayMonitors(dc, clip, selected_monitor, reinterpret_cast<LPARAM>(&next));
        }

        BOOL WINAPI enum_game_window(WNDENUMPROC callback, LPARAM data) {
            const auto hwnd = game_window.load();
            return !IsWindow(hwnd) || callback(hwnd, data);
        }

        using PointerMode = BOOL (WINAPI *)(BOOL);
        using PointerModeEnabled = BOOL (WINAPI *)();

        BOOL WINAPI enable_pointer_mode(BOOL enable) {
            const auto user32 = GetModuleHandleW(L"user32.dll");
            const auto active = reinterpret_cast<PointerModeEnabled>(
                GetProcAddress(user32, "IsMouseInPointerEnabled"));
            const auto set = reinterpret_cast<PointerMode>(
                GetProcAddress(user32, "EnableMouseInPointer"));
            return (enable && active && active()) || (set && set(enable));
        }

        HHOOK WINAPI install_pointer_hook(int type, HOOKPROC callback, HINSTANCE instance, DWORD id) {
            const auto hook = SetWindowsHookExA(type, callback, instance, id);
            if (type == WH_GETMESSAGE) {
                pointer_hook = hook;
            }
            return hook;
        }

        template<auto system_parameters>
        BOOL WINAPI set_subscreen_parameters(UINT action, UINT param, PVOID value, UINT flags) {
            // Keep the optional backend from changing desktop-wide touch feedback.
            if (action == 0x1049 || action == 0x104b) {
                return TRUE; // SPI_SETCONTACTVISUALIZATION / SPI_SETGESTUREVISUALIZATION
            }
            return system_parameters(action, param, value, flags);
        }

        BOOL WINAPI keep_game_thread_priority(HANDLE, int) {
            return TRUE;
        }

        void configure_backend(HMODULE module) {
            // The upstream backend initializes its touch/window code after a startup delay.
            detour::iat_try("EnumWindows", enum_game_window, module);
            detour::iat_try("EnumDisplayMonitors", enum_subscreen_monitor, module);
            detour::iat_try("EnableMouseInPointer", enable_pointer_mode, module);
            detour::iat_try("SetWindowsHookExA", install_pointer_hook, module);
            detour::iat_try("SystemParametersInfoA", set_subscreen_parameters<SystemParametersInfoA>,
                module);
            detour::iat_try("SystemParametersInfoW", set_subscreen_parameters<SystemParametersInfoW>,
                module);
            detour::iat_try("SetThreadPriority", keep_game_thread_priority, module);
        }

        template<class T>
        T load_export(HMODULE module, const char *name) {
            // PE exports use the same C ABI as the upstream BemaniTools wrappers.
            return reinterpret_cast<T>(GetProcAddress(module, name));
        }

        struct Backend {
            using Init = void (__cdecl *)();
            using Ticker = bool (__cdecl *)(const char *);
            using Read = uint8_t (__cdecl *)(uint8_t);
            using Keypad = uint16_t (__cdecl *)(uint8_t);
            using Pad = bool (__cdecl *)(uint64_t *);
            using Card = uint8_t (__cdecl *)(uint8_t, uint8_t *, uint8_t);

            Init vefx_init;
            Init eamio_init;
            Ticker write_ticker;
            Read read_slider;
            Keypad read_keypad;
            Pad read_pad;
            Read poll_card;
            Read card_sensor;
            Card read_card;

            explicit Backend(HMODULE module) :
                vefx_init(load_export<Init>(module, "backend_vefxio_init")),
                eamio_init(load_export<Init>(module, "backend_eamio_init")),
                write_ticker(load_export<Ticker>(module, "backend_vefxio_write_16seg")),
                read_slider(load_export<Read>(module, "backend_vefxio_read_slider")),
                read_keypad(load_export<Keypad>(module, "backend_eamio_get_keypad_state")),
                read_pad(load_export<Pad>(module, "backend_vefxio_io_recv")),
                poll_card(load_export<Read>(module, "backend_eamio_poll")),
                card_sensor(load_export<Read>(module, "backend_eamio_get_sensor_state")),
                read_card(load_export<Card>(module, "backend_eamio_read_card")) {}

            bool valid() const {
                return vefx_init && eamio_init && write_ticker && read_slider && read_keypad;
            }
        };

        class Subscreen {
            std::thread worker;
            std::mutex mutex;
            std::condition_variable wake;
            std::atomic_bool stopping = false;

            bool run_wait(std::chrono::milliseconds interval) {
                std::unique_lock lock(mutex);
                return !wake.wait_for(lock, interval, [this] { return stopping.load(); });
            }

            void poll(const Backend &backend, HWND hwnd) {
                std::array<char, 10> previous_ticker {};
                std::array<bool, 2> card_seen {};
                std::array<uint8_t, 5> previous_sliders {};
                bool sliders_ready = false;
                bool coin_down = false;
                auto next_topmost_check = std::chrono::steady_clock::now();
                log_info("iidx::subscreen", "Original subscreen window opened");

                do {
                    const auto now = std::chrono::steady_clock::now();
                    if (now >= next_topmost_check) {
                        keep_legacy_subscreen_topmost(hwnd);
                        next_topmost_check = now + std::chrono::milliseconds(
                            LEGACY_SUBSCREEN_TOPMOST_INTERVAL_MS);
                    }
                    const auto ticker = legacy_subscreen_ticker();
                    if (ticker != previous_ticker && backend.write_ticker(ticker.data())) {
                        previous_ticker = ticker;
                    }

                    std::array<uint8_t, 5> values {};
                    for (uint8_t i = 0; i < values.size(); ++i) {
                        values[i] = std::min<uint8_t>(15, backend.read_slider(i));
                        // Keep hardware bindings until the user moves each screen slider.
                        if (sliders_ready && values[i] != previous_sliders[i]) {
                            sliders[i] = values[i];
                        }
                    }
                    // The backend initially returns zero for all sliders before its first frame.
                    sliders_ready |= std::any_of(values.begin(), values.end(),
                        [](uint8_t value) { return value != 0; });
                    previous_sliders = values;
                    for (uint8_t player = 0; player < keypad.size(); ++player) {
                        keypad[player] = backend.read_keypad(player);
                        if (backend.poll_card && backend.card_sensor && backend.read_card) {
                            backend.poll_card(player);
                            const bool present = backend.card_sensor(player) != 0;
                            if (present && !card_seen[player]) {
                                std::array<uint8_t, 8> uid {};
                                if (backend.read_card(player, uid.data(), uid.size()) != 0) {
                                    eamuse_card_insert(player, uid.data());
                                }
                            }
                            card_seen[player] = present;
                        }
                    }
                    uint64_t pad = 0;
                    const bool coin = backend.read_pad && backend.read_pad(&pad) && (pad & COIN_BIT);
                    if (coin && !coin_down) {
                        eamuse_coin_insert();
                    }
                    coin_down = coin;
                } while (run_wait(8ms) && IsWindow(hwnd));

                for (auto &keys : keypad) {
                    keys = 0;
                }
                for (auto &slider : sliders) {
                    slider = -1;
                }
            }

            void close_original_window() {
                game_window = nullptr;
                display_override = nullptr;
                if (const auto hook = pointer_hook.exchange(nullptr)) {
                    UnhookWindowsHookEx(hook);
                }
                if (const auto window = find_original_window()) {
                    PostMessageW(window, WM_CLOSE, 0, 0);
                }
            }

            bool run_original(HWND main, HMONITOR monitor, std::filesystem::path path) {
                constexpr DWORD flags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;
                auto module = LoadLibraryExW(path.c_str(), nullptr, flags);
                auto error = module ? ERROR_SUCCESS : GetLastError();
                if (error == ERROR_BAD_EXE_FORMAT) {
                    const auto matching = path.parent_path() /
                        (sizeof(void *) == 8 ? L"backend64.dll" : L"backend32.dll");
                    if (matching != path) {
                        module = LoadLibraryExW(matching.c_str(), nullptr, flags);
                        if (module) {
                            path = matching;
                        }
                    }
                }
                if (!module) {
                    log_warning("iidx::subscreen",
                        "Could not run {} (Windows error {}); trying it as artwork", path, error);
                    return false;
                }

                // DllMain starts background threads and the original ABI has no shutdown export.
                const Backend backend(module);
                if (!backend.valid()) {
                    log_warning("iidx::subscreen", "{} lacks the iidx_btools_subscreen exports", path);
                    return false;
                }
                game_window = main;
                display_override = monitor;
                configure_backend(module);
                backend.vefx_init();
                backend.eamio_init();
                log_info("iidx::subscreen", "Starting original subscreen backend {}", path);

                const auto deadline = std::chrono::steady_clock::now() + 15s;
                while (run_wait(50ms)) {
                    if (const auto window = find_original_window()) {
                        if (IsWindowVisible(window)) {
                            poll(backend, window);
                            close_original_window();
                            return true;
                        }
                    }
                    if (std::chrono::steady_clock::now() >= deadline) {
                        log_warning("iidx::subscreen",
                            "Original backend did not open a window; trying the built-in renderer");
                        close_original_window();
                        return false;
                    }
                }
                close_original_window();
                return true;
            }

            void run(const std::string &monitor, const std::filesystem::path &artwork, bool original) {
                log_info("iidx::subscreen", "Waiting for the IIDX game window and secondary display");
                auto next_warning = std::chrono::steady_clock::now() + 10s;
                while (!stopping) {
                    HWND main = nullptr;
                    EnumWindows(find_game_window, reinterpret_cast<LPARAM>(&main));
                    if (main) {
                        MonitorSearch search {MonitorFromWindow(main, MONITOR_DEFAULTTONEAREST), monitor};
                        EnumDisplayMonitors(nullptr, nullptr, find_monitor, reinterpret_cast<LPARAM>(&search));
                        if (search.handle) {
                            if (original && !artwork.empty() && run_original(main, search.handle, artwork)) {
                                return;
                            }
                            original = false;
                            log_info("iidx::subscreen", "Opening built-in subscreen on {}", search.found.szDevice);
                            if (!run_builtin_legacy_subscreen(main, search.found.rcMonitor, artwork,
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
                    run_wait(250ms);
                }
            }

        public:
            ~Subscreen() { stop(); }

            void start(std::string monitor, std::filesystem::path artwork, bool original) {
                if (!worker.joinable()) {
                    stopping = false;
                    worker = std::thread([this, monitor = std::move(monitor), artwork = std::move(artwork), original] {
                        run(monitor, artwork, original);
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

    bool legacy_subscreen_start(const std::string &monitor, const std::string &path) {
#ifndef SPICE_XP
        if (GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetPointerInfo")) {
            std::error_code error;
            std::filesystem::path file = std::filesystem::absolute(
                std::filesystem::u8path(path.empty() ? "backend.dll" : path), error);
            if (error || !std::filesystem::is_regular_file(file, error)) {
                if (!path.empty()) {
                    log_warning("iidx::subscreen", "Subscreen artwork DLL not found: {}", path);
                }
                file.clear();
            }
            subscreen().start(monitor, file, !path.empty());
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
            (std::strcmp(name, LEGACY_SUBSCREEN_WINDOW_CLASS) == 0 ||
             std::strcmp(name, "BM2DX_SUBMON_TOUCH") == 0);
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
