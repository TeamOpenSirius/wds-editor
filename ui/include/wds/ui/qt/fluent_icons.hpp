#pragma once

#include <QIcon>
#include <QString>

namespace wds::ui {

// Segoe Fluent Icons glyph codepoints (Private Use Area). Names mirror the
// official Fluent icon font. Only the ones the editor uses are listed.
namespace fluent {
constexpr char32_t OpenFolder = 0xE838;
constexpr char32_t Save = 0xE74E;
constexpr char32_t Import = 0xE8B5;   // "ImportAll"
constexpr char32_t Export = 0xEDE1;   // "Export"
constexpr char32_t Music = 0xE8D6;    // "MusicInfo"
constexpr char32_t Line = 0xE9E9;     // curve/line "InkingTool"-ish
constexpr char32_t Curve = 0xF18D;    // "Line" style
constexpr char32_t Checklist = 0xE9D5;  // "Checklist"
constexpr char32_t Settings = 0xE713;
constexpr char32_t Undo = 0xE7A7;
constexpr char32_t Redo = 0xE7A6;
constexpr char32_t Play = 0xE768;
constexpr char32_t Pause = 0xE769;
constexpr char32_t Previous = 0xE892;   // "回到开头"
constexpr char32_t Add = 0xE710;
constexpr char32_t Info = 0xE946;       // about
constexpr char32_t Volume = 0xE767;
constexpr char32_t Speed = 0xEC4A;      // "Speed"/timer-ish
constexpr char32_t Files = 0xE8B7;      // file page
constexpr char32_t Audio = 0xE8D6;
constexpr char32_t Keyboard = 0xE765;
constexpr char32_t Display = 0xE7F4;    // "TVMonitor"
constexpr char32_t Ruler = 0xE799;      // width/zoom
constexpr char32_t Shield = 0xEA18;     // privacy
constexpr char32_t Grid = 0xF0E2;       // subdivisions
constexpr char32_t Timeline = 0xE81C;   // "Street"/timeline-ish
}  // namespace fluent

// Loads the bundled Segoe Fluent Icons font. Returns the family name, or empty
// if the font is missing. Call once at startup.
QString load_fluent_font(const QString& font_path);

// Renders a Fluent glyph to a themed QIcon. `color` defaults to the current
// palette text color when invalid.
QIcon fluent_icon(char32_t glyph, const QColor& color = QColor(), int px = 20);

}  // namespace wds::ui
