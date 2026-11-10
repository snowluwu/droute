# Droute: Proxy Support for Discord on Windows

<p align="center">
  <img src="external/droute.svg" alt="Droute Logo" width="128">
</p>

<p align="center">
  <a href="https://github.com/snowluwu/droute/releases"><img src="https://img.shields.io/github/v/release/snowluwu/droute?style=for-the-badge&label=Release&labelColor=3A3A3A&color=5662f6" alt="Release"></a>
</p>

**Droute** adds SOCKS5 proxy support to the Discord Desktop Client on Windows for chat, media, voice calls and screen sharing.

- Inspired by [force-proxy;](https://github.com/runetfreedom/force-proxy)
- Uses [Weave;](https://github.com/snowluwu/weave)
- Weave and Droute use [MinHook](https://github.com/tsudakageyu/minhook) for hooks.

> [!NOTE]
> Requires a running SOCKS5 proxy. Voice calls and screen sharing also need UDP support. A local proxy from v2rayN, Clash Verge or NekoBox works too.

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

- **TCP and UDP proxying:** Routes chat, media, voice calls and screen sharing through SOCKS5.
- **Automatic reconnection:** Restores UDP proxy connections after the proxy disconnects or restarts.
- **Update hook:** Installs Droute in new Discord versions before they launch.
- **Installer and CLI:** Configure the proxy and install or remove Droute through the installer or command line.
- **Discord builds:** Supports Stable, Canary and PTB on Windows x64.

---

## Installation

### Installer Mode

1. Start your SOCKS5 proxy. Check its address, port, and username/password if required.
2. Download `droute.zip` from the [latest release](https://github.com/snowluwu/droute/releases/latest) and extract it.
3. Open `droute.exe`.
4. Enter your SOCKS5 proxy settings and click **Apply**.
5. Select Stable, Canary, or PTB and click **Install**. With automatic restart enabled, the installer closes Discord first. Otherwise, close Discord yourself.
6. Close the installation log window. If the installer closed Discord, it starts it again. Otherwise, start Discord yourself.

To remove Droute, close Discord and use the installer's removal action for the selected build.

### CLI Mode

Close Discord before installing or removing Droute. The CLI does not close it automatically.

Install for Stable and save the proxy address:

```console
.\droute.exe --install --branch stable --host 127.0.0.1 --port 1080
```

Replace `127.0.0.1:1080` with your proxy address. Use `--branch canary` or `--branch ptb` for other builds. Add `--user` and `--password` if your proxy requires authentication.

Remove Droute from Stable:

```console
.\droute.exe --uninstall --branch stable
```

List available options:

```console
.\droute.exe --help
```

---

## How It Works

### 1. Core Architecture & Interception
* **Import Chain:** `version.dll` → `droute.dll` → `weave_proxy.dll`.
* **Deployment:** The installer copies `version.dll` from `System32` to the active Discord version folder (`app-*`), adds `droute.dll` to its import table, and places both `droute.dll` and `weave_proxy.dll` in the same directory.
* **Execution:** `Droute` reads proxy settings from the Windows registry (`HKCU\Software\droute`) and initializes `Weave` to handle Winsock interception and SOCKS5 routing. Original system binaries and Discord files on disk remain untouched.

### 2. In-Memory Initialization
`Droute` temporarily redirects the memory entry point of the executable:
1. Restores original bytes at the entry point.
2. Configures and launches `Weave` outside the loader lock.
3. Transfers execution to the application CRT.

* **Guarantee:** Interception activates before main application code executes, eliminating background thread initialization delays. Network activity inside early DLL initializers or TLS callbacks is not covered.

### 3. Auto-Updates & Updater Hook
* **Setup:** The installer places `Droute.UpdaterHook.dll` and `Update.exe.config` alongside Discord's `Update.exe` to hook the update process.
* **Verification:** Before Discord starts, the hook scans for the latest `app-*` folder and installs any missing components (`version.dll`, `droute.dll`, `weave_proxy.dll`).
* **Migration:** When Discord updates, the hook automatically migrates the file set. Files are published ahead of `version.dll`; a failure rolls back to previous binaries. *(Always close Discord before manually replacing installed files).*

### 4. Versioning
* **Droute:** Core version is maintained in `installer/Properties/AssemblyInfo.cs`. The native `droute.dll` embeds the Git revision into its product version at build time.
* **Weave:** Maintained as an independent library with its own versioning stored in `external/weave/VERSION`.

### 5. Configuration, Logging & Troubleshooting
* **Configuration:** Managed via the Windows registry at `HKCU\Software\droute` (requires restarting Discord after changes).
* **Logs Location:** `%LOCALAPPDATA%\droute\logs\<branch>\droute-YYYY-MM-DD.log` (where `<branch>` is `stable`, `canary`, or `ptb`; updater logs are saved nearby).
* **Log Levels (`LogLevel` DWORD in registry):**
  * `0` — `TRACE` (All events and granular UDP tracing)
  * `1` — `DEBUG` (Association attempts and SOCKS negotiations)
  * `2` — `INFO` *(Default)* (Lifecycle events and UDP summaries)
  * `3` — `WARN` (Non-fatal warnings)
  * `4` — `ERROR` (Critical errors)

* **Privacy & Bug Reporting:** When reporting an issue, provide your Droute version, Discord build, proxy software, and the daily log file with an approximate timestamp. Passwords, proxy credentials, payloads, and command-line arguments are **never logged** (only session IDs, PIDs, and connection generations are retained).

## Building

Clone with `--recurse-submodules`, or run `git submodule update --init --recursive` in an existing checkout. Weave is pinned at `external/weave`; builds use that revision rather than the latest remote branch.

Use Visual Studio 2022 with the C++ desktop workload, Windows SDK and .NET desktop tools:

```powershell
msbuild droute.sln /restore /p:Configuration=Release /p:Platform=x64
```

The solution builds weave, droute, updater hook and installer. Both native DLLs are embedded in the installer and updater hook. Releases include `droute_64.pdb` and `weave_proxy.pdb` from the same build as the installed DLLs; keep both for native debugging. The installed `droute.dll` is the build output `droute_64.dll` renamed by the installer.
