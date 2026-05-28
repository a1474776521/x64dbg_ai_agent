// dbg/event_bus.h
//
// EventBus：进程内调试器事件总线（单例，S2-A）。
//
// 背景：
//   x64dbg SDK 同一 (plugin, CBTYPE) 后注册者覆盖前注册者。历史上 trace_recorder
//   单独 _plugin_registercallback(CB_BREAKPOINT) 与 plugin_callbacks 互相挤掉。
//   本总线把所有调试器事件回调集中到 plugin_callbacks 注册一次，再分发给任意多
//   个消费者，并提供同步 wait_for_event 原语供 T-06 工具使用。
//
// 事件来源：
//   plugin_callbacks::cb* 中 publish；不直接 include x64dbg SDK 类型，
//   payload 用裸 uint64_t/void* 承载，调用方自行 cast。
//
// 线程：
//   publish() 由 x64dbg 调试线程调用（同 CB 上下文）；
//   subscribe()/unsubscribe()/waitOnce() 可在任意线程；
//   handler 在 publish 线程同步执行——必须短小（不要 sleep / 不要发 SDK 命令导致回调嵌套）。
//
// 使用例：
//   auto tok = EventBus::instance().subscribe(DbgEvent::Breakpoint,
//       [](const DbgEventPayload& p) { recorder->onBreakpoint(p.addr); });
//   ...
//   EventBus::instance().unsubscribe(tok);
//
//   // 工具线程：
//   DbgEventPayload p;
//   if (EventBus::instance().waitOnce(DbgEvent::Paused, 5000ms, &p, &cancel)) {
//       // 命中
//   }
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace x64ai {

enum class DbgEvent : std::uint8_t {
    Breakpoint   = 0,   // CB_BREAKPOINT；payload.addr = bp 地址
    Paused       = 1,   // CB_PAUSEDEBUG
    Resumed      = 2,   // CB_RESUMEDEBUG
    Stepped      = 3,   // CB_STEPPED
    DebugStarted = 4,   // 预留（CB_INITDEBUG 已由 ProjectContext 直接处理）
    DebugStopped = 5,   // 预留
    ProjectStoreReady = 6, // S3：ProjectContext 后台线程装好 store 后广播（UI 刷新状态栏/会话列表）
                           // payload.raw = const std::string* (UTF-8 sha256Hex；handler 内须立刻拷贝)
    _Count       = 7,
};

struct DbgEventPayload {
    DbgEvent      kind   = DbgEvent::Paused;
    std::uint64_t addr   = 0;     // Breakpoint: bp 地址；Stepped: 当前 CIP（若知）
    void*         raw    = nullptr; // 原始 SDK 结构（如 BRIDGEBP*）；handler 自行 cast
    std::uint64_t seq    = 0;     // 单调递增序号（每个 publish++）
};

class EventBus {
public:
    static EventBus& instance();

    using Token   = std::uint64_t;            // 0 表示无效
    using Handler = std::function<void(const DbgEventPayload&)>;

    // 订阅；返回非 0 token。多次订阅同一事件按注册顺序触发。
    Token subscribe(DbgEvent ev, Handler h);

    // 取消订阅；不存在的 token 静默忽略。
    void  unsubscribe(Token t);

    // 阻塞等待 ev 一次（最多 timeout）。
    //   outPayload 非空时拷贝命中的 payload。
    //   cancelFlag 非空且置为 true 时立刻返回 false。
    //   返回值：true=命中事件；false=超时或被取消。
    // 多个 waiter 等同一事件时全部唤醒；每个 waiter 仅消费一次。
    // 内部使用 condition_variable，等待期间不持有任何调试线程锁。
    bool waitOnce(DbgEvent ev,
                  std::chrono::milliseconds timeout,
                  DbgEventPayload* outPayload = nullptr,
                  const std::atomic<bool>* cancelFlag = nullptr);

    // 调试线程发布事件。同步触发所有 handler + 唤醒所有 waiter。
    void publish(DbgEvent ev, const DbgEventPayload& payload);

    // 取消所有 wait（如调试停止时）。已 waiting 的 waitOnce 返回 false。
    void cancelAllWaits();

    // 调试用：当前订阅总数
    std::size_t subscriberCount(DbgEvent ev) const;

private:
    EventBus() = default;
    EventBus(const EventBus&)            = delete;
    EventBus& operator=(const EventBus&) = delete;

    struct Sub { Token token; Handler handler; };

    static std::size_t idx(DbgEvent ev) { return static_cast<std::size_t>(ev); }

    mutable std::mutex      subMu_;
    std::vector<Sub>        subs_[static_cast<std::size_t>(DbgEvent::_Count)];
    std::atomic<Token>      tokenGen_{0};

    // wait 端：每个事件一个 (mutex, cv, latest payload, sequence)
    struct WaitSlot {
        std::mutex              mu;
        std::condition_variable cv;
        DbgEventPayload         lastPayload{};
        std::uint64_t           lastSeq = 0;   // ++ 每次 publish；waiter 比较初值即可知"新一次"
        bool                    cancelled = false;
    };
    WaitSlot waitSlots_[static_cast<std::size_t>(DbgEvent::_Count)];

    std::atomic<std::uint64_t> seqGen_{0};
};

}  // namespace x64ai
