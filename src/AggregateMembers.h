#pragma once

// FORMAT-2026-09 -- ROADMAP.md. Counts the members of an aggregate at compile
// time.
//
// C++ cannot enumerate a struct's members, so every hand-written list of
// them -- a serializer, a reader, a field-by-field copy -- silently stops
// covering the struct the day someone adds a member. QC-2026-09 A7 is what
// that costs: DocumentClip gained its audio gain and fades, the journal's
// exact-track format did not, and undoing from a reloaded journal reset
// them without an error. `static_assert(AggregateMemberCount<T>::value == N)`
// next to such a list turns the next added member into a compile error at
// the list that has to change.
//
// The count is the largest N for which T{a1, ..., aN} compiles when every
// ai converts to anything: aggregate initialization accepts fewer
// initializers than members, never more. It holds for the plain structs of
// Document.h and Operations.h, which have no base classes, C arrays or
// user-declared constructors. A struct that gains a constructor stops being
// an aggregate, stops counting, and trips the assertion that uses it.

#include <cstddef>
#include <cstdlib>
#include <type_traits>
#include <utility>

namespace aggregate_members {

// Converts to any member type. Only ever named in unevaluated operands; the
// body exists because instantiating std::optional's constexpr converting
// constructor makes clang want a definition, never because it can run.
struct AnyMember {
    template <typename T>
    operator T() const {
        std::abort();
    }
};

template <typename T, typename Void, typename... Members>
struct BraceInitializable : std::false_type {};

template <typename T, typename... Members>
struct BraceInitializable<
    T, std::void_t<decltype(T{std::declval<Members>()...})>, Members...>
    : std::true_type {};

template <typename T, typename... Members>
struct Count : std::conditional_t<
                   BraceInitializable<T, void, Members..., AnyMember>::value,
                   Count<T, Members..., AnyMember>,
                   std::integral_constant<size_t, sizeof...(Members)>> {};

}  // namespace aggregate_members

template <typename T>
struct AggregateMemberCount : aggregate_members::Count<T> {};
