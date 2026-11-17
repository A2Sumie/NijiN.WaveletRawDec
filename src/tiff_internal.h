// SPDX-License-Identifier: Apache-2.0
#ifndef NIJINWVDC_TIFF_INTERNAL_H
#define NIJINWVDC_TIFF_INTERNAL_H

#include "../include/nijinWVDC.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace nijinwvdc {
namespace detail {

inline uint16_t rd_be16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

inline uint32_t rd_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

inline uint64_t rd_be64(const uint8_t* p) {
    return (static_cast<uint64_t>(rd_be32(p)) << 32) | static_cast<uint64_t>(rd_be32(p + 4));
}

inline uint16_t rd_le16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

inline uint32_t rd_le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

inline uint16_t rd_u16_e(const uint8_t* p, bool be) {
    return be ? rd_be16(p) : rd_le16(p);
}

inline uint32_t rd_u32_e(const uint8_t* p, bool be) {
    return be ? rd_be32(p) : rd_le32(p);
}

inline float rd_be_f32(const uint8_t* p) {
    uint32_t u = rd_be32(p);
    float f = 0.0f;
    std::memcpy(&f, &u, sizeof(f));
    return f;
}

inline double rd_be_f64(const uint8_t* p) {
    uint64_t u = rd_be64(p);
    double d = 0.0;
    std::memcpy(&d, &u, sizeof(d));
    return d;
}

inline std::string fourcc_to_str(const uint8_t* p) {
    return std::string(reinterpret_cast<const char*>(p), 4);
}

inline std::string rstrip_ascii(const uint8_t* p, size_t n) {
    size_t len = 0;
    while (len < n && p[len] != '\0') {
        ++len;
    }
    while (len > 0 && p[len - 1] == ' ') {
        --len;
    }
    return std::string(reinterpret_cast<const char*>(p), len);
}

#ifdef _WIN32
inline std::wstring utf8_to_wide(const std::string& utf8) {
    if (utf8.empty()) {
        return std::wstring();
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (n <= 0) {
        return std::wstring(utf8.begin(), utf8.end());
    }
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), &w[0], n);
    return w;
}

inline FILE* open_c_file_read(const std::string& path) {
    return _wfopen(utf8_to_wide(path).c_str(), L"rb");
}

inline FILE* open_c_file_write(const std::string& path) {
    return _wfopen(utf8_to_wide(path).c_str(), L"wb");
}
#else
inline FILE* open_c_file_read(const std::string& path) {
    return std::fopen(path.c_str(), "rb");
}

inline FILE* open_c_file_write(const std::string& path) {
    return std::fopen(path.c_str(), "wb");
}
#endif

// Stateless, thread-safe 64-bit random-access reader supporting file (>4 GiB), memory, and user callbacks.
class RandomAccessReader {
public:
    enum class Mode : uint8_t { None = 0, File = 1, Memory = 2, Callbacks = 3 };

    RandomAccessReader() = default;
    ~RandomAccessReader() { close(); }

    RandomAccessReader(RandomAccessReader&& other) noexcept {
        move_from(other);
    }
    RandomAccessReader& operator=(RandomAccessReader&& other) noexcept {
        if (this != &other) {
            close();
            move_from(other);
        }
        return *this;
    }
    RandomAccessReader(const RandomAccessReader&) = delete;
    RandomAccessReader& operator=(const RandomAccessReader&) = delete;

    void close() {
#ifdef _WIN32
        if (win_handle_ != INVALID_HANDLE_VALUE && win_handle_ != nullptr) {
            CloseHandle(static_cast<HANDLE>(win_handle_));
        }
        win_handle_ = INVALID_HANDLE_VALUE;
#else
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = -1;
#endif
        mode_ = Mode::None;
        size_ = 0;
        mem_ = nullptr;
        callbacks_ = IoCallbacks{};
    }

