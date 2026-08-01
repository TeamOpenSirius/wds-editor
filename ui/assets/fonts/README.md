Noto Sans SC
============

Bundled UI face for the WDS editor (SIL Open Font License 1.1).

- Source: Noto Sans SC / Source Han Sans
- File: `NotoSansSC-Regular.ttf` (~7 MB, full fontsource Regular coverage)

Why a full face (not an auto-subset)
------------------------------------

A build-time subset that “tracks UI strings” is unreliable: C++ UI text is often
split across literals, built at runtime, or comes from dialogs/settings that a
static scan will miss. Missing a single character then shows blank glyphs unless
a system fallback happens to be present.

Instead we ship a full Regular face. At runtime `FontAtlas` keeps the TTF in
memory and **bakes only the glyphs that are drawn** into the GPU atlas
(`ensure_glyphs` on the paint/measure path). Adding new Chinese UI text needs no
font rebuild and no sync list.

The editor prefers this bundled file; if it is missing, it falls back to system
CJK/Unicode faces.
