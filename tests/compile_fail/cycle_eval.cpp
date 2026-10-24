// cycle_eval.cpp — [compile-fail test] eval dependency cycle:
// A's eval reads B, B's eval reads A (triggered by feature<A>()).
// Expected: compilation fails with a diagnostic containing
// "Cyclic eval dependency".
#include <flexplanner/planner.hpp>

using flexplanner::Planner;

struct B;

struct A {
    using handle_deps = std::tuple<>;
    using eval_deps   = std::tuple<B>;
    double value = 0;
    template<class C> void eval(C& c) { value = c.template get<B>().value; }
};
struct B {
    using handle_deps = std::tuple<>;
    using eval_deps   = std::tuple<A>;
    double value = 0;
    template<class C> void eval(C& c) { value = c.template get<A>().value; }
};

int main() { Planner<A, B> p; return p.feature<A>() > 0 ? 0 : 1; }
