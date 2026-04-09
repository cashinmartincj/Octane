/**
 * @file FileHandle.h
 * @brief File I/O utilities for Octane — memory mapped and standard reads
 *
 * Provides two file reading strategies:
 *   MappedFile — maps file into virtual memory, zero copy serving
 *   read_file  — reads entire file into a std::string (one-shot reads)
 *
 * Use MappedFile for static assets served repeatedly (HTML, images).
 * Use read_file for config files or one-time reads.
 */

#pragma once
#include <string_view>
#include <cstring>
#include <string>
#include <sys/inotify.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#endif

/**
 * @brief Read entire file into a std::string
 *
 * Simple one-shot file read into owned memory.
 * Prefer MappedFile for files served repeatedly.
 *
 * @param path  Absolute or relative path to file
 * @return file contents as std::string, empty string on error
 */
std::string read_file(const std::string& path);

/**
 * @brief Memory mapped file for zero copy static file serving
 *
 * Maps a file directly into the process virtual address space.
 * view() returns a string_view pointing into mapped memory —
 * no allocation, no copy, kernel handles caching automatically.
 *
 * Non-copyable — only one owner of the mapping at a time.
 * Use std::unique_ptr<MappedFile> to transfer ownership.
 *
 * Example:
 *   MappedFile f;
 *   if (f.open("index.html"))
 *       res.html_view(f.view());
 */
struct MappedFile
{
    void*       data = nullptr;   // pointer to mapped memory
    std::size_t size = 0;         // file size in bytes

#ifdef _WIN32
    HANDLE file    = INVALID_HANDLE_VALUE;  // file handle
    HANDLE mapping = nullptr;               // mapping handle
#else
    int fd = -1;   // file descriptor
#endif

    /**
     * @brief Map file into memory
     * @param path  Path to file
     * @return true on success, false on any error
     */
    bool open(const std::string& path);

    /**
     * @brief Get non-owning view into mapped memory
     *
     * Zero copy — points directly into mmap'd pages.
     * Valid only while MappedFile is alive and open() returned true.
     *
     * @return string_view into mapped file contents
     */
    std::string_view view() const;

    /**
     * @brief Destructor — unmaps memory and closes handles
     */
    ~MappedFile();

    MappedFile() = default;

    // non-copyable — mapping has exactly one owner
    MappedFile(const MappedFile&)            = delete;
    MappedFile& operator=(const MappedFile&) = delete;
};