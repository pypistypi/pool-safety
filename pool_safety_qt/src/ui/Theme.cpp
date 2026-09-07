#include "ui/Theme.h"

namespace ui {
namespace theme {

QString applicationStyleSheet()
{
    return QStringLiteral(R"(
QWidget {
    background-color: #15181d;
    color: #e6eaf0;
    font-family: "Segoe UI", sans-serif;
    font-size: 13px;
}
QFrame#Card {
    background-color: #1e232b;
    border: 1px solid #2f3742;
    border-radius: 8px;
}
QLabel#Title {
    font-size: 17px;
    font-weight: 600;
}
QLabel#Subtitle {
    color: #939fb0;
}
QPushButton {
    background-color: #2a313b;
    border: 1px solid #3a4450;
    border-radius: 6px;
    padding: 7px 14px;
}
QPushButton:hover   { background-color: #333c48; }
QPushButton:pressed { background-color: #232a33; }
QPushButton:disabled {
    color: #5d6874;
    background-color: #21262e;
    border-color: #2c333d;
}
QPushButton#AlarmButton {
    background-color: #b32d2d;
    border: 1px solid #ff4545;
    font-weight: 600;
    padding: 9px 18px;
}
QPushButton#AlarmButton:hover   { background-color: #cc3535; }
QPushButton#AlarmButton:pressed { background-color: #992626; }
QPushButton#PrimaryButton {
    background-color: #2f6fd0;
    border: 1px solid #4a9eff;
    font-weight: 600;
}
QPushButton#PrimaryButton:hover { background-color: #3a80e6; }
QPushButton#SourceButton {
    background-color: rgba(20, 24, 30, 200);
    border: 1px solid #3a4450;
    border-radius: 5px;
    padding: 4px 10px;
    font-size: 12px;
}
QPushButton#SourceButton:hover { background-color: rgba(45, 54, 66, 230); }
QPushButton#PanelAlarmButton {
    background-color: rgba(140, 34, 34, 210);
    border: 1px solid #ff4545;
    border-radius: 5px;
    padding: 4px 12px;
    font-size: 12px;
    font-weight: 600;
}
QPushButton#PanelAlarmButton:hover  { background-color: rgba(190, 46, 46, 235); }
QPushButton#PanelAlarmButton:disabled {
    background-color: rgba(60, 44, 44, 180);
    border-color: #6b4444;
    color: #8a7676;
}
QPushButton#CircumstanceButton {
    background-color: #3a2020;
    border: 1px solid #7a3838;
    border-radius: 6px;
    padding: 8px 14px;
    text-align: left;
    font-size: 14px;
    font-weight: 600;
}
QPushButton#CircumstanceButton:hover  { background-color: #4d2929; border-color: #ff4545; }
QPushButton#CircumstanceButton:pressed { background-color: #331c1c; }
QMenu {
    background-color: #1e232b;
    border: 1px solid #3a4450;
    padding: 4px;
}
QMenu::item { padding: 6px 22px 6px 14px; border-radius: 4px; }
QMenu::item:selected { background-color: #2f6fd0; }
QMenu::separator { height: 1px; background: #2f3742; margin: 4px 8px; }
QStatusBar { background-color: #1a1f26; color: #939fb0; }
QStatusBar::item { border: none; }
)");
}

} // namespace theme
} // namespace ui
