// =====================================================================
// login_demo_antidbg.cpp
//
// 在 plain 校验之上加 3 层反调试，任一命中即拒绝登录。
// 三层（与 K-39 plugin 侧 get_anti_debug_flags 工具对应）：
//   L1 - IsDebuggerPresent()                    （最朴素，PEB.BeingDebugged 直读 API）
//   L2 - PEB.NtGlobalFlag 检查 0x70 位            （x64 PEB+0xBC）
//   L3 - QueryPerformanceCounter Timing 检测     （sleep(1) 实际 elapsed > 阈值 = 被单步）
//
// 正确凭据（同 plain）：
//   用户名: admin
//   密码:   x64dbg2026
//
// 逆向练习目标：
//   1. 启动会被 L1 拦下 → 弹 "Debugger detected!" 而非到达校验
//   2. 用 K-39 get_anti_debug_flags 探 PEB → 确认 BeingDebugged/NtGlobalFlag
//   3. patch_memory 把 IsDebuggerPresent 返回值改 0 或跳过整个 antiDebugCheck
//   4. 绕过后 timing 检测会随机命中（单步过 sleep 区域）→ patch 第三处
//   5. 全部绕过后到 plain 校验逻辑（密码同 plain）
//
// 注意：本程序未做反附加（无 anti-attach），方便 attach_debug 测试。
// =====================================================================

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <winternl.h>

#include <QApplication>
#include <QWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QMessageBox>
#include <QTimer>

static const wchar_t* kExpectedUser     = L"admin";
static const wchar_t* kExpectedPassword = L"x64dbg2026";
static const wchar_t* kAppTitle         = L"Login Demo (antidbg)";
static const wchar_t* kMsgSuccess       = L"Login OK - welcome admin!";
static const wchar_t* kMsgFailUser      = L"Wrong username.";
static const wchar_t* kMsgFailPwd       = L"Wrong password.";
static const wchar_t* kMsgDebuggerL1    = L"Debugger detected (L1: IsDebuggerPresent)";
static const wchar_t* kMsgDebuggerL2    = L"Debugger detected (L2: PEB.NtGlobalFlag)";
static const wchar_t* kMsgDebuggerL3    = L"Debugger detected (L3: Timing anomaly)";

// ---------------------------------------------------------------------
// 反调试层（每层独立 noinline，方便单独 patch）
// ---------------------------------------------------------------------

#pragma optimize("", off)

__declspec(noinline)
static bool antiDebugL1_IsDebuggerPresent()
{
    return ::IsDebuggerPresent() != FALSE;
}

__declspec(noinline)
static bool antiDebugL2_NtGlobalFlag()
{
    // x64 PEB 偏移：NtGlobalFlag 在 PEB+0xBC
    // （x86 是 PEB+0x68，本 exe 仅 x64 编译）
#ifdef _WIN64
    const PEB* peb = reinterpret_cast<PEB*>(__readgsqword(0x60));
    const ULONG flag = *reinterpret_cast<const ULONG*>(
        reinterpret_cast<const uint8_t*>(peb) + 0xBC);
#else
    const PEB* peb = reinterpret_cast<PEB*>(__readfsdword(0x30));
    const ULONG flag = *reinterpret_cast<const ULONG*>(
        reinterpret_cast<const uint8_t*>(peb) + 0x68);
#endif
    // 0x70 = FLG_HEAP_ENABLE_TAIL_CHECK | FLG_HEAP_ENABLE_FREE_CHECK
    //        | FLG_HEAP_VALIDATE_PARAMETERS （调试器附加时 OS 自动设）
    return (flag & 0x70) == 0x70;
}

__declspec(noinline)
static bool antiDebugL3_Timing()
{
    // 测一段 Sleep(1) + 几条空操作的实际耗时
    // 正常情况下 wall-clock < 50 ms；单步/断点会显著放大
    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    Sleep(1);
    // 几条无副作用指令：让单步跑到这里的人多停几下
    volatile int sum = 0;
    for (int i = 0; i < 16; ++i) { sum += i; }

    QueryPerformanceCounter(&t1);
    const double elapsedMs = 1000.0 * (t1.QuadPart - t0.QuadPart) / freq.QuadPart;
    return elapsedMs > 200.0;  // 阈值留宽，避免 CI/慢机误报
}

// ---------------------------------------------------------------------
// 主校验：先反调试三层，再 plain 密码校验
//   返回值：0=OK / 1=user 错 / 2=pwd 错 / 11/12/13=反调试 L1/L2/L3 命中
// ---------------------------------------------------------------------
__declspec(noinline)
static int checkCredentials(const QString& user, const QString& password)
{
    if (antiDebugL1_IsDebuggerPresent()) return 11;
    if (antiDebugL2_NtGlobalFlag())      return 12;
    if (antiDebugL3_Timing())            return 13;

    std::wstring u = user.toStdWString();
    std::wstring p = password.toStdWString();

    if (wcscmp(u.c_str(), kExpectedUser)     != 0) return 1;
    if (wcscmp(p.c_str(), kExpectedPassword) != 0) return 2;
    return 0;
}

#pragma optimize("", on)

// ---------------------------------------------------------------------
class LoginWindow : public QWidget {
    Q_OBJECT
public:
    LoginWindow(QWidget* parent = nullptr) : QWidget(parent)
    {
        setWindowTitle(QString::fromWCharArray(kAppTitle));
        setFixedSize(360, 180);

        auto* userEdit = new QLineEdit(this);
        userEdit->setPlaceholderText("username");

        auto* pwdEdit = new QLineEdit(this);
        pwdEdit->setEchoMode(QLineEdit::Password);
        pwdEdit->setPlaceholderText("password");

        auto* loginBtn = new QPushButton("Login", this);
        loginBtn->setDefault(true);

        auto* form = new QFormLayout;
        form->addRow("User:", userEdit);
        form->addRow("Pass:", pwdEdit);

        auto* root = new QVBoxLayout(this);
        root->addLayout(form);
        root->addWidget(loginBtn);

        connect(loginBtn, &QPushButton::clicked, this, [=]() {
            const int rc = checkCredentials(userEdit->text(), pwdEdit->text());
            switch (rc) {
                case 0:
                    QMessageBox::information(this, "OK",
                        QString::fromWCharArray(kMsgSuccess));
                    break;
                case 1:
                    QMessageBox::warning(this, "Fail",
                        QString::fromWCharArray(kMsgFailUser));
                    break;
                case 2:
                    QMessageBox::warning(this, "Fail",
                        QString::fromWCharArray(kMsgFailPwd));
                    break;
                case 11:
                    QMessageBox::critical(this, "Anti-Debug",
                        QString::fromWCharArray(kMsgDebuggerL1));
                    break;
                case 12:
                    QMessageBox::critical(this, "Anti-Debug",
                        QString::fromWCharArray(kMsgDebuggerL2));
                    break;
                case 13:
                    QMessageBox::critical(this, "Anti-Debug",
                        QString::fromWCharArray(kMsgDebuggerL3));
                    break;
                default:
                    QMessageBox::critical(this, "Error",
                        QString("unexpected rc=%1").arg(rc));
                    break;
            }
        });
    }
};

#include "login_demo_antidbg.moc"

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    LoginWindow w;
    w.show();
    return app.exec();
}
