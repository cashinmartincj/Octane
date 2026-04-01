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


std::string read_file(const std::string& path);

struct MappedFile
{
    void* data = nullptr;
    std::size_t size = 0;

    #ifdef _WIN32
        HANDLE file = INVALID_HANDLE_VALUE;
        HANDLE mapping = nullptr;
    #else
        int fd = -1;
    #endif
    bool open(const std::string& path);
    std::string_view view() const;

    ~MappedFile();
    MappedFile() = default;
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
};

std::string read_file(const std::string& path);
