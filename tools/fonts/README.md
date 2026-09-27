# Vendored fonts

These files are the only font sources carried in the repository. The staged
copies the example uses live in `examples/assets/fonts/`, but **Ubuntu Mono is not a
system font on any target platform**, so it has to be carried with the source.

| File | Family | Upstream | Licence |
|---|---|---|---|
| `Ubuntu-Regular.ttf` | Ubuntu Regular | https://github.com/google/fonts/tree/main/ufl/ubuntu | Ubuntu Font Licence 1.0 |
| `Ubuntu-Bold.ttf` | Ubuntu Bold | (same) | Ubuntu Font Licence 1.0 |
| `Ubuntu-Light.ttf` | Ubuntu Light | (same) | Ubuntu Font Licence 1.0 |
| `UbuntuMono-Regular.ttf` | Ubuntu Mono Regular | https://github.com/google/fonts/tree/main/ufl/ubuntumono | Ubuntu Font Licence 1.0 |
| `UbuntuMono-Bold.ttf` | Ubuntu Mono Bold | (same) | Ubuntu Font Licence 1.0 |

The Ubuntu Font Licence permits redistribution and embedding, including in
commercial and web products, provided the font is not sold on its own and any
derivative is renamed. See https://ubuntu.com/legal/font-licence.

The staged copies are named `ubuntu.ttf`,
`ubuntu_bold.ttf`, `ubuntu_light.ttf`, `ubuntu_mono.ttf` and
`ubuntu_mono_bold.ttf`.
face cleanly when there is no network, so the build never fails.

The main menu uses this face (`mega::Assets::FontMono()`); every other scene
keeps the proportional UI face.
