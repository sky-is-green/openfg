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

To publish it on the AUR, create an `openfg-git` repository, copy `PKGBUILD`
and `.SRCINFO` into it, check the maintainer line, then push. Re-run
`makepkg --printsrcinfo > .SRCINFO` after every PKGBUILD change.
