#ifndef C1BED09D_40CC_4EA1_B687_38A5BCC31907
#define C1BED09D_40CC_4EA1_B687_38A5BCC31907

// Copyright © Conor Williams <conorwilliams@outlook.com>

// SPDX-License-Identifier: MPL-2.0

// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <algorithm>
#include <atomic>
#include <bit>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <thread>

#include "libfork/core.hpp"

#include "libfork/schedule/contexts.hpp"
#include "libfork/schedule/event_count.hpp"
#include "libfork/schedule/numa.hpp"
#include "libfork/schedule/random.hpp"

/**
 * @file lazy_pool.hpp
 *
 * @brief A work-stealing thread pool where threads sleep when idle.
 */

namespace lf {

namespace impl {

static constexpr std::memory_order acquire = std::memory_order_acquire;
static constexpr std::memory_order acq_rel = std::memory_order_acq_rel;
static constexpr std::memory_order release = std::memory_order_release;

static constexpr std::uint64_t k_thieve = 1;
static constexpr std::uint64_t k_active = k_thieve << 32U;

static constexpr std::uint64_t k_thieve_mask = (k_active - 1);
static constexpr std::uint64_t k_active_mask = ~k_thieve_mask;

/**
 * @brief Need to overload submit to add notifications.
 */
class lazy_context : public numa_worker_context<lazy_context> {
 public:
  struct remote_atomics {
    /**
     * Effect:
     *
     * T <- T - 1
     * S <- S
     * A <- A + 1
     *
     * A is now guaranteed to be greater than 0, if we were the last thief we try to wake someone.
     *
     * Then we do the task.
     *
     * Once we are done we perform:
     *
     * T <- T + 1
     * S <- S
     * A <- A - 1
     *
     * This never invalidates the invariant.
     *
     *
     * Overall effect: thief->active, do the work, active->thief.
     */
    template <typename Handle>
      requires std::same_as<Handle, task_h<lazy_context>> || std::same_as<Handle, intruded_h<lazy_context>>
    void thief_round_trip(Handle *handle) noexcept {

      auto prev_thieves = dual_count.fetch_add(k_active - k_thieve, acq_rel) & k_thieve_mask;

      if (prev_thieves == 1) {
        LF_LOG("The last thief wakes someone up");
        notifier.notify_one();
      }

      if constexpr (std::same_as<Handle, intruded_h<lazy_context>>) {
        for_each(handle, [](submit_h<lazy_context> *submitted) LF_STATIC_CALL noexcept {
          resume(submitted);
        });
      } else {
        resume(handle);
      }

      dual_count.fetch_sub(k_active - k_thieve, acq_rel);
    }

    alignas(k_cache_line) std::atomic_uint64_t dual_count = 0;
    alignas(k_cache_line) std::atomic_flag stop;
    alignas(k_cache_line) event_count notifier;
  };

  // ---------------------------------------------------------------------- //

  /**
   * @brief Submissions to the `lazy_pool` are very noisy (everyone wakes up).
   */
  auto submit(intruded_h<lazy_context> *node) noexcept -> void {
    numa_worker_context::submit(node);
    m_atomics->notifier.notify_all();
  }

  // ---------------------------------------------------------------------- //

  lazy_context(std::size_t n, xoshiro &rng, std::shared_ptr<remote_atomics> atomics)
      : numa_worker_context{n, rng},
        m_atomics(std::move(atomics)) {}

