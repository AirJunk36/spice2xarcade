// Resource IDs from iidx_btools_subscreen, copyright 2025 cloud11665.
// See licenses.txt. The images and font are read from the player-supplied DLL.
#pragma once

#include <array>
#include <cstring>
#include <filesystem>
#include <vector>
#include <d3d11.h>
#include <wincodec.h>
#include <wrl/client.h>

#include "imgui.h"

namespace games::iidx::legacy_subscreen {
    class Artwork {
    public:
        enum Image {
            KeypadIcon, Keypad, KeypadClose, Card, Settings, TrackBackground, TrackZero, TrackOne,
            TrackHead, ConcentrationOn, ConcentrationOff, Coin, ImageCount,
        };

        struct Texture {
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;

            void draw(ImDrawList *list, ImVec2 position, ImVec2 size, ImU32 tint = IM_COL32_WHITE) const {
                list->AddImage((ImTextureID)(uintptr_t)view.Get(), position,
                    {position.x + size.x, position.y + size.y}, {0, 0}, {1, 1}, tint);
            }
        };

        Artwork() = default;
        Artwork(const Artwork &) = delete;
        Artwork &operator=(const Artwork &) = delete;
        ~Artwork() {
            if (module) {
                FreeLibrary(module);
            }
        }

        const Texture &operator[](Image image) const { return images[image]; }

        bool load(const std::filesystem::path &path, ID3D11Device *device) {
            // DATAFILE maps the PE resources without resolving imports or running DllMain.
            module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE);
            if (!module) {
                return false;
            }
            if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
                return false;
            }
            const bool loaded = load_images(device);
            CoUninitialize();
            return loaded;
        }

        void load_font() const {
            const auto font = resource(101);
            if (font.size < 12 || font.size > 32 * 1024 * 1024 ||
                    std::memcmp(font.data, "ttcf", 4) != 0) {
                return;
            }
            ImFontConfig config;
            config.FontDataOwnedByAtlas = false;
            auto &io = ImGui::GetIO();
            io.FontDefault = io.Fonts->AddFontFromMemoryTTF(
                font.data, static_cast<int>(font.size), 20.0f, &config);
        }

    private:
        struct Resource {
            BYTE *data = nullptr;
            DWORD size = 0;
        };
        HMODULE module = nullptr;
        std::array<Texture, ImageCount> images {};

        Resource resource(WORD id) const {
            const auto info = FindResourceW(module, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10)); // RCDATA
            if (!info) {
                return {};
            }
            const auto bytes = static_cast<BYTE *>(LockResource(LoadResource(module, info)));
            return bytes ? Resource {bytes, SizeofResource(module, info)} : Resource {};
        }

        bool load_images(ID3D11Device *device) {
            // WIC 1.0 factory and 32bpp RGBA pixel format identifiers.
            static constexpr GUID factory_id {0xcacaf262, 0x9370, 0x4615,
                {0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a}};
            static constexpr GUID rgba {0xf5c7ad2d, 0x6a8d, 0x43dd,
                {0xa7, 0xa8, 0xa2, 0x99, 0x35, 0x26, 0x1a, 0xe9}};
            static constexpr std::array<WORD, ImageCount> ids {202, 203, 204, 208, 209,
                210, 211, 212, 213, 214, 215, 216};
            using Microsoft::WRL::ComPtr;
            ComPtr<IWICImagingFactory> factory;
            if (FAILED(CoCreateInstance(factory_id, nullptr, CLSCTX_INPROC_SERVER,
                    IID_PPV_ARGS(factory.GetAddressOf())))) {
                return false;
            }
            for (size_t i = 0; i < ids.size(); ++i) {
                const auto source = resource(ids[i]);
                if (!source.size && i == Coin) {
                    continue; // The original v0.2.0 release predates the coin icon.
                }
                ComPtr<IWICStream> stream;
                ComPtr<IWICBitmapDecoder> decoder;
                ComPtr<IWICBitmapFrameDecode> frame;
                ComPtr<IWICFormatConverter> converter;
                if (!source.size || FAILED(factory->CreateStream(stream.GetAddressOf())) ||
                        FAILED(stream->InitializeFromMemory(source.data, source.size)) ||
                        FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr,
                            WICDecodeMetadataCacheOnDemand, decoder.GetAddressOf())) ||
                        FAILED(decoder->GetFrame(0, frame.GetAddressOf())) ||
                        FAILED(factory->CreateFormatConverter(converter.GetAddressOf())) ||
                        FAILED(converter->Initialize(frame.Get(), rgba, WICBitmapDitherTypeNone,
                            nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
                    return false;
                }
                UINT width = 0, height = 0;
                if (FAILED(converter->GetSize(&width, &height)) || !width || !height ||
                        width > 2048 || height > 2048) {
                    return false;
                }
                std::vector<BYTE> pixels(width * height * 4u);
                if (FAILED(converter->CopyPixels(nullptr, width * 4u,
                        static_cast<UINT>(pixels.size()), pixels.data()))) {
                    return false;
                }
                D3D11_TEXTURE2D_DESC desc {};
                desc.Width = width;
                desc.Height = height;
                desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
                desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                desc.Usage = D3D11_USAGE_IMMUTABLE;
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                const D3D11_SUBRESOURCE_DATA data {pixels.data(), width * 4u, 0};
                ComPtr<ID3D11Texture2D> texture;
                auto &image = images[i];
                if (FAILED(device->CreateTexture2D(&desc, &data, texture.GetAddressOf())) ||
                        FAILED(device->CreateShaderResourceView(texture.Get(), nullptr,
                            image.view.GetAddressOf()))) {
                    return false;
                }
            }
            return true;
        }
    };
}
