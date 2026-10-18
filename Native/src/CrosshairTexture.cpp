#include "Oinkster/CrosshairTexture.hpp"
#include "Oinkster/Diagnostics.hpp"
#include "Oinkster/Json.hpp"
#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

namespace Oinkster::CrosshairTexture {
namespace {

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

constexpr std::array<std::uint8_t, 8> BrarchiveMagic{0x7D, 0x27, 0x25, 0xB1, 0xA0, 0x52, 0x70, 0x26};
constexpr std::size_t BrarchiveHeader = 16;
constexpr std::size_t BrarchiveEntry = 256;
constexpr std::size_t BrarchiveNameCapacity = 247;
constexpr std::uint32_t MaxBrarchiveEntries = 1U << 16;
constexpr std::uint32_t MaxImageEdge = 512;
constexpr std::uintmax_t MaxManifestBytes = 256 * 1024;
constexpr std::uintmax_t MaxPngBytes = 4 * 1024 * 1024;
constexpr std::uintmax_t MaxArchiveBytes = 64 * 1024 * 1024;
constexpr std::size_t MaxPackFolders = 8192;
constexpr std::string_view CrosshairFile = "cross_hair.png";

std::uint32_t ReadU32(std::span<const std::uint8_t> bytes, std::size_t offset) noexcept {
    std::uint32_t value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

// Removes // and /* */ comments outside strings (Bedrock tolerates them).
std::string StripComments(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    bool inString = false;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char character = text[index];
        if (inString) {
            result.push_back(character);
            if (character == '\\' && index + 1 < text.size()) result.push_back(text[++index]);
            else if (character == '"') inString = false;
            continue;
        }
        if (character == '"') { inString = true; result.push_back(character); continue; }
        if (character == '/' && index + 1 < text.size() && text[index + 1] == '/') {
            while (index < text.size() && text[index] != '\n') ++index;
            result.push_back('\n');
            continue;
        }
        if (character == '/' && index + 1 < text.size() && text[index + 1] == '*') {
            const auto end = text.find("*/", index + 2);
            if (end == std::string_view::npos) break;
            index = end + 1;
            continue;
        }
        result.push_back(character);
    }
    return result;
}

std::optional<std::vector<std::uint8_t>> ReadFileBytes(const fs::path& path, std::uintmax_t limit) {
    std::error_code error;
    const auto size = fs::file_size(path, error);
    if (error || size == 0 || size > limit) return std::nullopt;
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return std::nullopt;
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) return std::nullopt;
    return bytes;
}

std::optional<fs::path> EnvironmentPath(const wchar_t* name) {
    std::array<wchar_t, MAX_PATH * 2> buffer{};
    const DWORD length = GetEnvironmentVariableW(name, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return std::nullopt;
    return fs::path(std::wstring(buffer.data(), length));
}

std::optional<fs::path> InstallResourcePacks() {
    std::array<wchar_t, MAX_PATH * 2> buffer{};
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return std::nullopt;
    return fs::path(std::wstring(buffer.data(), length)).parent_path() / L"data" / L"resource_packs";
}

// Every directory that may hold resource-pack folders.
std::vector<fs::path> PackContainers() {
    std::vector<fs::path> containers;
    std::error_code error;
    const auto addGamesRoot = [&](const fs::path& mojang) {
        containers.push_back(mojang / L"resource_packs");
        containers.push_back(mojang / L"development_resource_packs");
        for (fs::directory_iterator world(mojang / L"minecraftWorlds", error), end; !error && world != end; world.increment(error)) {
            containers.push_back(world->path() / L"resource_packs");
        }
        error.clear();
    };
    if (const auto roaming = EnvironmentPath(L"APPDATA")) {
        for (fs::directory_iterator user(*roaming / L"Minecraft Bedrock" / L"Users", error), end; !error && user != end; user.increment(error)) {
            addGamesRoot(user->path() / L"games" / L"com.mojang");
        }
        error.clear();
    }
    if (const auto local = EnvironmentPath(L"LOCALAPPDATA")) {
        addGamesRoot(*local / L"Packages" / L"Microsoft.MinecraftUWP_8wekyb3d8bbwe" / L"LocalState" / L"games" / L"com.mojang");
    }
    if (const auto install = InstallResourcePacks()) containers.push_back(*install);
    return containers;
}

// uuid -> folder, first match wins.
std::unordered_map<std::string, fs::path> IndexPacks() {
    std::unordered_map<std::string, fs::path> index;
    std::size_t visited = 0;
    for (const auto& container : PackContainers()) {
        std::error_code error;
        for (fs::directory_iterator entry(container, error), end; !error && entry != end; entry.increment(error)) {
            if (++visited > MaxPackFolders) return index;
            std::error_code kind;
            if (!entry->is_directory(kind)) continue;
            const auto manifest = ReadFileBytes(entry->path() / L"manifest.json", MaxManifestBytes);
            if (!manifest) continue;
            const auto uuid = ManifestHeaderUuid(std::string_view(reinterpret_cast<const char*>(manifest->data()), manifest->size()));
            if (uuid) index.try_emplace(*uuid, entry->path());
        }
    }
    return index;
}

// The pack's crosshair: a loose PNG, else the entry in its texture archive.
std::optional<ArgbImage> LoadFromPack(const fs::path& pack) {
    if (const auto loose = ReadFileBytes(pack / L"textures" / L"ui" / L"cross_hair.png", MaxPngBytes)) {
        if (auto image = DecodePng(*loose)) return image;
    }
    if (const auto archive = ReadFileBytes(pack / L"__brarchive" / L"textures" / L"ui.brarchive", MaxArchiveBytes)) {
        if (const auto entry = FindBrarchiveEntry(*archive, CrosshairFile)) return DecodePng(*entry);
    }
    return std::nullopt;
}

struct Resolved {
    std::shared_ptr<const ArgbImage> image;
    std::string source;
};

Resolved Resolve(const std::vector<std::string>& uuids) {
    const auto index = IndexPacks();
    for (std::size_t priority = 0; priority < uuids.size(); ++priority) {
        const auto found = index.find(uuids[priority]);
        if (found == index.end()) continue;
        if (auto image = LoadFromPack(found->second)) {
            return {std::make_shared<const ArgbImage>(std::move(*image)), std::format("active pack #{}", priority + 1)};
        }
    }
    if (const auto install = InstallResourcePacks()) {
        if (auto image = LoadFromPack(*install / L"vanilla_base")) {
            return {std::make_shared<const ArgbImage>(std::move(*image)), "vanilla"};
        }
    }
    return {};
}

std::mutex mutex;
std::condition_variable wake;
// Deliberately explicit lifetime. During whole-process termination Windows
// does not run Runtime's normal Detach path, so a global joinable std::thread
// would be destroyed by the debug CRT and call std::terminate()/abort. A raw
// owner has no process-exit destructor; Stop still joins and deletes it during
// Oinkster's supported hot-unload path.
std::thread* worker{};
bool stopping{};
bool pending{};
std::vector<std::string> requested;
std::optional<std::vector<std::string>> resolvedFor;
std::shared_ptr<const ArgbImage> current;

void WorkerMain() {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (;;) {
        std::vector<std::string> uuids;
        {
            std::unique_lock lock(mutex);
            wake.wait(lock, [] { return stopping || pending; });
            if (stopping) break;
            pending = false;
            uuids = requested;
        }
        Resolved resolved;
        try { resolved = Resolve(uuids); } catch (...) {}
        if (resolved.image) {
            Diagnostics::LogLine("info", "crosshair_texture", std::format("{}x{} from {} ({} active packs)",
                resolved.image->width, resolved.image->height, resolved.source, uuids.size()));
        } else {
            Diagnostics::LogLine("warn", "crosshair_texture", std::format("no readable crosshair texture ({} active packs)", uuids.size()));
        }
        std::scoped_lock lock(mutex);
        current = std::move(resolved.image);
    }
    if (SUCCEEDED(initialized)) CoUninitialize();
}

}

std::optional<std::span<const std::uint8_t>> FindBrarchiveEntry(std::span<const std::uint8_t> archive, std::string_view name) noexcept {
    if (archive.size() < BrarchiveHeader || !std::equal(BrarchiveMagic.begin(), BrarchiveMagic.end(), archive.begin())) return std::nullopt;
    const std::uint32_t count = ReadU32(archive, 8);
    if (count == 0 || count > MaxBrarchiveEntries) return std::nullopt;
    const std::size_t dataStart = BrarchiveHeader + static_cast<std::size_t>(count) * BrarchiveEntry;
    if (dataStart > archive.size()) return std::nullopt;
    for (std::uint32_t index = 0; index < count; ++index) {
        const std::size_t entry = BrarchiveHeader + static_cast<std::size_t>(index) * BrarchiveEntry;
        const std::size_t length = archive[entry];
        if (length > BrarchiveNameCapacity) return std::nullopt;
        const std::string_view entryName(reinterpret_cast<const char*>(archive.data() + entry + 1), length);
        if (entryName != name) continue;
        const std::size_t offset = ReadU32(archive, entry + 248);
        const std::size_t size = ReadU32(archive, entry + 252);
        if (offset > archive.size() - dataStart || size > archive.size() - dataStart - offset) return std::nullopt;
        return archive.subspan(dataStart + offset, size);
    }
    return std::nullopt;
}

std::string FormatPackUuid(std::uint64_t high, std::uint64_t low) {
    return std::format("{:08x}-{:04x}-{:04x}-{:04x}-{:08x}{:04x}", high >> 32U, (high >> 16U) & 0xFFFFU, high & 0xFFFFU,
        low >> 48U, (low >> 16U) & 0xFFFFFFFFU, low & 0xFFFFU);
}

std::optional<std::string> ManifestHeaderUuid(std::string_view manifest) {
    const auto parsed = Json::Parse(StripComments(manifest));
    if (!parsed) return std::nullopt;
    const auto* header = parsed->Find("header");
    const auto* uuid = header ? header->Find("uuid") : nullptr;
    const auto* text = uuid ? uuid->AsString() : nullptr;
    if (!text || text->size() != 36) return std::nullopt;
    std::string lowered(*text);
    std::ranges::transform(lowered, lowered.begin(), [](char character) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    });
    return lowered;
}

