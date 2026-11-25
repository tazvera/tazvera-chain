#include "taz/filelock.hpp"
#include <cerrno>
#include <cstring>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <sys/file.h>
#  include <unistd.h>
#endif

namespace taz {

ChainFileLock::~ChainFileLock() {
    if (handle_ == kNone) return;
#ifdef _WIN32
    CloseHandle(reinterpret_cast<HANDLE>(handle_));
#else
    ::close(handle_);                                  // closing the descriptor releases the flock
#endif
    handle_ = kNone;
}

bool ChainFileLock::acquire(const std::string& chain_file, std::string* error) {
    auto fail = [&](const std::string& why) { if (error) *error = why; return false; };
    if (handle_ != kNone) return fail("this lock is already held");
    const std::string path = chain_file + ".lock";
#ifdef _WIN32
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0 /* no sharing: a second open fails */, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        if (e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION)
            return fail(chain_file + " is in use by another taznode process (" + path + " is locked)");
        return fail("cannot create the lock file " + path + " (error " + std::to_string(e) + ")");
    }
    handle_ = reinterpret_cast<long long>(h);
#else
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return fail("cannot create the lock file " + path + ": " + std::strerror(errno));
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        const int e = errno;
        ::close(fd);
        if (e == EWOULDBLOCK) return fail(chain_file + " is in use by another taznode process (" + path + " is locked)");
        return fail("cannot lock " + path + ": " + std::strerror(e));
    }
    handle_ = fd;
#endif
    return true;
}

}  // namespace taz
