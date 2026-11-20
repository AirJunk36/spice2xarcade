// Window, touch routing, and widget layout adapted from iidx_btools_subscreen.
// Copyright 2025 cloud11665. See the notices in licenses.txt.

#define _WIN32_WINNT 0x0602
#include <windows.h>
#include <windowsx.h>

#include "legacy_subscreen.h"

#ifndef SPICE_XP
#include <algorithm>
#include <chrono>
#include <optional>
#include <d3d11.h>

#include "backends/imgui_impl_dx11.h"
#include "misc/eamuse.h"
#include "overlay/overlay.h"
#include "util/libutils.h"
#include "util/logging.h"
#include "legacy_subscreen_artwork.h"
#include "legacy_subscreen_ticker_draw.h"

namespace games::iidx {
    namespace {
        constexpr wchar_t CLASS_NAME[] = L"SpiceIIDXLegacySubscreen";
        constexpr UINT FORWARD_DOWN = WM_APP + 1;
        constexpr UINT FORWARD_MOVE = WM_APP + 2;
        constexpr UINT FORWARD_UP = WM_APP + 3;
        constexpr UINT_PTR TOPMOST_TIMER = 1;
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
            bool display_changed = false;
            bool concentration = false;
            bool card_view = false;
            bool settings_view = false;
            std::optional<legacy_subscreen::Artwork> artwork;
            std::array<bool, 2> show_keypad {};
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
                    case WM_TIMER:
                        if (wp == TOPMOST_TIMER) {
                            keep_legacy_subscreen_topmost(hwnd);
                            return 0;
                        }
                        break;
                    case WM_DISPLAYCHANGE:
                        display_changed = true;
                        closing = true;
                        return 0;
                    case WM_CLOSE:
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

