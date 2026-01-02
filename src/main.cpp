#include "MainWindow.h"
#include <QApplication>
#include <QIcon> // 新增头文件

int main(int argc, char *argv[]) {
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    
    QApplication app(argc, argv);
    
    app.setStyle("Fusion");
    
    // --- 新增代码: 设置全局窗口图标 ---
    // ":/app.ico" 对应 .qrc 文件中的 prefix("/") + file path
    app.setWindowIcon(QIcon(":/app.ico")); 
    // --------------------------------
    
    MainWindow window;
    window.show();
    
    return app.exec();
}
