# docker-bindctl

Add/remove bind mounts to an already-running Docker container on Linux, and keep a ledger of mounts added by this tool.

## Requirements

- Linux kernel 3.8+ (mount namespace support in `setns(2)`)
- Docker Engine on Linux
- Rootful Docker: run mutations as root
- Standard Linux Rootless Docker: run as the daemon owner without sudo; Linux 5.2+ with user namespaces enabled
- Python 3
- C compiler at build time
- Local Docker daemon connected through a Unix socket; remote SSH/TCP contexts are unsupported

This does **not** change Docker's container configuration. The mount is injected directly into the running container's mount namespace, so `docker inspect .Mounts` does not learn about it and the mount disappears when the container is restarted/recreated. The tool stores its own ledger. Rootful operation uses
`/var/lib/docker-bindctl/state.json`; rootless operation uses
`$XDG_STATE_HOME/docker-bindctl/state.json`, falling back to
`~/.local/state/docker-bindctl/state.json`. `DOCKER_BINDCTL_STATE_DIR` overrides
the directory in either mode. State directories are owned by the invoking user
and restricted to mode `0700`; state files use `0600`. Use sudo for rootful
`list` when reading this restricted ledger.

## Install

```sh
make
sudo make install
```

## Rootful usage

```sh
sudo docker-bindctl add <container> <host-source> <container-target>
sudo docker-bindctl remove <container> <container-target>
sudo docker-bindctl list [container]
sudo docker-bindctl list --json
sudo docker-bindctl prune
```

Example:

```sh
docker run -d --name demo nginx:alpine
mkdir -p /tmp/live-config
echo hello > /tmp/live-config/example.txt

sudo docker-bindctl add demo /tmp/live-config /opt/live-config
docker exec demo cat /opt/live-config/example.txt

sudo docker-bindctl list
sudo docker-bindctl remove demo /opt/live-config
```

## Rootless usage

Use the same commands **without sudo**, as the user who owns the daemon:

```sh
docker context use rootless
docker run -d --name demo nginx:alpine
mkdir -p "$HOME/live-config"
echo hello > "$HOME/live-config/example.txt"

docker-bindctl add demo "$HOME/live-config" /opt/live-config
docker exec demo cat /opt/live-config/example.txt # prints hello
docker-bindctl list
docker-bindctl remove demo /opt/live-config
docker-bindctl prune
```

`DOCKER_HOST=unix://$XDG_RUNTIME_DIR/docker.sock` also works. The CLI honors
Docker's selected context, `DOCKER_HOST`, and `DOCKER_CONTEXT` (which takes
precedence over `DOCKER_HOST`). It detects rootless mode using the connected
daemon's `SecurityOptions`, then pins the selected endpoint for subsequent
Docker commands. Only local Unix-socket endpoints are accepted because container
PIDs must correspond to the local host's `/proc`. Custom RootlessKit PID namespace
configurations, nested user namespaces, and other nonstandard arrangements are
unsupported; the standard `dockerd-rootless.sh` arrangement shares the host PID
namespace outside the containers.

Host permissions and UID/GID mappings still apply. Normally the daemon owner's
host UID maps to UID 0 inside rootless containers, while other container UIDs/GIDs
map to subordinate host IDs. A container running as a nonzero UID may therefore
need group/world permissions or an appropriate host ACL to read a source owned
by the daemon user. Unmapped IDs may appear as overflow IDs. The tool does not
change ownership, create idmapped mounts, or bypass host filesystem permissions.

`list` status values:

- `active`: tracked PID still matches and target appears in that process's mountinfo.
- `stale(restarted)`: container restarted; the injected mount is gone.
- `stale`: same PID but target is no longer mounted.
- `stopped`: container exists but is stopped.
- `gone`: original container no longer exists.

`remove` forgets stale entries without trying to unmount them. `prune` forgets all non-active entries.

## Notes / limitations

