// recent_trade_vs_mid.cpp — the running example from Jane Street's
// "Flex Model" blog post: the average of (trade price − mid) over the
// last N trades, driven by book updates and trades.
//
// The planner compiles, per event kind, exactly the plans from the post:
//
//   on a book update :  book  handle
//   on a trade       :  mid   eval ; diff handle
//   when asked       :  diff  eval
//
// Build (standalone): any C++20 compiler, e.g.
//   c++ -std=c++20 -O2 -Iinclude examples/recent_trade_vs_mid.cpp && ./a.out
#include <flexplanner/planner.hpp>

#include <array>
#include <cstddef>
#include <iostream>

using flexplanner::Planner;

struct BookEvent { double bid, ask; };
struct TradeEvent { double price; };

struct Book {
    using handle_deps = std::tuple<>;
    double bid = 0, ask = 0;
    template<class C> void handle(C&, BookEvent e) { bid = e.bid; ask = e.ask; }
};

struct Mid {
    using eval_deps = std::tuple<Book>;
    double value = 0;
    template<class C> void eval(C& c) {
        auto& b = c.template get<Book>();
        value = (b.bid + b.ask) / 2;
    }
};

struct Diff {
    using handle_deps = std::tuple<Mid>;      // the handler reads mid
    using eval_deps   = std::tuple<>;
    static constexpr std::size_t N = 10;
    std::array<double, N> ring{};
    std::size_t pos = 0, count = 0;
    double sum = 0, value = 0;
    template<class C> void handle(C& c, TradeEvent e) {
        double x = e.price - c.template get<Mid>().value;   // must see every trade
        sum += x - ring[pos]; ring[pos] = x; pos = (pos + 1) % N;
        if (count < N) ++count;
    }
    template<class C> void eval(C&) { value = count ? sum / count : 0; }
};

int main() {
    // Nodes intentionally listed in reverse dependency order: the planner's
    // topological sort makes the order irrelevant.
    Planner<Diff, Mid, Book> p;

    p.on(BookEvent{100, 102});   // plan: book handle         (mid not needed yet)
    p.on(TradeEvent{103});       // plan: mid eval; diff handle (push 2)
    p.on(TradeEvent{103});       // every trade matters — same price, still pushed
    p.on(BookEvent{102, 104});   // plan: book handle         (mid moves to 103)
    p.on(TradeEvent{104});       // push 1 (mid taken at trade time)

    std::cout << p.feature<Diff>() << '\n';   // (2 + 2 + 1) / 3 = 1.66667
}
