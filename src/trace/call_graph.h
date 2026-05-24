// trace/call_graph.h
//
// CallGraph：把 TraceEvent 序列重建为调用树。
//
// 算法：
//   - 维护一个"当前节点"指针和一个 SP 栈
//   - Call 事件：以 callee 为新节点挂到当前节点 children；当前节点切换为新节点；记录其 sp
//   - Ret  事件：把当前节点切回到 sp >= 当前事件 sp 的最近祖先（处理 ret N 跨多层）
//
// 反向查询：findCallers(target) 在已构建树里 DFS 找出所有以 callee==target 结尾的路径。
//
// 视图层（CallNodeView）：在原始 CallNode 树之上做"折叠"和"过滤"，仅供 UI 渲染：
//   - 同一 caller 下重复进入相同 callee 的兄弟会合并为一个 view 节点，hits 累加
//   - 系统模块（ucrtbase/kernel32/ntdll 等）可整体折叠为占位节点
//   - 这样 1467 nodes 可以折叠到几十个，UI 不再被 CRT 噪声淹没
#pragma once

#include "trace/trace_event.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace x64ai {

struct CallNode {
    uint64_t                              addr     = 0;       // callee 地址（根节点为 0）
    std::string                           sym;                // 可读符号
    uint32_t                              hits     = 0;       // 该节点被进入次数（合并同 callee 兄弟时累加）
    uint64_t                              firstSeq = 0;       // 第一次进入时的事件 seq
    uint64_t                              spOnEntry = 0;      // 进入时栈指针，用于配对
    CallNode*                             parent   = nullptr;
    std::vector<std::unique_ptr<CallNode>> children;

    // 不合并兄弟（按时序保留每次进入），仅供 UI 渲染参考
};

struct CallPath {
    std::vector<CallNode*> nodes;   // 从根到目标节点（不含根）
};

// ---------------- 视图层 ----------------

struct CallNodeView {
    uint64_t                                  addr      = 0;
    std::string                               sym;
    std::string                               module;       // 模块名（小写，不带后缀），便于 UI 上色
    uint32_t                                  hits      = 0; // 折叠后总进入次数
    uint64_t                                  firstSeq  = 0;
    bool                                      isFolded  = false; // 是否为折叠占位（如"<system: 12 calls>"）
    std::vector<std::unique_ptr<CallNodeView>> children;
};

struct BuildViewOptions {
    // 隐藏系统模块（ucrtbase/kernel32/ntdll/...）：把系统模块下的整棵子树折叠为
    // 一个占位节点 "<system: N calls>"，避免 CRT 噪声淹没业务调用。
    bool hideSystemModules = true;

    // 合并相同 callee 的兄弟节点：同一父节点下 addr 相同的子节点合并为 1 个，
    // hits 累加，子节点继续递归合并。
    bool mergeSiblings = true;

    // 仅显示 hits >= minHits 的节点（0 = 不过滤）
    uint32_t minHits = 0;
};

class CallGraph {
public:
    // 由事件序列重建。返回根节点（addr=0）。
    static std::unique_ptr<CallNode> build(const std::vector<TraceEvent>& events);

    // 在 root 子树中查找所有 addr == target 的节点路径
    static std::vector<CallPath> findCallers(CallNode* root, uint64_t target);

    // 统计：节点总数 / 最大深度
    static void summarize(CallNode* root, std::size_t* nodes, std::size_t* maxDepth);

    // 视图：基于 root 构建一棵折叠+过滤后的只读视图树
    static std::unique_ptr<CallNodeView> buildView(CallNode* root,
                                                   const BuildViewOptions& opts);

    // 判定模块名是否属于"系统模块"（不区分大小写、不含后缀）
    static bool isSystemModule(const std::string& mod);
};

}  // namespace x64ai
