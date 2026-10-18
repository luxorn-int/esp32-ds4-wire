#include "main_window.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("ESP32 Gamepad Bridge"));
    QApplication::setOrganizationName(QStringLiteral("GamepadBridge"));

    MainWindow window;
    window.show();
    return app.exec();
}
