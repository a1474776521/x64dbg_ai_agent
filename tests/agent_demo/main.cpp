// tests/agent_demo/main.cpp
//
// Agent 工具调用测试程序
// ============================================================
// 设计目标：
//   为 x64dbg AI 插件的 M4 Agent 工具调用功能提供"谜题型"测试目标。
//   每个场景都是一段"光看反汇编 / 单看一个工具" 不容易看明白的代码，
//   迫使 LLM 真正组合多种工具（read_memory / disasm_at / read_string /
//   list_xrefs_to / find_pattern / ...）一步步弄明白函数意图。
//
// 同时也是验证下列功能的目标进程：
//   - 反汇编右键 AI ▶ 子菜单（M4.6f）：选不同预设跑同一段代码
//   - ToolCallCard 折叠卡片（M4 Agent UI）：观察工具调用序列
//   - ReasoningBlock 思考过程折叠面板（M4.6 Reasoning UI）：
//     用 deepseek-reasoner 跑，观察思考链按预期生成
//   - PresetEditorDialog（M4.6e）：增删改预设、勾选/取消工具，
//     回到本程序触发，验证 enabledTools 透传是否生效
//
// 测试场景：
//   S1: XOR 滚动 key 解码字符串（藏明文）
//   S2: CRC32 表驱动校验（藏 polynomial 与预期值）
//   S3: 5 callsite 状态机（验证 list_xrefs_to）
//   S4: 魔数 + 立即数 mov 代码模式（验证 find_pattern 通配）
//
// 推荐操作流程见 README.md。
//
// 命令行：
//   agent_demo.exe                   交互模式：每个场景按回车继续
//   agent_demo.exe --no-wait         一口气跑完所有场景（自动化）
//   agent_demo.exe --only=2          只跑场景 N（1..4）
//   agent_demo.exe --loop            循环跑（每轮间隔 1s），方便长时间挂
// ============================================================
#include <Windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "demo.h"

namespace {

void Pause(const char* hint, bool wait)
{
    if (!wait) return;
    std::printf("\n>>> %s  (PRESS ENTER) <<<\n", hint);
    (void)std::getchar();
}

void RunStage(int n, bool wait)
{
    switch (n) {
    case 1:
        Pause("S1: XOR 解码字符串。请先在 agent::s1::Stage1_DecodeAndPrint 下断", wait);
        agent::s1::Stage1_DecodeAndPrint();
        break;
    case 2:
        Pause("S2: CRC32 校验。请先在 agent::s2::Stage2_VerifyLicense 下断", wait);
        agent::s2::Stage2_VerifyLicense();
        break;
    case 3:
        Pause("S3: 状态机。请先在 agent::s3::Stage3_RunProtocol 下断", wait);
        agent::s3::Stage3_RunProtocol();
        break;
    case 4:
        Pause("S4: 魔数 + find_pattern。请先在 agent::s4::Stage4_TouchMagics 下断", wait);
        agent::s4::Stage4_TouchMagics();
        break;
    default:
        std::printf("[main] unknown stage %d\n", n);
        break;
    }
}

}  // namespace

int main(int argc, char** argv)
{
    // 控制台输出 UTF-8（源文件 UTF-8 + /utf-8，cmd 默认 CP936 否则乱码）
    SetConsoleOutputCP(CP_UTF8);

    bool wait = true;
    bool loop = false;
    int  only = 0;   // 0 = 全部

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--no-wait")     wait = false;
        else if (a == "--loop")   loop = true;
        else if (a.rfind("--only=", 0) == 0) only = std::atoi(a.c_str() + 7);
        else if (a == "--help" || a == "-h") {
            std::printf(
                "Usage: agent_demo.exe [--no-wait] [--loop] [--only=N]\n"
                "  --no-wait   Do not pause for keypress between stages\n"
                "  --loop      Run all stages in a loop\n"
                "  --only=N    Run only stage N (1..4)\n");
            return 0;
        }
    }

    std::printf("agent_demo (PID=%lu)\n", GetCurrentProcessId());
    std::printf("Attach x64dbg now, set breakpoints on agent::sX::StageX_* functions.\n");
    Pause("ready?", wait);

    int round = 0;
    do {
        if (loop) std::printf("\n========== Round %d ==========\n", ++round);
        if (only == 0) {
            for (int s = 1; s <= 4; ++s) RunStage(s, wait);
        } else {
            RunStage(only, wait);
        }
        if (loop) Sleep(1000);
    } while (loop);

    Pause("All done. ENTER to exit", wait);
    return 0;
}
