#pragma once

namespace wds::ui {

// Edit-area sprite layering. Pipeline has depthTest on / depthWrite off, so
// visual order follows draw order — ChartEditRenderer draws layers sorted by
// these depths (lower first). Adjusting a field directly changes stacking.
//
// All values must stay in (0, 1]: Vulkan viewport clips z outside [0, 1].
// Defaults: hold body < flat notes < mid-stars < arrows < ghosts.
struct EditDrawDepthConfig {
  float judgeline = 0.40f;
  float hold_body = 0.50f;        // Hold connection ribbon
  float note = 0.70f;             // Hold heads/tails, taps, flicks
  float mid_star = 0.75f;         // Sound / ScratchSound ticks
  float flick_arrow = 0.80f;      // Scratch / flick arrows on top of note body
  float ghost_hold_body = 0.85f;
  float ghost_note = 0.90f;
  float ghost_mid_star = 0.92f;
  float ghost_flick_arrow = 0.95f;

  // Within one layer: z -= start_tick * tick_bias (stable, tiny separation).
  float tick_bias = 1e-6f;
};

}  // namespace wds::ui
