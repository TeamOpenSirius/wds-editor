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

Source: `wds-resources/.../Texture2D/BombEffectDefault_7.png` + `Sprite/BombEffectDefault_10.asset`.

## Split lines

Light mode draws a procedural soft beam (`soft_split_line` × official LineColor). Do **not**
ship `Sirius Split Line _*.png`, `effects/split/`, or Transform 1/2 wipe plates — they are
not loaded. Appear/disappear windows match official fadeIn 1000ms / fadeOut 300ms via
`PreviewVisualConfig`.

## Root `skins/` keep-list (Light preview)

Required / used: note Top/Bottom/ticks, arrow, sync, official judgment
(`img_ingame_judgment_area3.png`), official start-line plate
(`img_game_common_start_line_500.png`), official lane border
(`img_ingame_lane_border2.png`), Auto judgment + Combo AP digits, `ingame_bg.png`,
and this `effects/bomb/` tree.

Do **not** keep: Combo FC/Normal, unused Judgment grades, Linear/Flick legacy Sonolus FX,
split-line PNG plates (root or `effects/split/`), Transform 1/2, unused start-line
heights (`img_game_common_start_line_{72..400}.png`), grid helpers (`_GRID_*`),
`_STAGE_*` covers, `Sirius Hidden Line.png`, `_JUDGMENT_LINE.png`.
