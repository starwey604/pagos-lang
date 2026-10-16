#include "pagos/hir/runtime_path.h"

#include <utility>

namespace pagos::hir {

struct RuntimePath::Step {
    std::shared_ptr<Step> previous;
    std::string text;

    // A long unshared suffix must not recurse through shared_ptr destructors.
    // No outside weak references exist. Only uniquely owned ancestors are
    // detached here; shared prefixes remain immutable and live for their
    // owners.
    ~Step() {
        while (previous && previous.use_count() == 1) {
            auto next = std::move(previous->previous);
            previous.reset();
            previous = std::move(next);
        }
    }
};

RuntimePath::RuntimePath(RuntimePath&& other) noexcept
    : tail_(std::move(other.tail_)), size_(std::exchange(other.size_, 0)) {}

RuntimePath& RuntimePath::operator=(RuntimePath&& other) noexcept {
    if (this != &other) {
        tail_ = std::move(other.tail_);
        size_ = std::exchange(other.size_, 0);
    }
    return *this;
}

RuntimePath::RuntimePath(std::initializer_list<std::string> steps) {
    for (const auto& step : steps)
        push_back(step);
}

RuntimePath::RuntimePath(std::vector<std::string> steps) {
    for (auto& step : steps)
        push_back(std::move(step));
}

void RuntimePath::push_back(std::string step) {
    auto next = std::make_shared<Step>();
    next->previous = tail_;
    next->text = std::move(step);
    tail_ = std::move(next);
    ++size_;
}

void RuntimePath::append(const RuntimePath& suffix) {
    // Materialize before mutation, including self-append. Parameter
    // substitution concatenates diagnostic paths; ordinary propagation needs
    // only push_back.
    for (auto& step : suffix.to_vector())
        push_back(std::move(step));
}

const std::string& RuntimePath::back() const { return tail_->text; }

std::vector<std::string> RuntimePath::to_vector() const {
    std::vector<std::string> result(size_);
    auto index = size_;
    for (auto step = tail_.get(); step; step = step->previous.get())
        result[--index] = step->text;
    return result;
}

void RuntimePath::Accounting::add(const RuntimePath& path) {
    ++paths;
    logical_steps += path.size();
    for (auto step = path.tail_.get(); step; step = step->previous.get()) {
        if (!seen_.insert(step).second)
            break;
        ++unique_steps;
        bytes += sizeof(Step);
        if (step->text.capacity() > std::string{}.capacity())
            bytes += step->text.capacity() + 1;
    }
}

} // namespace pagos::hir
