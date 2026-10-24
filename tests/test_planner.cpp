// test_planner.cpp — flexplanner 的完备单元测试 (10 个章节, 74 项检查)
//
// 被测对象: <flexplanner/planner.hpp> (公开头文件)。
// 节点是测试夹具: 每个 handle/eval 都往 g_log 追加一条记录,
// 因此每次 dispatch 之后的 g_log 快照 == 该事件计划的实际执行序列,
// 可以对计划本身做精确断言 (内容、顺序、去重、最小性)。
//
// 覆盖:
//   §1  计划最小性: Quote 只跑 book.handle; 无人需要时不算 Mid (博客原意)
//   §2  Trade 计划形态: 5 步精确序列 + 顺序不变式 + E:Mid 去重
//   §3  博客特征精确算术: 预热(count)、环驱逐、mid 随行情更新、重复事件都入环
//   §4  Vwap 算术(第二个环) + 多依赖 handler (Mid,Vwap) 的求值语义
//   §5  同一特征订阅两种事件 (Trade + Timer 复位); 节点级 handle_deps 语义
//   §6  无订阅事件: 计划为空
//   §7  查询计划: 只跑 eval 闭包, 不跑 handler, 幂等, 深链
//   §8  顺序无关性: 反序引擎重放同样事件 → 逐步骤同多重集合, 终值相等
//   §9  回归陷阱: 状态依赖 / 值依赖(又 handle 又 eval), 两种声明顺序
//   §10 特性化测试: 装饰性依赖的两条已知行为 (读未声明依赖=旧值; 惰性缺失依赖被静默忽略)
// 编译失败用例 (两类环 + 缺依赖) 见 compile_fail/, 由 ctest 验证。
#include <flexplanner/planner.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

using flexplanner::Planner;

// ---------------- 事件 ----------------
struct Quote    { double bid, ask; };
struct Trade    { double price, size; };
struct TimerEvt { int ms; };
struct Heartbeat {};                 // 无人订阅的事件

inline std::vector<std::string> g_log;
inline int g_f4 = 0;                               // §9: F4::eval 调用计数

