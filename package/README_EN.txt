WinRepair Toolkit — quick start
===============================

Folder contents
  WinRepair_x64.exe / WinRepair_x86.exe  — the program (a single file, no installation, no dependencies)
  Run-WinRepair.cmd                      — starts the right build from any drive letter (handy in WinPE/WinRE)
  Playbooks\                             — repair playbooks and symptoms (*.wrp); format: Playbooks\README_PLAYBOOKS.md
  Tools\                                 — optional tools (see Tools\README.txt, e.g. cdb.exe for dump analysis)
  Lang\en.txt                            — optional English dictionary override (English is already embedded in the exe)
  Data\                                  — created on first run: Logs, Reports, Backups (hive and BCD copies)
  SHA256.txt                             — checksums of the exe files

Language
  The interface is Russian or English. Switch with the "English" / "Русский" button at the top right of the main window (the program restarts),
  or start with /lang:en or /lang:ru. The choice is saved in WinRepair.ini next to the exe. Default: Russian system → Russian, otherwise English.
  Other languages: put Lang\<code>.txt (format: `Russian text ==> translation`, see docs/LOCALIZATION.md) next to the exe and start with /lang:<code>.

Quick start
  1. Copy the WinRepair folder to the root of a USB drive (\WinRepair\).
  2. Boot WinPE (e.g. Sergei Strelec) or WinRE, open the drive and run Run-WinRepair.cmd.
  3. Select the installed Windows in "Target OS" (click "Refresh"). If the disk is missing — "BitLocker and storage drivers".
  4. Start with the "What happened?" wizard on the "Overview" tab.

Important
  * In WinPE/WinRE the Data folder lives on the RAM disk and disappears at reboot. Use "Save journals and reports to disk…" (Repair tools tab)
    or run the program from the USB drive so that Data is created next to the exe.
  * Every system change is confirmed separately and written to Data\Logs\changes_*.log. Hive and BCD copies go to Data\Backups.
  * Playbooks can run any program — use only trusted ones.
