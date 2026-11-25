#pragma once
#include <string>

namespace taz {

// F10: an exclusive, advisory lock for the lifetime of one process on one chain file.
//
// A node loads the chain, keeps it in memory and rewrites the whole file after every change, so two nodes on the same
// file silently overwrite each other's blocks (last writer wins). The lock is taken on "<chain file>.lock", not on the
// chain file itself, because the chain file is replaced by rename on every save. It is released by the operating
// system when the process ends, however it ends, so a crash never leaves a stale lock behind.
//   POSIX:   flock(LOCK_EX | LOCK_NB)      Windows: CreateFile with no sharing
// The lock is advisory: it protects against two taznode processes, not against a person editing the file.
class ChainFileLock {
public:
    ChainFileLock() = default;
    ChainFileLock(const ChainFileLock&) = delete;
    ChainFileLock& operator=(const ChainFileLock&) = delete;
    ~ChainFileLock();
    // True when this object now holds the lock for chain_file. False (with *error) when another process holds it or
    // the lock file cannot be created. Calling it again on a held lock is an error and returns false.
    bool acquire(const std::string& chain_file, std::string* error = nullptr);
    bool held() const noexcept { return handle_ != kNone; }
private:
#ifdef _WIN32
    static constexpr long long kNone = -1;
#else
    static constexpr int kNone = -1;
#endif
#ifdef _WIN32
    long long handle_ = kNone;
#else
    int handle_ = kNone;
#endif
};

}  // namespace taz
