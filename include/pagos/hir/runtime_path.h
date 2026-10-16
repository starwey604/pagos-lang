#pragma once

#include <cstddef>
#include <initializer_list>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace pagos::hir {

// Persistent diagnostic path. Copies share immutable prefixes; appending never
// changes a path held by another expression. Flatten only at an output
// boundary.
class RuntimePath {
    struct Step;

  public:
    RuntimePath() = default;
    RuntimePath(const RuntimePath&) = default;
    RuntimePath& operator=(const RuntimePath&) = default;
    RuntimePath(RuntimePath&& other) noexcept;
    RuntimePath& operator=(RuntimePath&& other) noexcept;
    RuntimePath(std::initializer_list<std::string> steps);
    explicit RuntimePath(std::vector<std::string> steps);
    void push_back(std::string step);
    void append(const RuntimePath& suffix);
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] const std::string& back() const;
    [[nodiscard]] std::vector<std::string> to_vector() const;

    // Opt-in retained-storage census. Add every live path; prefixes are counted
    // once. Excludes shared_ptr control blocks, allocator overhead and the
    // census itself. bytes includes Step objects and heap string capacity, not
    // handles.
    struct Accounting {
        void add(const RuntimePath& path);
        std::size_t paths{}, logical_steps{}, unique_steps{}, bytes{};

      private:
        std::unordered_set<const Step*> seen_;
    };

  private:
    std::shared_ptr<Step> tail_;
    std::size_t size_{};
};

} // namespace pagos::hir
