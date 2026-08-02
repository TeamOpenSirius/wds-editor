#pragma once

#include <cstdint>
#include <utility>
#include <vector>

namespace wds::chart_editor::detail {

class StartMsAvlIndex {
 public:
  StartMsAvlIndex() = default;
  StartMsAvlIndex(const StartMsAvlIndex&) = delete;
  StartMsAvlIndex& operator=(const StartMsAvlIndex&) = delete;
  StartMsAvlIndex(StartMsAvlIndex&& other) noexcept;
  StartMsAvlIndex& operator=(StartMsAvlIndex&& other) noexcept;
  ~StartMsAvlIndex();

  void clear();
  void insert(int64_t start_ms, int32_t note_id);
  void erase(int64_t start_ms, int32_t note_id);
  void build(const std::vector<std::pair<int64_t, int32_t>>& entries);

  template <typename Fn>
  void for_each_in_range(int64_t lower_inclusive, int64_t upper_inclusive, Fn&& fn) const {
    struct Ctx {
      Fn* fn;
    } ctx{&fn};
    for_each_in_range_impl(
        lower_inclusive, upper_inclusive,
        [](int64_t key, int32_t note_id, void* userdata) {
          auto* c = static_cast<Ctx*>(userdata);
          (*c->fn)(key, note_id);
        },
        &ctx);
  }

  template <typename Fn>
  void for_each_up_to(int64_t upper_inclusive, Fn&& fn) const {
    struct Ctx {
      Fn* fn;
    } ctx{&fn};
    for_each_up_to_impl(
        upper_inclusive,
        [](int64_t key, int32_t note_id, void* userdata) {
          auto* c = static_cast<Ctx*>(userdata);
          (*c->fn)(key, note_id);
        },
        &ctx);
  }

 private:
  struct IdList {
    int32_t inline_ids[4]{};
    uint8_t inline_count = 0;
    int32_t* heap_ids = nullptr;
    uint32_t heap_count = 0;

    bool empty() const noexcept;
    void clear() noexcept;
    void steal_from(IdList& other) noexcept;
    void push(int32_t id);
    void remove(int32_t id);
  };

  struct Node {
    int64_t key = 0;
    IdList ids{};
    Node* left = nullptr;
    Node* right = nullptr;
    int8_t height = 1;
  };

  using VisitFn = void (*)(int64_t key, int32_t note_id, void* userdata);

  static int8_t height_of(const Node* node) noexcept;
  static void update_height(Node* node) noexcept;
  static int8_t balance_factor(const Node* node) noexcept;
  static Node* rotate_left(Node* x) noexcept;
  static Node* rotate_right(Node* x) noexcept;
  static Node* rebalance(Node* node) noexcept;

  Node* insert_node(Node* node, int64_t key, int32_t note_id, bool& tree_grew);
  Node* erase_node(Node* node, int64_t key, int32_t note_id, bool& tree_shrank);
  Node* erase_node_at_key(Node* node, int64_t key, bool& tree_shrank);
  static Node* min_node(Node* node) noexcept;
  static void destroy(Node* node) noexcept;
  static void visit_range(const Node* node, int64_t lower, int64_t upper, VisitFn fn,
                          void* userdata);
  static void visit_up_to(const Node* node, int64_t upper, VisitFn fn, void* userdata);

  void for_each_in_range_impl(int64_t lower_inclusive, int64_t upper_inclusive, VisitFn fn,
                              void* userdata) const;
  void for_each_up_to_impl(int64_t upper_inclusive, VisitFn fn, void* userdata) const;

  Node* root_ = nullptr;
};

}  // namespace wds::chart_editor::detail
