#!/bin/sh

# Periodically force the MX Keys backlight on by issuing OFF→ON within the CLI.

CLI="${MX_KEYS_CLI:-$HOME/.mx-keys-cli/bin/mx-keys-backlight}"

trap 'exit 0' INT TERM

while :; do
    if [ -x "$CLI" ]; then
        "$CLI" force-on >/dev/null 2>&1
        if [ $? -eq 0 ]; then
            sleep 1
        else
            sleep 2
        fi
    else
        sleep 5
    fi
done
