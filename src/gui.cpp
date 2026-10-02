#include "gui/window.hpp"

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setApplicationName("Stellastack");
    app.setOrganizationName("Stellastack");
    app.setApplicationVersion(STELLASTACK_VERSION);
    ss::gui::Window window;
    auto args = app.arguments();
    if (args.size() > 1 && !args[1].startsWith("--"))
        window.open(args[1]);
    window.show();
    if (args.contains("--smoke-test"))
        QTimer::singleShot(500, &app, &QCoreApplication::quit);
    return app.exec();
}