  static auto work(numa_topology::numa_node<lazy_context> node) {

    // ---- Initialization ---- //

    std::shared_ptr my_context = node.neighbors.front().front();

    LF_ASSERT(my_context.get());

    worker_init(my_context.get());

    impl::defer at_exit = [&]() noexcept {
      worker_finalize(my_context.get());
    };

    my_context->init_numa_and_bind(node);

    /**
     * Invariant we want to uphold:
     *
     *  If there is an active task their is always: [a thief] OR [no sleeping].
     *
     * Let:
     *  T = number of thieves
     *  S = number of sleeping threads
     *  A = number of active threads
     *
     * Invariant: *** if (A > 0) then (T >= 1 OR S == 0) ***
     *
     * Lemma 1: Promoting an S -> T guarantees that the invariant is upheld.
     *
     * Proof 1:
     *  Case S != 0, then T -> T + 1, hence T > 0 hence invariant maintained.
     *  Case S == 0, then invariant is already maintained.
     */

  wake_up:
    /**
     * Invariant maintained by Lemma 1 regardless if this is a wake up (S <- S - 1) or join (S <- S).
     */
    my_context->m_atomics->dual_count.fetch_add(k_thieve, release);

  continue_as_thief:
    /**
     * First we handle the fast path (work to do) before touching the notifier.
     */
    if (auto *submission = my_context->try_get_submitted()) {
      my_context->m_atomics->thief_round_trip(submission);
      goto continue_as_thief;
    }
    if (auto *stolen = my_context->try_steal()) {
      my_context->m_atomics->thief_round_trip(stolen);
      goto continue_as_thief;
    }

    /**
     * Now we are going to try and sleep if the conditions are correct.
     *
     * Event count pattern:
     *
     *    key <- prepare_wait()
     *
     *    Check condition for sleep:
     *      - We have no private work.
     *      - We are not the watch dog.
     *      - The scheduler has not stopped.
     *
     *    Commit/cancel wait on key.
     */

    auto key = my_context->m_atomics->notifier.prepare_wait();

    if (auto *submission = my_context->try_get_submitted()) {
      // Check our private **before** `stop`.
      my_context->m_atomics->notifier.cancel_wait();
      my_context->m_atomics->thief_round_trip(submission);
      goto continue_as_thief;
    }

    if (my_context->m_atomics->stop.test(acquire)) {
      // A stop has been requested, we will honor it under the assumption
      // that the requester has ensured that everyone is done. We cannot check
      // this i.e it is possible a thread that just signaled the master thread
      // is still `active` but act stalled.
      my_context->m_atomics->notifier.cancel_wait();
      // We leave a "ghost thief" here e.g. don't bother to reduce the counter,
      // This is fine because no-one can sleep now that the stop flag is set.
      return;
    }

    /**
     * Try:
     *
     * T <- T - 1
     * S <- S + 1
     * A <- A
     *
     * If new T == 0 and A > 0 then wake self immediately i.e:
     *
     * T <- T + 1
     * S <- S - 1
     * A <- A
     *
     * If we return true then we are safe to sleep, otherwise we must stay awake.
     */

    auto prev_dual = my_context->m_atomics->dual_count.fetch_sub(k_thieve, acq_rel);

    // We are now registered as a sleeping thread and may have broken the invariant.

    auto prev_thieves = prev_dual & k_thieve_mask;
    auto prev_actives = prev_dual & k_active_mask; // Again only need 0 or non-zero.

    if (prev_thieves == 1 && prev_actives != 0) {
      // Restore the invariant.
      goto wake_up;
    }

    LF_LOG("Goes to sleep");

    // We are safe to sleep.
    my_context->m_atomics->notifier.wait(key);
    // Note, this could be a spurious wakeup, that doesn't matter because we will just loop around.
    goto wake_up;
  }

 private:
  std::shared_ptr<remote_atomics> m_atomics;
};

} // namespace impl

/**
 * @brief A scheduler based on a [An Efficient Work-Stealing Scheduler for Task Dependency
 * Graph](https://doi.org/10.1109/icpads51040.2020.00018)
 *
 * This pool sleeps workers which cannot find any work, as such it should be the default choice for most
 * use cases. Additionally (if an installation of `hwloc` was found) this pool is NUMA aware.
 */
class lazy_pool {
 public:
  using context_type = impl::lazy_context;

 private:
  using remote = typename context_type::remote_atomics;

  std::shared_ptr<remote> m_atomics = std::make_shared<remote>();
  xoshiro m_rng{seed, std::random_device{}};
  std::uniform_int_distribution<std::size_t> m_dist;
  std::vector<std::shared_ptr<context_type>> m_contexts;
  std::vector<std::thread> m_workers;

  // Request all threads to stop, wake them up and then call join.
  auto clean_up() noexcept -> void {
    LF_LOG("Requesting a stop");

    // Set conditions for workers to stop.
    m_atomics->stop.test_and_set(std::memory_order_release);
    m_atomics->notifier.notify_all();

    for (auto &worker : m_workers) {
      worker.join();
    }
  }

 public:
  /**
   * @brief Schedule a task for execution.
   */
  auto schedule(intruded_h<context_type> *node) noexcept { m_contexts[m_dist(m_rng)]->submit(node); }

  /**
   * @brief Construct a new lazy_pool object and `n` worker threads.
   *
   * @param n The number of worker threads to create, defaults to the number of hardware threads.
   */
  explicit lazy_pool(std::size_t n = std::thread::hardware_concurrency()) : m_dist{0, n - 1} {

    for (std::size_t i = 0; i < n; ++i) {
      m_contexts.push_back(std::make_shared<context_type>(n, m_rng, m_atomics));
      m_rng.long_jump();
    }

    std::vector nodes = numa_topology{}.distribute(m_contexts);

    // clang-format off

    LF_TRY {
      for (auto &&node : nodes) {
        m_workers.emplace_back(context_type::work, std::move(node));
      }
    } LF_CATCH_ALL {
      clean_up();
      LF_RETHROW;
    }

    // clang-format on
  }

  ~lazy_pool() noexcept { clean_up(); }
};

static_assert(scheduler<lazy_pool>);

} // namespace lf

#endif /* C1BED09D_40CC_4EA1_B687_38A5BCC31907 */
