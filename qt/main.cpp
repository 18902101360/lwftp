/*
 * lwftp Qt 程序入口。
 * 按 Qt 5.14 编写：高分屏属性要在 QApplication 创建之前设置。
 */

#include "mainwindow.h"

#include <QApplication>

/* 创建主窗口并进入事件循环。FTP 的套接字回调由这个循环驱动。 */
int main(int argc, char *argv[])
{
#if QT_VERSION >= QT_VERSION_CHECK(5, 6, 0)
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
#endif

    QApplication app(argc, argv);
    MainWindow window;
    window.show();
    return app.exec();
}
