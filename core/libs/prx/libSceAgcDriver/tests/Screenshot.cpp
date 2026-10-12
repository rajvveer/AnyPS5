#include "prx/libSceAgcDriver/Execution/include/Screenshot.hpp"
#include "Decoder/Png.hpp"
#include <array>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <latch>
#include <limits>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void setScreenshotDirectory(const std::filesystem::path& directory) {
#ifdef _WIN32
    require(_wputenv_s(L"ANYPS5_SCREENSHOTS", directory.c_str()) == 0, "set ANYPS5_SCREENSHOTS");
#else
    require(::setenv("ANYPS5_SCREENSHOTS", directory.string().c_str(), 1) == 0, "set ANYPS5_SCREENSHOTS");
#endif
}

std::vector<std::uint8_t> readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    require(file.good(), "open the written screenshot");
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void requestsMerge() {
    require(!AgcDriver::Screenshot::Take(), "no screenshot is requested at start");
    AgcDriver::Screenshot::Request();
    AgcDriver::Screenshot::Request();
    require(AgcDriver::Screenshot::Take(), "a requested screenshot is taken");
    require(!AgcDriver::Screenshot::Take(), "repeated requests are taken once");
}

void writesRgbPng(const std::filesystem::path& directory) {
    const std::array<std::byte, 16> bgra{
        std::byte{0x10}, std::byte{0x20}, std::byte{0x30}, std::byte{0x00}, std::byte{0xA0}, std::byte{0xB0}, std::byte{0xC0}, std::byte{0x7F},
        std::byte{0x00}, std::byte{0xFF}, std::byte{0x80}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0x00}, std::byte{0x40}, std::byte{0x01}};
    const auto first = AgcDriver::Screenshot::Write("TEST00001", 2, 2, bgra);
    const auto bytes = readFile(first);
    const std::array<std::byte, 16> black{};
    const auto second = AgcDriver::Screenshot::Write("TEST00001", 2, 2, black);
    require(first.parent_path() == directory, "the screenshot is written to ANYPS5_SCREENSHOTS");
    require(first.extension() == ".png" && first.filename().string().starts_with("TEST00001-"), "the screenshot is named after the title ID");
    require(first != second && std::filesystem::exists(second), "a second screenshot does not replace the first");
    require(readFile(first) == bytes, "a subsequent screenshot preserves existing contents");
    const auto header = Decoder::Png::ParseHeader(bytes);
    require(header.has_value() && header->colorType == Decoder::Png::ColorType::Rgb && header->bitDepth == 8, "the screenshot is RGB8 PNG");
    const auto image = Decoder::Png::Decode(bytes);
    require(image.has_value() && image->width == 2 && image->height == 2, "the screenshot decodes at the frame extent");
    const std::vector<std::uint8_t> expected{0x30, 0x20, 0x10, 0xFF, 0xC0, 0xB0, 0xA0, 0xFF, 0x80, 0xFF, 0x00, 0xFF, 0x40, 0x00, 0xFF, 0xFF};
    require(image->pixels == expected, "the screenshot holds the frame's colors as opaque RGB");
}

void concurrentWrites(const std::filesystem::path& directory) {
    constexpr std::size_t count = 32;
    std::array<std::filesystem::path, count> paths;
    std::array<std::exception_ptr, count> errors{};
    std::latch ready(count);
    std::latch start(1);
    std::vector<std::jthread> threads;
    for (std::size_t index = 0; index < count; ++index) {
        threads.emplace_back([&, index] {
            ready.count_down();
            start.wait();
            try {
                const std::array<std::byte, 4> pixel{std::byte(index), std::byte{0x40}, std::byte{0x80}, std::byte{0}};
                paths[index] = AgcDriver::Screenshot::Write("CONCURRENT", 1, 1, pixel);
            } catch (...) {
                errors[index] = std::current_exception();
            }
        });
    }
    ready.wait();
    start.count_down();
    threads.clear();
    for (const auto& error : errors) if (error) std::rethrow_exception(error);
    require(std::set<std::filesystem::path>(paths.begin(), paths.end()).size() == count, "concurrent screenshots have unique names");
    bool collision = false;
    for (std::size_t index = 0; index < count; ++index) {
        require(paths[index].parent_path() == directory, "concurrent screenshots stay in the output directory");
        if (paths[index].stem().string().size() > std::string_view("CONCURRENT-YYYYMMDD-HHMMSS").size()) collision = true;
        const auto image = Decoder::Png::Decode(readFile(paths[index]));
        const std::vector<std::uint8_t> expected{0x80, 0x40, static_cast<std::uint8_t>(index), 0xFF};
        require(image.has_value() && image->width == 1 && image->height == 1 && image->pixels == expected, "concurrent screenshots retain their own pixels");
    }
    require(collision, "same-second screenshots use collision suffixes");
}

