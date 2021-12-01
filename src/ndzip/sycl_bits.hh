#pragma once

#include "gpu_common.hh"

#include <type_traits>

#include <SYCL/sycl.hpp>
#include <ndzip/sycl_encoder.hh>


namespace ndzip::detail::gpu_sycl {

using namespace ndzip::detail::gpu;

inline uint64_t earliest_event_start(const sycl::event &evt) {
    return evt.get_profiling_info<sycl::info::event_profiling::command_start>();
}

inline uint64_t earliest_event_start(const std::vector<sycl::event> &events) {
    uint64_t start = UINT64_MAX;
    for (auto &evt : events) {
        start = std::min(start, evt.get_profiling_info<sycl::info::event_profiling::command_start>());
    }
    return start;
}

inline uint64_t latest_event_end(const sycl::event &evt) {
    return evt.get_profiling_info<sycl::info::event_profiling::command_end>();
}

inline uint64_t latest_event_end(const std::vector<sycl::event> &events) {
    uint64_t end = 0;
    for (auto &evt : events) {
        end = std::max(end, evt.get_profiling_info<sycl::info::event_profiling::command_end>());
    }
    return end;
}

template<typename... Events>
std::tuple<uint64_t, uint64_t, kernel_duration> measure_duration(const Events &...events) {
    auto early = std::min<uint64_t>({earliest_event_start(events)...});
    auto late = std::max<uint64_t>({latest_event_end(events)...});
    return {early, late, kernel_duration{late - early}};
}

template<typename CGF>
auto submit_and_profile(sycl::queue &q, const char *label, CGF &&cgf) {
    if (verbose() && q.has_property<sycl::property::queue::enable_profiling>()) {
        auto evt = q.submit(std::forward<CGF>(cgf));
        auto [early, late, duration] = measure_duration(evt, evt);
        printf("[profile] %8lu %8lu %s: %.3fms\n", early, late, label, duration.count() * 1e-6);
        return evt;
    } else {
        return q.submit(std::forward<CGF>(cgf));
    }
}


template<index_type LocalSize>
class known_size_group : public sycl::group<1> {
  public:
    using sycl::group<1>::group;
    using sycl::group<1>::distribute_for;

    known_size_group(sycl::group<1> grp)  // NOLINT(google-explicit-constructor)
        : sycl::group<1>{grp} {}

    template<typename F>
    [[gnu::always_inline]] void distribute_for(index_type range, F &&f) {
        distribute_for([&](sycl::sub_group sg, sycl::logical_item<1> idx) {
            const index_type num_full_iterations = range / LocalSize;
            const auto tid = static_cast<index_type>(idx.get_local_id(0));

            for (index_type iteration = 0; iteration < num_full_iterations; ++iteration) {
                auto item = iteration * LocalSize + tid;
                invoke_f(f, item, iteration, idx, sg);
            }
            distribute_for_partial_iteration(range, sg, idx, f);
        });
    }

    template<index_type Range, typename F>
    [[gnu::always_inline]] void distribute_for(F &&f) {
        distribute_for([&](sycl::sub_group sg, sycl::logical_item<1> idx) {
            constexpr index_type num_full_iterations = Range / LocalSize;
            const auto tid = static_cast<index_type>(idx.get_local_id(0));

#pragma unroll
            for (index_type iteration = 0; iteration < num_full_iterations; ++iteration) {
                auto item = iteration * LocalSize + tid;
                invoke_f(f, item, iteration, idx, sg);
            }
            distribute_for_partial_iteration(Range, sg, idx, f);
        });
    }

  private:
    template<typename F>
    [[gnu::always_inline]] void
    invoke_f(F &&f, index_type item, index_type iteration, sycl::logical_item<1> idx, sycl::sub_group sg) const {
        if constexpr (std::is_invocable_v<F, index_type, index_type, sycl::logical_item<1>, sycl::sub_group>) {
            f(item, iteration, idx, sg);
        } else if constexpr (std::is_invocable_v<F, index_type, index_type, sycl::logical_item<1>>) {
            f(item, iteration, idx);
        } else if constexpr (std::is_invocable_v<F, index_type, index_type>) {
            f(item, iteration);
        } else {
            f(item);
        }
    }

