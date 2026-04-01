#include "./FileHandle.h"

bool MappedFile::open(const std::string& path) 
{
#ifdef _WIN32
    file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) 
        return false;
    size = GetFileSize(file, nullptr);
    mapping = CreateFileMappingA(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mapping) 
    { 
        CloseHandle(file); 
        return false; 
    }
    data = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (!data) 
    { 
        CloseHandle(mapping); 
        CloseHandle(file); 
        return false; 
    }
#else
    fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) 
        return false;
    struct stat st;
    fstat(fd, &st);
    size = st.st_size;

    data = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (data == MAP_FAILED) 
    { 
        close(fd); 
        data = nullptr; 
        return false; 
    }
#endif
    return true;
}

std::string_view MappedFile::view() const 
{
    return {static_cast<char*>(data), size};
}

MappedFile::~MappedFile() 
{
    #ifdef _WIN32
        if (data) UnmapViewOfFile(data);
        if (mapping) CloseHandle(mapping);
        if (file != INVALID_HANDLE_VALUE) 
            CloseHandle(file);
    #else
        if (data && data != MAP_FAILED) 
            munmap(data, size);
        if (fd >= 0) 
            ::close(fd);
    #endif
}


std::string read_file(const std::string& path) {
    #ifdef _WIN32
        HANDLE file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return "";

        DWORD size = GetFileSize(file, nullptr);
        std::string content(size, '\0');
        DWORD bytesRead;
        ReadFile(file, content.data(), size, &bytesRead, nullptr);
        CloseHandle(file);
        return content;
    #else
        int fd = open(path.c_str(), O_RDONLY);
        if (fd < 0) return "";

        struct stat st;
        fstat(fd, &st);

        std::string content(st.st_size, '\0');
        read(fd, content.data(), st.st_size);
        close(fd);
        return content;
    #endif
}