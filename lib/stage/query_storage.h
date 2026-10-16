#pragma once

#include "pagos/stage/provenance_query.h"
#include <array>
#include <iterator>
#include <llvm/ADT/DenseMap.h>

namespace pagos::stage::provenance::detail {
struct QueryVisit {
    QueryState state;
    std::size_t predecessor;
};
struct QueryRecord {
    std::uint64_t key{}, value{};
};
struct QuerySegment {
    std::uint32_t first{}, count{};
    cache::ObjectRef object;
    std::uint64_t minimum{}, maximum{}; // Sorted index run bounds, tables 4/5.
};
class QueryStorage {
  public:
    using Directory = std::array<std::vector<QuerySegment>, 6>;
    using Sizes = std::array<std::size_t, 6>;
    QueryStorage(cache::TaskStore store, QueryPagingConfig config,
                 cache::Limits limits);
    ~QueryStorage();
    [[nodiscard]] QueryRecord get(unsigned table, std::size_t position) const;
    void append(unsigned table, QueryRecord record);
    [[nodiscard]] std::optional<std::size_t> find(unsigned table,
                                                  std::uint64_t key) const;
    [[nodiscard]] std::size_t size(unsigned table) const;
    [[nodiscard]] std::size_t tail_bytes(unsigned table) const;
    [[nodiscard]] std::size_t cached_bytes() const;
    [[nodiscard]] std::size_t directory_bytes() const;
    [[nodiscard]] std::size_t peak_buffers() const;
    [[nodiscard]] QueryPagingStats stats() const;
    [[nodiscard]] const Directory& directory() const;
    [[nodiscard]] QueryPagingConfig config() const;
    void prepare(); // Seal full tails BEFORE a logical query step.
    void seal(cache::TaskWriter& writer);
    [[nodiscard]] Sizes mark() const;
    void rollback(const Sizes& sizes) noexcept;
    void restore(Directory directory, Sizes sizes);
    void check_store(const cache::TaskStore& store) const;
    void rebind(const cache::TaskStore& store);
    void evict() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
inline QueryRecord pack(Invocation v) {
    return {v.call.value | (std::uint64_t{v.parent} << 32U), v.owner.value};
}
inline QueryRecord pack(QueryVisit v) {
    return {v.state.node.value | (std::uint64_t{v.state.context} << 32U),
            v.predecessor};
}
inline QueryRecord pack(Source v) {
    return {v.node.value | (std::uint64_t{v.context} << 32U), v.site};
}
inline QueryRecord pack(QueryState v) {
    return {v.node.value | (std::uint64_t{v.context} << 32U), 0};
}
template <class T> T unpack(QueryRecord r);
template <> inline Invocation unpack(QueryRecord r) {
    return {{static_cast<std::uint32_t>(r.key)},
            static_cast<std::uint32_t>(r.key >> 32U),
            {static_cast<std::uint32_t>(r.value)}};
}
template <> inline QueryVisit unpack(QueryRecord r) {
    return {{{static_cast<std::uint32_t>(r.key)},
             static_cast<std::uint32_t>(r.key >> 32U)},
            static_cast<std::size_t>(r.value)};
}
template <> inline Source unpack(QueryRecord r) {
    return {{static_cast<std::uint32_t>(r.key)},
            static_cast<std::uint32_t>(r.value),
            static_cast<std::uint32_t>(r.key >> 32U)};
}
template <> inline QueryState unpack(QueryRecord r) {
    return {{static_cast<std::uint32_t>(r.key)},
            static_cast<std::uint32_t>(r.key >> 32U)};
}
// Values, never references into an evictable cache. Only explicit materialize()
// copies a complete table; range iteration remains bounded.
template <class T> class QueryVector {
  public:
    explicit QueryVector(unsigned table) : table_(table) {}
    [[nodiscard]] std::size_t size() const {
        return pages_ ? pages_->size(table_) : memory_.size();
    }
    [[nodiscard]] bool empty() const { return size() == 0; }
    [[nodiscard]] T operator[](std::size_t i) const {
        return pages_ ? unpack<T>(pages_->get(table_, i)) : memory_.at(i);
    }
    void push_back(T value) {
        if (pages_)
            pages_->append(table_, pack(value));
        else
            memory_.push_back(value);
    }
    void clear() { memory_.clear(); }
    [[nodiscard]] std::size_t bytes() const {
        return pages_ ? pages_->tail_bytes(table_)
                      : memory_.capacity() * sizeof(T);
    }
    void attach(std::shared_ptr<QueryStorage> pages, bool import = false) {
        if (import)
            for (const auto& value : memory_)
                pages->append(table_, pack(value));
        std::vector<T>{}.swap(memory_);
        pages_ = std::move(pages);
    }
    void release() {
        pages_.reset();
        std::vector<T>{}.swap(memory_);
    }
    [[nodiscard]] std::span<const T> resident_span() const { return memory_; }
    [[nodiscard]] std::vector<T> materialize() const {
        std::vector<T> values;
        values.reserve(size());
        for (std::size_t i = 0; i < size(); ++i)
            values.push_back((*this)[i]);
        return values;
    }
    struct Iterator {
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using iterator_category = std::input_iterator_tag;
        const QueryVector* owner{};
        std::size_t index{};
        T operator*() const { return (*owner)[index]; }
        Iterator& operator++() {
            ++index;
            return *this;
        }
        Iterator operator++(int) {
            auto copy = *this;
            ++*this;
            return copy;
        }
        bool operator==(const Iterator&) const = default;
    };
    Iterator begin() const { return {this, 0}; }
    Iterator end() const { return {this, size()}; }

  private:
    unsigned table_;
    std::vector<T> memory_;
    std::shared_ptr<QueryStorage> pages_;
};
class QueryIndex {
  public:
    explicit QueryIndex(unsigned table) : table_(table) {}
    [[nodiscard]] std::optional<std::size_t> find(std::uint64_t key) const {
        if (pages_)
            return pages_->find(table_, key);
        const auto found = memory_.find(key);
        return found == memory_.end() ? std::optional<std::size_t>{}
                                      : found->second;
    }
    [[nodiscard]] bool contains(std::uint64_t key) const {
        return find(key).has_value();
    }
    bool insert(std::uint64_t key, std::size_t value) {
        if (pages_) {
            pages_->append(table_, {key, value});
            return true;
        }
        return memory_.try_emplace(key, value).second;
    }
    [[nodiscard]] std::size_t bytes() const {
        return pages_ ? pages_->tail_bytes(table_) : memory_.getMemorySize();
    }
    void attach(std::shared_ptr<QueryStorage> pages) {
        release();
        pages_ = std::move(pages);
    }
    void release() {
        pages_.reset();
        decltype(memory_){}.swap(memory_);
    }

  private:
    unsigned table_;
    llvm::DenseMap<std::uint64_t, std::size_t> memory_;
    std::shared_ptr<QueryStorage> pages_;
};
} // namespace pagos::stage::provenance::detail
