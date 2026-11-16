<p align="center">
  <img src="assets/logo.svg" width="128" alt="TinyWeek logo">
</p>

<h1 align="center">TinyWeek</h1>

<p align="center">
  The ISO 8601 week number in the Windows notification area.<br>
  One 23 KB <code>.exe</code>. No installer, no runtime, no dependencies.
</p>

---

TinyWeek puts the current week number next to your clock, drawn crisp at the
taskbar's real pixel size. Hover for the full date, right-click for the menu.

## Features

- **Tiny**: plain Win32 C without the C runtime; the executable imports only
  system DLLs (`kernel32`, `user32`, `gdi32`, `shell32`, `advapi32`).
- **Zero idle cost**: no polling. It wakes up only at midnight, after
  sleep/resume, when the clock or time zone changes, or when Explorer restarts.
- **Big, sharp digits at any DPI**: rendered at the tray's native icon size,
  scaled by their true glyph bounds to fill the whole icon, with tight digit
  spacing. Proportions are never stretched.
- **Matches your system font**: uses the Windows UI font, and on Windows 11
  Segoe UI Variable, the same font as the taskbar clock.
- **Adjustable size and weight**: size from 100 % (largest that fits the
  icon) down to 60 %, weight Light, Regular, Semibold or Bold.
- **Follows the taskbar theme**: white digits on a dark taskbar, black on a light one.
- **Correct ISO 8601 weeks**: weeks start on Monday, and week 1 is the week
  that contains the year's first Thursday (verified for every day 1900–2199).
- **Start with Windows** toggle in the menu (per-user, no admin rights).
- Single instance; the date is shown in your own locale format.

## Install

TinyWeek is a single file, so installing means putting it in a permanent
place and telling Windows to start it.

### 1. Download

Get `TinyWeek.exe` from [Releases](../../releases).

The binary is not code-signed, so SmartScreen may warn the first time
(*More info → Run anyway*). Each release includes a SHA-256 checksum you can
compare with PowerShell's `Get-FileHash .\TinyWeek.exe`.

### 2. Move it to a permanent folder

Don't run it from *Downloads*. The recommended place is your per-user
programs folder, which needs no admin rights:

```
%LOCALAPPDATA%\Programs\TinyWeek\TinyWeek.exe
```

Paste `%LOCALAPPDATA%\Programs` into File Explorer's address bar, create a
`TinyWeek` folder and move the file there. Or run this in PowerShell from the
folder where you downloaded it:

```powershell
$dir = "$env:LOCALAPPDATA\Programs\TinyWeek"
New-Item -ItemType Directory -Force $dir | Out-Null
Move-Item .\TinyWeek.exe $dir -Force
Start-Process "$dir\TinyWeek.exe"
```

To install it for all users on the PC, `C:\Program Files\TinyWeek\` also
works (needs admin rights to copy there).

### 3. Start it with Windows

Run `TinyWeek.exe`, right-click the week number and tick **Start with Windows**.
This adds a per-user entry under
`HKCU\Software\Microsoft\Windows\CurrentVersion\Run`. You can also turn it
off in *Task Manager → Startup apps* or *Settings → Apps → Startup*.

If you move the exe later, start it once from the new place: it updates the
startup entry to its new path automatically.

### 4. Keep the icon visible (Windows 11)

New tray icons go to the overflow menu (**^**) by default. Either drag the
week number from the overflow onto the taskbar, or open
*Settings → Personalization → Taskbar → Other system tray icons* and switch
**TinyWeek** on.

### Update

Choose **Exit** from the menu, replace `TinyWeek.exe` with the new version
and start it again. The startup setting is kept.

### Uninstall

1. Right-click the icon and untick **Start with Windows**.
2. Choose **Exit**.
3. Delete the `TinyWeek` folder.
4. If you changed **Size** or **Weight**, also delete the registry key
   `HKCU\Software\TinyWeek`, for example in PowerShell:

   ```powershell
   Remove-Item HKCU:\Software\TinyWeek -ErrorAction SilentlyContinue
   ```

TinyWeek writes nothing else to your system: no files, and no registry keys
besides the startup entry and the size and weight settings.

## Usage

| Action | Result |
|---|---|
| Hover | Week number and full date |
| Double-click | Date dialog |
| Right-click | Show date · Refresh · Size · Weight · Start with Windows · Exit |

**Size** sets how large the digits are: 100 % is the largest that fits the
icon, and 90–60 % leave more space around them. **Weight** sets the stroke
thickness: Light, Regular, Semibold or Bold (default). Both choices are
remembered.

The icon updates by itself at midnight, after the PC wakes from sleep, when
the clock or time zone changes, and when you switch the taskbar between light
and dark, or when the system font changes. **Refresh** is only needed if something looks wrong.

## FAQ

**Why not inside the Windows clock?** Windows has no supported way to extend
the taskbar clock or calendar. Doing it means injecting code into Explorer,
which breaks with Windows updates and triggers antivirus software. A tray icon
is the robust option.

**How much memory and CPU does it use?** No CPU while idle: it sleeps until
Windows sends it a message. Memory is around 1–2 MB, mostly the system
libraries every GUI program loads.

**Which week numbering?** ISO 8601, used in Finland, the rest of Europe and
most of the world. US-style numbering (weeks starting on Sunday) is not
supported.

## Build

Requires MinGW-w64.

**Linux:**

```bash
sudo apt install gcc-mingw-w64-x86-64
./build.sh
```

**Windows (MSYS2 UCRT64 shell):**

```bash
pacman -S mingw-w64-ucrt-x86_64-gcc
CROSS= ./build.sh
```

The output is `build/TinyWeek.exe`.

GitHub Actions builds the executable on every push. To publish a release,
change the number in the `VERSION` file and push to `main`: the workflow tags
the commit as `v<version>` and attaches the exe and its SHA-256 checksum.

## Logo

Seven bars for the days Monday to Sunday. Thursday is highlighted because in
ISO 8601 Thursday decides which year and week a week belongs to.

## License

[MIT](LICENSE)
