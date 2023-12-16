#ifndef CF97E524_27A6_4CD9_8967_39F1B1BE97B6
#define CF97E524_27A6_4CD9_8967_39F1B1BE97B6

// Copyright © Conor Williams <conorwilliams@outlook.com>

// SPDX-License-Identifier: MPL-2.0

// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <stdexcept>

#include "libfork/core/macro.hpp"

#include "libfork/core/ext/context.hpp"

#include "libfork/core/impl/fibre.hpp"
#include "libfork/core/impl/manual_lifetime.hpp"

/**
 * @file tls.hpp
 *
 * @brief The thread local variables used by libfork.
 */

namespace lf {

namespace impl::tls {

constinit inline thread_local bool has_fibre = false;
constinit inline thread_local manual_lifetime<fibre> thread_fibre = {};

constinit inline thread_local bool has_context = false;
constinit inline thread_local manual_lifetime<full_context> thread_context = {};

[[nodiscard]] inline auto fibre() -> fibre * {
  LF_ASSERT(has_fibre);
  return thread_fibre.data();
}

[[nodiscard]] inline auto context() -> full_context * {
  LF_ASSERT(has_context);
  return thread_context.data();
}

} // namespace impl::tls

inline namespace ext {

/**
 * @brief Initialize thread-local variables before a worker can resume submitted tasks.
 *
 * \rst
 *
 * .. warning::
 *    These should be cleaned up with ``worker_finalize(...)``.
 *
 * \endrst
 */
[[nodiscard]] inline auto worker_init(nullary_function_t notify) -> worker_context * {

  LF_LOG("Initializing worker");

  if (impl::tls::has_context && impl::tls::has_fibre) {
    LF_THROW(std::runtime_error("Worker already initialized"));
  }

  worker_context *context = impl::tls::thread_context.construct(std::move(notify));

  // clang-format off

  LF_TRY {
    impl::tls::thread_fibre.construct();
  } LF_CATCH_ALL {
    impl::tls::thread_context.destroy();
  }

  impl::tls::has_fibre = true;
  impl::tls::has_context = true;

  // clang-format on

  return context;
}

/**
 * @brief Clean-up thread-local variable before destructing a worker's context.
 *
 * \rst
 *
 * .. warning::
 *    These must be initialized with ``worker_init(...)``.
 *
 * \endrst
 */
inline void finalize(worker_context *worker) {

  LF_LOG("Finalizing worker");

  if (worker != impl::tls::thread_context.data()) {
    LF_THROW(std::runtime_error("Finalize called on wrong thread"));
  }

  if (!impl::tls::has_context || !impl::tls::has_fibre) {
    LF_THROW(std::runtime_error("Finalize called before initialization or after finalization"));
  }

  impl::tls::thread_context.destroy();
  impl::tls::thread_fibre.destroy();

  impl::tls::has_fibre = false;
  impl::tls::has_context = false;
}

} // namespace ext

} // namespace lf

#endif /* CF97E524_27A6_4CD9_8967_39F1B1BE97B6 */
