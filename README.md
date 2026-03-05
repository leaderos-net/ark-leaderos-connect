# LeaderOSConnect Plugin for ARK: Survival Evolved

This plugin allows you to connect your ARK: Survival Evolved server to LeaderOS, enabling you to send commands to the server through the LeaderOS platform.

## Requirements

This plugin requires the Ark:Server API extension to be installed.

## Installation

### 1. Download the plugin

Download the latest release as a ZIP file from the link below and extract it:

[https://www.leaderos.net/plugin/ark](https://www.leaderos.net/plugin/ark)

### 2. Upload the plugin

Copy the `LeaderosConnect` folder from the extracted ZIP into your server's AseApi plugins directory:

```
ShooterGame/Binaries/Win64/ArkApi/Plugins/LeaderosConnect/
```

The folder should contain:

```
LeaderosConnect/
└── config.json
├── LeaderosConnect.dll
└── leaderos_pending.json
└── PluginInfo.json
```

### 3. Configure the plugin

Open `config.json` and fill in your credentials:

```json
{
    "WebsiteURL":   "https://yourwebsite.com",
    "APIKey":       "YOUR_API_KEY_HERE",
    "ConnectToken": "YOUR_SERVER_TOKEN_HERE",
    "DebugMode":    false,
    "CheckOnline":  true,
    "FreqMinutes":  2
}
```

### 4. Restart your server

Restart your server. The plugin is now active. Run `leaderos.status` in the server console to confirm everything is working.

## Configuration

| Option | Description |
|---|---|
| `WebsiteURL` | The URL of your LeaderOS website (e.g., `https://yourwebsite.com`). Must start with `https://`. |
| `APIKey` | Your LeaderOS API key. Find it on `Dashboard > Settings > API`. |
| `ConnectToken` | Your server token. Find it on `Dashboard > Store > Servers > Your Server > Server Token`. |
| `DebugMode` | Set to `true` to enable verbose debug logging, or `false` to disable it. |
| `CheckOnline` | Set to `true` to queue commands for offline players and deliver them on next login. Set to `false` to execute commands regardless of whether the target player is online. |
| `FreqMinutes` | How often (in minutes) the plugin polls the command queue. |

> **Note on `CheckOnline`:** When set to `false`, commands targeting offline players will still attempt to execute — but if the server has no players connected at all, the command will be lost permanently since it has already been marked as executed on the LeaderOS side. It is recommended to keep this set to `true`.

## Console Commands

All commands are server-console only and cannot be run by in-game players.

| Command | Description |
|---|---|
| `leaderos.status` | Displays the current configuration and plugin state. |
| `leaderos.reload` | Reloads `config.json` from disk and triggers an immediate poll. |
| `leaderos.poll` | Triggers an immediate queue poll without reloading config. |
| `leaderos.debug` | Toggles debug mode on or off at runtime. |

## Building from Source

This project is designed to be "Plug & Play" without the need for complex environment variables.

1. Download or clone the ARK Server API repository into this workspace and ensure its folder is named `AseApi`.
2. Clone this `LeaderosConnect` repository into the same workspace folder, right next to `AseApi`. Your folder structure must look exactly like this:

   ```text
   YourWorkspace/
   ├── AseApi/
   │   ├── out_lib/
   │   └── version/Core/Public/
   └── LeaderosConnect/
       ├── include/
       ├── lib/
       ├── LeaderosConnect.slnx
       └── LeaderosConnect.vcxproj
   ```

3. Open `LeaderosConnect.slnx` in Visual Studio.
4. Set the build configuration to **Release** and platform to **x64** using the toolbar at the top.
5. Click **Build > Rebuild Solution**.
6. Copy the resulting `LeaderosConnect.dll` from the `x64\Release` folder to your server's plugin directory alongside `config.json`.