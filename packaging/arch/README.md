# Arch / AUR development package

`PKGBUILD` builds `obs-vdoninja-git` from source against Arch's installed OBS,
FFmpeg, Qt, and libdatachannel. Do not install the Ubuntu binary archive on Arch.
This recipe is maintained here; it has not been submitted to the public AUR.

```bash
# In a copy of this directory, as a normal user:
makepkg -si
# Remove the package using pacman's ownership database:
sudo pacman -R obs-vdoninja-git
```

The source recipe requires OBS 32.2 or newer and builds against the installed SDK.
The resulting package records that SDK's minor compatibility band (for example,
33.0.x); rebuild after OBS minor, FFmpeg, or libdatachannel ABI upgrades. CI checks
the current Arch build with linked tests and real module initialization. This
does not change the OBS 32.2.0 baseline for the downloadable release archives.
Use a full Arch system upgrade; partial upgrades are not supported. The `-git`
version is derived from the checked-out source automatically.

Arch uses the system libdatachannel. The bundled UDP relay-policy qualification
for official archives does not establish Force TURN behavior for that library.

`check()` runs the real linked native media tests. CI builds the exact
repository revision in an Arch container, installs and reinstalls with pacman,
checks file ownership/dependencies, initializes the installed plugin in OBS,
then removes it and checks that the binary and data are gone. No custom install
or uninstall hook is needed. Do not run the standalone archive uninstaller on a
pacman-managed installation.

To reproduce CI from the repository root (Docker required):

```bash
docker run --rm -v "$PWD:/repo:ro" archlinux:base-devel \
  bash /repo/tests/test-arch-package.sh
```

The container builds the committed checkout. For an AUR submission, regenerate
`.SRCINFO` with `makepkg --printsrcinfo > .SRCINFO`, review the recipe and assign
an AUR maintainer. Packaging follows Arch's
[CMake guidelines](https://wiki.archlinux.org/title/CMake_package_guidelines) and
[package guidelines](https://wiki.archlinux.org/title/Arch_package_guidelines).
