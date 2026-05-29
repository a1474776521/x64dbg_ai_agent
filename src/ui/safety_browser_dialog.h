// ui/safety_browser_dialog.h
//
// 只读「安全护栏」对话框（K-32）。把分散在工具内部的 ACL/拒绝规则集中可视化：
//   Tab 1 - run_dbg_command 白名单（默认硬集 + 用户在 config.json 追加的）
//   Tab 2 - K-30 系统模块黑名单（断点防卡死，bpSysModules）
//   Tab 3 - K-30 高频 API 黑名单（断点防卡死，bpHotApis）
//   Tab 4 - 如何配置（一键打开 config.json、示例文本）
//
// 入口：tools_browser_dialog 顶部按钮 + 三个受影响工具的详情窗"查看安全护栏"链接
// （三个工具 = run_dbg_command / set_breakpoint / set_hw_breakpoint）。
//
// 重要：本对话框纯只读；config 修改要重启插件才生效（提示在 Tab 4）。
#pragma once

#include <QDialog>
#include <string>

class QTabWidget;
class QLineEdit;

namespace x64ai {

class SafetyBrowserDialog : public QDialog {
    Q_OBJECT
public:
    enum InitialTab {
        TabWhitelist     = 0,
        TabSysModules    = 1,
        TabHotApis       = 2,
        TabAutoApprove   = 3,
        TabHowTo         = 4,
    };

    explicit SafetyBrowserDialog(QWidget* parent, InitialTab initial = TabWhitelist);

private slots:
    void onOpenConfigJson();

private:
    void buildUi(InitialTab initial);
    QWidget* buildWhitelistTab();
    QWidget* buildSysModulesTab();
    QWidget* buildHotApisTab();
    QWidget* buildAutoApproveTab();
    QWidget* buildHowToTab();

    QTabWidget* tabs_ = nullptr;
};

}  // namespace x64ai
