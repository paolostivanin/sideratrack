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
        if (auto existing = qEnvironmentVariable("SIDERASTACK_REVIEW_PROJECT"); !existing.isEmpty()) {
            Window actual;
            actual.open(existing);
            actual.show();
            QTest::qWait(100);
            check(actual.analyzeAction->isEnabled(), "real project preparation readiness");
            int selected = 0;
            for (int row = 0; row < actual.model.rowCount() && selected < 2; ++row)
                if (actual.model.frames[size_t(row)].kind == "light") {
                    auto index = actual.proxy.mapFromSource(actual.model.index(row, 0));
                    actual.table.selectionModel()->select(index, QItemSelectionModel::Select |
                                                                     QItemSelectionModel::Rows);
                    actual.table.selectionModel()->setCurrentIndex(index, QItemSelectionModel::NoUpdate);
                    ++selected;
                }
            actual.blinkInterval.setValue(0.2);
            actual.blinkButton->setChecked(true);
            QElapsedTimer wait;
            wait.start();
            while ((actual.previewCache.size() < 2 || actual.previewThread) && wait.elapsed() < 30000)
                QTest::qWait(20);
            check(actual.previewCache.size() >= 2 && actual.blink.isActive(), "real dataset blink warmup");
            check(actual.view.pixels->pixmap().width() == 2048, "real blink uses smaller playback previews");
            auto renders = actual.previewRenders;
            QTest::qWait(1000);
            check(actual.previewRenders == renders, "real dataset warm playback avoids recomputation");
            auto screenshot = qEnvironmentVariable("SIDERASTACK_REVIEW_SCREENSHOT");
            if (!screenshot.isEmpty())
                check(actual.grab().save(screenshot), "real review screenshot");
            actual.blinkButton->setChecked(false);
            wait.restart();
            while (actual.previewThread && wait.elapsed() < 30000)
                QTest::qWait(20);
            check(actual.view.pixels->pixmap().width() == 6252, "real stop restores full resolution");
            QTimer::singleShot(200, [&] {
                for (auto *widget : QApplication::topLevelWidgets())
                    if (auto *dialog = qobject_cast<QDialog *>(widget);
                        dialog && dialog->windowTitle() == "Calibration assignments") {
                        if (!screenshot.isEmpty())
                            check(dialog->grab().save(screenshot + ".calibration.png"),
                                  "calibration summary screenshot");
                        dialog->reject();
                    }
            });
            actual.calibration();
            std::cout << "Real dataset GUI playback and assignment review passed\n";
            return 0;
        }
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
        // Exercise real progressive analysis and cached playback in a separate valid project.
        const auto reviewPath = root / "review.sidera";
        ss::Project reviewProject(reviewPath, true);
        auto reviewSettings = reviewProject.settings();
        reviewSettings.threads = 1;
        reviewSettings.memory = 256 * ss::MiB;
        reviewSettings.scratch = 128 * ss::MiB;
        reviewProject.settings(reviewSettings);
        ss::Image exposure;
        exposure.width = exposure.height = 512;
        exposure.header = {{"IMAGETYP", "Light"}, {"FILTER", "L"}, {"EXPTIME", 60}};
        exposure.pixels.assign(exposure.samples(), 100);
        for (int sy = 40; sy < 480; sy += 55)
            for (int sx = 40; sx < 480; sx += 55)
                for (int y = sy - 8; y <= sy + 8; ++y)
                    for (int x = sx - 8; x <= sx + 8; ++x)
                        exposure.pixels[size_t(y) * 512 + x] +=
                            float(1500 * std::exp(-((x - sx) * (x - sx) + (y - sy) * (y - sy)) / 8.0));
        auto reviewInputs = root / "review-inputs";
        ss::fs::create_directories(reviewInputs);
        for (int i = 0; i < 32; ++i)
            ss::writeImage(reviewInputs / ("light-" + std::to_string(i) + ".fits"), exposure,
                           ss::Format::Fits);
        exposure.header["IMAGETYP"] = "Bias";
        exposure.pixels.assign(exposure.samples(), 10);
        ss::writeImage(reviewInputs / "bias.fits", exposure, ss::Format::Fits);
        Window review;
        review.open(QString::fromStdString(reviewPath.string()));
        review.show();
        auto finish = [&] {
            elapsed.restart();
            while ((review.worker.state() != QProcess::NotRunning || review.model.busy) &&
                   elapsed.elapsed() < 30000)
                QTest::qWait(20);
            check(!review.model.busy && review.worker.exitCode() == 0, "review worker completes");
        };
        check(!review.analyzeAction->isEnabled(), "analysis requires calibrated lights");
        review.start("import", {QString::fromStdString(reviewInputs.string())});
        finish();
        review.start("calibrate");
        finish();
        check(review.analyzeAction->isEnabled(), "calibration enables analysis");
        review.proxy.sort(8, Qt::DescendingOrder);
        std::vector<int64_t> selected;
        for (int row = 0; row < review.model.rowCount() && selected.size() < 2; ++row)
            if (review.model.frames[size_t(row)].kind == "light") {
                selected.push_back(review.model.frames[size_t(row)].id);
                auto index = review.proxy.mapFromSource(review.model.index(row, 0));
                review.table.selectionModel()->select(index, QItemSelectionModel::Select |
                                                                 QItemSelectionModel::Rows);
                review.table.selectionModel()->setCurrentIndex(index, QItemSelectionModel::NoUpdate);
            }
        bool progressive = false;
        QTimer monitor;
        QObject::connect(&monitor, &QTimer::timeout, [&] {
            if (review.worker.state() == QProcess::Running)
                for (const auto &frame : review.model.frames)
                    progressive |= frame.kind == "light" && frame.metrics.stars > 0;
        });
        monitor.start(10);
        review.start("analyze");
        finish();
        monitor.stop();
        check(progressive, "analysis results appear before worker exits");
        auto selection = review.selectedRows();
        check(selection.size() == 2, "live sorted updates preserve selection");
        for (auto row : selection)
            check(std::find(selected.begin(), selected.end(), review.model.frames[row].id) != selected.end(),
                  "selected frame identities survive sorting");
        review.blinkInterval.setValue(0.1);
        review.blinkButton->setChecked(true);
        elapsed.restart();
        while ((review.previewCache.size() < 2 || review.previewThread) && elapsed.elapsed() < 10000)
            QTest::qWait(20);
        check(review.previewCache.size() >= 2 && review.blink.isActive(), "blink warms selected previews");
        auto renders = review.previewRenders;
        QTest::qWait(600);
        check(review.previewRenders == renders, "warm blink switches do not decode or stretch again");
        review.blinkInterval.setValue(5);
        check(review.blink.interval() == 5000, "blink speed updates during playback");
        review.blinkButton->setChecked(false);
        elapsed.restart();
        while (review.previewThread && elapsed.elapsed() < 10000)
            QTest::qWait(20);
        check(!review.blink.isActive() && review.view.pixels->pixmap().size() == QSize(512, 512),
              "stop restores full-resolution inspection");
        auto downsampled = exposure;
        downsampled.width = 3000;
        downsampled.height = 1000;
        downsampled.pixels.assign(downsampled.samples(), 100);
        double lo = NAN, hi = NAN;
        auto smaller = preview(downsampled, lo, hi, false, 2048);
        check(smaller.width() == 2048 && smaller.height() <= 684, "blink preview resolution cap");
        ss::Frame geometry;
        geometry.descriptor = downsampled;
        geometry.stars.push_back({1500, 500, 1000, 3, 2, .1});
        auto markers = review.displayStars(geometry, smaller);
        check(std::abs(markers[0].x - smaller.width() / 2.0) < .01 &&
                  std::abs(markers[0].y - smaller.height() / 2.0) < .01,
              "scaled blink overlays align");
        const auto cachedCount = review.previewCache.size();
        review.blinkButton->setChecked(true);
        QTest::qWait(50);
        check(review.previewCache.size() == cachedCount && !review.previewThread,
              "restart keeps warm blink cache");
        review.blinkButton->setChecked(false);
        elapsed.restart();
        while (review.previewThread && elapsed.elapsed() < 10000)
            QTest::qWait(20);
        review.previewCache.clear();
        renders = review.previewRenders;
        review.blinkButton->setChecked(true);
        elapsed.restart();
        while ((review.previewCache.size() < 2 || review.previewThread) && elapsed.elapsed() < 10000)
            QTest::qWait(20);
        check(review.previewRenders == renders && review.previewCache.size() >= 2,
              "evicted RAM previews are restored from disk without recomputing");
        review.blinkButton->setChecked(false);
        review.close();

        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
