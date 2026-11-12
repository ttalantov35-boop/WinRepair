[English](GITHUB_GUIDE.md) · [Русский](GITHUB_GUIDE.ru.md)

# Publishing WinRepair on GitHub — step by step

For someone who has never used GitHub. Everything is done on your Windows computer. Time: 30–40 minutes.

## What you get
- a project page `https://github.com/<your-name>/WinRepair` with the code and its history;
- automatic checks: on every push GitHub builds `WinRepair.exe` on Windows and runs the tests;
- Releases: push a version tag and GitHub builds x64 + x86, packs a zip and attaches the SHA-256 hash and the ready `.exe` files.

## Step 0. Decide first
1. **License** (mandatory). The project uses **MIT** (`LICENSE.txt`). Check the name and year in the first lines. Other ready texts are in `docs/license-options/`; background in `docs/LICENSING_AND_DISTRIBUTION.md`.
2. **Public or private repository.** Start with **Private**, check that everything builds, then switch to Public (Settings → General → Danger Zone → Change visibility). While the repository is private, nobody except you can download the files (links give 404).
3. **Name in the exe**: `WR_COMPANY` and `WR_COPYRIGHT` in `res/version.h`.
4. **`bsod-codes.json` data**: make sure you may publish it (see `THIRD_PARTY_NOTICES.md`).

## Step 1. GitHub account
1. Go to https://github.com/signup, enter e-mail, password and a username (it will be part of the project address), confirm the e-mail.
2. Turn on two-factor authentication: Settings → Password and authentication → Enable two-factor authentication (authenticator app).
3. **Hide your private e-mail.** Settings → Emails → tick "Keep my email addresses private" and "Block command line pushes that expose my email". Note the address like `12345678+username@users.noreply.github.com` shown there — you need it in step 3.

## Step 2. Install Git
1. Download "Git for Windows" from https://git-scm.com/download/win and install it, clicking Next everywhere (defaults are fine; Git Credential Manager is on by default and opens the browser for sign-in).
2. Start menu → **Git Bash** (or PowerShell) and check: `git --version`.

## Step 3. Introduce yourself to Git
In Git Bash (use your own values):
```
git config --global user.name "Your Name"
git config --global user.email "12345678+username@users.noreply.github.com"
git config --global init.defaultBranch main
```
Why noreply: the address in commits is visible to everyone on the internet. Older commits may contain another address (see step 5, "E-mail in old commits").

## Step 4. Prepare the project folder
1. Unpack `WinRepairGUI.zip`, e.g. to `C:\Projects\WinRepair`. The archive already contains the hidden `.git` folder with history (commits and the `v1.0.0-rc1` tag).
2. Check `LICENSE.txt` and `res/version.h`, then in the project folder:
```
cd /c/Projects/WinRepair
git status
git add -A
git commit -m "Set license and product identity"
```
3. If there is no `.git` folder (hidden files were not unpacked), run `git init`, `git add .`, `git commit -m "Initial commit"` there.

## Step 5. Create an empty repository on GitHub
1. On github.com click **+** → **New repository**.
2. Repository name: `WinRepair`. Description: "Offline Windows repair toolkit (WinPE/WinRE) — registry, BCD, autoruns, apps, explorer".
3. Choose **Private** (or Public). **Do not tick** "Add a README", ".gitignore", "license" — they already exist in the project and would conflict.
4. Click **Create repository**. A page with the address `https://github.com/<username>/WinRepair.git` opens.

**E-mail in old commits (optional).** If you do not want your old e-mail visible in history, run this once BEFORE the first push (the history is small):
```
git filter-branch -f --env-filter 'export GIT_AUTHOR_EMAIL="12345678+username@users.noreply.github.com"; export GIT_COMMITTER_EMAIL="12345678+username@users.noreply.github.com"' -- --all
```
Commit hashes will change. Afterwards delete and re-create the tag: `git tag -d v1.0.0-rc1` and `git tag v1.0.0-rc1`.

## Step 6. Upload the project
```
git remote add origin https://github.com/<username>/WinRepair.git
git branch -M main
git push -u origin main
git push origin --tags
```
On the first push a browser window opens — sign in and allow access (Git Credential Manager). If the console asks for a password: your normal password will not work, you need a **Personal Access Token** (Settings → Developer settings → Personal access tokens → Fine-grained → access to this repository only, permission "Contents: Read and write"); paste it instead of the password.

Refresh the repository page — the files should appear.