            bool create_renderer(const std::filesystem::path &artwork_path) {
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
                if (!create) {
                    log_warning("iidx::subscreen", "D3D11CreateDeviceAndSwapChain is unavailable");
                    return false;
                }
                const auto create_device = [&](D3D_DRIVER_TYPE driver) {
                    return create(nullptr, driver, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                        &desc, &swap, &device, nullptr, &device_context);
                };
                const auto hardware = create_device(D3D_DRIVER_TYPE_HARDWARE);
                if (FAILED(hardware)) {
                    const auto warp = create_device(D3D_DRIVER_TYPE_WARP);
                    if (FAILED(warp)) {
                        log_warning("iidx::subscreen", "D3D11 swap chain failed: hardware 0x{:08x}, WARP 0x{:08x}",
                            static_cast<uint32_t>(hardware), static_cast<uint32_t>(warp));
                        return false;
                    }
                    log_info("iidx::subscreen", "Using WARP (hardware D3D11 returned 0x{:08x})",
                        static_cast<uint32_t>(hardware));
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
                if (!artwork_path.empty()) {
                    artwork.emplace();
                    if (artwork->load(artwork_path, device)) {
                        artwork->load_font();
                        log_info("iidx::subscreen", "Loaded original artwork from {}", artwork_path);
                    } else {
                        artwork.reset();
                        log_warning("iidx::subscreen", "Could not read original artwork from {}", artwork_path);
                    }
                }
                renderer_started = ImGui_ImplDX11_Init(device, device_context);
                return renderer_started && ImGui_ImplDX11_CreateDeviceObjects();
            }

            using Art = legacy_subscreen::Artwork;
            static constexpr auto fixed = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
                | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings;

            void panel(const char *id, ImVec2 position, ImVec2 size, ImGuiWindowFlags flags = 0) {
                ImGui::SetNextWindowPos(position);
                ImGui::SetNextWindowSize(size);
                ImGui::Begin(id, nullptr, fixed | flags);
            }

            bool icon(const char *label, Art::Image image, ImVec2 position, ImVec2 size,
                    ImU32 tint = IM_COL32_WHITE) {
                ImGui::SetCursorPos(position);
                if (!artwork || !(*artwork)[image].view) {
                    return ImGui::Button(label, size);
                }
                const auto cursor = ImGui::GetCursorScreenPos();
                const auto clicked = ImGui::InvisibleButton(label, size);
                (*artwork)[image].draw(ImGui::GetWindowDrawList(), cursor, size, tint);
                return clicked;
            }

            void concentration_button(size_t player, float x, float s) {
                ImGui::PushID(static_cast<int>(player));
                const auto image = concentration ? Art::ConcentrationOn : Art::ConcentrationOff;
                if (icon("Concentrate", image, {x * s, 8 * s}, {100 * s, 80 * s})) {
                    concentration = !concentration;
                }
                if (artwork) {
                    ImGui::PushFont(nullptr, 12 * s);
                    ImGui::SetCursorPos({(x + 19) * s, 8 * s});
                    ImGui::Text("PLAYER %02u", static_cast<unsigned>(player + 1));
                    ImGui::SetCursorPos({(x + 8) * s, 69 * s});
                    ImGui::TextUnformatted("concentration");
                    ImGui::SetCursorPos({(x + 33) * s, 82 * s});
                    ImGui::TextUnformatted("mode");
                    ImGui::PopFont();
                }
                ImGui::PopID();
            }

            void draw_keypad(size_t player, float x, float y, float s) {
                if (!show_keypad[player]) {
                    keypads[player] = 0;
                    return;
                }
                panel(player == 0 ? "P1 Keypad" : "P2 Keypad", {x, y}, {424 * s, 450 * s},
                    ImGuiWindowFlags_NoBackground);
                const float keypad_x = player == 0 ? 0.0f : 96.0f;
                if (artwork) {
                    (*artwork)[Art::Keypad].draw(ImGui::GetWindowDrawList(),
                        {x + keypad_x * s, y}, {325 * s, 443 * s});
                }
                if (icon("Close", Art::KeypadClose, {(player == 0 ? 325.0f : 0.0f) * s, 347 * s},
                        {95 * s, 95 * s})) {
                    show_keypad[player] = false;
                }
                constexpr const char *labels[] {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "."};
                constexpr int codes[] {EAM_IO_KEYPAD_1, EAM_IO_KEYPAD_2, EAM_IO_KEYPAD_3,
                    EAM_IO_KEYPAD_4, EAM_IO_KEYPAD_5, EAM_IO_KEYPAD_6, EAM_IO_KEYPAD_7,
                    EAM_IO_KEYPAD_8, EAM_IO_KEYPAD_9, EAM_IO_KEYPAD_0, EAM_IO_KEYPAD_DECIMAL};
                uint16_t pressed = 0;
                for (size_t key = 0; key < std::size(labels); ++key) {
                    ImGui::SetCursorPos({(keypad_x + 24 + 98 * (key % 3)) * s, (24 + 98 * (key / 3)) * s});
                    const auto pos = ImGui::GetCursorScreenPos();
                    const ImVec2 size {(key == 10 ? 179.0f : 81.0f) * s, 81 * s};
                    if (artwork) {
                        ImGui::InvisibleButton(labels[key], size);
                        if (ImGui::IsItemActive()) {
                            ImGui::GetWindowDrawList()->AddRectFilled(pos, {pos.x + size.x, pos.y + size.y},
                                IM_COL32(255, 255, 255, 100), 2 * s);
                        }
                    } else {
                        ImGui::Button(labels[key], size);
                    }
                    if (ImGui::IsItemActive()) {
                        pressed |= 1u << codes[key];
                    }
                }
                keypads[player] = pressed;
                ImGui::End();
            }

            void draw_effects(float x, float y, float s) {
                panel("VEFX", {x + 420 * s, y + 335 * s}, {1080 * s, 700 * s},
                    ImGuiWindowFlags_NoBackground);
                constexpr const char *labels[] {"VEFX", "low-EQ", "hi-EQ", "filter", "play volume"};
                constexpr const char *descriptions[] {"Effect amount", "Low frequencies", "High frequencies",
                    "Filter setting", "Volume setting"};
                for (size_t i = 0; i < values.size(); ++i) {
                    const float center = 110 + 215 * i;
                    ImGui::PushID(static_cast<int>(i));
                    ImGui::SetCursorPos({center * s - ImGui::CalcTextSize(labels[i]).x * 0.5f, 6 * s});
                    ImGui::TextUnformatted(labels[i]);
                    ImGui::SetCursorPos({center * s - ImGui::CalcTextSize(descriptions[i]).x * 0.5f, 33 * s});
                    ImGui::TextUnformatted(descriptions[i]);
                    ImGui::SetCursorPos({(center - 72) * s, 119 * s});
                    if (artwork) {
                        const auto pos = ImGui::GetCursorScreenPos();
                        const float height = 421.5f * s;
                        const float width = 144 * s;
                        auto *list = ImGui::GetWindowDrawList();
                        (*artwork)[Art::TrackBackground].draw(list, {pos.x - 27 * s, pos.y - 70.5f * s},
                            {198 * s, 564 * s});
                        (*artwork)[i < 3 ? Art::TrackZero : Art::TrackOne].draw(
                            list, {pos.x - 42 * s, pos.y - 30 * s}, {228 * s, 483 * s});
                        ImGui::InvisibleButton("Fader", {width, height});
                        if (ImGui::IsItemActive()) {
                            const auto t = std::clamp((ImGui::GetIO().MousePos.y - pos.y) / height, 0.f, 1.f);
                            values[i] = static_cast<int>(15.f * (1.f - t));
                            sliders[i] = values[i];
                        }
                        const float knob_y = pos.y + (1.f - values[i] / 15.f) * height - 28.5f * s;
                        (*artwork)[Art::TrackHead].draw(list, {pos.x, knob_y}, {width, 57 * s});
                    } else if (ImGui::VSliderInt("Fader", {144 * s, 421.5f * s}, &values[i], 0, 15, "%d")
                            || ImGui::IsItemActivated()) {
                        sliders[i] = values[i];
                    }
                    ImGui::PopID();
                }
                ImGui::End();
            }

            void draw(const std::array<char, 10> &ticker) {
                const auto &io = ImGui::GetIO();
                const float s = std::min(io.DisplaySize.x / 1920, io.DisplaySize.y / 1080);
                const float x = (io.DisplaySize.x - 1920 * s) / 2;
                const float y = (io.DisplaySize.y - 1080 * s) / 2;
                ImGui::PushFont(nullptr, 20 * s);
                panel("Navigation", {x, y}, {1920 * s, 100 * s});
                const auto tint = [&](size_t player) {
                    return show_keypad[player] && !concentration ? IM_COL32_WHITE : IM_COL32(128, 128, 128, 255);
                };
                if (icon("P1 Keypad", Art::KeypadIcon, {12 * s, 8 * s}, {65 * s, 82 * s}, tint(0))) {
                    show_keypad[0] = !show_keypad[0];
                }
                if (icon("P2 Keypad", Art::KeypadIcon, {1843 * s, 8 * s}, {65 * s, 82 * s}, tint(1))) {
                    show_keypad[1] = !show_keypad[1];
                }
                concentration_button(0, 94, s);
                concentration_button(1, 1726, s);
                if (icon("Cards", Art::Card, {814 * s, 8 * s}, {82 * s, 82 * s})) {
                    card_view = !card_view;
                }
                if (icon("Settings", Art::Settings, {919 * s, 8 * s}, {82 * s, 82 * s})) {
                    settings_view = !settings_view;
                }
                if (icon("Coin", Art::Coin, {1024 * s, 8 * s}, {82 * s, 82 * s})) {
                    eamuse_coin_insert();
                }
                ImGui::End();
                if (concentration) {
                    auto *front = ImGui::GetForegroundDrawList();
                    front->AddRectFilled({0, y + 100 * s}, io.DisplaySize, IM_COL32_BLACK);
                    front->AddRectFilled({0, 0}, {io.DisplaySize.x, y + 100 * s}, IM_COL32(0, 0, 0, 128));
                    if (io.MousePos.y > y + 100 * s && ImGui::IsMouseClicked(0)) {
                        concentration = false;
                    }
                    keypads[0] = keypads[1] = 0;
                    ImGui::PopFont();
                    return;
                }

                panel("Ticker", {x + 320 * s, y + 100 * s}, {1280 * s, 220 * s});
                ImGui::SetCursorPos({8 * s, 8 * s});
                legacy_subscreen::draw_ticker(ticker, s);
                ImGui::Dummy({1260 * s, 200 * s});
                ImGui::End();

                draw_effects(x, y, s);
                draw_keypad(0, x + 20 * s, y + 617 * s, s);
                draw_keypad(1, x + 1476 * s, y + 617 * s, s);

                if (card_view) {
                    panel("Cards", {x + 726 * s, y + 327 * s}, {468 * s, 92 * s});
                    if (ImGui::Button("Insert P1 Card", {214 * s, 62 * s})) {
                        eamuse_card_insert(0);
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Insert P2 Card", {214 * s, 62 * s})) {
                        eamuse_card_insert(1);
                    }
                    ImGui::End();
                }
                if (settings_view) {
                    panel("Settings", {x + 26 * s, y + 108 * s}, {278 * s, 138 * s});
                    ImGui::Checkbox("Show P1 keypad", &show_keypad[0]);
                    ImGui::Checkbox("Show P2 keypad", &show_keypad[1]);
                    ImGui::TextUnformatted(pointer_hook ? "Touch relay active" : "Direct touch only");
                    ImGui::End();
                }
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
                artwork.reset();
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

            bool run(HWND main, const RECT &rect, const std::filesystem::path &artwork_path,
                    const std::atomic_bool &stopping) {
                if (!create_window(rect) || !create_renderer(artwork_path)) {
                    log_warning("iidx::subscreen", "Could not open the built-in D3D11 subscreen");
                    return false;
                }
                forward_from = main;
                forward_to = window;
                pointer_hook = SetWindowsHookExW(WH_GETMESSAGE, game_pointer_hook, nullptr,
                    GetWindowThreadProcessId(main, nullptr));
                if (!pointer_hook) {
                    log_warning("iidx::subscreen", "Could not install the game window pointer hook");
                }
                keep_legacy_subscreen_topmost(window, true);
                SetTimer(window, TOPMOST_TIMER, LEGACY_SUBSCREEN_TOPMOST_INTERVAL_MS, nullptr);
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
                if (display_changed && !stopping) {
                    log_info("iidx::subscreen", "Display mode changed; relocating the subscreen");
                    return true;
                }
                return false;
            }
        };
    }

    bool run_builtin_legacy_subscreen(HWND main, const RECT &rect,
        const std::filesystem::path &artwork, const std::atomic_bool &stopping,
        std::array<std::atomic<uint16_t>, 2> &keypads, std::array<std::atomic<int>, 5> &sliders) {
        BuiltinSubscreen panel(keypads, sliders);
        return panel.run(main, rect, artwork, stopping);
    }
}
#endif
