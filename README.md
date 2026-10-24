# flexplanner

A header-only C++20 library that turns a graph of *features* into
compile-time **event plans** — a compact re-implementation of the planner
architecture described in Jane Street's blog post
[Flex Model: an event-driven feature engineering system](https://blog.janestreet.com/flex-model-an-event-driven-feature-engineering-system/).

[![CI](https://github.com/your-org/flexplanner/actions/workflows/ci.yml/badge.svg)](https://github.com/your-org/flexplanner/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

## The idea

Raw market data is a stream of events. A *feature* is a number derived from
that stream, and many features need **history** (e.g. the average of
`trade_price − mid` over the last 10 trades). Keeping history inside the
computation graph is only tractable if the graph is static — so flexplanner
gives up runtime dynamism in exchange for doing **all planning at compile
time**, exactly like the post's planner does at startup, one phase earlier:

| Concept | In flexplanner |
|---|---|
| **Handler** | `template<class C> void handle(C&, Event)` — runs on *every* event of its kind, mutates the node's state. No coalescing, no cutoffs: two trades at the same price are two events. |
| **Eval** | `template<class C> void eval(C&)` — pure, runs only when a value is actually needed (by a handler this event, or by `feature<F>()`). |
| **Planner** | A `consteval` DFS with gray/black marking that builds, per event kind, an ordered plan: handlers chosen by event kind, order fixed by what they read. Cycles are compile errors. |

Because the plan is an array of `{node index, is_handle}` actions computed in
a constant expression and re-fed into templates as NTTPs, dispatch compiles
into **straight-line code**: zero indirect calls, zero allocations on the hot
path (verified by the test suite's assembly checks during development).

## Quick start

```cpp
#include <flexplanner/planner.hpp>

struct BookEvent { double bid, ask; };
struct TradeEvent { double price; };

struct Book {                                   // state: our copy of the book
    using handle_deps = std::tuple<>;
    double bid = 0, ask = 0;
    template<class C> void handle(C&, BookEvent e) { bid = e.bid; ask = e.ask; }
};

struct Mid {                                    // stateless: eval only
    using eval_deps = std::tuple<Book>;
    double value = 0;
    template<class C> void eval(C& c) {
        auto& b = c.template get<Book>();
        value = (b.bid + b.ask) / 2;
    }
};

struct Diff {                                   // the post's example feature
    using handle_deps = std::tuple<Mid>;        // the handler reads mid
    using eval_deps   = std::tuple<>;
    double ring[10] = {}; int pos = 0, count = 0; double sum = 0, value = 0;
    template<class C> void handle(C& c, TradeEvent e) {
        double x = e.price - c.template get<Mid>().value;
        sum += x - ring[pos]; ring[pos] = x; pos = (pos + 1) % 10;
        if (count < 10) ++count;
    }
    template<class C> void eval(C&) { value = count ? sum / count : 0; }
};

int main() {
    flexplanner::Planner<Diff, Mid, Book> p;    // any order — plans are
    p.on(BookEvent{100, 102});                  // topologically sorted
    p.on(TradeEvent{103});
    p.on(TradeEvent{103});                      // every trade matters
    p.on(BookEvent{102, 104});
    p.on(TradeEvent{104});
    std::printf("%g\n", p.feature<Diff>());     // (2 + 2 + 1) / 3 = 1.66667
}
```

This produces exactly the plans from the blog post:

```text
on a book update :  book  handle
on a trade       :  mid   eval ; diff handle   (mid computed first)
when asked       :  diff  eval
```

## Node protocol

A node is a plain struct. It may define:

| Member | Meaning |
|---|---|
| `using handle_deps = std::tuple<Dep...>` | values its **handlers** read (ordering only) |
| `using eval_deps = std::tuple<Dep...>` | values its **eval** reads (ordering only) |
| `template<class C> void handle(C&, SomeEvent)` | one overload per handled event; subscription is *detected*, not declared |
| `template<class C> void eval(C&)` | computes the node's value |
| `value` member | the value slot returned by `feature<Node>()` |

Handlers/evals fetch dependencies themselves via `ctx.template get<Dep>()`.

## Guarantees (all pinned by tests)

* **Order-independent** — the plan is a true topological order regardless of
  the order nodes are listed in `Planner<...>`.
* **Deduplicated** — a shared dependency is evaluated exactly once per event.
* **Eval-after-handle** — if a node is read on an event it also handles, its
  handler runs first so readers see the updated state.
* **Minimal** — only handlers of the event and values transitively read are
  scheduled; nobody pays for features they don't use.
* **Cycles rejected at compile time** with targeted diagnostics
  (`Cyclic handle dependency`, `Cyclic eval dependency`), as are dependencies
  missing from the node list (`Dependency absent from Planner`).

## Known limitations (characterized by tests)

* Dependencies are *declared*, not *enforced*: a body may read a node via
  `ctx.get<X>()` without listing `X` in its deps tuple and will observe stale
  values. Section §10 of the test suite pins this behavior.
* `handle_deps` is node-granular: it applies to every handler of the node,
  even ones that read nothing.
* A declared dependency that has neither `handle` nor `eval` and is absent
  from the node list is silently ignored.

## Requirements & integration

* A C++20 compiler (consteval, concepts, template lambdas). Tested with
  Clang 19; CI covers GCC/Clang/MSVC.
* CMake ≥ 3.16 via `add_subdirectory` / `FetchContent`, or simply copy the
  single header `include/flexplanner/planner.hpp`.

```cmake
add_subdirectory(flexplanner)
target_link_libraries(your_target PRIVATE flexplanner::flexplanner)
```

```sh
cmake -S . -B build && cmake --build build && ctest --test-dir build
```

## Project layout

```text
include/flexplanner/planner.hpp   the library (single header)
examples/recent_trade_vs_mid.cpp  the blog-post example
tests/test_planner.cpp            runtime suite: 10 sections, 74 checks
tests/compile_fail/               compile-fail cases (cycles, missing dep)
.github/workflows/ci.yml          build + ctest on Linux/macOS/Windows
```

## Testing notes

The runtime suite instruments every `handle`/`eval` with a log entry, so each
dispatch's log snapshot **is** the executed plan — the tests assert exact
plan contents, ordering invariants, dedup, warm-up/eviction arithmetic,
mid-freshness at trade time, query-plan laziness, order-independence
(reversed node lists replayed), and the regression traps that motivated this
planner design. See `tests/test_planner.cpp` for the full section list.

## License

[MIT](LICENSE)
