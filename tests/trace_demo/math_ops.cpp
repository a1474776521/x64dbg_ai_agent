// tests/trace_demo/math_ops.cpp
//
// 嵌套静态函数链（深度 4）：A -> B -> C -> D
// 跨 TU 暴露 LayerA。在 LayerA 入口下断 -> 追溯应看到 LayerB/C/D。
#include "demo.h"

#include <cstdio>

namespace demo::math {

namespace {

#define NOINLINE __declspec(noinline)

NOINLINE int LeafD(int x)
{
    std::printf("    [LeafD]   x=%d\n", x);
    return x + 1;
}

NOINLINE int LayerC(int x)
{
    std::printf("   [LayerC]  x=%d\n", x);
    return LeafD(x) * 2;
}

NOINLINE int LayerB(int x)
{
    std::printf("  [LayerB]  x=%d\n", x);
    return LayerC(x) + 3;
}

}  // namespace

int LayerA(int x)
{
    std::printf(" [LayerA]  x=%d\n", x);
    return LayerB(x) + 5;
}

}  // namespace demo::math
