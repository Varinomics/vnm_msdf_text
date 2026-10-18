#pragma once

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <vector>

namespace vnm::msdf_text::detail {

/// Reserve before a transactional append without recopying the prefix for every label.
template<typename T>
void reserve_for_append(std::vector<T>& values, std::size_t additional)
{
    if (additional > values.max_size() - values.size()) {
        throw std::length_error("MSDF text vector exceeds its maximum size");
    }

    const std::size_t required = values.size() + additional;
    if (required <= values.capacity()) {
        return;
    }

    const std::size_t growth = values.capacity() / 2u;
    const std::size_t grown = growth > values.max_size() - values.capacity()
        ? values.max_size()
        : values.capacity() + growth;
    values.reserve(std::max(required, grown));
}

} // namespace vnm::msdf_text::detail
