#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/ioctl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/mount.h>
#include <unistd.h>

/* Keep building with older headers; these stable nsfs ioctls are required
   only by rootless mode (Linux 4.11+). */
#ifndef NS_GET_USERNS
#define NS_GET_USERNS _IO(0xb7, 0x1)
#define NS_GET_PARENT _IO(0xb7, 0x2)
#define NS_GET_OWNER_UID _IO(0xb7, 0x4)
#endif

static int rootless;

#ifndef OPEN_TREE_CLONE
#define OPEN_TREE_CLONE 1
#endif
#ifndef OPEN_TREE_CLOEXEC
#define OPEN_TREE_CLOEXEC O_CLOEXEC
#endif
#ifndef MOVE_MOUNT_F_EMPTY_PATH
#define MOVE_MOUNT_F_EMPTY_PATH 0x00000004
#endif
#ifndef AT_RECURSIVE
#define AT_RECURSIVE 0x8000
#endif

static int xopen_tree(int dfd, const char *path, unsigned flags) {
#ifdef SYS_open_tree
    return (int)syscall(SYS_open_tree, dfd, path, flags);
#else
    (void)dfd; (void)path; (void)flags;
    errno = ENOSYS;
    return -1;
#endif
}
static int xmove_mount(int from_dfd, const char *from_path,
                       int to_dfd, const char *to_path, unsigned flags) {
#ifdef SYS_move_mount
    return (int)syscall(SYS_move_mount, from_dfd, from_path,
                        to_dfd, to_path, flags);
#else
    (void)from_dfd; (void)from_path; (void)to_dfd; (void)to_path; (void)flags;
    errno = ENOSYS;
    return -1;
#endif
}

static void die(const char *what) {
    fprintf(stderr, "docker-bindctl: %s: %s\n", what, strerror(errno));
    exit(1);
}

static void usage(const char *argv0) {
    fprintf(stderr,
        "usage:\n"
        "  %s [--rootless] add <pid> <host-source> <container-target>\n"
        "  %s [--rootless] remove <pid> <container-target>\n", argv0, argv0);
    exit(2);
}

static int valid_target(const char *p) {
    if (!p || p[0] != '/' || p[strspn(p, "/")] == '\0') return 0;
    for (const char *part = p; *part;) {
        part += strspn(part, "/");
        size_t n = strcspn(part, "/");
        if ((n == 1 && part[0] == '.') ||
            (n == 2 && part[0] == '.' && part[1] == '.')) return 0;
        part += n;
    }
    return 1;
}

static void mkdir_p(const char *path, mode_t mode) {
    char *tmp = strdup(path);
    if (!tmp) die("strdup");
    size_t n = strlen(tmp);
    while (n > 1 && tmp[n-1] == '/') tmp[--n] = '\0';

    for (char *p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, mode) < 0 && errno != EEXIST) die("mkdir");
            *p = '/';
        }
    }
    if (mkdir(tmp, mode) < 0 && errno != EEXIST) die("mkdir");
    free(tmp);
}

static void ensure_target(const char *dst, mode_t src_mode) {
    struct stat st;
    if (lstat(dst, &st) == 0) {
        if ((src_mode & S_IFMT) != (st.st_mode & S_IFMT)) {
            errno = EINVAL;
            die("target type differs from source");
        }
        return;
    }
    if (errno != ENOENT) die("lstat target");

    char *parent = strdup(dst);
    if (!parent) die("strdup");
    char *slash = strrchr(parent, '/');
    if (!slash) { free(parent); errno = EINVAL; die("invalid target"); }
    if (slash == parent) parent[1] = '\0'; else *slash = '\0';
    mkdir_p(parent, 0755);
    free(parent);

    if (S_ISDIR(src_mode)) {
        if (mkdir(dst, 0755) < 0 && errno != EEXIST) die("mkdir target");
    } else if (S_ISREG(src_mode)) {
        int fd = open(dst, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0644);
        if (fd < 0 && errno != EEXIST) die("create target file");
        if (fd >= 0) close(fd);
    } else {
        errno = ENOTSUP;
        die("source type unsupported (only regular files/directories)");
    }
}

