#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/mount.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/mount.h>
#include <unistd.h>

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
    return (int)syscall(SYS_open_tree, dfd, path, flags);
}
static int xmove_mount(int from_dfd, const char *from_path,
                       int to_dfd, const char *to_path, unsigned flags) {
    return (int)syscall(SYS_move_mount, from_dfd, from_path,
                        to_dfd, to_path, flags);
}

static void die(const char *what) {
    fprintf(stderr, "docker-bindctl: %s: %s\n", what, strerror(errno));
    exit(1);
}

static void usage(const char *argv0) {
    fprintf(stderr,
        "usage:\n"
        "  %s add <pid> <host-source> <container-target>\n"
        "  %s remove <pid> <container-target>\n", argv0, argv0);
    exit(2);
}

static int valid_target(const char *p) {
    return p && p[0] == '/' && strcmp(p, "/") != 0 && strstr(p, "/../") == NULL &&
           strcmp(p, "/..") != 0 && (strlen(p) < 3 || strcmp(p + strlen(p) - 3, "/..") != 0);
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
        if (S_ISDIR(src_mode) != S_ISDIR(st.st_mode)) {
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

static void enter_container_mountns(pid_t pid) {
    char p[128];
    snprintf(p, sizeof(p), "/proc/%d/ns/mnt", pid);
    int nsfd = open(p, O_RDONLY | O_CLOEXEC);
    if (nsfd < 0) die("open mount namespace");

    snprintf(p, sizeof(p), "/proc/%d/root", pid);
    int rootfd = open(p, O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (rootfd < 0) die("open container root");

    if (setns(nsfd, CLONE_NEWNS) < 0) die("setns(CLONE_NEWNS)");
    close(nsfd);

    if (fchdir(rootfd) < 0) die("fchdir container root");
    if (chroot(".") < 0) die("chroot container root");
    if (chdir("/") < 0) die("chdir /");
    close(rootfd);
}

static void do_add(pid_t pid, const char *src, const char *dst) {
    if (!valid_target(dst)) { errno = EINVAL; die("invalid container target"); }

    struct stat st;
    if (stat(src, &st) < 0) die("stat source");
    if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)) {
        errno = ENOTSUP;
        die("source type unsupported (only regular files/directories)");
    }

    unsigned flags = OPEN_TREE_CLONE | OPEN_TREE_CLOEXEC;
    if (S_ISDIR(st.st_mode)) flags |= AT_RECURSIVE;
    int treefd = xopen_tree(AT_FDCWD, src, flags);
    if (treefd < 0) die("open_tree source (requires Linux 5.2+ and CAP_SYS_ADMIN)");

    enter_container_mountns(pid);
    ensure_target(dst, st.st_mode);

    if (xmove_mount(treefd, "", AT_FDCWD, dst, MOVE_MOUNT_F_EMPTY_PATH) < 0)
        die("move_mount target");
    close(treefd);
}

static void do_remove(pid_t pid, const char *dst) {
    if (!valid_target(dst)) { errno = EINVAL; die("invalid container target"); }
    enter_container_mountns(pid);
    if (umount2(dst, MNT_DETACH) < 0) die("umount2 target");
}

int main(int argc, char **argv) {
    if (geteuid() != 0) {
        fprintf(stderr, "docker-bindctl: must run as root\n");
        return 1;
    }
    if (argc < 2) usage(argv[0]);
    if (strcmp(argv[1], "add") == 0) {
        if (argc != 5) usage(argv[0]);
        char *end = NULL;
        long pid = strtol(argv[2], &end, 10);
        if (!end || *end || pid <= 0) usage(argv[0]);
        do_add((pid_t)pid, argv[3], argv[4]);
        return 0;
    }
    if (strcmp(argv[1], "remove") == 0) {
        if (argc != 4) usage(argv[0]);
        char *end = NULL;
        long pid = strtol(argv[2], &end, 10);
        if (!end || *end || pid <= 0) usage(argv[0]);
        do_remove((pid_t)pid, argv[3]);
        return 0;
    }
    usage(argv[0]);
}
