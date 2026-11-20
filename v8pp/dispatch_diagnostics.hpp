#pragma once

#include <cstdint>

#include "v8pp/config.hpp"

namespace v8pp::detail {

struct dispatch_counts
{
	std::uint64_t receiver_checks = 0;
	std::uint64_t expected_class_lookups = 0;
	std::uint64_t derived_registry_probes = 0;
	std::uint64_t shared_owner_copies = 0;
	std::uint64_t string_conversions = 0;
	std::uint64_t checked_member_entries = 0;
	std::uint64_t optimized_member_entries = 0;
};

#if V8PP_ENABLE_DISPATCH_DIAGNOSTICS
inline thread_local dispatch_counts current_dispatch_counts;

inline void reset_dispatch_counts() noexcept
{ current_dispatch_counts = {}; }
inline dispatch_counts read_dispatch_counts() noexcept
{ return current_dispatch_counts; }
#endif

inline void count_dispatch(std::uint64_t dispatch_counts::* counter) noexcept
{
#if V8PP_ENABLE_DISPATCH_DIAGNOSTICS
	++(current_dispatch_counts.*counter);
#else
	(void)counter;
#endif
}

} // namespace v8pp::detail
