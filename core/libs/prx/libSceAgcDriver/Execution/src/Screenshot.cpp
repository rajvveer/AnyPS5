#include "prx/libSceAgcDriver/Execution/include/Screenshot.hpp"
#include "Decoder/Png.hpp"
#include "SDL_filesystem.h"
#include "SDL_error.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::atomic<bool> requested{false};

std::filesystem::path directory() {
#ifdef _WIN32
    const wchar_t* configured = _wgetenv(L"ANYPS5_SCREENSHOTS");
    if (configured != nullptr && configured[0] != L'\0') return configured;
#else
    const char* configured = std::getenv("ANYPS5_SCREENSHOTS");
    if (configured != nullptr && configured[0] != '\0') return configured;
#endif
    const std::unique_ptr<char, decltype(&SDL_free)> basePath(SDL_GetBasePath(), SDL_free);
    if (!basePath) throw std::runtime_error(std::string("Screenshot: cannot locate the executable: ") + SDL_GetError());
    return std::filesystem::path(reinterpret_cast<const char8_t*>(basePath.get())) / "anyps5-screenshots";
}

std::string localTimestamp() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    if (localtime_s(&local, &now) != 0) throw std::runtime_error("Screenshot: cannot read the local time");
#else
    if (localtime_r(&now, &local) == nullptr) throw std::runtime_error("Screenshot: cannot read the local time");
#endif
    char text[32];
    if (std::strftime(text, sizeof(text), "%Y%m%d-%H%M%S", &local) == 0) throw std::runtime_error("Screenshot: cannot format the local time");
    return text;
}

}

void AgcDriver::Screenshot::Request() {
    requested.store(true);
}

bool AgcDriver::Screenshot::Take() {
    return requested.exchange(false);
}

std::filesystem::path AgcDriver::Screenshot::Write(std::string_view titleId, std::uint32_t width, std::uint32_t height, std::span<const std::byte> bgra) {
    if (titleId.empty() || titleId.back() == '.' || titleId.back() == ' ' || titleId.find_first_of("<>:\"/\\|?*") != std::string_view::npos
        || std::any_of(titleId.begin(), titleId.end(), [](unsigned char character) { return character < 0x20 || character == 0x7F; })) {
        throw std::invalid_argument("Screenshot: invalid title ID");
    }
    const auto rowSize = static_cast<std::uint64_t>(width) * 3 + 1;
    if (width == 0 || height == 0 || rowSize > INT_MAX || height > INT_MAX / rowSize
        || width > std::numeric_limits<std::size_t>::max() / 4 / height) {
        throw std::invalid_argument("Screenshot: unsupported frame dimensions");
    }
    const auto pixelCount = static_cast<std::size_t>(width) * height;
    if (bgra.size() != pixelCount * 4) throw std::invalid_argument("Screenshot: invalid BGRA8 frame");
    std::vector<std::uint8_t> rgb(pixelCount * 3);
    for (std::size_t pixel = 0; pixel < pixelCount; ++pixel) {
        rgb[pixel * 3] = std::to_integer<std::uint8_t>(bgra[pixel * 4 + 2]);
        rgb[pixel * 3 + 1] = std::to_integer<std::uint8_t>(bgra[pixel * 4 + 1]);
        rgb[pixel * 3 + 2] = std::to_integer<std::uint8_t>(bgra[pixel * 4]);
    }
    const auto png = Decoder::Png::Encode(rgb, width, height, 3);
    const auto folder = directory();
    std::error_code directoryError;
    std::filesystem::create_directories(folder, directoryError);
    if (directoryError) throw std::filesystem::filesystem_error("Screenshot: cannot create output directory", folder, directoryError);
    const auto stem = std::string(titleId) + "-" + localTimestamp();
    for (int index = 0; index < 1000; ++index) {
        const auto filename = index == 0 ? stem + ".png" : stem + "-" + std::to_string(index) + ".png";
        const auto path = folder / std::filesystem::path(reinterpret_cast<const char8_t*>(filename.c_str()));
#ifdef _WIN32
        std::FILE* const file = _wfopen(path.c_str(), L"wbx");
#else
        std::FILE* const file = std::fopen(path.c_str(), "wbx");
#endif
        if (file == nullptr) {
            if (errno == EEXIST) continue;
            throw std::filesystem::filesystem_error("Screenshot: cannot create PNG", path, std::error_code(errno, std::generic_category()));
        }
        const bool written = std::fwrite(png.data(), 1, png.size(), file) == png.size();
        const int writeError = errno;
        const int closed = std::fclose(file);
        if (!written || closed != 0) {
            const int error = written ? errno : writeError;
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
            throw std::filesystem::filesystem_error("Screenshot: cannot write PNG", path, std::error_code(error == 0 ? EIO : error, std::generic_category()));
        }
        return path;
    }
    throw std::filesystem::filesystem_error("Screenshot: no unused file name for " + stem, folder, std::make_error_code(std::errc::file_exists));
}
