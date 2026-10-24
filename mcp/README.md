# kdenlive-mcp

MCP server that drives a running Kdenlive (this fork's remote-control build) over D-Bus.

## Run

1. Build and install the fork, then start it:
   ```sh
   cmake -S . -B ../build -G Ninja -DCMAKE_BUILD_TYPE=Release \
     -DCMAKE_INSTALL_PREFIX=$HOME/.local/opt/kdenlive-rc -DKDE_INSTALL_USE_QT_SYS_PATHS=OFF
   ninja -C ../build -j4 kdenlive kdenlive_render mltpreview && cmake --install ../build
   systemd-run --user --unit=kdenlive-rc --collect --setenv=DISPLAY=$DISPLAY \
     --setenv=WAYLAND_DISPLAY=$WAYLAND_DISPLAY --setenv=DBUS_SESSION_BUS_ADDRESS=$DBUS_SESSION_BUS_ADDRESS \
     $PWD/dev/kdenlive-rc.sh        # or run dev/kdenlive-rc.sh from a terminal
   ```
2. Register with Claude Code:
   ```sh
   claude mcp add kdenlive -- uv --directory /path/to/fork/mcp run kdenlive-mcp
   ```

With several remote-control instances running, set `KDENLIVE_PID`.

## Test

```sh
uv run pytest -q              # unit tests
uv run python tests/e2e_live.py   # against the running fork
```
