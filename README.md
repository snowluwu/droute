# Discord Droute (Native Proxy for Discord)

<p align="center">
  <img src="external/droute.svg" alt="Droute Logo" width="128">
</p>

<p align="center">
  <a href="https://github.com/snowluwu/droute/releases"><img src="https://img.shields.io/github/v/release/snowluwu/droute?style=for-the-badge&label=Release&labelColor=3A3A3A&color=5662f6" alt="Release"></a>
</p>

**Droute** is a tool that adds SOCKS5 proxy support directly into the Discord Windows client. It fixes the lack of built-in proxy settings in Discord, so you don't have to mess with TUN interfaces or full-system VPNs.

The project is inspired by the [force-proxy](https://github.com/runetfreedom/force-proxy) concept and uses the [MinHook](https://github.com/tsudakageyu/minhook) library.

> [!NOTE]
> **Droute is not a standalone tool for bypassing censorship or blocks.** 
> It requires an existing SOCKS5 proxy to function. You must provide your own proxy server or run a local client like **FlClash, v2rayN, Nekobox, or Amnezia**. Droute only handles the routing part inside Discord.

## Screenshots
<details>
<summary>Installer previews</summary>
<br>
<img width="451" height="415" alt="image" src="https://github.com/user-attachments/assets/11509cf4-1a11-4744-92e6-d340c49de6cd" />
<br>
<img width="581" height="449" alt="image" src="https://github.com/user-attachments/assets/9cc70f54-c621-4ebd-8931-15ec48f4d8ea" />
</details>

<details>
<summary>CLI Previews</summary>
<br>
<img width="1113" height="626" alt="image" src="https://github.com/user-attachments/assets/3fa1e04f-7364-4a81-9a38-c653502fac12" />
<br>
<img width="1113" height="721" alt="image" src="https://github.com/user-attachments/assets/ef23cfba-913a-4e82-85b0-7c15139aff55" />
</details>

---

## Features

- **Full Proxying:** Routes all Discord traffic through your proxy, completely ignoring system proxy settings, TUN interfaces, or VPNs.
- **Voice Chats & Streams:** Proxies both TCP (chat, media) and UDP traffic, so voice calls and screen shares work perfectly.
- **Isolated:** Works entirely within Discord's memory. It doesn't create system services or change Windows network settings.
- **Survives Updates:** The patch automatically reapplies itself whenever Discord updates.
- **Multi-Client Support:** Works with Discord Stable, Canary, and PTB.

---

## Installation

### Installer Mode
1. Download the latest release from the [releases page](https://github.com/snowluwu/droute/releases/latest).
2. Open `droute.exe`, enter your SOCKS5 proxy details, and choose your Discord build.
3. Click **Apply** to save the configuration, then click **Install** to apply the patch.

### CLI Mode
To install via the command line, run the following command:
```bash
.\droute.exe -i --branch stable --host 127.0.0.1 --port 1080
```
If you need help or want to see all available options, run:
```bash
.\droute.exe --help
```
> [!WARNING]
> CLI Mode will not automatically close Discord. You must close Discord manually before running the command so the installer can apply the patch.

---

## How It Works

### Clean Integration
Droute doesn't modify Discord's actual executable files. Instead, it hooks into the process using DLL Hijacking and .NET config files.

### Intercepting Traffic
The tool places `version.dll` and `droute.dll` into the Discord folder.
- When Discord starts, it loads the local `version.dll` instead of the system one.
- This local `version.dll` forwards all standard requests to the real system library while loading `droute.dll` in the background.
- Using **MinHook**, `droute.dll` hooks into Discord's low-level network functions and redirects traffic to your proxy.

### Handling Discord Updates
When Discord updates, it creates a folder with the new version number. Droute hooks into this update process to stay active:
- It drops a `.config` file for the .NET application into the folder with `Update.exe` (Squirrel Updater).
- When `Update.exe` runs, it automatically loads `Droute.UpdaterHook.dll`.
- This library hooks the process creation function. As soon as Squirrel Updater creates the new version directory, the hook patches it before the new Discord client even launches.

### Settings & Logs
- **Configuration:** All settings are stored in the Windows Registry at `HKCU/Software/droute` and can be tweaked via `regedit`.
- **Logs:** `%LOCALAPPDATA%\droute\logs\<branch>\droute-YYYY-MM-DD.log` and `updater-YYYY-MM-DD.log` (`stable`, `canary`, or `ptb`; `unknown` for other hosts). Processes of a branch share one file per component and local calendar date. Files switch dates automatically on the next entry; old logs are retained.
- **Detail level:** `LogLevel` (DWORD) in `HKCU\Software\droute`: `0` TRACE, `1` DEBUG, `2` INFO (default), `3` WARN, `4` ERROR. Both components use this setting at startup. INFO records lifecycle events and UDP summaries; DEBUG adds individual association attempts, SOCKS negotiation, routing decisions and installation stages; TRACE adds UDP operation registration and completion details.

Entries include PID, native TID, a process creation `SESSION` identifier and monotonic `UP` milliseconds since logger initialization. Correlate UDP records using `(SESSION, PID, flow)`; `socket` alone may be reused. `generation` increases on each successful association, while `attempt` counts initial and subsequent attempts. TCP connections have a separate `connection` identifier. Updater patch records use an `operation` identifier and report the stage, result, duration and full exception on failure. Packet contents, proxy credentials and command-line argument contents are not logged.

The reconnect worker emits `udp.summary` at intervals of at least 10 seconds for activated sockets, including inactive ones, and a final `udp.close` snapshot at cleanup. Summaries contain cumulative proxy packet/byte counts, packet deltas over `interval_ms`, ages of the last completed send/receive (`-1` means none), outstanding intercepted operations, errors, Winsock would-block responses and sends blocked before reaching the relay. Repeated identical association failures move to DEBUG; attempt counts and the latest error remain in INFO summaries. The first successful proxy send and receive of each generation are recorded separately. A successful send means the local Winsock operation completed; it does not prove delivery to the proxy or remote server. Completion errors include the submitting API; TRACE also identifies the observed completion API. Logging failures fall back to debugger output.
