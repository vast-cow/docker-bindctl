# docker-bindctl

Add/remove bind mounts to an already-running Docker container on Linux, and keep a ledger of mounts added by this tool.

## Requirements

- Linux kernel 3.8+ (mount namespace support in `setns(2)`)
- Docker Engine on Linux
- root / `CAP_SYS_ADMIN`
- Python 3
- C compiler at build time
- Normal rootful Docker containers. Rootless Docker / unusual user namespaces are not supported by this MVP.

This does **not** change Docker's container configuration. The mount is injected directly into the running container's mount namespace, so `docker inspect .Mounts` does not learn about it and the mount disappears when the container is restarted/recreated. The tool stores its own ledger at `/var/lib/docker-bindctl/state.json`.

## Install

```sh
make
sudo make install
```

## Usage

```sh
sudo docker-bindctl add <container> <host-source> <container-target>
sudo docker-bindctl remove <container> <container-target>
docker-bindctl list [container]
docker-bindctl list --json
sudo docker-bindctl prune
```

Example:

```sh
docker run -d --name demo nginx:alpine
mkdir -p /tmp/live-config
echo hello > /tmp/live-config/example.txt

sudo docker-bindctl add demo /tmp/live-config /opt/live-config
docker exec demo cat /opt/live-config/example.txt

docker-bindctl list
sudo docker-bindctl remove demo /opt/live-config
```

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
- On Linux 5.2+, the helper uses `open_tree(2)` / `move_mount(2)`.
  If either syscall returns `ENOSYS`, it falls back to traditional
  `mount(2)` with `MS_BIND` (and `MS_REC` for directories), supporting Linux
  3.8–5.1. Other errors, including permission errors, are reported without
  falling back. Builds with older headers lacking these syscall numbers
  use the legacy path directly.
- The legacy path retains a host source parent-directory FD before entering
  the container namespace and chroot, then mounts a relative source path.
  It resolves source symlinks on the host, but the source basename can still
  be replaced between lookup and mount. Avoid concurrent changes to the
  source path while adding a mount.
- The destination is created if missing. Existing destination data is hidden while mounted and becomes visible again after removal.
- The helper uses `MNT_DETACH` for removal.
- This is a mount-namespace operation outside Docker's supported lifecycle/configuration model; use it primarily for debugging, development, emergency operations, or tooling where ephemeral mounts are intentional.

Kernel API references: [setns(2)](https://man7.org/linux/man-pages/man2/setns.2.html),
[open_tree(2)](https://man7.org/linux/man-pages/man2/open_tree.2.html),
[move_mount(2)](https://man7.org/linux/man-pages/man2/move_mount.2.html),
and [mount(2)](https://man7.org/linux/man-pages/man2/mount.2.html).