void rejectsInvalidFrames() {
    const std::array<std::byte, 4> pixel{};
    const auto rejects = [](auto write) {
        try {
            write();
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    require(rejects([&] { AgcDriver::Screenshot::Write("TEST00001", 2, 1, pixel); }), "a frame smaller than its extent is rejected");
    require(rejects([&] { AgcDriver::Screenshot::Write("TEST00001", 1, 1, std::array<std::byte, 8>{}); }), "a frame larger than its extent is rejected");
    require(rejects([&] { AgcDriver::Screenshot::Write("TEST00001", 0, 1, {}); }), "an empty frame is rejected");
    require(rejects([&] { AgcDriver::Screenshot::Write("TEST00001", 1, 0, {}); }), "zero height is rejected");
    require(rejects([&] { AgcDriver::Screenshot::Write("TEST00001", 0x80000000u, 0x80000000u, {}); }), "overflowing BGRA byte counts are rejected before allocation");
    require(rejects([&] { AgcDriver::Screenshot::Write("TEST00001", std::numeric_limits<std::uint32_t>::max(), 1, {}); }), "rows exceeding encoder limits are rejected");
    require(rejects([&] { AgcDriver::Screenshot::Write("TEST00001", 1, std::numeric_limits<std::uint32_t>::max(), {}); }), "images exceeding encoder limits are rejected");
    const std::string_view invalidIds[]{"", ".", "..", "../escape", "..\\escape", "/absolute", "\\absolute", "C:relative", "C:\\absolute",
            "\\\\server\\share", "bad*name", "bad|name", "bad?name", "bad<name", "bad>name", "bad\"name", "trailing.", "trailing ",
            "control\x01", "control\x1F", "control\x7F", std::string_view("nul\0tail", 8)};
    for (const auto titleId : invalidIds) {
        require(rejects([&] { AgcDriver::Screenshot::Write(titleId, 1, 1, pixel); }), "unsafe title IDs are rejected");
    }
}

void reportsOutputErrors(const std::filesystem::path& root) {
    const auto blocked = root / "not-a-directory";
    std::ofstream(blocked) << "keep";
    setScreenshotDirectory(blocked / "screenshots");
    bool failed = false;
    try {
        AgcDriver::Screenshot::Write("TEST00001", 1, 1, std::array<std::byte, 4>{});
    } catch (const std::filesystem::filesystem_error&) {
        failed = true;
    }
    require(failed, "an unusable output directory is reported");
    require(readFile(blocked) == std::vector<std::uint8_t>{'k', 'e', 'e', 'p'}, "an output error preserves existing files");
    setScreenshotDirectory(root);
    failed = false;
    try {
        AgcDriver::Screenshot::Write(std::string(256, 'A'), 1, 1, std::array<std::byte, 4>{});
    } catch (const std::filesystem::filesystem_error&) {
        failed = true;
    }
    require(failed, "file creation errors are reported");
}

}

int main() try {
    const auto root = std::filesystem::temp_directory_path() / ("anyps5_screenshot-" + std::to_string(std::random_device{}()));
    require(std::filesystem::create_directory(root), "create a unique screenshot test directory");
    const auto directory = root / u8"screenshots-\u65E5\u672C\u8A9E-\u03A9" / "nested";
    setScreenshotDirectory(directory);
    requestsMerge();
    writesRgbPng(directory);
    concurrentWrites(directory);
    rejectsInvalidFrames();
    reportsOutputErrors(root);
    std::filesystem::remove_all(root);
    std::cout << "agc screenshot tests passed\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
