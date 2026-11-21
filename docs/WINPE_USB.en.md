# Guide: a bootable WinPE USB stick with WinRepair

## 1. What you need

- A USB stick of 2 GB or more (8 GB+ recommended).
- `WinRepair_x64.exe`, run **as administrator**.
- **Base "Windows install image" (no ADK, easiest).** An ISO of Windows 10/11 (Microsoft's "Download Windows" page) or a Windows install USB stick is enough. The program takes a clean WinPE from it; WinRepair starts first and `rstrui` works. Prefer the same version and architecture as your Windows.
- **Base ADK WinPE.** Requires the *Windows ADK* and the *Windows PE add-on*. Same result; handy if ADK is already installed.
- Fallback base: this system's `winre.wim`. No ADK needed, but WinRepair must be started by hand: language → "Advanced options" → "Command prompt" → `winrepair`.

## 2. Which file system

| File system | Boots on UEFI (Secure Boot on) | Advice |
|---|---|---|
| **FAT32** | Yes | **Use it.** Microsoft's boot loader reads only FAT32. |
| NTFS | No | Not suitable |
| exFAT | No | Not suitable |

FAT32 limits:
- A single file can't exceed 4 GB (the WinPE image is smaller, so this doesn't matter).
- Windows' built-in formatter only offers FAT32 up to 32 GB. For larger sticks use `diskpart` (below) or a third-party formatter.
- Any volume label works, e.g. `WINPE`.

## 3. An empty stick

1. Insert the stick and note its drive letter (e.g. `E:`). **Double-check it**: formatting erases everything.
2. File Explorer → right-click the stick → **Format** → **FAT32** → **Start**.
3. For sticks larger than 32 GB, open an administrator command prompt:
   ```
   diskpart
   list disk
   select disk N        (your stick, NOT the Windows disk!)
   clean
   create partition primary
   format fs=fat32 quick label=WINPE
   active
   assign
   exit
   ```
   `clean` wipes the whole stick: verify the disk number by its size.

## 4. A stick with data you can't erase

You can write the image without formatting **only if the stick is already FAT32** and has about 1 GB free. WinPE files go to the root next to your data and nothing is deleted. But if the root already has `EFI`, `boot`, `sources` or `bootmgr`, their contents will mix with the image.

- **NTFS or exFAT:** it can't be converted to FAT32 without data loss (`convert` only works the other way). Copy the data off, format as FAT32, copy it back.
- **Best option:** a separate small stick just for WinPE.

## 5. Creating the stick in the program

1. WinRepair → **Overview** tab (the orange button) or "Repair tools" → **"Create a bootable WinPE USB stick with WinRepair…"**.
2. In the dialog choose:
   - **Destination:** the stick's root (e.g. `E:\`). A folder also works (default `D:\WinPE_WR`); copy its contents to the stick manually afterwards.
   - **Base:** **"Windows install image"** (no ADK), **"ADK WinPE"** or this system's `winre.wim`.
   - **Windows install image:** an `.iso` file, the letter of a mounted image/install stick, or a folder with `sources\boot.wim`. Only needed for the first base.
3. Confirm. The build takes a few minutes. The log should end with **"Autostart: winpeshl.ini → wpeinit → WinRepair"**. If you see a write-protection warning (0x780) instead, you chose `winre.wim`: rebuild with the install image or ADK base.
4. When it finishes, use **Safely Remove Hardware** before pulling the stick.

## 6. Booting from the stick

1. Insert the stick into the target PC and power it on.
2. Immediately press the boot-menu key: usually **F12**, **F11**, **Esc** or **F8** (depends on the vendor).
3. Pick **UEFI: <your stick>**. **Secure Boot can stay on.**
4. WinPE loads and WinRepair opens right away.
5. In **"Target OS"** choose the Windows you want to repair.

## 7. FAQ

- **Old PC without UEFI (Legacy/BIOS).** Also make the stick bootable: `bootsect /nt60 E: /force /mbr` (from the ADK folder). The program warns about it in the log. Not needed on modern PCs.
- **The stick isn't in the boot menu.** Check that it's FAT32 and USB boot is enabled; on some PCs temporarily disable **Fast Boot**.
- **Can I share the finished stick?** No: Microsoft's licence forbids redistributing the image. Everyone builds their own with one click.
- **WinRepair can't see Windows.** The disk may need a storage driver (e.g. Intel VMD/RST) — see "BitLocker and storage drivers"; if the disk is BitLocker-encrypted, unlock it first.
