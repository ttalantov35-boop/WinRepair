# Guide: Windows has no winre.wim (stripped build)

Many stripped-down Windows builds remove the recovery environment to save space: there is no `winre.wim` and `reagentc /info` says WinRE is disabled. There are two ways forward.

## Path 1 (easiest): the "Windows install image" base — no `winre.wim`, no ADK

Download a Windows ISO from Microsoft (same architecture; ideally the same version) or use a Windows install stick. In WinRepair open the WinPE stick builder, choose the **"Windows install image"** base and point it at the ISO (or drive letter). The program takes a clean WinPE from `sources\boot.wim`, adds WinRepair and `rstrui`. WinRepair starts first. Details: `WINPE_USB.en.md`. If ADK is already installed you can pick the ADK WinPE base instead.

## Path 2: extract winre.wim from a Windows image and restore it

Use this if you specifically need this system's WinRE (a boot-menu entry, WinRE + WinRepair).

### Step 1. Download an image of the same Windows
- You need the **same edition, architecture and, ideally, the same build** (x64 with x64, ARM64 with ARM64). `winver` shows your version.
- Use an official ISO: Microsoft's "Download Windows 10/11" page (Media Creation Tool or ISO).
- Mount the ISO (right-click → **Mount**); it gets a drive letter such as `E:`. Inside you'll find `E:\sources\install.wim` or `E:\sources\install.esd`.

### Step 2. Extract winre.wim

**Option A: 7-Zip (simplest, no mounting).**
1. Install 7-Zip (7-zip.org).
2. Open `E:\sources\install.wim` (or `.esd`) in 7-Zip.
3. Open the folder with the edition number (e.g. `1`, `6`) → `Windows` → `System32` → `Recovery`.
4. Copy `Winre.wim` to the desktop or `C:\Temp`. If the `Recovery` folder is missing, try another edition number.

**Option B: DISM (administrator command prompt).**
```
dism /Get-WimInfo /WimFile:E:\sources\install.wim
```
Note the edition index, e.g. `1`. For `install.esd` use the same command with `.esd`.
```
mkdir C:\mnt
dism /Mount-Image /ImageFile:E:\sources\install.wim /Index:1 /MountDir:C:\mnt /ReadOnly
copy /Y C:\mnt\Windows\System32\Recovery\Winre.wim C:\Temp\
dism /Unmount-Image /MountDir:C:\mnt /Discard
```
If DISM refuses to read (error 0x780 or similar), use Option A.

### Step 3. Put the file in place

The file must live in `C:\Windows\System32\Recovery\` (create the folder if missing). Administrator command prompt:
```
mkdir C:\Windows\System32\Recovery
copy /Y C:\Temp\Winre.wim C:\Windows\System32\Recovery\Winre.wim
attrib +h +s C:\Windows\System32\Recovery\Winre.wim
```
If Windows refuses to copy, take ownership of the folder:
```
takeown /f C:\Windows\System32\Recovery /r /d y
icacls C:\Windows\System32\Recovery /grant *S-1-5-32-544:F /t
```

### Step 4. Register WinRE with the system
```
reagentc /setreimage /path C:\Windows\System32\Recovery /target C:\Windows
reagentc /enable
reagentc /info
```
`reagentc /info` should show **Windows RE status: Enabled** and the path to `Winre.wim`. If `reagentc /enable` fails, the recovery subsystem itself has been stripped from your build; use **Path 1**.

### Step 5. Run WinRepair
Run the program as administrator. The WinRE features (boot-menu entry, WinRE + WinRepair, a stick based on `winre.wim`) now see the image. For the stick we still recommend the ADK WinPE base, because only it guarantees that WinRepair starts first.

## Notes
- Don't use a `Winre.wim` of a different architecture (x64 ≠ ARM64).
- The image's build should match your Windows as closely as possible. A difference of a build or two is usually fine; widely different versions can cause driver problems.
- Use only official images; never take WinRE files from unknown sources.
