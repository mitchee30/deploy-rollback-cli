#ifndef DEPLOYGUARD_FILE_LOCK_H
#define DEPLOYGUARD_FILE_LOCK_H

#include <optional>
#include <string>

namespace DeployGuard {

// ------------------------------------------------------------------------
// FileLock - an exclusive, non-blocking flock() on a lock file.
//
// Why deploy-cli needs it: crash recovery treats a leftover "INSTALLING"
// marker as proof that the previous run died. Without a lock, a second
// deploy-cli started while the first is still deploying sees the *live*
// run's marker, concludes it crashed, and rolls the target back underneath
// it. Holding this lock for the whole run makes the two cases distinguishable:
//
//   * flock() locks are released by the kernel when the holder exits for any
//     reason, including SIGKILL - so if we can take the lock, any leftover
//     marker really is from a dead process;
//   * if we cannot take it, another deploy-cli is alive and we refuse to run.
//
// Details that matter:
//   * The descriptor is opened with O_CLOEXEC. flock() locks belong to the
//     open file description, which a forked child shares; without CLOEXEC a
//     daemon started by the deploy script would keep the lock held forever.
//   * The lock file is never deleted. Unlinking it would let two processes
//     end up locking two different inodes with the same name.
//   * flock() is not reliable on some network file systems (e.g. NFS); keep
//     targets on a local disk.
// ------------------------------------------------------------------------
class FileLock {
public:
    // Creates the lock file if needed and tries to lock it without blocking.
    // On failure returns std::nullopt and fills *error (e.g. "held by pid 42").
    static std::optional<FileLock> tryAcquire(const std::string& path, std::string* error = nullptr);

    // Reports whether tryAcquire() would currently succeed, without creating
    // or modifying the lock file (used by --dry-run).
    static bool isAvailable(const std::string& path, std::string* error = nullptr);

    FileLock(FileLock&& other) noexcept;
    FileLock& operator=(FileLock&& other) noexcept;
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;
    ~FileLock();  // releases the lock

    const std::string& path() const { return path_; }

private:
    FileLock(int fd, std::string path);
    void release() noexcept;

    int fd_ = -1;
    std::string path_;
};

} // namespace DeployGuard

#endif // DEPLOYGUARD_FILE_LOCK_H
