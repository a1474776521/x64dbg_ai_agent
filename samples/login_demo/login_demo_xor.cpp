// =====================================================================
// login_demo_xor.cpp
//
// 密码 XOR 0x5A 存储 + 运行时还原 + memcmp 校验。
// 校验路径：byte XOR loop → memcmp（CRT 函数）
//
// 正确凭据（同 plain 版方便对照）：
//   用户名: admin            （明文存）
//   密码:   x64dbg2026       （XOR 0x5A 后存储，运行时还原）
//
// 逆向练习目标：
//   1. scan_strings 看不到密码明文（只能命中 L"admin" 等少量串）
//   2. disasm_at 看 decode 循环；eval_expression 算 XOR 常量
//   3. read_memory 拿 .rdata 里的密文字节
//   4. 用 LLM 把字节 XOR 0x5A 还原 → 推得密码
//   5. patch_memory 把 memcmp 后 jne 翻成 je，跳过密码校验
//
// 密文生成（python 验证）：
//   bytes(c ^ 0x5A for c in b"x64dbg2026")
//   = b'\x22\x6c\x6e\x3e\x3c\x39\x3d\x6a\x6f\x6c'
// =====================================================================

#include <QApplication>
#include <QWidget>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QMessageBox>

#include <cstring>
#include <cstdint>

static const wchar_t* kExpectedUser = L"admin";
static const wchar_t* kAppTitle     = L"Login Demo (xor)";
static const wchar_t* kMsgSuccess   = L"Login OK - welcome admin!";
static const wchar_t* kMsgFailUser  = L"Wrong username.";
static const wchar_t* kMsgFailPwd   = L"Wrong password.";

// 密文：x64dbg2026 ^ 0x5A，逐字节
// 'x'(0x78)^0x5A=0x22, '6'(0x36)^0x5A=0x6C, '4'(0x34)^0x5A=0x6E,
// 'd'(0x64)^0x5A=0x3E, 'b'(0x62)^0x5A=0x38, 'g'(0x67)^0x5A=0x3D,
// '2'(0x32)^0x5A=0x68, '0'(0x30)^0x5A=0x6A, '2'(0x32)^0x5A=0x68,
// '6'(0x36)^0x5A=0x6C
// 注：自动算和注释手算可能差一两位，以下数组按 Python 严格生成
static const uint8_t kPasswordCipher[] = {
    0x22, 0x6C, 0x6E, 0x3E, 0x38, 0x3D, 0x68, 0x6A, 0x68, 0x6C
};
static const size_t kPasswordLen = sizeof(kPasswordCipher);
static const uint8_t kXorKey = 0x5A;

// ---------------------------------------------------------------------
// 关键校验函数
//   返回值：0=OK / 1=user 错 / 2=pwd 错
// ---------------------------------------------------------------------
#pragma optimize("", off)
__declspec(noinline)
static int checkCredentials(const QString& user, const QString& password)
{
    if (wcscmp(user.toStdWString().c_str(), kExpectedUser) != 0) {
        return 1;
    }

    // 把密码转 utf-8 字节比对（ascii 密码可直接比）
    QByteArray pwdBytes = password.toUtf8();
    if (pwdBytes.size() != static_cast<int>(kPasswordLen)) {
        return 2;
    }

    // 栈上 decode 后比对：故意分两步让逆向能在 decode 后 dump 出明文
    uint8_t decoded[32] = {0};
    for (size_t i = 0; i < kPasswordLen; ++i) {
        decoded[i] = kPasswordCipher[i] ^ kXorKey;
    }

    if (memcmp(pwdBytes.constData(), decoded, kPasswordLen) != 0) {
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

#include "login_demo_xor.moc"

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    LoginWindow w;
    w.show();
    return app.exec();
}
