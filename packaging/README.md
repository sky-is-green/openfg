# Arch/CachyOS package

The `PKGBUILD` builds the current git tree and installs the Vulkan layer,
`fg-lab`, `openfg-cli`, `openfg-gpu-probe`, the GUI and the docs.

```sh
cd packaging
makepkg -si
```

`makepkg -f` builds the package without installing it. The package name is
`openfg-git`; a tagged release package should use `openfg`.

After installing, create a per-user config and enable it:

```sh
openfg-cli init
$EDITOR ~/.config/openfg/conf.toml
openfg-cli validate
openfg-gpu-probe
```

The GUI is optional (`pyside6`); the layer and CLI work without it.

## Publishing to the AUR

The `openfg-git` name is currently free. Publishing needs the maintainer's
AUR account and an SSH key registered at <https://aur.archlinux.org>:

```sh
ssh-keygen -t ed25519 -f ~/.ssh/aur -C "your AUR email"
# add ~/.ssh/aur.pub at https://aur.archlinux.org/account/
git clone ssh://aur@aur.archlinux.org/openfg-git.git
cp PKGBUILD .SRCINFO openfg-git/
cd openfg-git
git add PKGBUILD .SRCINFO
git commit -m "Initial import"
git push
```

Check the `# Maintainer:` line in `PKGBUILD` first. Re-run
`makepkg --printsrcinfo > .SRCINFO` after every PKGBUILD change. The package
installs the layer, CLI, probe, fg-lab, GUI and docs from the current git
tree; it is not a tagged-release package.
