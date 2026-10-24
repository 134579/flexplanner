// cycle_handle.cpp — [compile-fail test] handler dependency cycle:
// A's handler reads B, B's handler reads A.
// Expected: compilation fails with a diagnostic containing
// "Cyclic handle dependency".
#include <flexplanner/planner.hpp>

using flexplanner::Planner;

struct Tick {};
struct B;

struct A {
    using handle_deps = std::tuple<B>;
    int x = 0; double value = 0;
    template<class C> void handle(C&, Tick) { ++x; }
    template<class C> void eval(C&) { value = x; }
};
struct B {
    using handle_deps = std::tuple<A>;
    double value = 0;
    template<class C> void handle(C&, Tick) {}
};

int main() { Planner<A, B> p; p.on(Tick{}); }
