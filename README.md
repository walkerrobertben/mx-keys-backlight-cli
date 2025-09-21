### MX Keys Backlight CLI / Always-On Mode (macOS)

Small CLI for toggling the backlight on Logitech MX Keys keyboards via HID++. Includes an optional “always on” LaunchAgent that keeps the backlight on by sending fast OFF→ON cycles inside a single command.

### Requirements

- macOS
- hidapi (Homebrew is the suggested method of installation)

Install dependencies:

```bash
brew install hidapi
```

### Build and install

Installs to `~/.mx-keys-cli/bin`

```bash
# from repo root
make install
```

CLI will be at `~/.mx-keys-cli/bin/mx-keys-backlight`.

### Usage

```bash
# turn backlight on (default auto mode that uses proximity/keypress detection)
~/.mx-keys-cli/bin/mx-keys-backlight on

# turn backlight off
~/.mx-keys-cli/bin/mx-keys-backlight off

# sends OFF and ON commands in quick succession
~/.mx-keys-cli/bin/mx-keys-backlight force-on
```

### Always-on service (LaunchAgent)

Installs a per-user LaunchAgent that runs on login and restarts automatically if it exits.

```bash
# from repo root
make install-alwayson-service
```

What it does:
- Installs script to `~/Library/Application Support/mx-keys-backlight-cli/mx-keys-always-on.sh`
- Installs LaunchAgent to `~/Library/LaunchAgents/com.walkerrobertben.mxkeys.alwayson.plist`
- Starts it immediately; it will also start automatically on future logins

Uninstall:

```bash
# from repo root
make uninstall-alwayson-service
```

Remove the CLI (optional):

```bash
# from repo root
make uninstall
```

### Environment overrides (optional)

This tool is designed to use the Logitech Unifying receiver. If your receiver uses a different VID/PID, set these and re-run the CLI or service:

```bash
export MX_KEYS_RECEIVER_VID=0x046D
export MX_KEYS_RECEIVER_PID=0xC52B
```
