/**
 * @file FileHandle.h
 * @brief Zero-copy virtual memory file mapper for static assets.
 *
 * @details
 * `FileHandle.h` defines high-performance file reading utilities:
 * - `read_file`: Standard heap-allocated file reader using C++ streams.
 * - `MappedFile`: High-throughput RAII memory-mapping (`mmap` on Linux, `MapViewOfFile` on Windows).
 *   Maps file contents directly into the process's virtual address space, enabling zero-copy
 *   HTTP responses via `HttpResponse::html_view` or `HttpResponse::image_view` without reading
 *   bytes into user-space heap buffers.
 *
 * Where this is imported / used:
 * - Direct Include: `#include "FileHandle.h"`
 * - Static Asset Handlers: Used in routes that serve HTML, CSS, JavaScript, PDF, or image files
 *   (e.g. gym contracts, profile photos, invoice PDFs).
 * - Compiled Source: Implemented in `src/FileHandle.cpp`.
 *
 * @author Octane Framework Team / FitOps Backend Core
 * @date 2026
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
    /**
     * @brief Reads an entire file from disk into a newly allocated std::string buffer.
     * @param path Filesystem path to read.
     * @return File bytes as std::string, or empty string on failure.
     */
    std::string read_file(const std::string& path);

    /**
     * @struct MappedFile
     * @brief Move-only RAII container for memory-mapped files (`mmap` / `MapViewOfFile`).
     *
     * @details
     * Maps a disk file directly into the process virtual memory table in read-only mode (`PROT_READ`).
     * Prevents user-space buffer copies and allows the OS page cache to manage memory paging.
     * Copy operations are deleted to prevent double-free of file descriptors and mapped pointers.
     */
    struct MappedFile
    {
        void*       data = nullptr; ///< Pointer to memory-mapped byte buffer in process memory space
        std::size_t size = 0;       ///< Total mapped file size in bytes

    #ifdef _WIN32
        HANDLE file    = INVALID_HANDLE_VALUE; ///< Windows file handle
        HANDLE mapping = nullptr;              ///< Windows file mapping handle
    #else
        int fd = -1;                           ///< POSIX file descriptor
    #endif

        /**
         * @brief Opens and maps the specified disk file into process memory.
         * @param path Path to the target file.
         * @return True if successfully opened and mapped; false otherwise.
         */
        bool open(const std::string& path);

        /**
         * @brief Returns a non-owning string_view over the mapped memory region.
         * @return Non-owning view of file bytes, valid for the lifetime of this MappedFile object.
         */
        [[nodiscard]] std::string_view view() const noexcept;

        /**
         * @brief Destructor that unmaps virtual memory and closes underlying file descriptors.
         */
        ~MappedFile();

        MappedFile() = default;
        MappedFile(const MappedFile&)            = delete;
        MappedFile& operator=(const MappedFile&) = delete;

        /**
         * @brief Move constructor transferring file descriptor and mapped memory ownership.
         * @param o Source MappedFile instance (reset to empty state).
         */
        MappedFile(MappedFile&& o) noexcept;

        /**
         * @brief Move assignment operator unmapping existing resources before transferring ownership.
         * @param o Source MappedFile instance.
         * @return Reference to `*this`.
         */
        MappedFile& operator=(MappedFile&& o) noexcept;
    };
}