<p align="center"><img src="branding/logo-256.png" width="120" alt="WinRepair Toolkit"></p>

# WinRepair Toolkit
### Windows won't boot? One file — and you're back in control.

*Tynychsoft presents*

---

## A familiar nightmare

A black screen. A blue screen. An endless reboot loop. An antivirus that "protected" the system right out of starting. A work PC with a deadline tomorrow.

Usually, that's when the quest begins: find a second computer, download somebody's WinPE build of unknown origin, juggle ten utilities that each do "a little bit". And the clock keeps ticking.

**WinRepair Toolkit answers all of that with a single file.** No installation. No .NET. No PowerShell. Just `WinRepair.exe`.

## What's inside

**🔧 A workshop for a sick system.** A regedit-grade offline registry editor. A BCD and boot-mode editor. Autoruns-style startup and services. An offline file explorer. Forced removal of the program that keeps Windows from starting. All of it aimed at *another* Windows that sits on a disk and can't switch on.

**🧭 The "What happened?" wizard.** Not sure where to begin? Pick a symptom and the program checks BitLocker, disks, partitions, the bootloader, registry hives, updates, crash dumps, services and the network, then lists findings by severity with a recommended action. Nothing happens without your "yes".

**💾 Your own bootable WinPE stick.** One button gives you a USB stick that boots on its own and opens WinRepair first. No third-party builds. Even System Restore (`rstrui`) works right from it: roll Windows back to a restore point without ever booting it. Works with Secure Boot left on.

**↩️ One-click undo.** The program keeps a journal of everything it changed — registry, files, BCD, quarantine. Changed your mind? One button reverts it all in the safe order, even after a restart.

**📱 ARM, too.** A native build for Windows on ARM: Snapdragon laptops are no longer left without a tool.

**🛡️ Safety as a habit.** Confirmation before every action, a backup before every change, quarantine instead of deletion, a complete log. And not a single byte leaves your machine — the program sends nothing anywhere.

## How it looks in practice

1. Download `WinRepair_x64.exe`.
2. Press "Create a bootable WinPE USB stick with WinRepair…" — your own rescue media is ready.
3. Boot a "dead" PC from it — WinRepair opens by itself.
4. "What happened?" → symptom → findings → fix. Or a restore point via `rstrui`.
5. Something went wrong? "Undo session changes".

Five steps instead of an evening of searching.

## Who it's for

- **People who fix other people's PCs:** technicians, admins, service engineers. One stick, every case.
- **People who fix their own:** no magic and no gambling — confirmations, and a way back.
- **People who like to understand:** every command is visible, every action is logged, every step can be checked.

## Facts and figures

- **79** commands in the catalog, each run confirmed
- **3** builds: x64, x86, ARM64
- **2** interface languages: English and Русский (add your own via `Lang\<code>.txt`)
- **0** dependencies and **0** bytes of telemetry
- **MIT** licence: use and modify freely

## Give it a try

Grab the latest version from [Releases](../../../releases), run it as administrator — and the very first diagnosis shows what one small file can do.

**WinRepair Toolkit · Tynychsoft**