## Step 7. Check the automatic build
1. **Actions** tab → workflow **CI**. It starts by itself after the push: "Core tests" (Linux) and "Build WinRepair.exe (MSVC x64)" (Windows).
2. Green tick = everything built; at the bottom of a finished run, under **Artifacts**, there is `WinRepair-x64-…` with the exe.
3. Red cross = open the run → click the failed step → copy the error text and send it to the developer.
4. If a run fails at "upload-artifact" with `Unexpected token '<' … is not valid JSON`, this is a temporary GitHub outage: use **Re-run failed jobs** later (status: https://www.githubstatus.com).
5. If Actions are disabled: Settings → Actions → General → Allow all actions.
6. To avoid building on every documentation edit, set in `.github/workflows/ci.yml`:
```
on:
  push:
    branches: [main]
    paths-ignore: ['**.md', 'docs/**', '.github/ISSUE_TEMPLATE/**']
  pull_request:
    paths-ignore: ['**.md', 'docs/**']
```

## Step 8. Set up the project page
- **About** (gear icon, top right): description and topics `windows`, `winpe`, `winre`, `repair`, `registry`, `bcd`, `offline`, `autoruns`.
- `README.md` is shown on the main page automatically (English; Russian is `README.ru.md`).
- Settings → General → Features: enable Issues (bug reports), optionally Discussions.
- Settings → Code security: enable "Private vulnerability reporting" (matches `SECURITY.md`).
- Settings → Branches → add a rule for `main`: "Require a pull request" — only if you will have contributors.

## Step 9. Make the first release
1. Raise the version in `res/version.h` (e.g. `WR_VERSION_STR "1.0.0"`), write the changes in `CHANGELOG.md`, then:
```
git add -A
git commit -m "Release 1.0.0"
git push
git tag v1.0.0
git push origin v1.0.0
```
Without the command line: **Releases → Draft a new release → Choose a tag → type `v1.0.0` → Create new tag on publish → Publish release**.
2. **Actions** → workflow **Release** builds x64 and x86 and creates `WinRepair_x64.exe`, `WinRepair_x86.exe`, `WinRepair-v1.0.0.zip` and the `.sha256` file (1–3 minutes).
3. The **Releases** page shows the release with downloadable files; release notes are generated automatically (edit with the pencil icon). Mark release candidates (`-rc`) as "pre-release".
4. **Important:** links like `releases/latest/download/…` in the README skip pre-releases. For the download links to work, the release must be a normal one ("Set as the latest release").
5. Put the SHA-256 of the archive in the release text so users can verify the download.

Note: code signing is not performed in CI (a certificate is needed, see `CODE_SIGNING_POLICY.md`). Never commit a certificate or its password to the repository; if signing is added later, keep them in Settings → Secrets and variables → Actions.

## Step 10. Everyday work
```
git pull                 # get changes
# ...edit files...
./run_tests.sh           # check (Git Bash / WSL)
git add -A
git commit -m "What was changed"
git push
```
Better to work in branches: `git switch -c fix/registry-d-drive` → push → on GitHub click "Compare & pull request" → after green CI click "Merge".
Bug reports arrive in the Issues tab (the template asks for version, environment, steps and log).

## Without the command line
Install **GitHub Desktop** (https://desktop.github.com): File → Add local repository → choose the project folder → Publish repository (untick "Keep this code private" only for a public one). Then: change files → write a description at the bottom left → Commit → Push origin.

## Common problems
| Symptom | Solution |
|---|---|
| `remote: Repository not found` | Wrong address or no access: check `git remote -v` and that you are signed in to the right account |
| `rejected … fetch first` | A README was created together with the repository. `git pull --rebase origin main`, then `git push` again |
| `GH007: push would publish a private email` | You enabled e-mail blocking (step 1.3): repeat step 3 with the noreply address and rewrite the commits (step 5) |
| CI "Build" is red | Open the log of the "Build" step and send the error |
| CI is "Queued" for hours | GitHub outage or no free Actions minutes (Settings → Billing → Usage); cancel the run and retry later |
| Download links give 404 | Repository is private, or the release is a pre-release, or the release has no `WinRepair_x64.exe` in Assets |
| Antivirus flags the exe | Normal for unsigned repair tools: sign the exe (see `CODE_SIGNING_POLICY.md`) and report false positives to the antivirus vendor (`SECURITY.md`) |
| File larger than 100 MB | GitHub rejects it; publish big files only in Releases |

## Checklist before going Public
- [ ] `LICENSE.txt` is the real license (MIT) with the correct name
- [ ] no passwords, keys, `.pfx`, personal paths or logs (`git grep -i "password\|pfx"`)
- [ ] rights to `bsod-codes.json` and other data confirmed
- [ ] your private e-mail is not in the commits (step 5)
- [ ] CI is green, the release downloads and starts in WinPE
- [ ] README and CHANGELOG are up to date