// ---------------- 夹具节点 (协议 = v4 原协议 + 日志插桩) ----------------
struct Book {
    using handle_deps = std::tuple<>;
    double bid{}, ask{};
    template<class C> void handle(C&, Quote e) { bid = e.bid; ask = e.ask; g_log.push_back("H:Book"); }
};
struct Mid {
    using eval_deps = std::tuple<Book>;
    double value{};
    template<class C> void eval(C& c) { auto& b = c.template get<Book>(); value = (b.bid + b.ask) / 2; g_log.push_back("E:Mid"); }
};
struct Spread {
    using eval_deps = std::tuple<Book>;
    double value{};
    template<class C> void eval(C& c) { auto& b = c.template get<Book>(); value = b.ask - b.bid; g_log.push_back("E:Spread"); }
};
struct Vwap {                                     // 成交量加权价, 环 N=4
    using handle_deps = std::tuple<>;
    using eval_deps   = std::tuple<>;
    static constexpr std::size_t N = 4;
    double ps[N]{}, ss[N]{}, sum_ps{}, sum_s{}, value{};
    std::size_t pos{}, cnt{};
    template<class C> void handle(C&, Trade e) {
        if (cnt == N) { sum_ps -= ps[pos]; sum_s -= ss[pos]; } else ++cnt;
        ps[pos] = e.price * e.size; ss[pos] = e.size;
        sum_ps += ps[pos]; sum_s += ss[pos];
        pos = (pos + 1) % N; g_log.push_back("H:Vwap");
    }
    template<class C> void eval(C&) { value = sum_s ? sum_ps / sum_s : 0; g_log.push_back("E:Vwap"); }
};
struct RecentTradeVsMid {                         // 博客示例特征, 环 N=3; Timer 复位
    using handle_deps = std::tuple<Mid>;
    using eval_deps   = std::tuple<>;
    static constexpr std::size_t N = 3;
    double ring[N]{}, sum{}, value{};
    std::size_t pos{}, cnt{};
    template<class C> void handle(C& c, Trade e) {
        double d = e.price - c.template get<Mid>().value;
        if (cnt == N) sum -= ring[pos]; else ++cnt;
        ring[pos] = d; sum += d; pos = (pos + 1) % N;
        g_log.push_back("H:RTVM");
    }
    template<class C> void handle(C&, TimerEvt) { pos = 0; cnt = 0; sum = 0; g_log.push_back("H:RTVM:reset"); }
    template<class C> void eval(C&) { value = cnt ? sum / cnt : 0; g_log.push_back("E:RTVM"); }
};
struct MidVsVwap {                                // 多依赖 handler: 读 (Mid, Vwap) 两个值
    using handle_deps = std::tuple<Mid, Vwap>;
    using eval_deps   = std::tuple<>;
    static constexpr std::size_t N = 3;
    double ring[N]{}, sum{}, value{};
    std::size_t pos{}, cnt{};
    template<class C> void handle(C& c, Trade e) {
        (void)e;
        double d = c.template get<Mid>().value - c.template get<Vwap>().value;
        if (cnt == N) sum -= ring[pos]; else ++cnt;
        ring[pos] = d; sum += d; pos = (pos + 1) % N;
        g_log.push_back("H:MidVsVwap");
    }
    template<class C> void eval(C&) { value = cnt ? sum / cnt : 0; g_log.push_back("E:MidVsVwap"); }
};
struct NormDist {                                 // 深查询链: 读 (RTVM, Spread) 两个值
    using eval_deps = std::tuple<RecentTradeVsMid, Spread>;
    double value{};
    template<class C> void eval(C& c) {
        auto& r = c.template get<RecentTradeVsMid>(); auto& s = c.template get<Spread>();
        value = s.value ? r.value / s.value : 0; g_log.push_back("E:NormDist");
    }
};
struct Clock {
    using handle_deps = std::tuple<>;
    int ms{};
    template<class C> void handle(C&, TimerEvt e) { ms = e.ms; g_log.push_back("H:Clock"); }
};

// (Planner 来自文件顶部的 <flexplanner/planner.hpp>)

