#include "MainWindow.h"
#include <QApplication>

int main(int argc, char *argv[]) {
    // 处理高分屏缩放
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    
    QApplication app(argc, argv);
    
    // 设置应用程序风格 (Fusion 风格在各平台表现一致且现代)
    app.setStyle("Fusion");
    
    MainWindow window;
    window.show();
    
    return app.exec();
}
