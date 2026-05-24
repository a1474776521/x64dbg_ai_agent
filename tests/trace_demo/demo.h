// tests/trace_demo/demo.h
//
// 跨翻译单元的接口声明，确保 business / math 模块产生
// 真实的非 inline call 指令，便于 trace 捕获。
#pragma once

namespace demo::business {

// 模拟下单业务：内部依次调用 Validate / CalcTotal / ApplyDiscount / SaveToDb
// premium=true 时启用会员折扣分支。
bool ProcessOrder(int orderId, int qty, bool premium);

}  // namespace demo::business

namespace demo::math {

unsigned long long Factorial(unsigned n);
int                LayerA(int x);

}  // namespace demo::math
