#include "prx/libSceVideoOut/include/VideoOutDriver.hpp"
#include "prx/libSceVideoOut/include/Buffer.hpp"
#include "prx/libSceVideoOut/include/Output.hpp"
#include "prx/libSceAgcDriver/Execution/include/DisplayBuffer.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/HostHdr.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/Shutdown.hpp"
#include "tests/VideoOutTestEnvironment.hpp"
#include "Decoder/Png.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <io.h>
#define APS5_DUP _dup
#define APS5_DUP2 _dup2
#define APS5_CLOSE _close
#define APS5_FILENO _fileno
#else
#include <unistd.h>
#define APS5_DUP dup
#define APS5_DUP2 dup2
#define APS5_CLOSE close
#define APS5_FILENO fileno
#endif

#undef main

namespace {

constexpr std::uint64_t Bgra8 = 0x8000000000000000ull;
constexpr std::uint64_t Bgr10Pq = 0x8100070400000000ull;
constexpr std::uint64_t Rgb10Pq = 0x8100070422000000ull;
constexpr std::uint32_t ClearWord = 600u | (500u << 10u) | (300u << 20u) | (3u << 30u);
alignas(256) constexpr std::array<std::uint32_t, 8> StoreCode{
    0x7e020280, 0x7e0402f2, 0x7e0602f2, 0x7e0802f2, 0x7e0a02f2, 0xf0201f08, 0x00010200, 0xbf810000,
};

void check(bool condition, const std::string& reason) {
    if (!condition) throw std::runtime_error(reason);
}

void setEnvironment(const char* name, const char* value) {
#ifdef _WIN32
    check(_putenv_s(name, value) == 0, std::string("cannot set ") + name);
#else
    check(setenv(name, value, 1) == 0, std::string("cannot set ") + name);
#endif
}

class CapturedLog {
    int saved;
public:
    CapturedLog() : saved(APS5_DUP(APS5_FILENO(stderr))) {
        check(saved >= 0, "cannot save stderr");
        check(std::freopen("screenshot-present.log", "wb", stderr) != nullptr, "cannot capture stderr");
    }
    ~CapturedLog() {
        std::fflush(stderr);
        APS5_DUP2(saved, APS5_FILENO(stderr));
        APS5_CLOSE(saved);
    }
    std::string Read() const {
        std::fflush(stderr);
        std::ifstream file("screenshot-present.log", std::ios::binary);
        return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }
    std::size_t Saved() const {
        const auto text = Read();
        std::size_t count = 0;
        for (std::size_t offset = 0; (offset = text.find("[gpu] screenshot saved:", offset)) != std::string::npos; ++offset) ++count;
        return count;
    }
    std::vector<std::filesystem::path> Paths() const {
        const auto text = Read();
        const std::string marker = "[gpu] screenshot saved: ";
        std::vector<std::filesystem::path> paths;
        for (std::size_t offset = 0; (offset = text.find(marker, offset)) != std::string::npos;) {
            offset += marker.size();
            const auto end = text.find_first_of("\r\n", offset);
            check(end != std::string::npos, "the screenshot completion log is incomplete");
            paths.emplace_back(text.substr(offset, end - offset));
            offset = end;
        }
        return paths;
    }
};

std::span<std::byte> alignedBuffer(std::vector<std::byte>& allocation, std::size_t bytes) {
    allocation.resize(bytes + 65535u);
    const auto address = reinterpret_cast<std::uintptr_t>(allocation.data());
    const auto offset = (65536u - (address & 65535u)) & 65535u;
    return {allocation.data() + offset, bytes};
}

void writeResident(std::span<std::byte> storage, std::uint32_t width, std::uint32_t height) {
    const auto address = reinterpret_cast<std::uint64_t>(storage.data());
    const std::array<std::uint32_t, 8> texture{
        static_cast<std::uint32_t>(address >> 8u),
        static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (50u << 20u) | (((width - 1u) & 3u) << 30u),
        ((width - 1u) >> 2u) | ((height - 1u) << 14u),
        0xfacu | (0x1bu << 20u) | (9u << 28u), 0u, 0u, 0u, 0u};
    std::array<std::uint32_t, 16> userData{};
    std::copy(texture.begin(), texture.end(), userData.begin() + 4);
    std::vector<std::uint32_t> words;
    const auto registers = [&](std::uint32_t first, std::span<const std::uint32_t> values) {
        words.push_back(0xc0007600u | (static_cast<std::uint32_t>(values.size()) << 16u));
        words.push_back(first);
        words.insert(words.end(), values.begin(), values.end());
    };
    registers(0x207, std::array<std::uint32_t, 3>{32, 1, 1});
    const auto program = reinterpret_cast<std::uint64_t>(StoreCode.data());
    registers(0x20c, std::array<std::uint32_t, 2>{static_cast<std::uint32_t>(program >> 8u), static_cast<std::uint32_t>(program >> 40u)});
    registers(0x213, std::array<std::uint32_t, 1>{32});
    registers(0x240, userData);
    words.insert(words.end(), {0xc0031500u, 1, 1, 1, 0x41u});
    Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
    AgcDriver::Submit(&packet, 0);
    AgcDriverWaitIdle_nid_postfix();
    check(AgcDriver::Graphics::StorageTexture::FindPending(address, storage.size()) != nullptr, "the compute dispatch did not leave a resident display image");
}

void flip(int handle, const std::shared_ptr<VideoOutConfig>& cfg, int index) {
    std::uint64_t target = 0;
    {
        std::lock_guard lock(cfg->mutex);
        target = cfg->flipStatus.count + 1;
    }
    check(sceVideoOutSubmitFlip(handle, index, 1, index) == 0, "flip submission failed");
    std::unique_lock lock(cfg->mutex);
    check(cfg->vblankCond.wait_for(lock, std::chrono::seconds(10), [&] { return cfg->failure != nullptr || cfg->flipStatus.count == target; }), "presentation did not complete");
    if (cfg->failure) std::rethrow_exception(cfg->failure);
    check(cfg->flipStatus.currentBuffer == index && cfg->flipStatus.flipPendingNum == 0, "presentation status is wrong");
}

void pushScreenshotKey(std::uint32_t windowId, bool down, bool repeat = false) {
    SDL_Event event{};
    event.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    event.key.windowID = windowId;
    event.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    event.key.repeat = repeat ? 1 : 0;
    event.key.keysym.scancode = SDL_SCANCODE_F12;
    event.key.keysym.sym = SDLK_F12;
    check(SDL_PushEvent(&event) == 1, "the screenshot key event was not queued");
}

template<typename TReady, typename TProgress>
void waitFor(TReady ready, TProgress progress) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (!ready()) {
        check(std::chrono::steady_clock::now() < deadline, "screenshot completion timed out");
        progress();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

std::vector<std::filesystem::path> screenshots() {
    std::vector<std::filesystem::path> paths;
    if (!std::filesystem::is_directory("captures")) return paths;
    for (const auto& file : std::filesystem::directory_iterator("captures")) {
        check(file.path().extension() == ".png", "the screenshot directory contains a non-PNG file");
        check(file.path().filename().string().starts_with("PPSA00000-"), "the screenshot filename lacks the title ID");
        paths.push_back(file.path());
    }
    std::sort(paths.begin(), paths.end());
    return paths;
}

void checkImage(const std::filesystem::path& path, std::uint32_t width, std::uint32_t height, std::span<const std::byte> expected) {
    std::ifstream file(path, std::ios::binary);
    const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    const auto header = Decoder::Png::ParseHeader(bytes);
    check(header && header->colorType == Decoder::Png::ColorType::Rgb && header->bitDepth == 8, "the screenshot is not RGB8 PNG");
    const auto image = Decoder::Png::Decode(bytes);
    check(image && image->width == width && image->height == height, "the screenshot was scaled to the window or BMP dump extent");
    check(image->pixels.size() == static_cast<std::size_t>(width) * height * 4, "the decoded PNG size is wrong");
    int worst = 0;
    for (std::size_t pixel = 0; pixel < image->pixels.size(); pixel += 4) {
        const auto source = expected.size() == 4 ? 0 : pixel;
        for (std::size_t channel = 0; channel < 3; ++channel) worst = std::max(worst, std::abs(static_cast<int>(image->pixels[pixel + channel]) - std::to_integer<int>(expected[source + 2 - channel])));
        check(image->pixels[pixel + 3] == 255, "the screenshot is not opaque");
    }
    check(worst <= 1, "the screenshot differs from the CPU conversion by " + std::to_string(worst));
}

int run(const std::string& mode) {
    check(mode == "present" || mode == "bmp" || mode == "failures" || mode == "8k" || mode == "resident" || mode == "input", "unknown screenshot test mode");
    const bool large = mode == "8k";
    const bool bmp = mode == "bmp";
    const bool failures = mode == "failures";
    const bool resident = mode == "resident";
    const bool input = mode == "input";
    const auto width = large ? 7680u : 259u;
    const auto height = large ? 4320u : 137u;
    VideoOutTestEnvironment environment;
    CapturedLog log;
    setEnvironment("ANYPS5_SCREENSHOTS", "captures");
    setEnvironment("APS5_DUMP_SCALE", "4");
    setEnvironment("APS5_DUMP_FRAMES", bmp || failures ? "2" : "0");
    if (input) {
        std::ofstream config("screenshot-input.ini");
        config << "Screenshot = KEY:F12\n";
        check(config.good(), "cannot write the screenshot input mapping");
        setEnvironment("ANYPS5_INPUT_CONFIG", "screenshot-input.ini");
    }
    if (failures) {
        std::filesystem::create_directory("frame_000.bmp");
        std::ofstream blocked("captures");
        blocked << "keep this file";
        check(blocked.good(), "cannot obstruct the screenshot directory");
    }
    std::array<std::vector<std::byte>, 4> allocations;
    std::array<std::span<std::byte>, 4> storage;
    std::vector<std::uint8_t> keys(4096, 0x20);
    const auto linearBytes = static_cast<std::size_t>(width) * height * 4;
    const auto tiledBytes = static_cast<std::size_t>((width + 127u) / 128u) * ((height + 127u) / 128u) * 65536u;
    int handle = 0;
    bool registeredMemory = false;
    try {
        try {
            handle = sceVideoOutOpen(255, 0, 0, nullptr);
        } catch (const std::runtime_error& error) {
            const std::string reason = error.what();
            const bool unavailable = reason.starts_with("SDL_InitSubSystem") || reason.starts_with("Vulkan: no Vulkan 1.1") || reason.find("Vulkan support is either not configured") != std::string::npos;
            if (std::getenv("ANYPS5_REQUIRE_DISPLAY") != nullptr || !unavailable) throw;
            std::printf("skipped, no display or Vulkan device: %s\n", error.what());
            LibcRunShutdown_nid_postfix();
            return 77;
        }
        check(handle > 0, "sceVideoOutOpen failed");
        auto cfg = VideoOutDriver::Get().GetConfig(handle);
        AgcDriverSetHostDisplayHdr_nid_postfix({});
        AgcDriverNoteReportedToneMapPeak_nid_postfix(437.0f);
        const int count = mode == "present" ? 4 : 1;
        for (int index = 0; index < count; ++index) {
            const bool linear = !resident && (index == 0 || index == 2);
            storage[index] = alignedBuffer(allocations[index], linear ? linearBytes : tiledBytes);
            for (std::size_t offset = 0; offset < storage[index].size(); offset += 4) {
                const auto code = static_cast<std::uint32_t>((offset / 4) % 1024u);
                const std::uint32_t word = large ? 0xFF735129u : index == 0 && !resident ? (code * 0x010305u) | 0xFF000000u : code | (((code * 7u) % 1024u) << 10u) | ((1023u - code) << 20u) | (3u << 30u);
                std::memcpy(storage[index].data() + offset, &word, 4);
            }
            VideoOutBufferAttribute2 attribute{};
            const auto format = resident ? Bgr10Pq : index == 0 ? Bgra8 : index == 1 ? Bgr10Pq : Rgb10Pq;
            sceVideoOutSetBufferAttribute2(&attribute, format, linear ? 1 : 0, width, height, 0, index == 3 ? 0x208 : 0, index == 3 ? ClearWord : 0);
            VideoOutBuffers buffer{storage[index].data(), index == 3 ? keys.data() : nullptr, {nullptr, nullptr}};
            check(sceVideoOutRegisterBuffers2(handle, index, index, &buffer, 1, &attribute, index == 3 ? VIDEO_OUT_BUFFER_ATTRIBUTE_CATEGORY_COMPRESSED : 0, nullptr) == 0, "the display buffer was not registered");
        }
        const auto present = [&](int index) {
            flip(handle, cfg, index);
            check(!AgcDriverPresentingHdr10_nid_postfix(), "the screenshot test did not use the SDR presentation path");
        };
        std::uint32_t windowId = 0;
        if (input) {
            present(0);
            const auto* driver = SDL_GetCurrentVideoDriver();
            if (driver != nullptr && (std::strcmp(driver, "offscreen") == 0 || std::strcmp(driver, "dummy") == 0)) {
                check(std::getenv("ANYPS5_REQUIRE_DISPLAY") == nullptr, "the screenshot input test requires a focused display window");
                sceVideoOutClose(handle);
                LibcRunShutdown_nid_postfix();
                std::printf("skipped, the %s SDL driver has no interactive window focus\n", driver);
                return 77;
            }
            auto* window = SDL_GetKeyboardFocus();
            check(window != nullptr && std::string(SDL_GetWindowTitle(window)).starts_with("Example"), "the game window does not have keyboard focus");
            windowId = SDL_GetWindowID(window);
            check(windowId != 0, "the game window has no SDL ID");
        }
        auto sdrExpected = large ? std::vector<std::byte>(storage[0].begin(), storage[0].begin() + 4) : AgcDriver::DecodeDisplayBuffer({reinterpret_cast<std::uint64_t>(storage[0].data()), resident ? Bgr10Pq : Bgra8, width, height, resident ? 0u : 1u}, storage[0]);
        if (resident) {
            {
                GuestAllocations::Mutation mutation;
                mutation.Add(storage[0].data(), storage[0].size(), true, true, true);
                registeredMemory = true;
            }
            const auto white = AgcDriver::PqToSdrPixel(0xffffffffu, false, 437.0f);
            for (std::size_t pixel = 0; pixel < 32; ++pixel) std::memcpy(sdrExpected.data() + pixel * 4, white.data(), 4);
            writeResident(storage[0], width, height);
        }
        if (failures) {
            AgcDriverRequestScreenshot_nid_postfix();
            waitFor([&] { const auto text = log.Read(); return text.find("optional BMP dumps disabled:") != std::string::npos && text.find("screenshot failed:") != std::string::npos; }, [&] { present(0); });
            check(std::filesystem::is_regular_file("captures"), "a failed screenshot replaced an existing file");
            check(screenshots().empty(), "the failed screenshot produced a PNG");
            std::filesystem::remove("captures");
            std::filesystem::create_directory("captures");
        }
        if (large) present(0);
        if (input) pushScreenshotKey(windowId, true);
        else {
            AgcDriverRequestScreenshot_nid_postfix();
            AgcDriverRequestScreenshot_nid_postfix();
        }
        present(0);
        auto nextRetry = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        waitFor([&] { return log.Saved() >= 1; }, [&] {
            if (!large || std::chrono::steady_clock::now() >= nextRetry) {
                present(0);
                nextRetry = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
            }
        });
        check(log.Saved() == 1 && screenshots().size() == 1, "repeated requests did not coalesce");
        checkImage(screenshots().front(), width, height, sdrExpected);
        if (input) {
            pushScreenshotKey(windowId, true, true);
            pushScreenshotKey(windowId, true);
            pushScreenshotKey(windowId, false);
            pushScreenshotKey(windowId, false);
            pushScreenshotKey(windowId, true, true);
            pushScreenshotKey(windowId + 1u, true);
            pushScreenshotKey(windowId + 1u, false);
            present(0);
            present(0);
        }
        if (resident) {
            const auto address = reinterpret_cast<std::uint64_t>(storage[0].data());
            check(AgcDriver::Graphics::StorageTexture::FindPending(address, storage[0].size()) != nullptr, "the screenshot flushed the resident image to guest memory");
            std::uint32_t word = 0;
            std::memcpy(&word, storage[0].data(), 4);
            check(word == (1023u << 20u | 3u << 30u), "the screenshot read back the guest image instead of the resident display");
        }
        if (mode == "present") {
            AgcDriverRequestScreenshot_nid_postfix();
            present(1);
            waitFor([&] { return log.Saved() >= 2; }, [&] { present(1); });
            AgcDriverRequestScreenshot_nid_postfix();
            present(2);
            waitFor([&] { return log.Saved() >= 3; }, [&] { present(2); });
            auto paths = log.Paths();
            check(paths.size() == 3, "a subsequent request was lost or duplicated");
            checkImage(paths[1], width, height, AgcDriver::DecodeDisplayBuffer({reinterpret_cast<std::uint64_t>(storage[1].data()), Bgr10Pq, width, height}, storage[1]));
            checkImage(paths[2], width, height, AgcDriver::DecodeDisplayBuffer({reinterpret_cast<std::uint64_t>(storage[2].data()), Rgb10Pq, width, height, 1}, storage[2]));
            AgcDriverRequestScreenshot_nid_postfix();
            present(3);
            waitFor([&] { return log.Saved() >= 4; }, [&] { present(3); });
            const auto clear = AgcDriver::PqToSdrPixel(ClearWord, true, 437.0f);
            checkImage(log.Paths().back(), width, height, std::as_bytes(std::span(clear)));
        }
        for (int index = 0; index < count; ++index) check(sceVideoOutUnregisterBuffers(handle, index) == 0, "buffer unregister failed");
        check(sceVideoOutClose(handle) == 0, "sceVideoOutClose failed");
        LibcRunShutdown_nid_postfix();
        if (registeredMemory) {
            GuestAllocations::Mutation mutation;
            mutation.Remove(storage[0].data());
            registeredMemory = false;
        }
        check(screenshots().size() == static_cast<std::size_t>(mode == "present" ? 4 : 1), "unrequested screenshots were written");
        if (bmp) {
            for (const char* name : {"frame_000.bmp", "frame_001.bmp"}) {
                std::ifstream file(name, std::ios::binary);
                std::array<char, 54> header{};
                check(static_cast<bool>(file.read(header.data(), header.size())), "the BMP dump is missing");
                std::uint32_t bmpWidth = 0, bmpHeight = 0;
                std::memcpy(&bmpWidth, header.data() + 18, 4);
                std::memcpy(&bmpHeight, header.data() + 22, 4);
                check(bmpWidth == (width + 3) / 4 && bmpHeight == static_cast<std::uint32_t>(-static_cast<std::int32_t>((height + 3) / 4)), "PNG capture changed the BMP dump scale");
            }
        }
        std::printf("screenshot %s passed; HDR10 monitor presentation not exercised\n", mode.c_str());
        return 0;
    } catch (...) {
        try { LibcRunShutdown_nid_postfix(); } catch (...) {}
        if (registeredMemory) {
            GuestAllocations::Mutation mutation;
            mutation.Remove(storage[0].data());
        }
        std::printf("%s", log.Read().c_str());
        throw;
    }
}

}

int main(int argc, char** argv) {
    try {
        check(argc <= 2, "expected one screenshot test mode");
        return run(argc == 2 ? argv[1] : "present");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
