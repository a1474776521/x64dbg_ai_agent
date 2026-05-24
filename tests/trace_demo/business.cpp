// tests/trace_demo/business.cpp
//
// 业务调用链：模拟一次下单流程，作为多层嵌套调用的主测试目标。
//
// 调用结构：
//   ProcessOrder
//     ├─ ValidateOrder
//     │     └─ CheckInventory
//     ├─ CalcTotal
//     │     ├─ GetPrice
//     │     └─ ApplyTax
//     ├─ ApplyDiscount   (仅 premium=true)
//     │     └─ LoadCoupon
//     └─ SaveToDb
//           ├─ OpenConnection
//           ├─ ExecInsert
//           └─ CloseConnection
//
// 所有函数都做了简单 printf + 少量计算，避免被编译器优化为 inline。
#include "demo.h"

#include <Windows.h>
#include <cstdio>
#include <string>

namespace demo::business {

namespace {

// 用 volatile 防止整体被优化掉
volatile int g_sink = 0;

// Release(/O2) 会把 TU 内的 static helper 全部 inline 进 ProcessOrder，
// 导致 trace 抓不到 ValidateOrder/CalcTotal/SaveToDb 等用户级 call 指令。
// 用 NOINLINE 强制保留真实的 call/ret 序列，方便 CallGraph 验证。
#define NOINLINE __declspec(noinline)

NOINLINE bool CheckInventory(int orderId, int qty)
{
    std::printf("    [CheckInventory] order=%d qty=%d\n", orderId, qty);
    g_sink += orderId + qty;
    return qty > 0 && qty < 1000;
}

NOINLINE bool ValidateOrder(int orderId, int qty)
{
    std::printf("  [ValidateOrder] order=%d\n", orderId);
    if (!CheckInventory(orderId, qty)) return false;
    g_sink += 1;
    return true;
}

NOINLINE int GetPrice(int orderId)
{
    std::printf("    [GetPrice] order=%d\n", orderId);
    return 100 + (orderId % 50);
}

NOINLINE int ApplyTax(int subtotal)
{
    std::printf("    [ApplyTax] subtotal=%d\n", subtotal);
    return subtotal + subtotal / 10;
}

NOINLINE int CalcTotal(int orderId, int qty)
{
    std::printf("  [CalcTotal] order=%d qty=%d\n", orderId, qty);
    int price = GetPrice(orderId);
    int sub   = price * qty;
    return ApplyTax(sub);
}

NOINLINE int LoadCoupon(int orderId)
{
    std::printf("    [LoadCoupon] order=%d\n", orderId);
    return 5 + (orderId % 7);
}

NOINLINE int ApplyDiscount(int orderId, int total)
{
    std::printf("  [ApplyDiscount] order=%d total=%d\n", orderId, total);
    int coupon = LoadCoupon(orderId);
    return total - coupon;
}

NOINLINE void OpenConnection()
{
    std::printf("    [OpenConnection]\n");
    Sleep(20);
}

NOINLINE void ExecInsert(int orderId, int total)
{
    std::printf("    [ExecInsert] order=%d total=%d\n", orderId, total);
    g_sink += total;
}

NOINLINE void CloseConnection()
{
    std::printf("    [CloseConnection]\n");
    Sleep(10);
}

NOINLINE void SaveToDb(int orderId, int total)
{
    std::printf("  [SaveToDb] order=%d total=%d\n", orderId, total);
    OpenConnection();
    ExecInsert(orderId, total);
    CloseConnection();
}

}  // namespace

// 入口：在此处下断 -> "AI: 追溯函数调用链"
// 预期 CallGraph 至少能看到：
//   ProcessOrder
//     ValidateOrder -> CheckInventory
//     CalcTotal -> GetPrice / ApplyTax
//    [ApplyDiscount -> LoadCoupon]   (premium 分支)
//     SaveToDb -> OpenConnection / ExecInsert / CloseConnection
bool ProcessOrder(int orderId, int qty, bool premium)
{
    std::printf("[ProcessOrder] order=%d qty=%d premium=%s\n",
                orderId, qty, premium ? "true" : "false");

    if (!ValidateOrder(orderId, qty)) {
        std::printf("[ProcessOrder] validation failed\n");
        return false;
    }

    int total = CalcTotal(orderId, qty);
    if (premium) {
        total = ApplyDiscount(orderId, total);
    }
    SaveToDb(orderId, total);

    std::printf("[ProcessOrder] done, total=%d\n", total);
    return true;
}

}  // namespace demo::business
