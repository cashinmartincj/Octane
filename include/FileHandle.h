/**
 * @file FileHandle.h
 * @brief Zero-copy virtual memory file mapper for static assets.
 */

#pragma once
#include <string_view>
#include <string>
#include <cstddef>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#endif

namespace octane::utils
{
    std::string read_file(const std::string& path);

    struct MappedFile
    {
        void*       data = nullptr;
        std::size_t size = 0;

    #ifdef _WIN32
        HANDLE file    = INVALID_HANDLE_VALUE;
        HANDLE mapping = nullptr;
    #else
        int fd = -1;
    #endif

        bool open(const std::string& path);
        [[nodiscard]] std::string_view view() const noexcept;
        ~MappedFile();

        MappedFile() = default;
        MappedFile(const MappedFile&)            = delete;
        MappedFile& operator=(const MappedFile&) = delete;
        MappedFile(MappedFile&& o) noexcept;
        MappedFile& operator=(MappedFile&& o) noexcept;
    };
}