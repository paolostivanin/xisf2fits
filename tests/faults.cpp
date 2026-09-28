// Test-only interposition at the actual publication boundary. This is never
// linked into or installed with the converter.
#include <cerrno>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {
bool mode(const char* name) {
    const auto* value = std::getenv("XISF2FITS_TEST_FAULT");
    return value && std::strcmp(value, name) == 0;
}
void marker(const char* path, const char* text, int flags) {
    if (!path) return;
    int fd = ::open(path, O_WRONLY | O_CREAT | flags, 0600);
    if (fd >= 0) { auto ignored = ::write(fd, text, std::strlen(text)); (void)ignored; ::close(fd); }
}
}
extern "C" int fsync(int fd) {
    auto real = reinterpret_cast<int (*)(int)>(dlsym(RTLD_NEXT, "fsync"));
    char proc[64], path[4096] = {};
    std::snprintf(proc, sizeof(proc), "/proc/self/fd/%d", fd);
    const auto n = ::readlink(proc, path, sizeof(path) - 1);
    const bool file = n > 0 && std::strstr(path, "/image.tmp");
    struct stat st{};
    const bool directory = ::fstat(fd, &st) == 0 && S_ISDIR(st.st_mode);
    marker(std::getenv("XISF2FITS_TEST_SYNC_LOG"), file ? "F" : (directory ? "D" : "?"), O_APPEND);
    if ((file && mode("file-sync-error")) || (directory && mode("directory-sync-error"))) { errno = EIO; return -1; }
    if (file && mode("interrupt")) ::raise(SIGTERM);
    if (file && mode("publish-race")) marker(std::getenv("XISF2FITS_TEST_DEST"), "race winner", O_EXCL);
    return real(fd);
}
extern "C" int link(const char* from, const char* to) {
    if (mode("no-hardlinks")) { errno = EOPNOTSUPP; return -1; }
    auto real = reinterpret_cast<int (*)(const char*, const char*)>(dlsym(RTLD_NEXT, "link"));
    return real(from, to);
}
extern "C" int linkat(int fromfd, const char* from, int tofd, const char* to, int flags) {
    if (mode("no-hardlinks")) { errno = EOPNOTSUPP; return -1; }
    auto real = reinterpret_cast<int (*)(int, const char*, int, const char*, int)>(dlsym(RTLD_NEXT, "linkat"));
    return real(fromfd, from, tofd, to, flags);
}
// Force the hard-link publication fallback and make removing the temporary
// link fail after the output is already published.
extern "C" long syscall(long number, ...) {
    va_list list;
    va_start(list, number);
    long args[6];
    for (auto& arg : args) arg = va_arg(list, long);
    va_end(list);
#ifdef SYS_renameat2
    if (number == SYS_renameat2 && mode("stale-temporary")) { errno = ENOSYS; return -1; }
#endif
    auto real = reinterpret_cast<long (*)(long, ...)>(dlsym(RTLD_NEXT, "syscall"));
    return real(number, args[0], args[1], args[2], args[3], args[4], args[5]);
}
extern "C" int remove(const char* path) {
    if (mode("stale-temporary") && std::strstr(path, "/image.tmp")) { errno = EBUSY; return -1; }
    auto real = reinterpret_cast<int (*)(const char*)>(dlsym(RTLD_NEXT, "remove"));
    return real(path);
}
extern "C" int unlink(const char* path) {
    if (mode("stale-temporary") && std::strstr(path, "/image.tmp")) { errno = EBUSY; return -1; }
    auto real = reinterpret_cast<int (*)(const char*)>(dlsym(RTLD_NEXT, "unlink"));
    return real(path);
}
