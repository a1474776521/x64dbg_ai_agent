// =====================================================================
// login_demo_plain.cpp
//
// 最简单的硬编码密码校验样本。
// 校验路径：QString::operator== → 内部 wcscmp（CRT 函数）
//
// 正确凭据：
//   用户名: admin
//   密码:   x64dbg2026
//
// 逆向练习目标（agent 工具链对照）：
//   1. scan_strings 应直接命中 L"admin" / L"x64dbg2026" / L"Login OK"
//   2. list_xrefs_to(addr_of_string) 找到比较点
//   3. set_breakpoint(addr_of_cmp_after_call) + 单步看 ZF
//   4. patch_memory 把校验后的 JNE/JE 翻转 → 任意密码登录
//   5. patch_file 把上述 patch 落盘
//
// 编译：CMake POST_BUILD 会跑 windeployqt 拷 platform plugin
// =====================================================================

#include <QApplication>
#include <QWidget>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QMessageBox>

// 故意定义为全局可寻址常量字符串（不要 constexpr 内联掉）
// 让 scan_strings 在 .rdata 段直接命中
static const wchar_t* kExpectedUser     = L"admin";
static const wchar_t* kExpectedPassword = L"x64dbg2026";
static const wchar_t* kAppTitle         = L"Login Demo (plain)";
static const wchar_t* kMsgSuccess       = L"Login OK - welcome admin!";
static const wchar_t* kMsgFailUser      = L"Wrong username.";
static const wchar_t* kMsgFailPwd       = L"Wrong password.";

// ---------------------------------------------------------------------
// 关键校验函数（noinline 防止编译器把它折叠到 onLogin 里）
//   返回值：0=OK / 1=user 错 / 2=pwd 错
// ---------------------------------------------------------------------
#pragma optimize("", off)
__declspec(noinline)
static int checkCredentials(const QString& user, const QString& password)
{
    // 用 toStdWString + wcscmp 而不是 QString==，让 CRT 函数显式出现
    // x64dbg 里能直接 bp wcscmp 抓到调用点
    std::wstring u = user.toStdWString();
    std::wstring p = password.toStdWString();

    if (wcscmp(u.c_str(), kExpectedUser) != 0) {
        return 1;
    }
    if (wcscmp(p.c_str(), kExpectedPassword) != 0) {
        return 2;
    }
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
        setFixedSize(320, 160);

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
                default:
                    QMessageBox::critical(this, "Error",
                        QString("unexpected rc=%1").arg(rc));
                    break;
            }
        });
    }
};

#include "login_demo_plain.moc"

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    LoginWindow w;
    w.show();
    return app.exec();
}
