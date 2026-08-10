# Preview effects (official Light / reduced)

Directory layout for chart-preview VFX. Do **not** place effect PNGs at `skins/` root.

## bomb/light/default/

Official Default Bomb at `GameTapEffectType.Light` (BomSquare + BomFlare only).

- `{normal,critical,scratch,hold,sound}/square.png` — hollow frame (`BombEffectDefault_3`)
- `{normal,critical,scratch,hold,sound}/flare.png` — soft ring halo (`BombEffectDefault_10`); drawn only for Critical/Flick.
  Recentering: Unity sprite `m_Pivot` `{x: 0.51368976, y: 0.48324347}` is placed on the
  plate geometric center (engine draws UV mid = bomb anchor). Do **not** use alpha-centroid
  recentering — that left the visual ring ~1% right of the judgeline. Do **not** map
  `ParticleSystemRenderer.pivot` `{x:-0.015,y:0.267}` into stage Y (wrong space for 2D).
- `meta.json` — duration / slice hints

Source: `wds-resources/.../Texture2D/BombEffectDefault_7.png` + `Sprite/BombEffectDefault_10.asset`.

## split/lines/

Preferred over root `Sirius Split Line _*` placeholders when present:

```text
split/lines/
  base/{colorId}.png
```

Light mode draws lines + fade only (no SplitEffect particles / no Sonolus Transform wipe).
`transform1/` / `transform2/` trees are unused and should not be shipped. Appear/disappear
windows match official Show≈1000ms / Hide≈500ms via `PreviewVisualConfig`.

Preview/edit soft ribbons are **not** the stock 8-tap hard-core plate as-is: the engine
rebakes a wide gaussian glow (`split_soft_profile.hpp` → `soft_split_line` / `##soft48g`)
so edges blur and the judgeline shows through. Draw path: white soft plate × `split_slot_color`
tint; tip RGB lerp to white; optional mild additive body glow.

## Root `skins/` keep-list (Light preview)

Required / used: note Top/Bottom/ticks, arrow, sync, stage cover + bottom border, judgment
line, hidden line, Auto judgment + Combo AP digits, `ingame_bg.png`, base split-line color
plates (`Sirius Split Line _*.png`), and this `effects/` tree.

Do **not** keep: Combo FC/Normal, unused Judgment grades, Linear/Flick legacy Sonolus FX,
Transform 1/2 split plates, grid helpers (`_GRID_*`), `_STAGE_MIDDLE`.
