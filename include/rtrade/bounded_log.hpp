#pragma once

#include <cstddef>
#include <iterator>
#include <vector>

namespace rtrade {

// The most recent `capacity` entries of an append-only log, oldest first.
//
// Storage is allocated once, up front, and reused as a ring: appending never
// allocates and never moves the entries already there. When full, each new
// entry replaces the oldest. (The vector this replaced trimmed itself by
// erasing its oldest 2,000 entries from the front, moving the rest every
// time: the cause of the engine's slowest calls. See docs/LATENCY.md.)
template <class T>
class BoundedLog {
public:
  explicit BoundedLog(std::size_t capacity) : slots_(capacity) {}

  void push_back(const T& entry) {
    slots_[(first_ + size_) % slots_.size()] = entry;
    if (size_ < slots_.size()) {
      ++size_;
    } else {
      first_ = (first_ + 1) % slots_.size();
    }
  }

  // Entry `i`, counting from the oldest kept.
  [[nodiscard]] const T& operator[](std::size_t i) const { return slots_[(first_ + i) % slots_.size()]; }
  [[nodiscard]] const T& front() const { return (*this)[0]; }
  [[nodiscard]] const T& back() const { return (*this)[size_ - 1]; }

  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
  [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }

  class const_iterator {
  public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = T;
    using difference_type = std::ptrdiff_t;
    using pointer = const T*;
    using reference = const T&;

    const_iterator() = default;
    const_iterator(const BoundedLog* log, std::size_t at) : log_(log), at_(at) {}
    reference operator*() const { return (*log_)[at_]; }
    pointer operator->() const { return &(*log_)[at_]; }
    const_iterator& operator++() { ++at_; return *this; }
    const_iterator operator++(int) { const_iterator was = *this; ++at_; return was; }
    bool operator==(const const_iterator& other) const { return at_ == other.at_; }

  private:
    const BoundedLog* log_ = nullptr;
    std::size_t at_ = 0;
  };

  [[nodiscard]] const_iterator begin() const { return {this, 0}; }
  [[nodiscard]] const_iterator end() const { return {this, size_}; }

private:
  std::vector<T> slots_;
  std::size_t first_ = 0;  // where the oldest entry is
  std::size_t size_ = 0;
};

} // namespace rtrade