std::optional<ArgbImage> DecodePng(std::span<const std::uint8_t> bytes) noexcept {
    constexpr std::array<std::uint8_t, 8> signature{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (bytes.size() < signature.size() || bytes.size() > MaxPngBytes
        || !std::equal(signature.begin(), signature.end(), bytes.begin())) {
        return std::nullopt;
    }
    try {
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::optional<ArgbImage> result;
        {
            ComPtr<IWICImagingFactory> factory;
            ComPtr<IWICStream> stream;
            ComPtr<IWICBitmapDecoder> decoder;
            ComPtr<IWICBitmapFrameDecode> frame;
            ComPtr<IWICFormatConverter> converter;
            UINT width{};
            UINT height{};
            bool success = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))
                && SUCCEEDED(factory->CreateStream(&stream))
                && SUCCEEDED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size())))
                && SUCCEEDED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder))
                && SUCCEEDED(decoder->GetFrame(0, &frame))
                && SUCCEEDED(factory->CreateFormatConverter(&converter))
                && SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                    WICBitmapPaletteTypeCustom))
                && SUCCEEDED(converter->GetSize(&width, &height))
                && width != 0 && height != 0 && width <= MaxImageEdge && height <= MaxImageEdge;
            std::vector<std::uint8_t> rgba;
            if (success) {
                rgba.resize(static_cast<std::size_t>(width) * height * 4);
                success = SUCCEEDED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(rgba.size()), rgba.data()));
            }
            if (success) {
                ArgbImage image{width, height, std::vector<std::uint32_t>(static_cast<std::size_t>(width) * height)};
                for (std::size_t index = 0; index < image.pixels.size(); ++index) {
                    const auto* pixel = rgba.data() + index * 4;
                    image.pixels[index] = (static_cast<std::uint32_t>(pixel[3]) << 24U) | (static_cast<std::uint32_t>(pixel[0]) << 16U)
                        | (static_cast<std::uint32_t>(pixel[1]) << 8U) | pixel[2];
                }
                result = std::move(image);
            }
        }
        if (SUCCEEDED(initialized)) CoUninitialize();
        return result;
    } catch (...) {
        return std::nullopt;
    }
}

