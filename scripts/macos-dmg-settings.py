# dmgbuild settings — invoked as:
#   dmgbuild -s macos-dmg-settings.py -D app=... -D background=... [-D icon=...] \
#     "WDS Editor" out.dmg
#
# Writes .DS_Store directly (no Finder AppleScript).
# background.png must sit next to background@2x.png so dmgbuild builds a Retina TIFF.

import os

application = defines["app"]  # noqa: F821
background_path = defines["background"]  # noqa: F821
appname = os.path.basename(application)

format = "UDZO"
files = [application]
symlinks = {"Applications": "/Applications"}

# Volume / .dmg icon (Finder sidebar + disk image file).
if "icon" in defines:  # noqa: F821
    icon = defines["icon"]  # noqa: F821

# Logical art size is 720×480. On modern macOS, WindowBounds height is the
# *content* area — matching the background exactly avoids a white strip under
# the art (older recipes added ~40pt for the title bar and caused the gap).
_CONTENT_W = 720
_CONTENT_H = 480

background = background_path
show_status_bar = False
show_tab_view = False
show_toolbar = False
show_pathbar = False
show_sidebar = False
sidebar_width = 0

window_rect = ((260, 100), (_CONTENT_W, _CONTENT_H))
default_view = "icon-view"

icon_size = 128
text_size = 13
label_pos = "bottom"
arrange_by = None  # not arranged — honor icon_locations

# Align with background.png slots (left = app, right = Applications).
icon_locations = {
    appname: (190, 150),
    "Applications": (530, 150),
}

include_icon_view_settings = True
include_list_view_settings = False
