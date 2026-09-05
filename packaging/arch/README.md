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

The package supports OBS 32.2.x. Rebuild after OBS, FFmpeg, or libdatachannel ABI
upgrades. A new OBS minor release needs compatibility review before relaxing the
upper dependency bound. Use a full Arch system upgrade; partial upgrades are not
supported. The `-git` version is derived from the checked-out source automatically.

`check()` runs all nine real linked native media tests. CI builds the exact
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
