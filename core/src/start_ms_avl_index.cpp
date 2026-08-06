#include <wds/core/detail/start_ms_avl_index.hpp>

#include <algorithm>
#include <cstring>
#include <new>
#include <utility>

namespace wds::chart_editor::detail {

namespace {

template <typename IdListT>
void foreach_id(const IdListT& list,
                void (*fn)(int64_t key, int32_t id, void* userdata), int64_t key, void* userdata) {
  for (uint8_t i = 0; i < list.inline_count; ++i) {
    fn(key, list.inline_ids[i], userdata);
  }
  for (uint32_t i = 0; i < list.heap_count; ++i) {
    fn(key, list.heap_ids[i], userdata);
  }
}

}  // namespace

bool StartMsAvlIndex::IdList::empty() const noexcept {
  return inline_count == 0 && heap_count == 0;
}

void StartMsAvlIndex::IdList::clear() noexcept {
  inline_count = 0;
  heap_count = 0;
  delete[] heap_ids;
  heap_ids = nullptr;
}

void StartMsAvlIndex::IdList::steal_from(IdList& other) noexcept {
  clear();
  inline_count = other.inline_count;
  std::memcpy(inline_ids, other.inline_ids, sizeof(inline_ids));
  heap_ids = other.heap_ids;
  heap_count = other.heap_count;
  other.inline_count = 0;
  other.heap_count = 0;
  other.heap_ids = nullptr;
}

void StartMsAvlIndex::IdList::push(int32_t id) {
  for (uint8_t i = 0; i < inline_count; ++i) {
    if (inline_ids[i] == id) {
      return;
    }
  }
  for (uint32_t i = 0; i < heap_count; ++i) {
    if (heap_ids[i] == id) {
      return;
    }
  }

  if (inline_count < 4) {
    inline_ids[inline_count++] = id;
    return;
  }

  if (heap_count == 0) {
    if (heap_ids != nullptr) {
      // Reuse previous heap allocation after remove emptied it.
    } else {
      heap_ids = new int32_t[8];
    }
    std::memcpy(heap_ids, inline_ids, sizeof(inline_ids));
    heap_count = 4;
    inline_count = 0;
  } else if ((heap_count & 7) == 0) {
    const uint32_t new_cap = heap_count * 2;
    int32_t* next = new int32_t[new_cap];
    std::memcpy(next, heap_ids, sizeof(int32_t) * heap_count);
    delete[] heap_ids;
    heap_ids = next;
  }

  heap_ids[heap_count++] = id;
}

void StartMsAvlIndex::IdList::remove(int32_t id) {
  for (uint8_t i = 0; i < inline_count; ++i) {
    if (inline_ids[i] == id) {
      inline_ids[i] = inline_ids[inline_count - 1];
      --inline_count;
      return;
    }
  }

  for (uint32_t i = 0; i < heap_count; ++i) {
    if (heap_ids[i] == id) {
      heap_ids[i] = heap_ids[heap_count - 1];
      --heap_count;
      return;
    }
  }
}

StartMsAvlIndex::StartMsAvlIndex(StartMsAvlIndex&& other) noexcept : root_(other.root_) {
  other.root_ = nullptr;
}

StartMsAvlIndex& StartMsAvlIndex::operator=(StartMsAvlIndex&& other) noexcept {
  if (this != &other) {
    clear();
    root_ = other.root_;
    other.root_ = nullptr;
  }
  return *this;
}

StartMsAvlIndex::~StartMsAvlIndex() { clear(); }

void StartMsAvlIndex::clear() {
  destroy(root_);
  root_ = nullptr;
}

int8_t StartMsAvlIndex::height_of(const Node* node) noexcept {
  return node ? node->height : 0;
}

void StartMsAvlIndex::update_height(Node* node) noexcept {
  const int8_t hl = height_of(node->left);
  const int8_t hr = height_of(node->right);
  node->height = static_cast<int8_t>(1 + (hl > hr ? hl : hr));
}

int8_t StartMsAvlIndex::balance_factor(const Node* node) noexcept {
  return static_cast<int8_t>(height_of(node->left) - height_of(node->right));
}

StartMsAvlIndex::Node* StartMsAvlIndex::rotate_left(Node* x) noexcept {
  Node* y = x->right;
  x->right = y->left;
  y->left = x;
  update_height(x);
  update_height(y);
  return y;
}

StartMsAvlIndex::Node* StartMsAvlIndex::rotate_right(Node* y) noexcept {
  Node* x = y->left;
  y->left = x->right;
  x->right = y;
  update_height(y);
  update_height(x);
  return x;
}

StartMsAvlIndex::Node* StartMsAvlIndex::rebalance(Node* node) noexcept {
  update_height(node);
  const int8_t bf = balance_factor(node);

  if (bf > 1) {
    if (balance_factor(node->left) < 0) {
      node->left = rotate_left(node->left);
    }
    return rotate_right(node);
  }

  if (bf < -1) {
    if (balance_factor(node->right) > 0) {
      node->right = rotate_right(node->right);
    }
    return rotate_left(node);
  }

  return node;
}

StartMsAvlIndex::Node* StartMsAvlIndex::insert_node(Node* node, int64_t key, int32_t note_id,
                                                    bool& tree_grew) {
  if (!node) {
    tree_grew = true;
    Node* created = new Node();
    created->key = key;
    created->ids.push(note_id);
    return created;
  }

  if (key < node->key) {
    node->left = insert_node(node->left, key, note_id, tree_grew);
  } else if (key > node->key) {
    node->right = insert_node(node->right, key, note_id, tree_grew);
  } else {
    node->ids.push(note_id);
    tree_grew = false;
    return node;
  }

  return tree_grew ? rebalance(node) : node;
}

StartMsAvlIndex::Node* StartMsAvlIndex::min_node(Node* node) noexcept {
  while (node && node->left) {
    node = node->left;
  }
  return node;
}

StartMsAvlIndex::Node* StartMsAvlIndex::erase_node_at_key(Node* node, int64_t key,
                                                          bool& tree_shrank) {
  if (!node) {
    tree_shrank = false;
    return nullptr;
  }

  if (key < node->key) {
    node->left = erase_node_at_key(node->left, key, tree_shrank);
  } else if (key > node->key) {
    node->right = erase_node_at_key(node->right, key, tree_shrank);
  } else {
    tree_shrank = true;
    if (!node->left || !node->right) {
      Node* child = node->left ? node->left : node->right;
      node->ids.clear();
      delete node;
      return child;
    }

    Node* successor = min_node(node->right);
    node->key = successor->key;
    node->ids.clear();
    node->ids.steal_from(successor->ids);
    node->right = erase_node_at_key(node->right, successor->key, tree_shrank);
    tree_shrank = true;
  }

  return tree_shrank ? rebalance(node) : node;
}

StartMsAvlIndex::Node* StartMsAvlIndex::erase_node(Node* node, int64_t key, int32_t note_id,
                                                   bool& tree_shrank) {
  if (!node) {
    tree_shrank = false;
    return nullptr;
  }

  if (key < node->key) {
    node->left = erase_node(node->left, key, note_id, tree_shrank);
  } else if (key > node->key) {
    node->right = erase_node(node->right, key, note_id, tree_shrank);
  } else {
    node->ids.remove(note_id);
    if (!node->ids.empty()) {
      tree_shrank = false;
      return node;
    }
    return erase_node_at_key(node, key, tree_shrank);
  }

  return tree_shrank ? rebalance(node) : node;
}

void StartMsAvlIndex::destroy(Node* node) noexcept {
  if (!node) {
    return;
  }
  destroy(node->left);
  destroy(node->right);
  node->ids.clear();
  delete node;
}

void StartMsAvlIndex::visit_range(const Node* node, int64_t lower, int64_t upper, VisitFn fn,
                                  void* userdata) {
  if (!node) {
    return;
  }

  if (node->key > lower) {
    visit_range(node->left, lower, upper, fn, userdata);
  }

  if (node->key >= lower && node->key <= upper) {
    foreach_id(node->ids, fn, node->key, userdata);
  }

  if (node->key < upper) {
    visit_range(node->right, lower, upper, fn, userdata);
  }
}

void StartMsAvlIndex::visit_up_to(const Node* node, int64_t upper, VisitFn fn, void* userdata) {
  if (!node) {
    return;
  }

  if (node->key <= upper) {
    visit_up_to(node->left, upper, fn, userdata);
    foreach_id(node->ids, fn, node->key, userdata);
    visit_up_to(node->right, upper, fn, userdata);
  } else {
    visit_up_to(node->left, upper, fn, userdata);
  }
}

void StartMsAvlIndex::insert(int64_t start_ms, int32_t note_id) {
  bool grew = false;
  root_ = insert_node(root_, start_ms, note_id, grew);
}

void StartMsAvlIndex::erase(int64_t start_ms, int32_t note_id) {
  bool shrank = false;
  root_ = erase_node(root_, start_ms, note_id, shrank);
}

void StartMsAvlIndex::build(const std::vector<std::pair<int64_t, int32_t>>& entries) {
  clear();
  for (const auto& entry : entries) {
    insert(entry.first, entry.second);
  }
}

void StartMsAvlIndex::for_each_in_range_impl(int64_t lower_inclusive, int64_t upper_inclusive,
                                             VisitFn fn, void* userdata) const {
  visit_range(root_, lower_inclusive, upper_inclusive, fn, userdata);
}

void StartMsAvlIndex::for_each_up_to_impl(int64_t upper_inclusive, VisitFn fn,
                                          void* userdata) const {
  visit_up_to(root_, upper_inclusive, fn, userdata);
}

}  // namespace wds::chart_editor::detail
