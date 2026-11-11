#pragma once

#include <visionpro/core/diagnostic.hpp>

#include <variant>

namespace visionpro::core {

// Expected failures contain a Diagnostic, never a sentinel value or log text.
// Inspect with get_if/holds_alternative before accessing the selected branch.
// Allocation failures and other unexpected exceptions are not swallowed here.
template <class T>
using Result = std::variant<T, Diagnostic>;

} // namespace visionpro::core