    template<typename F>
    [[gnu::always_inline]] void
    distribute_for_partial_iteration(index_type range, sycl::sub_group sg, sycl::logical_item<1> idx, F &&f) {
        const index_type num_full_iterations = range / LocalSize;
        const index_type partial_iteration_length = range % LocalSize;
        const auto tid = static_cast<index_type>(idx.get_local_id(0));
        if (tid < partial_iteration_length) {
            auto iteration = num_full_iterations;
            auto item = iteration * LocalSize + tid;
            invoke_f(f, item, iteration, idx, sg);
        }
    }
};


template<index_type LocalSize, typename F>
[[gnu::always_inline]] void
distribute_for(index_type range, known_size_group<LocalSize> group, F &&f) {
    group.distribute_for(range, f);
}

template<index_type Range, index_type LocalSize, typename F>
[[gnu::always_inline]] void
distribute_for(known_size_group<LocalSize> group, F &&f) {
    return group.template distribute_for<Range>(f);
}


template<typename Value, index_type Range, typename Enable=void>
struct inclusive_scan_local_allocation {};

template<typename Value, index_type Range>
struct inclusive_scan_local_allocation<Value, Range, std::enable_if_t<(Range > warp_size)>>
{
    Value memory[div_ceil(Range, warp_size)];
    inclusive_scan_local_allocation<Value, div_ceil(Range, warp_size)> next;
};


template<index_type Range, index_type LocalSize, typename Accessor, typename BinaryOp>
std::enable_if_t<(Range <= warp_size)> inclusive_scan_over_group(known_size_group<LocalSize> grp, Accessor acc,
        inclusive_scan_local_allocation<std::decay_t<decltype(acc[index_type{}])>, Range> &, BinaryOp op) {
    static_assert(LocalSize % warp_size == 0);
    distribute_for<ceil(Range, warp_size)>(grp,
            [&](index_type item, index_type, sycl::logical_item<1>, sycl::sub_group sg) {
                auto a = item < Range ? acc[item] : 0;
                auto b = sycl::inclusive_scan_over_group(sg, a, op);
                if (item < Range) { acc[item] = b; }
            });
}

template<index_type Range, index_type LocalSize, typename Accessor, typename BinaryOp>
std::enable_if_t<(Range > warp_size)> inclusive_scan_over_group(known_size_group<LocalSize> grp, Accessor acc,
        inclusive_scan_local_allocation<std::decay_t<decltype(acc[index_type{}])>, Range> &lm,
        BinaryOp op) {
    static_assert(LocalSize % warp_size == 0);
    using value_type = std::decay_t<decltype(acc[index_type{}])>;

    value_type fine[div_ceil(Range, LocalSize)]; // per-thread
    const auto coarse = lm.memory;
    distribute_for<ceil(Range, warp_size)>(grp,
            [&](index_type item, index_type iteration, sycl::logical_item<1>, sycl::sub_group sg) {
                fine[iteration] = sycl::inclusive_scan_over_group(sg, item < Range ? acc[item] : 0, op);
                if (item % warp_size == warp_size - 1) { coarse[item / warp_size] = fine[iteration]; }
            });
    inclusive_scan_over_group(grp, coarse, lm.next, op);
    distribute_for<Range>(grp, [&](index_type item, index_type iteration) {
        auto value = fine[iteration];
        if (item >= warp_size) { value = op(value, coarse[item / warp_size - 1]); }
        acc[item] = value;
    });
}

template<typename Scalar>
std::vector<sycl::buffer<Scalar>> hierarchical_inclusive_scan_allocate(index_type in_out_buffer_size) {
    constexpr index_type granularity = hierarchical_inclusive_scan_granularity;

    std::vector<sycl::buffer<Scalar>> intermediate_bufs;
    assert(in_out_buffer_size % granularity == 0);  // otherwise we will overrun the in_out buffer bounds

    auto n_elems = in_out_buffer_size;
    while (n_elems > 1) {
        n_elems = div_ceil(n_elems, granularity);
        intermediate_bufs.emplace_back(ceil(n_elems, granularity));
    }

    return intermediate_bufs;
}

template<typename, typename>
class hierarchical_inclusive_scan_reduction_kernel;

template<typename, typename>
class hierarchical_inclusive_scan_expansion_kernel;

template<typename Scalar, typename BinaryOp>
void hierarchical_inclusive_scan(sycl::queue &queue, sycl::buffer<Scalar> &in_out_buffer,
        std::vector<sycl::buffer<Scalar>> &intermediate_bufs, BinaryOp op = {}) {
    using sam = sycl::access::mode;

    constexpr index_type granularity = hierarchical_inclusive_scan_granularity;
    constexpr index_type local_size = 256;

    for (index_type i = 0; i < intermediate_bufs.size(); ++i) {
        auto &big_buffer = i > 0 ? intermediate_bufs[i - 1] : in_out_buffer;
        auto &small_buffer = intermediate_bufs[i];
        const auto group_range = sycl::range<1>{div_ceil(static_cast<index_type>(big_buffer.get_count()), granularity)};
        const auto local_range = sycl::range<1>{local_size};

        char label[50];
        sprintf(label, "hierarchical_inclusive_scan reduce %u", i);
        submit_and_profile(queue, label, [&](sycl::handler &cgh) {
            auto big_acc = big_buffer.template get_access<sam::read_write>(cgh);
            auto small_acc = small_buffer.template get_access<sam::discard_write>(cgh);
            sycl::local_accessor<inclusive_scan_local_allocation<Scalar, granularity>> lm{1, cgh};
            cgh.parallel<hierarchical_inclusive_scan_reduction_kernel<Scalar, BinaryOp>>(group_range, local_range,
                    [big_acc, small_acc, lm, op](known_size_group<local_size> grp, sycl::physical_item<1>) {
                        auto group_index = static_cast<index_type>(grp.get_group_id(0));
                        Scalar *big = &big_acc[group_index * granularity];
                        Scalar &small = small_acc[group_index];
                        inclusive_scan_over_group(grp, big, lm[0], op);
                        // TODO unnecessary GM read from big -- maybe return final sum
                        //  in the last item? Or allow additional accessor in inclusive_scan?
                        grp.single_item([&] { small = big[granularity - 1]; });
                    });
        });
    }

    for (index_type i = 1; i < intermediate_bufs.size(); ++i) {
        auto ii = static_cast<index_type>(intermediate_bufs.size()) - 1 - i;
        auto &small_buffer = intermediate_bufs[ii];
        auto &big_buffer = ii > 0 ? intermediate_bufs[ii - 1] : in_out_buffer;
        const auto group_range
                = sycl::range<1>{div_ceil(static_cast<index_type>(big_buffer.get_count()), granularity) - 1};
        const auto local_range = sycl::range<1>{local_size};

        char label[50];
        sprintf(label, "hierarchical_inclusive_scan expand %u", ii);
        submit_and_profile(queue, label, [&](sycl::handler &cgh) {
            auto small_acc = small_buffer.template get_access<sam::read>(cgh);
            auto big_acc = big_buffer.template get_access<sam::read_write>(cgh);
            cgh.parallel<hierarchical_inclusive_scan_expansion_kernel<Scalar, BinaryOp>>(group_range, local_range,
                    [small_acc, big_acc, op](known_size_group<local_size> grp, sycl::physical_item<1>) {
                        auto group_index = static_cast<index_type>(grp.get_group_id(0));
                        Scalar *big = &big_acc[(group_index + 1) * granularity];
                        Scalar small = small_acc[group_index];
                        distribute_for(granularity, grp, [&](index_type i) { big[i] = op(big[i], small); });
                    });
        });
    }
}

template<unsigned Dims, typename U, typename T>
U extent_cast(const T &e) {
    U v;
    for (unsigned i = 0; i < Dims; ++i) {
        v[i] = e[i];
    }
    return v;
}

template<typename U, unsigned Dims>
U extent_cast(const extent<Dims> &e) {
    return extent_cast<Dims, U>(e);
}

template<typename T, int Dims>
T extent_cast(const sycl::range<Dims> &r) {
    return extent_cast<static_cast<unsigned>(Dims), T>(r);
}

template<typename T, int Dims>
T extent_cast(const sycl::id<Dims> &r) {
    return extent_cast<static_cast<unsigned>(Dims), T>(r);
}

}  // namespace ndzip::detail::gpu_sycl
