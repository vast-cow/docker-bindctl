# docker-bindctl

Add/remove bind mounts to an already-running Docker container on Linux, and keep a ledger of mounts added by this tool.

## Requirements

- Linux kernel 5.2+ (`open_tree(2)` / `move_mount(2)`)
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
- The destination is created if missing. Existing destination data is hidden while mounted and becomes visible again after removal.
- The helper uses `MNT_DETACH` for removal.
- This is a mount-namespace operation outside Docker's supported lifecycle/configuration model; use it primarily for debugging, development, emergency operations, or tooling where ephemeral mounts are intentional.
