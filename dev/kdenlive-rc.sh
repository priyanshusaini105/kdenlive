#!/bin/sh
# Launch the remote-control Kdenlive build with its own data files and settings,
# so it never touches the stock install's kdenliverc or ~/.local/share/kdenlive.
P="$HOME/.local/opt/kdenlive-rc"
export XDG_DATA_DIRS="$P/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
export XDG_CONFIG_HOME="$P/home/config"
export XDG_DATA_HOME="$P/home/data"
export XDG_CACHE_HOME="$P/home/cache"
mkdir -p "$XDG_CONFIG_HOME" "$XDG_DATA_HOME" "$XDG_CACHE_HOME"
[ -d "$P/lib/qml" ] && export QML_IMPORT_PATH="$P/lib/qml${QML_IMPORT_PATH:+:$QML_IMPORT_PATH}"
[ -d "$P/lib/plugins" ] && export QT_PLUGIN_PATH="$P/lib/plugins${QT_PLUGIN_PATH:+:$QT_PLUGIN_PATH}"
exec "$P/bin/kdenlive" "$@"
