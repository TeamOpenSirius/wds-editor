#include <wds/core/core.hpp>

#include <cstdio>

int main() {
  using namespace wds::chart_editor;

  ChartEditorEngine engine;
  engine.set_snapshot_callback([](const PreviewSnapshot& snapshot) {
    std::printf("[preview] t=%lld lanes=%d notes=%zu splits=%zu\n",
                static_cast<long long>(snapshot.timeline_ms), snapshot.active_lane_count,
                snapshot.notes.size(), snapshot.split_lanes.size());
  });

  NotationNote tap;
  tap.start_tick = 480;
  tap.lane = 2;
  tap.width = 1;
  tap.note_type = NoteType::Normal;
  const int32_t tap_id = engine.add_note(tap);
  std::printf("added tap id=%d\n", tap_id);

  NotationNote split;
  split.start_tick = 0;
  split.end_tick = 1920;
  split.lane = 0;
  split.width = 6;
  split.gimmick_type = GimmickType::Split3;
  const int32_t split_id = engine.add_note(split);
  std::printf("added split id=%d\n", split_id);

  engine.seek(0);
  engine.play();
  for (int i = 0; i < 5; ++i) {
    engine.tick(16);
  }

  engine.seek(0);
  engine.rebuild_snapshot();

  const auto save = engine.save_to_file("example_chart.wdschart");
  if (save.error != SerializeError::Ok) {
    std::printf("save failed: %s\n", save.message.c_str());
    return 1;
  }

  std::printf("saved chart with %zu notes, ids 0..%zu\n", engine.document().notes().size(),
              engine.document().notes().empty() ? 0 : engine.document().notes().size() - 1);
  for (const auto& note : engine.document().notes()) {
    std::printf("  note id=%d start_tick=%d lane=%d gimmick=%d\n", note.id, note.start_tick,
                note.lane, static_cast<int>(note.gimmick_type));
  }

  return 0;
}
