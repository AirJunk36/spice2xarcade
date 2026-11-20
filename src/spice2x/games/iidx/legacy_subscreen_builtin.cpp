// Window, touch routing, and widget layout adapted from iidx_btools_subscreen.
// Copyright 2025 cloud11665. See the notices in licenses.txt.

#define _WIN32_WINNT 0x0602
#include <windows.h>
#include <windowsx.h>

#include "legacy_subscreen.h"

#ifndef SPICE_XP
#include <algorithm>
#include <chrono>
#include <d3d11.h>

#include "backends/imgui_impl_dx11.h"
#include "misc/eamuse.h"
#include "overlay/overlay.h"
#include "util/libutils.h"
#include "util/logging.h"
#include "legacy_subscreen_ticker_draw.h"

namespace games::iidx {
    namespace {
        constexpr wchar_t CLASS_NAME[] = L"BM2DX_SUBMON_TOUCH";
        constexpr UINT FORWARD_DOWN = WM_APP + 1;
        constexpr UINT FORWARD_MOVE = WM_APP + 2;
        constexpr UINT FORWARD_UP = WM_APP + 3;
        constexpr UINT32 MOUSE_CONTACT = UINT32_MAX;
        std::atomic<HWND> forward_from {nullptr};
        std::atomic<HWND> forward_to {nullptr};

        bool read_pointer(UINT32 id, POINTER_INFO &info) {
            static const auto get = libutils::try_proc<decltype(&GetPointerInfo)>(
                GetModuleHandleW(L"user32.dll"), "GetPointerInfo");
            return get && get(id, &info);
        }

        // Some cabinets deliver the digitizer to the main game window. The
        // original project observes that thread's pointer messages and
        // scales its coordinates onto the secondary screen.
        LRESULT CALLBACK game_pointer_hook(int code, WPARAM removing, LPARAM data) {
            if (code < 0 || removing != PM_REMOVE) {
                return CallNextHookEx(nullptr, code, removing, data);
            }
            const auto &msg = *reinterpret_cast<const MSG *>(data);
            const auto game = forward_from.load();
            const auto panel = forward_to.load();
            if (!game || !panel || (msg.hwnd != game && !IsChild(game, msg.hwnd))) {
                return CallNextHookEx(nullptr, code, removing, data);
            }
            UINT forward;
            switch (msg.message) {
                case WM_POINTERDOWN: forward = FORWARD_DOWN; break;
                case WM_POINTERUPDATE: forward = FORWARD_MOVE; break;
                case WM_POINTERUP:
                case WM_POINTERCAPTURECHANGED: forward = FORWARD_UP; break;
                default: return CallNextHookEx(nullptr, code, removing, data);
            }
            POINTER_INFO pointer {};
            const auto id = GET_POINTERID_WPARAM(msg.wParam);
            if (forward == FORWARD_UP) {
                PostMessageW(panel, forward, id, 0);
            } else if (read_pointer(id, pointer) && pointer.pointerType == PT_TOUCH) {
                RECT source {}, dest {};
                POINT position = pointer.ptPixelLocation;
                GetClientRect(game, &source);
                GetClientRect(panel, &dest);
                ScreenToClient(panel, &position);
                if (!PtInRect(&dest, position) && source.right > 0 && source.bottom > 0) {
                    position = pointer.ptPixelLocation;
                    ScreenToClient(game, &position);
                    position.x = MulDiv(position.x, dest.right, source.right);
                    position.y = MulDiv(position.y, dest.bottom, source.bottom);
                }
                PostMessageW(panel, forward, id, MAKELPARAM(position.x, position.y));
            }
            return CallNextHookEx(nullptr, code, removing, data);
        }

