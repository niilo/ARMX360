# Launcher icon

`armx360_icon_512.png` is the **master artwork** and the single source of truth
for the ARMX360 launcher icon. It is tracked here on purpose: it used to live
only in `build/`, which is git-ignored, so the icon's source was unversioned.

## Regenerating the launcher resources

```sh
python3 design/install_icon.py        # needs Pillow
```

That rewrites, for all five densities (`mdpi` … `xxxhdpi`):

| Resource | Size | Notes |
|---|---|---|
| `ic_launcher_background.webp` | 108dp | flat `#131516`, full-bleed, opaque |
| `ic_launcher_foreground.webp` | 108dp | mark + wordmark, transparent elsewhere |
| `ic_launcher.webp` | 48dp | legacy squircle |
| `ic_launcher_round.webp` | 48dp | legacy circle |

It also regenerates `app/src/main/assets/ARMX360_foreground.png`, the logo the
README renders.

**The `mipmap-*` resources are generated output — do not hand-edit them.** Edit
the master PNG and re-run the script. After a re-run on a clean tree
`git status` should show no change to `app/src/main/res/`; that is the check
that the tracked artwork really is the icon that shipped.

## Why it is not a straight copy

The master is a finished, pre-masked icon, and two properties of that artwork
shape how it has to be installed:

- **Opaque white corners.** Alpha is 255 across the whole image, corners
  included. Installed directly as an adaptive background, four white corners get
  baked into every launcher mask, so the background layer is a flat field
  instead and the launcher does its own masking.
- **A near-black keyline.** 3077 pixels sit below luminance 8 (pure `(0,0,0)` at
  y=5) around the `#131516` field. Any brightness-based extraction treats that
  rim as artwork, which inflates the content bounding box to the whole canvas and
  produces a white square behind the mark. The extractor erodes 3px from the
  shape's rim for exactly this reason.

The foreground is sized by **circumscribed circle**, not bounding box, because
circular masks clip the corners and the content is 398×349 rather than square.