static int same_object(int a, int b) {
    struct stat sa, sb;
    if (fstat(a, &sa) < 0 || fstat(b, &sb) < 0) die("fstat pinned object");
    return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

static void enter_rootless_userns(int nsfd) {
    int userfd = ioctl(nsfd, NS_GET_USERNS);
    if (userfd < 0) die("NS_GET_USERNS (rootless requires Linux 4.11+)");
    int selffd = open("/proc/self/ns/user", O_RDONLY | O_CLOEXEC);
    if (selffd < 0) die("open caller user namespace");
    int parentfd = ioctl(userfd, NS_GET_PARENT);
    if (parentfd < 0) die("rootless mount namespace must belong to a child user namespace");
    uid_t owner;
    if (ioctl(userfd, NS_GET_OWNER_UID, &owner) < 0) die("NS_GET_OWNER_UID");
    /* Standard RootlessKit creates a direct child of the host user namespace.
       Do not enter the initial namespace, a sibling, or another user's child.
       Container processes may run under subordinate UIDs, so checking the
       process's UID instead would incorrectly reject legitimate containers. */
    if (!same_object(parentfd, selffd) || owner != geteuid()) {
        errno = EPERM;
        die("rootless mount namespace is not owned by the current user");
    }
    close(parentfd);
    close(selffd);
    if (setns(userfd, CLONE_NEWUSER) < 0) die("setns(CLONE_NEWUSER)");
    close(userfd);
}

struct container_ns { int procfd, nsfd; };

static struct container_ns open_container(pid_t pid) {
    char p[128];
    snprintf(p, sizeof(p), "/proc/%d", pid);
    /* Pin the process directory so PID reuse cannot mix namespace/root FDs. */
    struct container_ns ns;
    ns.procfd = open(p, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (ns.procfd < 0) die("open container process");
    ns.nsfd = openat(ns.procfd, "ns/mnt", O_RDONLY | O_CLOEXEC);
    if (ns.nsfd < 0) die("open mount namespace");
    if (rootless) enter_rootless_userns(ns.nsfd);
    return ns;
}

static void enter_container(struct container_ns ns) {
    /* Subordinate-UID containers require the newly acquired capabilities. */
    int rootfd = openat(ns.procfd, "root", O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (rootfd < 0) die("open container root");
    close(ns.procfd);
    if (setns(ns.nsfd, CLONE_NEWNS) < 0) die("setns(CLONE_NEWNS)");
    close(ns.nsfd);
    if (fchdir(rootfd) < 0) die("fchdir container root");
    if (chroot(".") < 0) die("chroot container root");
    if (chdir("/") < 0) die("chdir /");
    close(rootfd);
}

static int rootless_source_tree(const char *source, int pinnedfd, unsigned flags) {
    /* We now have capabilities in the container mount namespace's owner,
       but not in the host mount namespace's owner. A private copy is owned
       by our new user namespace, so open_tree(CLONE) is permitted there.
       A direct bind of the original FD after setns would fail: Linux forbids
       cloning a source mount belonging to a different mount namespace. */
    if (unshare(CLONE_NEWNS) < 0) die("unshare rootless staging mount namespace");
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0)
        die("make staging mounts private");
    /* An FD still references the original host mount. Resolve the canonical
       host path in its copied mount tree, and verify against the pinned inode
       before cloning. Fail closed if the host source was replaced. */
    int copiedfd = open(source, O_PATH | O_CLOEXEC);
    if (copiedfd < 0) die("open source in staging namespace");
    if (!same_object(pinnedfd, copiedfd)) {
        errno = ESTALE;
        die("host source changed while preparing rootless mount");
    }
    int treefd = xopen_tree(copiedfd, "", flags);
    if (treefd < 0) die("open_tree rootless source (requires Linux 5.2+)");
    close(copiedfd);
    return treefd;
}

static void do_add(pid_t pid, const char *src, const char *dst) {
    if (!valid_target(dst)) { errno = EINVAL; die("invalid container target"); }

    char *source = realpath(src, NULL);
    if (!source) die("realpath source");
    int sourcefd = open(source, O_PATH | O_CLOEXEC);
    if (sourcefd < 0) die("open source");
    struct stat st;
    if (fstat(sourcefd, &st) < 0) die("fstat source");
    if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)) {
        errno = ENOTSUP;
        die("source type unsupported (only regular files/directories)");
    }
    /* Pin host procfs as well as the exact source, allowing its magic FD link
       to be resolved even after entering a container with no /proc mounted.
       Unlike a retained parent + basename, this survives source replacement. */
    int source_procfd = open("/proc/self/fd", O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (source_procfd < 0) die("open host FD directory");
    char relative[64];
    snprintf(relative, sizeof(relative), "./%d", sourcefd);

    unsigned flags = OPEN_TREE_CLONE | OPEN_TREE_CLOEXEC | AT_EMPTY_PATH;
    if (S_ISDIR(st.st_mode)) flags |= AT_RECURSIVE;
    int treefd;
    struct container_ns ns;
    if (rootless) {
        ns = open_container(pid);
        treefd = rootless_source_tree(source, sourcefd, flags);
    } else {
        treefd = xopen_tree(sourcefd, "", flags);
        if (treefd < 0 && errno != ENOSYS) die("open_tree source");
        ns = open_container(pid);
    }
    free(source);
    enter_container(ns);
    ensure_target(dst, st.st_mode);

    if (treefd >= 0) {
        if (xmove_mount(treefd, "", AT_FDCWD, dst, MOVE_MOUNT_F_EMPTY_PATH) == 0) {
            close(treefd);
            close(sourcefd);
            close(source_procfd);
            return;
        }
        if (rootless || errno != ENOSYS) die("move_mount target");
        close(treefd);
    }

    if (fchdir(source_procfd) < 0) die("fchdir host FD directory");
    unsigned long bind_flags = MS_BIND;
    if (S_ISDIR(st.st_mode)) bind_flags |= MS_REC;
    if (mount(relative, dst, NULL, bind_flags, NULL) < 0) die("bind mount target");
    if (chdir("/") < 0) die("chdir /");
    close(sourcefd);
    close(source_procfd);
}

static void do_remove(pid_t pid, const char *dst) {
    if (!valid_target(dst)) { errno = EINVAL; die("invalid container target"); }
    enter_container(open_container(pid));
    if (umount2(dst, MNT_DETACH) < 0) die("umount2 target");
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--rootless") == 0) {
        rootless = 1;
        argc--; argv++;
    }
    if (geteuid() != getuid() || getegid() != getgid()) {
        fprintf(stderr, "docker-bindctl: setuid/setgid helper execution is unsupported\n");
        return 1;
    }
    if (!rootless && geteuid() != 0) {
        fprintf(stderr, "docker-bindctl: must run as root\n");
        return 1;
    }
    if (argc < 2) usage(argv[0]);
    if (strcmp(argv[1], "add") == 0) {
        if (argc != 5) usage(argv[0]);
        char *end = NULL;
        long pid = strtol(argv[2], &end, 10);
        if (!end || *end || pid <= 0 || pid > INT_MAX) usage(argv[0]);
        do_add((pid_t)pid, argv[3], argv[4]);
        return 0;
    }
    if (strcmp(argv[1], "remove") == 0) {
        if (argc != 4) usage(argv[0]);
        char *end = NULL;
        long pid = strtol(argv[2], &end, 10);
        if (!end || *end || pid <= 0 || pid > INT_MAX) usage(argv[0]);
        do_remove((pid_t)pid, argv[3]);
        return 0;
    }
    usage(argv[0]);
    return 2;
}