        class BuiltinSubscreen {
            std::array<std::atomic<uint16_t>, 2> &keypads;
            std::array<std::atomic<int>, 5> &sliders;
            HWND window = nullptr;
            HHOOK pointer_hook = nullptr;
            ImGuiContext **original_slot = nullptr;
            ImGuiContext *context = nullptr;
            HMODULE dx11 = nullptr;
            ID3D11Device *device = nullptr;
            ID3D11DeviceContext *device_context = nullptr;
            IDXGISwapChain *swap = nullptr;
            ID3D11RenderTargetView *target = nullptr;
            bool renderer_started = false;
            bool window_class_registered = false;
            bool closing = false;
            bool concentration = false;
            bool card_view = false;
            std::array<bool, 2> show_keypad {true, true};
            std::array<int, 5> values {7, 7, 7, 15, 15};
            UINT32 active_contact = 0;
            unsigned redraw_frames = 2;

            static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
                if (msg == WM_NCCREATE) {
                    const auto args = reinterpret_cast<const CREATESTRUCTW *>(lp);
                    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(args->lpCreateParams));
                }
                auto *self = reinterpret_cast<BuiltinSubscreen *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
                if (!self) {
                    return DefWindowProcW(hwnd, msg, wp, lp);
                }
                return self->message(hwnd, msg, wp, lp);
            }

            void input(UINT kind, POINT position, UINT32 id) {
                if (kind == FORWARD_DOWN && active_contact == 0) {
                    active_contact = id;
                }
                const bool hover = active_contact == 0 && id == MOUSE_CONTACT && kind == FORWARD_MOVE;
                if (!context || (active_contact != id && !hover)) {
                    return;
                }
                redraw_frames = 2;
                auto &io = ImGui::GetIO();
                io.AddMouseSourceEvent(id == MOUSE_CONTACT
                    ? ImGuiMouseSource_Mouse : ImGuiMouseSource_TouchScreen);
                if (kind != FORWARD_UP || id == MOUSE_CONTACT) {
                    io.AddMousePosEvent(static_cast<float>(position.x), static_cast<float>(position.y));
                }
                if (kind != FORWARD_MOVE) {
                    io.AddMouseButtonEvent(0, kind == FORWARD_DOWN);
                }
                if (kind == FORWARD_UP) {
                    active_contact = 0;
                }
            }

            LRESULT message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
                POINT position {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                switch (msg) {
                    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
                    case WM_POINTERACTIVATE: return PA_NOACTIVATE;
                    case WM_ERASEBKGND: return 1;
                    case WM_PAINT:
                        ValidateRect(hwnd, nullptr);
                        redraw_frames = std::max(redraw_frames, 1u);
                        return 0;
                    case WM_CLOSE:
                    case WM_DISPLAYCHANGE:
                        closing = true;
                        return 0;
                    case WM_POINTERDOWN:
                    case WM_POINTERUPDATE:
                    case WM_POINTERUP:
                    case WM_POINTERCAPTURECHANGED:
                        ScreenToClient(hwnd, &position);
                        input(msg == WM_POINTERDOWN ? FORWARD_DOWN
                            : msg == WM_POINTERUPDATE ? FORWARD_MOVE : FORWARD_UP,
                            position, GET_POINTERID_WPARAM(wp));
                        return 0;
                    case FORWARD_DOWN:
                    case FORWARD_MOVE:
                    case FORWARD_UP:
                        input(msg, position, static_cast<UINT32>(wp));
                        return 0;
                    case WM_LBUTTONDOWN:
                    case WM_LBUTTONUP:
                    case WM_MOUSEMOVE:
                        if ((GetMessageExtraInfo() & 0xffffff80) != 0xff515700) {
                            if (msg == WM_LBUTTONDOWN) {
                                SetCapture(hwnd);
                            }
                            input(msg == WM_LBUTTONDOWN ? FORWARD_DOWN
                                : msg == WM_MOUSEMOVE ? FORWARD_MOVE : FORWARD_UP,
                                position, MOUSE_CONTACT);
                            if (msg == WM_LBUTTONUP && GetCapture() == hwnd) {
                                ReleaseCapture();
                            }
                        }
                        return 0;
                    case WM_CANCELMODE:
                    case WM_CAPTURECHANGED:
                        if (active_contact) {
                            input(FORWARD_UP, position, active_contact);
                        }
                        return 0;
                }
                return DefWindowProcW(hwnd, msg, wp, lp);
            }

