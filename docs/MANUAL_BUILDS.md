# Desktop builds

The [Build and release workflow](../.github/workflows/release.yml) runs the source checks, builds the Linux AppImage and Windows portable ZIP, and verifies both packages. The same packages can be built locally with `build.sh`, `packaging/appimage.sh`, and `packaging/windows/make_portable.py`.

## Build on GitHub

Open **Actions → Build and release → Run workflow**, choose `main`, and run it. The source checks run first; when they pass, Linux and Windows build jobs run. Download `linux-build` and `windows-build` from the completed workflow run's **Artifacts** section. These manual builds do not create a GitHub Release.

From a shell with GitHub CLI authentication, the equivalent command is:

```bash
gh workflow run release.yml --ref main -R GetLostIn/CTR-NitroFueled-PC
```

To build and publish automatically, push a new `v*` tag on a commit that contains the workflow. For example, after pushing the commit to `main`:

```bash
git tag v0.6.0
git push origin v0.6.0
```

The tag run performs the same checks and builds, then creates a **draft** GitHub Release with the two packages: `SacredFire-<version>-x86_64.AppImage` and `CTR-NitroFueled-PC-portable-<version>-windows.zip`. Write the release notes, try the packages and publish the draft from the Releases page. The SHA-256 files and the AppImage's `.build.txt` (source commit and nixpkgs version) stay in the workflow run's artifacts, not in the release. Untagged manual builds use a `git describe` version such as `v0.5-8-gabcdef0`. A `main` push or pull request runs source checks without packaging.

## Build locally

Run the commands from the repository root. Start from the commit you intend to package, then initialize its pinned dependencies:

```bash
git submodule update --init --recursive
git status --short
git describe --tags --match 'v*' --always
```

For a release package, use a clean, committed checkout. The Linux packager checks that `out/VERSION` matches the checkout; a tag such as `v0.5` gives the package that version. The packages contain the launcher and runtime, not the game files.

## Linux AppImage

Install [Nix](https://nixos.org/download/) with `nix-shell`, `nix-instantiate`, and `nix bundle`, and make `<nixpkgs>` available through `NIX_PATH` or a Nix channel. On Arch or Omarchy, a native `bash build.sh` produces an executable for the host, but the AppImage needs binaries linked against the Nix libraries in `shell.nix`. Enable the Nix command and flakes features for the bundler:

```bash
nix-instantiate --eval -E '(import <nixpkgs> {}).lib.version'
export NIX_CONFIG='experimental-features = nix-command flakes'
nix-shell shell.nix --run 'BB_IN_NIX_SHELL=1 bash build.sh'
bash packaging/appimage.sh
(cd dist && sha256sum --check SacredFire-x86_64.AppImage.sha256)
```

The result is `dist/SacredFire-x86_64.AppImage`, its `.sha256` checksum, and `SacredFire-x86_64.AppImage.build.txt` with the source commit and nixpkgs version. The AppImage opens the GTK launcher; `--play` starts with saved launcher settings. This build can take substantial disk space and time because Nix builds the bundled library closure. The nixpkgs channel is not pinned in this repository, so matching another build byte for byte also requires its recorded nixpkgs revision.

If `out/` already holds an Arch build, move it aside with `mv out out.arch` before the Nix build. After packaging, `mv out out.nix` and `mv out.arch out` restores the native build. Skip those moves when there is no existing native `out/`.

For a local test with uncommitted changes, rebuild first and run `BB_ALLOW_DIRTY=1 bash packaging/appimage.sh`. The flag skips the source/version match check, so use a clean checkout for a release asset. The Nix package is defined in `packaging/default.nix`; `packaging/appimage.sh` collects the built binaries' Nix library paths and calls `nix bundle`.

## Windows portable ZIP

Use Windows 10 or 11 and the **MSYS2 CLANG64** shell. Install the packages listed in [the Windows build guide](../packaging/windows/README.md#build), then run:

```bash
git submodule update --init --recursive
bash build.sh
python3 packaging/windows/make_portable.py
(cd dist && sha256sum --check CTR-NitroFueled-PC-portable-windows.zip.sha256)
```

`build.sh` creates `out/sacredfire.exe`, `out/bb-gpu-capabilities.exe`, and `out/CTR-NitroFueled-PC.exe`. The packager gathers their MSYS2 DLL dependencies, the launcher, scripts, patches, a pinned embeddable Python download, licenses, and default settings. It writes `dist/CTR-NitroFueled-PC-portable-windows.zip`, its `.sha256` checksum, and an unpacked `dist/CTR-NitroFueled-PC-portable/` folder. The packaged ZIP contains no game files or personal saves.

The optional `--version vX.Y` argument changes the version recorded inside the ZIP; without it the packager uses `out/VERSION`. The script always writes the unversioned ZIP filename. The [upstream v0.5 release](https://github.com/Ryu7w7/OCTR-NitroFueled-PC/releases/tag/v0.5) uses a versioned Windows asset name. To match that naming pattern, rename the ZIP and update its checksum file after packaging:

```bash
release_version=$(git describe --tags --match 'v*' --always)
mv dist/CTR-NitroFueled-PC-portable-windows.zip "dist/CTR-NitroFueled-PC-portable-${release_version}-windows.zip"
mv dist/CTR-NitroFueled-PC-portable-windows.zip.sha256 "dist/CTR-NitroFueled-PC-portable-${release_version}-windows.zip.sha256"
(cd dist && sha256sum "CTR-NitroFueled-PC-portable-${release_version}-windows.zip" > "CTR-NitroFueled-PC-portable-${release_version}-windows.zip.sha256")
(cd dist && sha256sum --check "CTR-NitroFueled-PC-portable-${release_version}-windows.zip.sha256")
```

The Windows build path in `build.sh` runs under MSYS2. This repository does not provide a Linux to Windows cross compilation script.
