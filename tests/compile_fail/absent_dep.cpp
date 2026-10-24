// absent_dep.cpp — [compile-fail test] missing dependency:
// Ghost has an eval but was forgotten in the Planner node list.
// Expected: compilation fails with a diagnostic containing
// "Dependency absent from Planner".
#include <flexplanner/planner.hpp>

using flexplanner::Planner;

struct Tick {};

struct Ghost {                       // a real feature, forgotten in the list
    double value = 0;
    template<class C> void eval(C&) { value = 1; }
};
struct A {
    using handle_deps = std::tuple<Ghost>;
    double value = 0;
    template<class C> void handle(C&, Tick) {}
};

int main() { Planner<A> p; p.on(Tick{}); }