// ---------------- 迷你测试框架 ----------------
static int g_checks = 0, g_failures = 0;
static void section(const char* s) { std::printf("\n== %s ==\n", s); }
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failures; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_NEAR(a, b) do { ++g_checks; double _x = (a), _y = (b); \
    if (std::fabs(_x - _y) > 1e-9) { ++g_failures; std::printf("FAIL %s:%d: %s = %g, want %g\n", __FILE__, __LINE__, #a, _x, _y); } } while (0)

static std::vector<std::string> take_log() { auto v = g_log; g_log.clear(); return v; }
static void expect_log(std::initializer_list<const char*> exp, const char* ctx) {
    ++g_checks;
    auto got = take_log();
    bool ok = got.size() == exp.size();
    if (ok) { size_t i = 0; for (auto* s : exp) if (got[i++] != s) { ok = false; break; } }
    if (!ok) {
        ++g_failures;
        std::printf("FAIL [%s] 计划执行序列不符, 期望:", ctx);
        for (auto* s : exp) std::printf(" %s", s);
        std::printf(" | 实际:");
        for (auto& s : got) std::printf(" %s", s.c_str());
        std::printf("\n");
    }
}
// 顺序不变式: 作用于一份已捕获的快照
static std::ptrdiff_t find_in(const std::vector<std::string>& v, const char* s) {
    for (size_t i = 0; i < v.size(); ++i) if (v[i] == s) return (std::ptrdiff_t)i;
    return -1;
}
static void check_before(const std::vector<std::string>& v, const char* a, const char* b, const char* ctx) {
    ++g_checks;
    auto pa = find_in(v, a), pb = find_in(v, b);
    if (!(pa >= 0 && pb >= 0 && pa < pb)) { ++g_failures; std::printf("FAIL [%s] %s(位置%td) 应先于 %s(位置%td)\n", ctx, a, pa, b, pb); }
}
static void check_once(const std::vector<std::string>& v, const char* a, const char* ctx) {
    ++g_checks;
    size_t n = 0; for (auto& s : v) if (s == a) ++n;
    if (n != 1) { ++g_failures; std::printf("FAIL [%s] %s 出现 %zu 次, 应恰为 1 次\n", ctx, a, n); }
}
static bool same_multiset(std::vector<std::string> a, std::vector<std::string> b) {
    std::sort(a.begin(), a.end()); std::sort(b.begin(), b.end());
    return a == b;
}

using World  = Planner<Book, Mid, Spread, Vwap, RecentTradeVsMid, MidVsVwap, NormDist, Clock>;
using WorldR = Planner<Clock, NormDist, MidVsVwap, RecentTradeVsMid, Vwap, Spread, Mid, Book>;  // 完全反序

// ---- §9/§10 夹具 (局部类不能有成员模板, 提到文件作用域) ----
struct Q2 { double b, a; };
struct Counter3 {
    using handle_deps = std::tuple<>;
    int count = 0;
    template<class C> void handle(C&, Q2) { ++count; }
};
struct Snap3 {                                            // handler 读另一 handler 的状态 (无 eval)
    using handle_deps = std::tuple<Counter3>;
    int seen = -1;
    template<class C> void handle(C& c, Q2) { seen = c.template get<Counter3>().count; }
};
struct T3 { double p; };
struct F4 {                                               // 又 handle 又 eval
    using handle_deps = std::tuple<>;
    using eval_deps   = std::tuple<>;
    int x = 0; double value = 0;
    template<class C> void handle(C&, T3) { ++x; }
    template<class C> void eval(C&) { ++g_f4; value = x; }
};
struct G4 {                                               // handler 读 F4 的值
    using handle_deps = std::tuple<F4>;
    double seen = -1;
    template<class C> void handle(C& c, T3) { seen = c.template get<F4>().value; }
};
struct T4 { double p; };
struct CntI {
    using handle_deps = std::tuple<>;
    int count = 0;
    template<class C> void handle(C&, T4) { ++count; }
};
struct MiI {                                              // 值 = Counter 状态 (声明了 eval_deps)
    using eval_deps = std::tuple<CntI>;
    double value = 0;
    template<class C> void eval(C& c) { value = c.template get<CntI>().count; }
};
struct R0I {                                              // 不声明 MiI, 直接读 → 计划不会算它
    using handle_deps = std::tuple<>;
    double seen = -1;
    template<class C> void handle(C& c, T4) { seen = c.template get<MiI>().value; }
};
struct R1I {                                              // 声明 → 每事件新鲜
    using handle_deps = std::tuple<MiI>;
    double seen = -1;
    template<class C> void handle(C& c, T4) { seen = c.template get<MiI>().value; }
};
struct Ghost {};                                          // 惰性类型: 无 handle 无 eval
struct UsesGhost {
    using handle_deps = std::tuple<Ghost>;                // 声明了依赖, 但 Ghost 不在 Planner 里
    double value = 0;
    template<class C> void handle(C&, Heartbeat) { value = 2; g_log.push_back("H:UsesGhost"); }
};

// Trade 计划的顺序不变式 (与声明顺序无关都必须成立)
static void trade_invariants(const std::vector<std::string>& v, const char* ctx) {
    check_before(v, "E:Mid", "H:RTVM", ctx);            // RTVM 的 handler 读 Mid 的值
    check_before(v, "E:Mid", "H:MidVsVwap", ctx);       // MidVsVwap 也读
    check_before(v, "H:Vwap", "E:Vwap", ctx);           // eval 必须看到本事件 handle 之后的 state
    check_before(v, "E:Vwap", "H:MidVsVwap", ctx);      // MidVsVwap 读 handle 后的 Vwap
    check_once(v, "E:Mid", ctx);                        // 共享依赖去重: 两个读者, 一次求值
    check_once(v, "E:Vwap", ctx);
}

int main() {
    // ============ §1 计划最小性: on(Quote) 只更新 book ============
    section("§1 Quote 计划最小性 (mid 没人要 → 不算)");
    {
        World w;
        w.on(Quote{100, 102});
        CHECK(take_log() == std::vector<std::string>{"H:Book"});   // 计划 = [handle book] 恰一步
        CHECK_NEAR(w.feature<Mid>(), 101);                          // 查询时才算, 且基于最新 book
        expect_log({"E:Mid"}, "query Mid");
    }

    // ============ §2 Trade 计划形态 ============
    section("§2 Trade 计划形态与去重");
    {
        World w;
        w.on(Quote{100, 102}); take_log();
        w.on(Trade{101, 1});
        auto snap = take_log();
        std::printf("  Trade 计划:");
        for (auto& s : snap) std::printf(" %s", s.c_str());
        std::printf("\n");
        CHECK(snap == (std::vector<std::string>{"H:Vwap", "E:Mid", "H:RTVM", "E:Vwap", "H:MidVsVwap"}));
        trade_invariants(snap, "§2");
    }

    // ============ §3 博客特征精确算术 ============
    section("§3 RTVM: 预热/驱逐/重复事件/mid 新鲜度");
    {
        World w;
        w.on(Quote{100, 102}); take_log();                          // mid = 101
        w.on(Trade{101, 1}); take_log();                            // d = 0, count=1 (预热按 count 除)
        CHECK_NEAR(w.feature<RecentTradeVsMid>(), 0);
        expect_log({"E:RTVM"}, "query RTVM");

        w.on(Trade{104, 3}); take_log();                            // d = 3
        CHECK_NEAR(w.feature<RecentTradeVsMid>(), 1.5);             // (0+3)/2

        w.on(Trade{104, 3}); take_log();                            // 同价两笔都入环: d = 3 (无 coalescing/cutoff)
        CHECK_NEAR(w.feature<RecentTradeVsMid>(), 2);               // (0+3+3)/3

        w.on(Quote{102, 104}); take_log();                          // mid → 103
        w.on(Trade{110, 1}); take_log();                            // d = 110-103 = 7 (mid 按当下行情), 驱逐最老的 0
        CHECK_NEAR(w.feature<RecentTradeVsMid>(), 13.0 / 3.0);      // (3+3+7)/3, 若用了旧 mid 会得 5
        w.on(Trade{110, 1}); take_log();                            // d = 7, 驱逐 3
        CHECK_NEAR(w.feature<RecentTradeVsMid>(), 17.0 / 3.0);      // (3+7+7)/3
    }

    // ============ §4 Vwap 与多依赖 handler ============
    section("§4 Vwap 算术与 (Mid,Vwap) 多依赖 handler");
    {
        World w;
        w.on(Quote{100, 102}); take_log();
        w.on(Trade{101, 1}); take_log();                            // vwap=101, push=0
        w.on(Trade{104, 3}); take_log();                            // vwap=(101+312)/4=103.25, push=101-103.25
        CHECK_NEAR(w.feature<Vwap>(), 103.25);
        expect_log({"E:Vwap"}, "query Vwap");
        CHECK_NEAR(w.feature<MidVsVwap>(), (0 + (101 - 103.25)) / 2);   // -1.125; 语义: 读到含本笔成交的 Vwap
        expect_log({"E:MidVsVwap"}, "query MidVsVwap");

        w.on(Trade{104, 3}); take_log();                            // vwap=725/7, push=101-725/7
        w.on(Trade{110, 1}); take_log();                            // vwap=835/8=104.375; 本世界未换盘口, mid=101 → push=101-104.375
        CHECK_NEAR(w.feature<Vwap>(), 104.375);
        CHECK_NEAR(w.feature<MidVsVwap>(), ((101 - 103.25) + (101 - 725.0 / 7.0) + (101 - 104.375)) / 3.0);

        World w2;                                                   // 固定盘口下的精确驱逐序列
        w2.on(Quote{100, 102}); take_log();
        w2.on(Trade{104, 2}); take_log();                           // vwap=104, push=-3
        w2.on(Trade{104, 2}); take_log();                           // push=-3
        CHECK_NEAR(w2.feature<MidVsVwap>(), -3);
        w2.on(Trade{101, 1}); take_log();                           // vwap=(208+208+101)/5=103.4, push=-2.4
        w2.on(Trade{101, 1}); take_log();                           // vwap=618/6=103, push=-2, 驱逐最老的 -3
        CHECK_NEAR(w2.feature<MidVsVwap>(), (-3.0 - 2.4 - 2.0) / 3.0);
    }

    // ============ §5 同一特征订阅两种事件 + 节点级 handle_deps 语义 ============
    section("§5 Timer: 复位 + Clock (注意 E:Mid: handle_deps 是节点级的)");
    {
        World w;
        w.on(Quote{100, 102}); take_log();
        w.on(Trade{104, 3}); take_log();
        w.on(TimerEvt{500});
        // RTVM 的 handle_deps=[Mid] 对它的所有 handler 生效, 因此复位计划也先算 Mid (特性化: 节点级粒度)
        expect_log({"E:Mid", "H:RTVM:reset", "H:Clock"}, "timer plan");
        CHECK(w.get<Clock>().ms == 500);
        CHECK_NEAR(w.feature<RecentTradeVsMid>(), 0);               // 复位后回到预热
        expect_log({"E:RTVM"}, "query after reset");
        w.on(Trade{106, 1}); take_log();                            // d=5, count=1
        CHECK_NEAR(w.feature<RecentTradeVsMid>(), 5);
        take_log();                                                 // 消费 E:RTVM, 避免污染下一节
    }

    // ============ §6 无订阅事件 ============
    section("§6 Heartbeat: 计划为空");
    {
        World w;
        w.on(Heartbeat{});
        CHECK(take_log().empty());
    }

    // ============ §7 查询计划 ============
    section("§7 查询: 只跑 eval 闭包, 不跑 handler, 幂等, 深链");
    {
        World w;
        w.on(Quote{100, 102}); take_log();
        w.on(Trade{104, 3}); take_log();                            // d = 3
        w.on(Trade{104, 3}); take_log();                            // d = 3 → rtvm = (3+3)/2 = 3 (预热按 count)
        CHECK_NEAR(w.feature<NormDist>(), 3 / 2.0);                 // rtvm/spread = 3/2
        // 深链查询计划: RTVM → (自身 eval), Spread → (Book 无 eval, 不产生步骤), NormDist
        expect_log({"E:RTVM", "E:Spread", "E:NormDist"}, "query NormDist");
        CHECK_NEAR(w.feature<Mid>(), 101);                          // 另一条链, 互不牵连
        expect_log({"E:Mid"}, "query Mid");
        double a = w.feature<RecentTradeVsMid>(); take_log();
        double b = w.feature<RecentTradeVsMid>(); take_log();
        CHECK_NEAR(a, b);                                           // 幂等
        w.on(Heartbeat{}); CHECK(take_log().empty());               // 查询不改变任何 handler 状态
    }

    // ============ §8 顺序无关性 ============
    section("§8 反序引擎重放: 逐步骤同多重集合, 终值相等");
    {
        auto replay = [](auto& p, std::vector<std::vector<std::string>>& traces) {
            p.on(Quote{100, 102}); traces.push_back(take_log());
            p.on(Trade{101, 1});    traces.push_back(take_log());
            p.on(Trade{104, 3});    traces.push_back(take_log());
            p.on(Trade{104, 3});    traces.push_back(take_log());
            p.on(Quote{102, 104});  traces.push_back(take_log());
            p.on(Trade{110, 1});    traces.push_back(take_log());
        };
        std::vector<std::vector<std::string>> ta, tb;
        World  wa; replay(wa, ta);
        WorldR wb; replay(wb, tb);
        const char* names[] = {"Quote#1", "Trade#1", "Trade#2", "Trade#3", "Quote#2", "Trade#4"};
        for (size_t i = 0; i < ta.size(); ++i) {
            ++g_checks;
            if (!same_multiset(ta[i], tb[i])) { ++g_failures; std::printf("FAIL [§8] %s 两引擎步骤集合不同\n", names[i]); }
        }
        trade_invariants(tb[1], "§8 反序 Trade#1");
        trade_invariants(tb[3], "§8 反序 Trade#3");
        trade_invariants(tb[5], "§8 反序 Trade#4");
        CHECK_NEAR(wa.feature<RecentTradeVsMid>(), wb.feature<RecentTradeVsMid>()); take_log(); take_log();
        CHECK_NEAR(wa.feature<RecentTradeVsMid>(), 13.0 / 3.0);
        CHECK_NEAR(wa.feature<Vwap>(), wb.feature<Vwap>()); take_log(); take_log();
        CHECK_NEAR(wa.feature<NormDist>(), wb.feature<NormDist>()); take_log(); take_log();
        CHECK_NEAR(wa.feature<Mid>(), 103);
    }

    // ============ §9 回归陷阱 ============
    section("§9 回归: 状态依赖 / 值依赖, 两种声明顺序");
    {
        Planner<Counter3, Snap3> p1;
        for (int i = 0; i < 3; ++i) p1.on(Q2{1, 2});
        CHECK(p1.get<Snap3>().seen == 3);
        Planner<Snap3, Counter3> p2;                              // 只是换了声明顺序
        for (int i = 0; i < 3; ++i) p2.on(Q2{1, 2});
        CHECK(p2.get<Snap3>().seen == 3);

        g_f4 = 0;
        Planner<F4, G4> p3;
        for (int i = 0; i < 3; ++i) p3.on(T3{1});
        CHECK(p3.get<G4>().seen == 3);
        CHECK(g_f4 == 3);                                         // 每事件恰好一次 (去重)
        g_f4 = 0;
        Planner<G4, F4> p4;
        for (int i = 0; i < 3; ++i) p4.on(T3{1});
        CHECK(p4.get<G4>().seen == 3);
        CHECK(g_f4 == 3);
    }

    // ============ §10 特性化测试: 装饰性依赖的已知行为 ============
    section("§10 特性化: 装饰性依赖 (读未声明的依赖 → 旧值; 惰性缺失依赖被静默忽略)");
    {
        Planner<CntI, MiI, R0I, R1I> p;
        p.on(T4{1}); take_log();                                  // 事件1: count=1; E:MiI 因 R1I 而算 → 1
        p.on(T4{1}); take_log();                                  // 事件2: count=2; E:MiI → 2
        CHECK(p.get<R1I>().seen == 2);                            // 声明者: 新鲜
        CHECK(p.get<R0I>().seen == 1);                            // 未声明者: 拿到上一事件的旧值 (已知行为, 若未来强制依赖请改此断言)

        Planner<UsesGhost> q;                                     // Ghost 无 handle/eval → 两处 requires 都不命中
        q.on(Heartbeat{});                                        // → index<Ghost>() 从未被调用 → 静默通过
        expect_log({"H:UsesGhost"}, "inert ghost");               // 已知行为: 缺失的惰性依赖被静默忽略
                                                                  // (带 eval 的缺失依赖会编译报错, 见 cf_absent_dep.cpp)
    }

    std::printf("\n========================================\n%d 项检查, %d 失败\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
