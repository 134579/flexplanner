# flexplanner

一个单头文件的 C++20 库:你把一组"特征"写成普通的 struct,它在**编译期**为每种事件生成一份排好序的执行计划。思路来自 Jane Street 博客
[Flex Model: an event-driven feature engineering system](https://blog.janestreet.com/flex-model-an-event-driven-feature-engineering-system/)
——原文在系统启动时做规划,这里把规划又往前挪了一个阶段,整个搬进编译期。

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

## 从一个具体的难题说起

市场数据不是一张表格,而是一股事件流:一笔订单挂上簿、一笔订单撤掉、一笔成交。特征(feature)就是从这股流里算出来的数字。最简单的特征比如中间价 mid,只看当下的盘口;但稍微复杂一点的：**最近 10 笔成交价与当时 mid 之差的均值**,就需要历史:你得记得每一笔成交发生那一刻的 mid 是多少。

需要历史,麻烦就来了:**每一个事件都很重要,但不是每个事件都改变值**。两笔同价的交易是两个独立事件,都该进缓冲区;一次盘口更新则可能完全不移动 mid。如果套用电子表格式的响应式计算(博客里拿 Incremental 举的例子),两个经典优化会直接把这类特征算错:

- **合并更新**(coalescing):重算之前值变了两次,只保留最后一次——第二笔交易凭空消失;
- **截断**(cutoff):重算完发现值没变,就不再向下传播——依赖它的历史缓冲区错过了这次更新。

Flex Model 的答案是:把每个特征拆成"事件处理器 + 求值函数"两半,再由一个规划器排出"每种事件该跑哪些步骤"。它成立的前提是博客里引用的那句话——*"history is made tractable by limiting dynamism"*,历史之所以可行,是因为放弃了动态性。flexplanner 把这句话贯彻到底:图一经写死,计划就成了常量表达式。

## 三块积木

| 概念 | 在 flexplanner 里 |
|---|---|
| **Handler(事件处理器)** | `template<class C> void handle(C&, Event)`。每种它关心的事件到来时都会运行,更新节点自己的状态。历史就存在图里,所以不做合并、不做截断:同价的两笔交易,两笔都进环。 |
| **Eval(求值函数)** | `template<class C> void eval(C&)`,纯读取。只在真的有人要这个值时才运行——本事件里有 handler 要读它,或者你调了 `feature<F>()`。没人要,就不算。 |
| **Planner(计划器)** | 一段带灰/黑标记的 `consteval` DFS。事件种类决定哪些 handler 进计划,读取关系决定先后;依赖环直接编译失败。 |

计划本身是常量表达式算出来的 `{节点下标, 是否为 handle}` 数组,再作为模板参数喂回去,于是事件分发最终被编译成一段**直线代码**:热路径上零间接调用、零分配(开发过程中用汇编核对过:整个程序 0 次间接调用、0 次堆分配)。

## 快速上手

```cpp
#include <flexplanner/planner.hpp>

struct BookEvent { double bid, ask; };
struct TradeEvent { double price; };

struct Book {                                   // 状态: 自己那份订单簿
    using handle_deps = std::tuple<>;
    double bid = 0, ask = 0;
    template<class C> void handle(C&, BookEvent e) { bid = e.bid; ask = e.ask; }
};

struct Mid {                                    // 无状态: 只有 eval
    using eval_deps = std::tuple<Book>;
    double value = 0;
    template<class C> void eval(C& c) {
        auto& b = c.template get<Book>();
        value = (b.bid + b.ask) / 2;
    }
};

struct Diff {                                   // 博客里的示例特征
    using handle_deps = std::tuple<Mid>;        // handler 要读 mid
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
    flexplanner::Planner<Diff, Mid, Book> p;    // 顺序随便写, 计划是拓扑序
    p.on(BookEvent{100, 102});                  // 计划: book.handle
    p.on(TradeEvent{103});                      // 计划: mid.eval → diff.handle
    p.on(TradeEvent{103});                      // 每笔交易都算数
    p.on(BookEvent{102, 104});                  // 盘口变了, mid 随之变成 103
    p.on(TradeEvent{104});                      // 这笔按新的 mid 入环
    std::printf("%g\n", p.feature<Diff>());     // (2 + 2 + 1) / 3 = 1.66667
}
```

节点故意按依赖的**反序**写进 `Planner<Diff, Mid, Book>`——反正计划会被拓扑排序,声明顺序无所谓。这个例子生成的正是博客原文里的三份计划:

```text
盘口更新 :  book  handle                (mid 没人要, 就不算)
一笔成交 :  mid   eval → diff handle    (handler 要读 mid, 先算它)
模型索取 :  diff  eval
```

## 节点协议

节点就是一个普通的 struct,需要什么写什么:

| 成员 | 含义 |
|---|---|
| `using handle_deps = std::tuple<Dep...>` | 它的 **handler** 读哪些值(只用于排序) |
| `using eval_deps = std::tuple<Dep...>` | 它的 **eval** 读哪些值(只用于排序) |
| `template<class C> void handle(C&, SomeEvent)` | 每种要处理的事件写一个重载;订阅是**检测**出来的,不用登记 |
| `template<class C> void eval(C&)` | 计算自己的值 |
| `value` 成员 | 值槽,`feature<Node>()` 返回它 |

handler 和 eval 的函数体里用 `ctx.template get<Dep>()` 自取依赖。

## 语义保证

- **与声明顺序无关**：无论节点以什么顺序列进 `Planner<...>`,计划都是真正的拓扑序;
- **共享依赖每个事件只算一次**：两个读者,一次求值,不重复付钱;
- **先 handle 后 eval**：被读的节点若同时处理这个事件,它的 handler 先跑,读者看到的是更新后的状态;
- **计划最小**：只有本事件的 handler 和它们(传递地)要读的值会被排进计划,没人读的值绝不"顺便"算,别处新增特征不会拖慢你;
- **环与缺失依赖都是编译错误**,而且诊断直指节点:`Cyclic handle dependency`、`Cyclic eval dependency`、`Dependency absent from Planner`。

## 已知限制

- 依赖是**声明**出来的,不是**强制**的:函数体完全可以读一个没写进 deps 的节点,照样编译,只是会读到旧值。测试套件 §10 把这条行为钉住了，将来若改成参数传递式的强制依赖,这条测试会先变红提醒你;
- `handle_deps` 是节点级的:它作用于该节点的所有 handler,哪怕某个 handler 其实什么都不读;
- 声明了一个既没有 handle 也没有 eval、又不在节点列表里的依赖,会被静默忽略。

## 环境要求与集成

- 任何 C++20 编译器(consteval、concepts、模板 lambda)。本仓库用 Clang 19 验证,CI 覆盖 GCC/Clang/MSVC;
- CMake ≥ 3.16,`add_subdirectory` 或 `FetchContent` 均可;嫌麻烦的话,直接把 `include/flexplanner/planner.hpp` 这一个文件拷走也能用。

```cmake
add_subdirectory(flexplanner)
target_link_libraries(your_target PRIVATE flexplanner::flexplanner)
```

```sh
cmake -S . -B build && cmake --build build && ctest --test-dir build
```

## 目录结构

```text
include/flexplanner/planner.hpp   库本体(单头文件)
examples/recent_trade_vs_mid.cpp  博客示例
tests/test_planner.cpp            运行时套件: 10 个章节, 74 项检查
tests/compile_fail/               编译失败用例(两类环 + 缺失依赖)
.github/workflows/ci.yml          Linux/macOS/Windows 构建 + ctest
```

## 测试

运行时套件给每个 handle/eval 插了一条日志,于是**每次分发之后的日志快照就是计划本身**。测试可以直接对计划断言:精确的步骤序列、先后不变式、去重次数、预热与驱逐的逐笔算术、成交时刻的 mid 新鲜度、查询计划的惰性,以及"反序声明完整重放、终值全等"的顺序无关性。共 10 个章节 74 项检查;另有 3 个编译失败用例由 ctest 验证"确实编不过,且报错里带着那句话"。完整清单见 `tests/test_planner.cpp`。

## 许可

[MIT](LICENSE)