void Start() noexcept {
    std::scoped_lock lock(mutex);
    if (worker && worker->joinable()) return;
    stopping = false;
    pending = false;
    resolvedFor.reset();
    current.reset();
    try { worker = new std::thread(WorkerMain); } catch (...) {
        worker = nullptr;
        Diagnostics::LogLine("warn", "crosshair_texture", "the texture worker could not start");
    }
}

void Stop() noexcept {
    std::thread* stoppingWorker{};
    {
        std::scoped_lock lock(mutex);
        stopping = true;
        stoppingWorker = std::exchange(worker, nullptr);
    }
    wake.notify_all();
    if (stoppingWorker) {
        if (stoppingWorker->joinable()) stoppingWorker->join();
        delete stoppingWorker;
    }
    std::scoped_lock lock(mutex);
    current.reset();
    resolvedFor.reset();
}

void Request(std::vector<std::string> packUuids) noexcept {
    try {
        std::scoped_lock lock(mutex);
        if (!worker || !worker->joinable() || stopping || (resolvedFor && *resolvedFor == packUuids)) return;
        resolvedFor = packUuids;
        requested = std::move(packUuids);
        pending = true;
    } catch (...) {
        return;
    }
    wake.notify_one();
}

std::shared_ptr<const ArgbImage> Current() noexcept {
    std::scoped_lock lock(mutex);
    return current;
}

}
