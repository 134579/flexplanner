// flexplanner/planner.hpp — header-only, compile-time event-driven feature planner
//
// flexplanner re-implements, in C++20 template metaprogramming, the planner
// architecture described in Jane Street's blog post "Flex Model: an
// event-driven feature engineering system"
// (https://blog.janestreet.com/flex-model-an-event-driven-feature-engineering-system/).
//
//   Handler : runs on EVERY event of a kind, mutates a node's state
//             (history lives inside the graph -> no coalescing, no cutoffs)
//   Eval    : runs only when a value is needed; pure read of state + deps
//   Planner : builds, AT COMPILE TIME (consteval), one ordered plan per event
//             kind — an ordered list of evals & handlers in which anything a
//             step reads is computed before it.  Plans are deduplicated
//             (each step runs at most once per event) and dependency cycles
//             are rejected at compile time.
//
// Because a plan is an array of {node index, is_handle} actions computed in a
// consteval DFS (gray/black marking), dispatch compiles into straight-line
// code with zero indirect calls and zero allocations on the hot path.
//
// Node (feature) protocol
// -----------------------
// A node is a plain struct.  It may define:
//
//   using handle_deps = std::tuple<Dep...>;  // what its HANDLER reads
//   using eval_deps   = std::tuple<Dep...>;  // what its EVAL reads
//   template<class C> void handle(C& ctx, SomeEvent e);  // one overload per
//                                                        // handled event
//   template<class C> void eval(C& ctx);      // computes `value`
//   double value = 0;                         // value slot (any type)
//
// Subscription is detected: a node handles an event iff `n.handle(ctx, e)` is
// well-formed for that event type.  `handle_deps`/`eval_deps` drive ordering
// only; handlers/evals fetch dependencies themselves via ctx.get<Dep>().
//
// Semantics guaranteed (and pinned by the test suite):
//   * order-independent: the plan is a true topological order regardless of
//     the order nodes are listed in Planner<...>;
//   * deduplicated: a shared dependency is evaluated exactly once per event;
//   * eval-after-handle: if a node is read on an event it also handles, its
//     handler runs first, then its eval;
//   * minimal: only handlers of the event and the values they transitively
//     read are scheduled; nobody's eval runs "just in case";
//   * cyclic dependencies are a compile error with a targeted diagnostic.
//
// Known limitations (documented, characterized by tests):
//   * dependencies are declared, not enforced — a body may read a node via
//     ctx.get<X>() without listing X, and will then observe stale values;
//   * handle_deps is node-granular: it applies to every handler of the node,
//     even ones that read nothing;
//   * a declared dependency that has neither handle nor eval and is absent
//     from the node list is silently ignored.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <type_traits>
#include <utility>

namespace flexplanner {

template<class... Nodes>
class Planner {
    static constexpr std::size_t N = sizeof...(Nodes);
    using NodeTuple = std::tuple<Nodes...>;
    template<class T> static consteval std::size_t index() {
        static_assert((std::is_same_v<T,Nodes> || ...), "Dependency absent from Planner");
        constexpr bool hits[]{std::is_same_v<T,Nodes>...};
        for(std::size_t i=0;i<N;++i) if(hits[i]) return i;
        return N;
    }
    struct Action {std::size_t index; bool handle;};
    struct Plan {
        std::array<Action,2*N> actions{};   // a node contributes at most one
        std::array<std::uint8_t,2*N> marks{};   // handle + one eval per plan
        std::size_t size{};
        consteval void add(std::size_t i, bool h) {   // 1 = in progress, 2 = done
            auto k=i+(h?N:0);
            if(marks[k]==1) throw "Cyclic feature dependency";
            if(marks[k]==2) return;
            marks[k]=1;
        }
        consteval void done(std::size_t i, bool h) {
            auto k=i+(h?N:0);
            marks[k]=2; actions[size++]={i,h};
        }
    };
    template<class Node, class Event>
    static consteval void visit_handle(Plan& p) {
        if constexpr (requires(Node& n, Planner& c, Event e){ n.handle(c,e); }) {
            constexpr auto i=index<Node>();
            if(p.marks[i+N]==2) return;
            if(p.marks[i+N]==1) throw "Cyclic handle dependency";
            p.add(i,true);
            if constexpr (requires {typename Node::handle_deps;}) {
                []<class... Ds>(Plan& s,std::tuple<Ds...>*) consteval {
                    (visit_eval<Ds,Event>(s),...);
                }(p,static_cast<typename Node::handle_deps*>(nullptr));
            }
            p.done(i,true);
        }
    }
    template<class Node, class Event>
    static consteval void visit_eval(Plan& p) {
        // A value dep on an event also handled by the node runs the node's
        // handler first, so its eval sees the updated state.
        if constexpr (!std::is_void_v<Event>) visit_handle<Node,Event>(p);
        if constexpr (requires(Node& n, Planner& c){n.eval(c);}) {
            constexpr auto i=index<Node>();
            if(p.marks[i]==2) return;
            if(p.marks[i]==1) throw "Cyclic eval dependency";
            p.add(i,false);
            if constexpr(requires {typename Node::eval_deps;}) {
                []<class... Ds>(Plan& s,std::tuple<Ds...>*) consteval {
                    (visit_eval<Ds,Event>(s),...);
                }(p,static_cast<typename Node::eval_deps*>(nullptr));
            }
            p.done(i,false);
        }
    }
    template<class Event> static consteval Plan event_plan() {
        Plan p;
        (visit_handle<Nodes,Event>(p),...);
        return p;
    }
    template<class Feature> static consteval Plan query_plan() {
        Plan p; visit_eval<Feature,void>(p); return p;
    }
    template<auto A, class Event> void perform(const Event& e) {
        using Node=std::tuple_element_t<A.index,NodeTuple>;
        if constexpr(A.handle) get<Node>().handle(*this,e);
        else get<Node>().eval(*this);
    }
    template<class Event,std::size_t K> void event_step(const Event& e) {
        constexpr auto p=event_plan<Event>();
        if constexpr(K<p.size) perform<p.actions[K]>(e);
    }
    template<class Feature,std::size_t K> void query_step() {
        constexpr auto p=query_plan<Feature>();
        if constexpr(K<p.size) {
            constexpr auto a=p.actions[K];
            using Node=std::tuple_element_t<a.index,NodeTuple>;
            get<Node>().eval(*this);
        }
    }
public:
    NodeTuple nodes{};
    template<class T> T& get() {return std::get<T>(nodes);}
    template<class Event> void on(const Event& e) {
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            (event_step<Event,I>(e),...);
        }(std::make_index_sequence<2*N>{});
    }
    template<class Feature> auto feature() {
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            (query_step<Feature,I>(),...);
        }(std::make_index_sequence<2*N>{});
        return get<Feature>().value;
    }
};

}  // namespace flexplanner
