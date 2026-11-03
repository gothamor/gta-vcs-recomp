/* libc functions a PS5 title cannot use as the console has them, linked into the game in their place
 * (tools/ps5_link.py: --defsym=<name>=vcs_ps5_<name>, the names kept local).
 *
 * The payload SDK's stub libraries list what the console's modules export, not what a title may call: an import that
 * only libkernel_sys has (a system-only module) is left pointing at nothing when the title loads, and calling it jumps
 * to address 0. The first console run died that way at start-up in libc++'s std::filesystem::current_path(), which
 * calls pathconf (libkernel_sys only) and then getcwd (libc's asks __getcwd, also libkernel_sys only, and faults). Newer
 * revisions of the SDK fork have their own versions of some of these (ps5platform/libc.h); the fork ps5-plume-triangle
 * pins (2026-09-28) does not, so the game brings these. tools/ps5_link.py refuses a link that still imports anything
 * only libkernel_sys provides. */
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define VCS_PS5_HIDDEN __attribute__((visibility("hidden")))

/* The working directory the classic way: from "." up to the root, each directory named by the entry of its parent that
 * has its device and inode (lstat, so a mount point such as /app0 is crossed too). NULL buffer: malloc'd, at least
 * `size` bytes. opendir / readdir / closedir are the SDK platform's (the link redirects them). */
VCS_PS5_HIDDEN char *vcs_ps5_getcwd(char *buffer, size_t size)
{
    char path[PATH_MAX]; /* built from its end */
    size_t start = sizeof(path) - 1;
    path[start] = '\0';
    char up[PATH_MAX] = ".";
    struct stat here, parent;
    if (buffer && size == 0) {
        errno = EINVAL;
        return NULL;
    }
    if (stat(".", &here) != 0)
        return NULL;
    for (;;) {
        char above[PATH_MAX];
        if ((size_t)snprintf(above, sizeof(above), "%s/..", up) >= sizeof(above)) {
            errno = ENAMETOOLONG;
            return NULL;
        }
        if (stat(above, &parent) != 0)
            return NULL;
        if (parent.st_dev == here.st_dev && parent.st_ino == here.st_ino)
            break; /* the root is its own parent */
        DIR *dir = opendir(above);
        if (!dir)
            return NULL;
        int found = 0;
        struct dirent *entry;
        while (!found && (entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                continue;
            char candidate[PATH_MAX];
            struct stat st;
            if ((size_t)snprintf(candidate, sizeof(candidate), "%s/%s", above, entry->d_name) >= sizeof(candidate) ||
                lstat(candidate, &st) != 0 || st.st_dev != here.st_dev || st.st_ino != here.st_ino)
                continue;
            const size_t length = strlen(entry->d_name);
            if (length + 1 > start) {
                closedir(dir);
                errno = ENAMETOOLONG;
                return NULL;
            }
            start -= length;
            memcpy(path + start, entry->d_name, length);
            path[--start] = '/';
            found = 1;
        }
        closedir(dir);
        if (!found) {
            errno = ENOENT;
            return NULL;
        }
        here = parent;
        memcpy(up, above, strlen(above) + 1);
    }
    if (path[start] == '\0')
        path[--start] = '/'; /* the root itself */
    const size_t length = sizeof(path) - 1 - start;
    if (!buffer) {
        buffer = (char *)malloc(size > length ? size : length + 1);
        if (!buffer)
            return NULL;
    } else if (size <= length) {
        errno = ERANGE;
        return NULL;
    }
    memcpy(buffer, path + start, length + 1);
    return buffer;
}

/* The limits libc++ asks for (current_path: _PC_PATH_MAX), for a path that exists; others are not known (EINVAL). */
VCS_PS5_HIDDEN long vcs_ps5_pathconf(const char *path, int name)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return -1;
    switch (name) {
    case _PC_PATH_MAX:
        return PATH_MAX;
    case _PC_NAME_MAX:
        return NAME_MAX;
    default:
        errno = EINVAL;
        return -1;
    }
}

/* A title has no links: an existing path is no link (EINVAL), and making one is refused. */
VCS_PS5_HIDDEN ssize_t vcs_ps5_readlink(const char *path, char *buffer, size_t size)
{
    (void)buffer;
    (void)size;
    struct stat st;
    if (lstat(path, &st) != 0)
        return -1;
    errno = EINVAL;
    return -1;
}

VCS_PS5_HIDDEN int vcs_ps5_symlink(const char *target, const char *name)
{
    (void)target;
    (void)name;
    errno = EPERM;
    return -1;
}

VCS_PS5_HIDDEN int vcs_ps5_link(const char *existing, const char *name)
{
    (void)existing;
    (void)name;
    errno = EPERM;
    return -1;
}

/* A title starts no process. */
VCS_PS5_HIDDEN pid_t vcs_ps5_fork(void)
{
    errno = ENOSYS;
    return -1;
}