            bool create_window(const RECT &rect) {
                WNDCLASSEXW cls {};
                cls.cbSize = sizeof(cls);
                cls.lpfnWndProc = window_proc;
                cls.hInstance = GetModuleHandleW(nullptr);
                cls.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
                cls.lpszClassName = CLASS_NAME;
                window_class_registered = RegisterClassExW(&cls) != 0;
                if (!window_class_registered) {
                    return false;
                }
                window = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                    CLASS_NAME, L"IIDX Legacy Subscreen", WS_POPUP, rect.left, rect.top,
                    rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr,
                    cls.hInstance, this);
                return window != nullptr;
            }

            bool create_renderer() {
                dx11 = LoadLibraryW(L"d3d11.dll");
                const auto create = libutils::try_proc<decltype(&D3D11CreateDeviceAndSwapChain)>(
                    dx11, "D3D11CreateDeviceAndSwapChain");
                DXGI_SWAP_CHAIN_DESC desc {};
                desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                desc.BufferCount = 2;
                desc.SampleDesc.Count = 1;
                desc.OutputWindow = window;
                desc.Windowed = TRUE;
                desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
                if (!create || FAILED(create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                        nullptr, 0, D3D11_SDK_VERSION, &desc, &swap, &device,
                        nullptr, &device_context))) {
                    return false;
                }
                ID3D11Texture2D *back = nullptr;
                if (FAILED(swap->GetBuffer(0, IID_PPV_ARGS(&back)))) {
                    return false;
                }
                const auto result = device->CreateRenderTargetView(back, nullptr, &target);
                back->Release();
                if (FAILED(result)) {
                    return false;
                }
                original_slot = spice_imgui_context_slot;
                spice_imgui_context_slot = &context;
                overlay::initialize_imgui_allocators();
                IMGUI_CHECKVERSION();
                ImGui::CreateContext();
                ImGui::GetIO().IniFilename = nullptr;
                ImGui::GetIO().LogFilename = nullptr;
                ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_IsTouchScreen;
                ImGui::StyleColorsDark();
                renderer_started = ImGui_ImplDX11_Init(device, device_context);
                return renderer_started && ImGui_ImplDX11_CreateDeviceObjects();
            }

            void draw_keypad(size_t player, float x, float y, float s) {
                if (!show_keypad[player]) {
                    keypads[player] = 0;
                    return;
                }
                ImGui::SetNextWindowPos({x, y});
                ImGui::SetNextWindowSize({320 * s, 318 * s});
                const char *title = player == 0 ? "P1 Keypad" : "P2 Keypad";
                ImGui::Begin(title, &show_keypad[player],
                    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
                constexpr const char *labels[] {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "."};
                constexpr int codes[] {EAM_IO_KEYPAD_1, EAM_IO_KEYPAD_2, EAM_IO_KEYPAD_3,
                    EAM_IO_KEYPAD_4, EAM_IO_KEYPAD_5, EAM_IO_KEYPAD_6, EAM_IO_KEYPAD_7,
                    EAM_IO_KEYPAD_8, EAM_IO_KEYPAD_9, EAM_IO_KEYPAD_0, EAM_IO_KEYPAD_DECIMAL};
                uint16_t pressed = 0;
                for (size_t key = 0; key < std::size(labels); ++key) {
                    const bool decimal = key == 10;
                    ImGui::Button(labels[key], {s * (decimal ? 192.0f : 92.0f), 62 * s});
                    if (ImGui::IsItemActive()) {
                        pressed |= 1u << codes[key];
                    }
                    if (key % 3 != 2 && !decimal) {
                        ImGui::SameLine();
                    }
                }
                keypads[player] = pressed;
                ImGui::End();
            }

            void draw(const std::array<char, 10> &ticker) {
                auto &io = ImGui::GetIO();
                const float s = std::min(io.DisplaySize.x / 1920, io.DisplaySize.y / 1080);
                const float x = (io.DisplaySize.x - 1920 * s) / 2;
                const float y = (io.DisplaySize.y - 1080 * s) / 2;
                ImGui::PushFont(nullptr, 24 * s);
                constexpr auto fixed = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
                    | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar;
                ImGui::SetNextWindowPos({x, y});
                ImGui::SetNextWindowSize({1920 * s, 96 * s});
                ImGui::Begin("Navigation", nullptr, fixed);
                if (ImGui::Button("P1 Keypad", {185 * s, 64 * s})) {
                    show_keypad[0] = !show_keypad[0];
                }
                ImGui::SameLine(650 * s);
                if (ImGui::Button("Cards", {180 * s, 64 * s})) {
                    card_view = !card_view;
                }
                ImGui::SameLine();
                if (ImGui::Button("Insert Coin", {180 * s, 64 * s})) {
                    eamuse_coin_insert();
                }
                ImGui::SameLine();
                if (ImGui::Button("Concentration", {220 * s, 64 * s})) {
                    concentration = !concentration;
                }
                ImGui::SameLine(1720 * s);
                if (ImGui::Button("P2 Keypad", {185 * s, 64 * s})) {
                    show_keypad[1] = !show_keypad[1];
                }
                ImGui::End();
                if (concentration) {
                    keypads[0] = keypads[1] = 0;
                    ImGui::PopFont();
                    return;
                }

                ImGui::SetNextWindowPos({x + 320 * s, y + 100 * s});
                ImGui::SetNextWindowSize({1280 * s, 220 * s});
                ImGui::Begin("Ticker", nullptr, fixed);
                legacy_subscreen::draw_ticker(ticker, s);
                ImGui::End();
                if (card_view) {
                    ImGui::SetNextWindowPos({x + 730 * s, y + 330 * s});
                    ImGui::SetNextWindowSize({460 * s, 86 * s});
                    ImGui::Begin("Cards", nullptr, fixed);
                    if (ImGui::Button("Insert P1 Card", {210 * s, 56 * s})) {
                        eamuse_card_insert(0);
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Insert P2 Card", {210 * s, 56 * s})) {
                        eamuse_card_insert(1);
                    }
                    ImGui::End();
                }
                ImGui::SetNextWindowPos({x + 450 * s, y + 455 * s});
                ImGui::SetNextWindowSize({1020 * s, 550 * s});
                ImGui::Begin("VEFX", nullptr, fixed);
                constexpr const char *labels[] {"VEFX", "Low EQ", "High EQ", "Filter", "Play Volume"};
                for (size_t i = 0; i < values.size(); ++i) {
                    ImGui::SetCursorPos({(18 + i * 200) * s, 12 * s});
                    ImGui::TextUnformatted(labels[i]);
                    ImGui::SetCursorPos({(62 + i * 200) * s, 58 * s});
                    ImGui::PushID(static_cast<int>(i));
                    if (ImGui::VSliderInt("##level", {80 * s, 460 * s}, &values[i], 0, 15, "%d")
                            || ImGui::IsItemActivated()) {
                        sliders[i] = values[i];
                    }
                    ImGui::PopID();
                }
                ImGui::End();
                draw_keypad(0, x + 22 * s, y + 733 * s, s);
                draw_keypad(1, x + 1578 * s, y + 733 * s, s);
                ImGui::PopFont();
            }

        public:
            BuiltinSubscreen(std::array<std::atomic<uint16_t>, 2> &keys,
                std::array<std::atomic<int>, 5> &effects) : keypads(keys), sliders(effects) {}

            ~BuiltinSubscreen() {
                forward_to = nullptr;
                forward_from = nullptr;
                if (pointer_hook) {
                    UnhookWindowsHookEx(pointer_hook);
                }
                for (auto &keys : keypads) {
                    keys = 0;
                }
                for (auto &slider : sliders) {
                    slider = -1;
                }
                if (renderer_started) {
                    ImGui_ImplDX11_Shutdown();
                }
                if (context) {
                    ImGui::DestroyContext(context);
                    spice_imgui_context_slot = original_slot;
                }
                if (target) { target->Release(); }
                if (swap) { swap->Release(); }
                if (device_context) { device_context->Release(); }
                if (device) { device->Release(); }
                if (dx11) { FreeLibrary(dx11); }
                if (window) { DestroyWindow(window); }
                if (window_class_registered) {
                    UnregisterClassW(CLASS_NAME, GetModuleHandleW(nullptr));
                }
            }

            void run(HWND main, const RECT &rect, const std::atomic_bool &stopping) {
                if (!create_window(rect) || !create_renderer()) {
                    log_warning("iidx::subscreen", "Could not open the built-in D3D11 subscreen");
                    return;
                }
                forward_from = main;
                forward_to = window;
                pointer_hook = SetWindowsHookExW(WH_GETMESSAGE, game_pointer_hook, nullptr,
                    GetWindowThreadProcessId(main, nullptr));
                if (!pointer_hook) {
                    log_warning("iidx::subscreen", "Could not install the game window pointer hook");
                }
                ShowWindow(window, SW_SHOWNOACTIVATE);
                ImGui::GetIO().DisplaySize = {
                    static_cast<float>(rect.right - rect.left),
                    static_cast<float>(rect.bottom - rect.top),
                };
                log_info("iidx::subscreen", "Built-in subscreen window opened");
                auto previous = std::chrono::steady_clock::now();
                std::array<char, 10> last_ticker {};
                while (!closing && !stopping && IsWindow(main)) {
                    MSG msg {};
                    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                        TranslateMessage(&msg);
                        DispatchMessageW(&msg);
                    }
                    if (closing || stopping) {
                        break;
                    }
                    const auto ticker = legacy_subscreen_ticker();
                    if (!redraw_frames && ticker == last_ticker) {
                        MsgWaitForMultipleObjects(0, nullptr, FALSE, 16, QS_ALLINPUT);
                        continue;
                    }
                    if (redraw_frames) {
                        --redraw_frames;
                    }
                    last_ticker = ticker;
                    const auto now = std::chrono::steady_clock::now();
                    ImGui::GetIO().DeltaTime = std::max(
                        std::chrono::duration<float>(now - previous).count(), 0.001f);
                    previous = now;
                    ImGui_ImplDX11_NewFrame();
                    ImGui::NewFrame();
                    draw(ticker);
                    ImGui::Render();
                    constexpr float clear[] {0.10f, 0.10f, 0.10f, 1.0f};
                    device_context->OMSetRenderTargets(1, &target, nullptr);
                    device_context->ClearRenderTargetView(target, clear);
                    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
                    const auto result = swap->Present(1, 0);
                    if (FAILED(result)) {
                        log_warning("iidx::subscreen", "Subscreen present failed: 0x{:08x}",
                            static_cast<uint32_t>(result));
                        break;
                    }
                    if (result == DXGI_STATUS_OCCLUDED) {
                        MsgWaitForMultipleObjects(0, nullptr, FALSE, 16, QS_ALLINPUT);
                    }
                }
            }
        };
    }

    void run_builtin_legacy_subscreen(HWND main, const RECT &rect, const std::atomic_bool &stopping,
        std::array<std::atomic<uint16_t>, 2> &keypads, std::array<std::atomic<int>, 5> &sliders) {
        BuiltinSubscreen panel(keypads, sliders);
        panel.run(main, rect, stopping);
    }
}
#endif
