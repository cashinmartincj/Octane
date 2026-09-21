#include "../include/FileHandle.h"
#include <fstream>

namespace octane::utils
{
    std::string read_file(const std::string& path) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) return {};

        auto file_size = file.tellg();
        if (file_size <= 0) return {};

        std::string buffer;
        buffer.resize(static_cast<std::size_t>(file_size));
        file.seekg(0, std::ios::beg);
        file.read(buffer.data(), file_size);
        return buffer;
    }

    bool MappedFile::open(const std::string& path) {
    #ifdef _WIN32
        file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;

        LARGE_INTEGER li;
        if (!GetFileSizeEx(file, &li)) {
            CloseHandle(file);
            file = INVALID_HANDLE_VALUE;
            return false;
        }
        size = static_cast<std::size_t>(li.QuadPart);

        mapping = CreateFileMappingA(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!mapping) {
            CloseHandle(file);
            file = INVALID_HANDLE_VALUE;
            return false;
        }

        data = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
        return data != nullptr;
    #else
        fd = ::open(path.c_str(), O_RDONLY);
        if (fd == -1) return false;

        struct stat st;
        if (::fstat(fd, &st) == -1 || st.st_size == 0) {
            ::close(fd);
            fd = -1;
            return false;
        }
        size = static_cast<std::size_t>(st.st_size);

        data = ::mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
        if (data == MAP_FAILED) {
            data = nullptr;
            ::close(fd);
            fd = -1;
            return false;
        }
        return true;
    #endif
    }

    std::string_view MappedFile::view() const noexcept {
        if (!data || size == 0) return {};
        return std::string_view(static_cast<const char*>(data), size);
    }

    MappedFile::~MappedFile() {
    #ifdef _WIN32
        if (data) UnmapViewOfFile(data);
        if (mapping) CloseHandle(mapping);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    #else
        if (data) ::munmap(data, size);
        if (fd != -1) ::close(fd);
    #endif
    }

    MappedFile::MappedFile(MappedFile&& o) noexcept 
        : data(o.data), size(o.size)
    #ifdef _WIN32
        , file(o.file), mapping(o.mapping)
    #else
        , fd(o.fd)
    #endif
    {
        o.data = nullptr;
        o.size = 0;
    #ifdef _WIN32
        o.file = INVALID_HANDLE_VALUE;
        o.mapping = nullptr;
    #else
        o.fd = -1;
    #endif
    }

    MappedFile& MappedFile::operator=(MappedFile&& o) noexcept {
        if (this != &o) {
            this->~MappedFile();
            data = o.data;
            size = o.size;
        #ifdef _WIN32
            file = o.file;
            mapping = o.mapping;
            o.file = INVALID_HANDLE_VALUE;
            o.mapping = nullptr;
        #else
            fd = o.fd;
            o.fd = -1;
        #endif
            o.data = nullptr;
            o.size = 0;
        }
        return *this;
    }
}