- Only regular files and directories are accepted as sources.
- Directory binds are recursive (submounts are included).
- For rootful Docker on Linux 5.2+, the helper uses `open_tree(2)` / `move_mount(2)`.
  If either syscall returns `ENOSYS`, it falls back to traditional
  `mount(2)` with `MS_BIND` (and `MS_REC` for directories), supporting Linux
  3.8–5.1. Other errors, including permission errors, are reported without
  falling back. Builds with older headers lacking these syscall numbers
  use the legacy path directly.
- The helper pins the exact source with an `O_PATH` FD and validates its type
  before changing namespaces. The rootful legacy fallback retains a host
  `/proc/self/fd` directory FD to resolve that source after chroot, including
  when the container has no procfs mounted. Source symlinks resolve on the host.
- Rootless add requires Linux 5.2+ and headers exposing `open_tree` and
  `move_mount` syscall numbers. It does not use the rootful legacy fallback.
  The helper obtains the mount namespace's owner with `NS_GET_USERNS`, checks
  `NS_GET_PARENT` and `NS_GET_OWNER_UID`, and accepts only a direct child user
  namespace owned by the caller. These nsfs ioctls require Linux 4.11+.
- After joining that owning user namespace, the single-threaded helper has
  capabilities there. It copies the host mount namespace using `unshare`, makes
  the copy private, reopens the canonical source in the copied mount tree and
  verifies its device/inode against the pinned source, then uses
  `open_tree(OPEN_TREE_CLONE)`. It enters the container mount namespace and
  chroots before attaching the detached mount with `move_mount`. Replaced sources
  are rejected. No RootlessKit PID file is needed, and no capabilities in the
  host's initial user namespace are required.
- Cloning before user-namespace entry would require host privileges. A direct
  `MS_BIND` using the host FD after entering the container also fails because
  Linux rejects cloning mounts from another mount namespace. Private staging
  avoids both constraints. Permission errors never trigger syscall fallbacks.
- Install the helper as a normal executable, never setuid-root or with host
  capability grants. The helper rejects setuid/setgid execution. Kernel checks
  and the namespace-owner validation prevent another unprivileged user from
  entering an unrelated user's namespace.
- The destination is created if missing. Existing destination data is hidden while mounted and becomes visible again after removal.
- The helper uses `MNT_DETACH` for removal.
- This is a mount-namespace operation outside Docker's supported lifecycle/configuration model; use it primarily for debugging, development, emergency operations, or tooling where ephemeral mounts are intentional.

## Testing

```sh
make
make test
```

Tests cover rootful modern/legacy syscall paths (including old headers and
permission failures), rootless staging and namespace ownership rejection,
mode/context detection, state selection and permissions, path validation, and
stale bookkeeping. Privileged syscalls are mocked in the C unit tests.

On a Linux host with Rootless Docker installed, run:

```sh
docker context use rootless
bash tests/integration.sh
# Alternatively select the daemon using DOCKER_HOST (unset DOCKER_CONTEXT first).
DOCKER_HOST="unix://$XDG_RUNTIME_DIR/docker.sock" bash tests/integration.sh
```

The script uses a disposable Alpine container and a temporary source under your
home directory, verifies directory/file binds, `docker exec` contents, listing,
removal, restart and recreation bookkeeping, and cleans up its fixtures. It sets
`DOCKER_BINDCTL_HELPER` to the built helper and overrides the state directory to
avoid changing your normal ledger. For rootful integration, run it as root with
the intended local rootful daemon selected, for example
`sudo env DOCKER_HOST=unix:///var/run/docker.sock bash tests/integration.sh`.

Kernel API references: [setns(2)](https://man7.org/linux/man-pages/man2/setns.2.html),
[open_tree(2)](https://man7.org/linux/man-pages/man2/open_tree.2.html),
[move_mount(2)](https://man7.org/linux/man-pages/man2/move_mount.2.html),
[ioctl_nsfs(2)](https://man7.org/linux/man-pages/man2/ioctl_nsfs.2.html),
and [mount(2)](https://man7.org/linux/man-pages/man2/mount.2.html).