    bool open_file(const std::string& path, std::string& err) {
        close();
#ifdef _WIN32
        std::wstring wpath = utf8_to_wide(path);
        HANDLE h = CreateFileW(
            wpath.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
            nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            err = "Cannot open input file: " + path;
            return false;
        }
        LARGE_INTEGER li{};
        if (!GetFileSizeEx(h, &li) || li.QuadPart < 0) {
            CloseHandle(h);
            err = "Cannot determine size of input file: " + path;
            return false;
        }
        win_handle_ = h;
        size_ = static_cast<uint64_t>(li.QuadPart);
        mode_ = Mode::File;
        return true;
#else
        int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) {
            err = "Cannot open input file: " + path;
            return false;
        }
        struct stat st {};
        if (::fstat(fd, &st) != 0 || st.st_size < 0) {
            ::close(fd);
            err = "Cannot stat input file: " + path;
            return false;
        }
        fd_ = fd;
        size_ = static_cast<uint64_t>(st.st_size);
        mode_ = Mode::File;
        return true;
#endif
    }

    bool open_memory(const uint8_t* data, size_t size, std::string& err) {
        close();
        if (!data || size == 0) {
            err = "Empty memory buffer";
            return false;
        }
        mem_ = data;
        size_ = static_cast<uint64_t>(size);
        mode_ = Mode::Memory;
        return true;
    }

    bool open_callbacks(const IoCallbacks& cb, std::string& err) {
        close();
        if (!cb.read_at || cb.file_size == 0) {
            err = "Invalid IoCallbacks (read_at callback and non-zero file_size required)";
            return false;
        }
        callbacks_ = cb;
        size_ = cb.file_size;
        mode_ = Mode::Callbacks;
        return true;
    }

    bool is_open() const { return mode_ != Mode::None; }
    uint64_t size() const { return size_; }

    bool read(uint64_t off, void* dst, size_t n) const {
        if (n == 0) {
            return true;
        }
        if (!is_open() || !dst || off > size_ || n > size_ - off) {
            return false;
        }
        if (mode_ == Mode::Memory) {
            std::memcpy(dst, mem_ + static_cast<size_t>(off), n);
            return true;
        }
        if (mode_ == Mode::Callbacks) {
            size_t got = 0;
            return callbacks_.read_at(callbacks_.user_data, off, dst, n, &got) && got == n;
        }
        uint8_t* p = static_cast<uint8_t*>(dst);
        size_t rem = n;
        uint64_t cur = off;
#ifdef _WIN32
        HANDLE h = static_cast<HANDLE>(win_handle_);
        HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ev) {
            return false;
        }
        bool ok = true;
        while (rem > 0) {
            DWORD chunk = (rem > 0x40000000ULL) ? 0x40000000UL : static_cast<DWORD>(rem);
            OVERLAPPED ov{};
            ov.Offset = static_cast<DWORD>(cur & 0xFFFFFFFFULL);
            ov.OffsetHigh = static_cast<DWORD>(cur >> 32);
            ov.hEvent = ev;
            ResetEvent(ev);
            DWORD got = 0;
            BOOL r = ReadFile(h, p, chunk, &got, &ov);
            if (!r) {
                DWORD le = GetLastError();
                if (le != ERROR_IO_PENDING || !GetOverlappedResult(h, &ov, &got, TRUE)) {
                    ok = false;
                    break;
                }
            }
            if (got == 0) {
                ok = false;
                break;
            }
            p += got;
            cur += got;
            rem -= got;
        }
        CloseHandle(ev);
        return ok;
#else
        while (rem > 0) {
            ssize_t r = ::pread(fd_, p, rem, static_cast<off_t>(cur));
            if (r < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return false;
            }
            if (r == 0) {
                return false;
            }
            p += static_cast<size_t>(r);
            cur += static_cast<uint64_t>(r);
            rem -= static_cast<size_t>(r);
        }
        return true;
#endif
    }

private:
    void move_from(RandomAccessReader& other) {
        mode_ = other.mode_;
        size_ = other.size_;
        mem_ = other.mem_;
        callbacks_ = other.callbacks_;
#ifdef _WIN32
        win_handle_ = other.win_handle_;
        other.win_handle_ = INVALID_HANDLE_VALUE;
#else
        fd_ = other.fd_;
        other.fd_ = -1;
#endif
        other.mode_ = Mode::None;
        other.size_ = 0;
        other.mem_ = nullptr;
        other.callbacks_ = IoCallbacks{};
    }

    Mode mode_ = Mode::None;
    uint64_t size_ = 0;
#ifdef _WIN32
    void* win_handle_ = INVALID_HANDLE_VALUE;
#else
    int fd_ = -1;
#endif
    const uint8_t* mem_ = nullptr;
    IoCallbacks callbacks_{};
};

struct ScopedFile {
    FILE* fp = nullptr;
    explicit ScopedFile(const std::string& path) : fp(open_c_file_read(path)) {}
    ~ScopedFile() {
        if (fp) {
            std::fclose(fp);
        }
    }
    ScopedFile(const ScopedFile&) = delete;
    ScopedFile& operator=(const ScopedFile&) = delete;
};

inline bool seek_read(FILE* fp, uint64_t off, void* dst, size_t n) {
    if (n == 0) {
        return true;
    }
#ifdef _WIN32
    if (_fseeki64(fp, static_cast<__int64>(off), SEEK_SET) != 0) {
        return false;
    }
#else
    if (fseeko(fp, static_cast<off_t>(off), SEEK_SET) != 0) {
        return false;
    }
#endif
    return std::fread(dst, 1, n, fp) == n;
}

struct TiffTagValue {
    uint16_t tag = 0;
    uint16_t dtype = 0;
    uint32_t count = 0;
    uint64_t file_offset = 0;
    std::vector<uint8_t> data;
};

bool read_tiff_ifd(
    const RandomAccessReader& r,
    uint64_t tiff_base,
    bool be,
    uint32_t ifd_rel_off,
    std::vector<TiffTagValue>& out_tags,
    uint32_t* next_ifd_off = nullptr);

uint32_t tag_as_u32(const TiffTagValue& tv, bool be);
bool tag_as_rational(const TiffTagValue& tv, bool be, double& out);

bool parse_sony_arw_tiff_reader(
    const RandomAccessReader& r,
    bool be,
    uint32_t ifd0_next_off,
    const std::vector<TiffTagValue>& ifd0,
    const std::vector<uint32_t>& subifds,
    uint32_t exif_ifd_off,
    NevClipInfo& clip,
    NevFrameMeta& meta,
    std::string& err);

bool parse_nev_clip_reader(
    const RandomAccessReader& r,
    const std::string& display_path,
    NevClipInfo& clip,
    std::string& err);

bool read_nev_frame_meta_and_codestream_reader(
    const RandomAccessReader& r,
    const NevClipInfo& clip,
    uint32_t frame_index,
    NevFrameMeta& meta,
    std::vector<uint8_t>* codestream_out,
    std::string& err);

bool extract_blob_reader(
    const RandomAccessReader& r,
    uint64_t offset,
    uint32_t size,
    std::vector<uint8_t>& out,
    std::string& err);

} // namespace detail
} // namespace nijinwvdc

#endif // NIJINWVDC_TIFF_INTERNAL_H
