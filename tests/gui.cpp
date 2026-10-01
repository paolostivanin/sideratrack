// Exercise the real internal widgets and supervised worker with synthetic projects.
#define main siderastack_desktop_main
#include "../src/gui.cpp"
#undef main
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QtTest/QTest>
#include <iostream>

void check(bool ok, const char *message) {
    if (!ok)
        throw ss::Error(message);
}
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setApplicationName("SideraStack-gui-test");
    QTemporaryDir temp;
    qputenv("XDG_CACHE_HOME", (temp.path() + "/cache").toUtf8());
    try {
        const auto root = ss::fs::path(temp.path().toStdString());
        const auto projectPath = root / "scale.sidera";
        {
            ss::Project project(projectPath, true);
            project.transaction([&] {
                for (int i = 0; i < 10000; ++i) {
                    ss::Frame f;
                    f.path = root / ("frame-" + std::to_string(i) + ".fits");
                    f.kind = "light";
                    f.filter = i % 2 ? "L" : "Ha";
                    f.session = "night-" + std::to_string(i % 4);
                    f.descriptor.width = f.descriptor.height = 160;
                    f.descriptor.header["EXPTIME"] = 60;
                    f.metrics.hfr = i % 2 ? 2 : 10;
                    f.metrics.stars = 500;
                    for (int j = 0; j < 500; ++j)
                        f.stars.push_back(
                            {double(j % 25 * 5 + 10), double(j / 25 * 5 + 10), 1000.0 + j, 3, 2, .1});
                    project.save(f);
                }
            });
        }
        QElapsedTimer elapsed;
        elapsed.start();
        Window window;
        window.open(QString::fromStdString(projectPath.string()));
        window.show();
        QTest::qWait(100);
        check(window.model.rowCount() == 10000, "10,000 frame model");
        check(elapsed.elapsed() < 10000, "10,000 frame open time exceeds ten seconds");
        std::cout << "10,000 frame open/paint: " << elapsed.elapsed() << " ms\n";
        window.proxy.sort(9, Qt::AscendingOrder);
        check(window.proxy.index(0, 9).data().toDouble() == 2, "metric numeric sort");
        window.proxy.sort(9, Qt::DescendingOrder);
        check(window.proxy.index(0, 9).data().toDouble() == 10, "descending metric sort");
        const auto groups = ss::calibrationPlan(window.model.frames);
        check(groups.size() == 4, "calibration groups ignore per-frame metadata");
        auto index = window.model.index(0, 0);
        check(window.model.setData(index, Qt::Unchecked, Qt::CheckStateRole), "exclude frame");
        check(window.project->frames()[0].selection == -1, "persist exclusion");
        window.setBusy(true);
        check(!window.model.setData(index, Qt::Checked, Qt::CheckStateRole), "busy edits disabled");
        window.setBusy(false);
        ss::Image im;
        im.width = im.height = 32;
        im.header["IMAGETYP"] = "Light";
        im.pixels.assign(im.samples(), 100);
        auto input = root / "imported.fits";
        ss::writeImage(input, im, ss::Format::Fits);
        int ticks = 0;
        QTimer timer;
        QObject::connect(&timer, &QTimer::timeout, [&] { ++ticks; });
        timer.start(10);
        window.start("import", {QString::fromStdString(input.string())});
        elapsed.restart();
        while ((window.worker.state() != QProcess::NotRunning || window.model.busy) &&
               elapsed.elapsed() < 15000)
            QTest::qWait(20);
        check(!window.model.busy && window.worker.exitCode() == 0, "supervised CLI import");
        check(window.model.rowCount() == 10001, "progressive import updates model");
        check(ticks > 0, "GUI event loop remains responsive");
        std::cout << "10,001 frame worker refresh: " << elapsed.elapsed() << " ms; " << ticks
                  << " UI ticks\n";
        ss::Image calibration = im;
        calibration.header["IMAGETYP"] = "Bias";
        calibration.header["EXPTIME"] = 0;
        calibration.pixels.assign(calibration.samples(), 80);
        auto bias = root / "bias.fits";
        ss::writeImage(bias, calibration, ss::Format::Fits);
        calibration.header["IMAGETYP"] = "Flat";
        calibration.header["EXPTIME"] = 1;
        calibration.header["FILTER"] = "L";
        calibration.pixels.assign(calibration.samples(), 1080);
        auto flat = root / "flat.fits";
        ss::writeImage(flat, calibration, ss::Format::Fits);
        auto waitWorker = [&] {
            elapsed.restart();
            while ((window.worker.state() != QProcess::NotRunning || window.model.busy) &&
                   elapsed.elapsed() < 20000)
                QTest::qWait(20);
            check(!window.model.busy && window.worker.exitCode() == 0, "calibration GUI worker");
        };
        window.start("import",
                     {QString::fromStdString(bias.string()), QString::fromStdString(flat.string())});
        waitWorker();
        window.start("masters",
                     {QString::fromStdString((root / "masters").string()), "--flat-calibration", "bias"});
        waitWorker();
        check(window.project->record("calibration-results")["masters"].toArray().size() == 2,
              "GUI exports bias and flat masters");
        window.close();
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
