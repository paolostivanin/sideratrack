// Exercise the real internal widgets and supervised worker with synthetic projects.
#include "gui/window.hpp"
using namespace ss::gui;
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QtTest/QTest>
#include <iostream>

void check(bool ok, const char *message) {
    if (!ok)
        throw ss::Error(message);
}
void editNight(Window &window, const QString &value) {
    check(!window.model.busy && !window.selectedRows().empty(), "night correction has an editable selection");
    bool edited = false;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        auto *night = dialog ? dialog->findChild<QLineEdit *>("metadataNight") : nullptr;
        if (night) {
            night->setText(value);
            edited = true;
            dialog->accept();
        } else if (dialog)
            dialog->reject();
    });
    window.editSelected();
    check(edited, "night correction uses the metadata dialog");
}
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setOrganizationName("Stellastack-tests");
    app.setApplicationName("Stellastack-gui-test");
    QTemporaryDir temp;
    qputenv("XDG_CACHE_HOME", (temp.path() + "/cache").toUtf8());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temp.path() + "/config");
    auto capture = [&](Window &window, const QString &name) {
        auto directory = qEnvironmentVariable("STELLASTACK_UI_SCREENSHOTS");
        if (directory.isEmpty())
            return;
        QDir().mkpath(directory);
        QTest::qWait(100);
        check(window.grab().save(directory + "/" + name + ".png"), "UI screenshot");
    };
    try {
        if (app.arguments().contains("--appearance-test")) {
            Window appearance;
            appearance.show();
            auto dark = app.palette();
            dark.setColor(QPalette::Window, QColor("#252a32"));
            dark.setColor(QPalette::WindowText, Qt::white);
            dark.setColor(QPalette::Base, QColor("#1d2229"));
            dark.setColor(QPalette::Text, Qt::white);
            app.setPalette(dark);
            QTest::qWait(20);
            check(appearance.pageTitle.palette().color(QPalette::WindowText) == Qt::white,
                  "palette changes update styled descendants");
            capture(appearance, "welcome-dark");
            return 0;
        }
        {
            Window welcome;
            welcome.show();
            check(welcome.welcomeOrProject->currentIndex() == 0, "first launch shows welcome");
            check(!welcome.analyzeAction->isEnabled(), "no processing without a project");
            capture(welcome, "welcome");
        }
        if (auto existing = qEnvironmentVariable("STELLASTACK_REVIEW_PROJECT"); !existing.isEmpty()) {
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
            auto screenshot = qEnvironmentVariable("STELLASTACK_REVIEW_SCREENSHOT");
            if (!screenshot.isEmpty())
                check(actual.grab().save(screenshot), "real review screenshot");
            actual.blinkButton->setChecked(false);
            wait.restart();
            while (actual.previewThread && wait.elapsed() < 30000)
                QTest::qWait(20);
            check(actual.view.pixels->pixmap().width() == 6252, "real stop restores full resolution");
            actual.calibration();
            QTest::qWait(100);
            if (!screenshot.isEmpty())
                check(actual.grab().save(screenshot + ".calibration.png"), "calibration page screenshot");
            std::cout << "Real dataset GUI playback and assignment review passed\n";
            return 0;
        }
        const auto root = ss::fs::path(temp.path().toStdString());
        {
            const auto nightPath = root / "nights.stella";
            ss::Project project(nightPath, true);
            auto add = [&](const char *file, const char *kind, const char *band, const char *night,
                           const QJsonValue &time = QJsonValue(), bool master = false, bool excluded = false,
                           bool otherFolder = false) {
                ss::Frame frame;
                frame.path = root / (otherFolder ? "second" : "first") / "shared" / file;
                frame.kind = kind;
                frame.filter = band;
                frame.session = night;
                frame.master = master;
                frame.selection = excluded ? -1 : 0;
                frame.descriptor.width = frame.descriptor.height = 32;
                frame.descriptor.header["EXPTIME"] = 60;
                if (!time.isNull())
                    frame.descriptor.header["DATE-OBS"] = time;
                project.save(frame);
                return frame.id;
            };
            const auto light = add("light.fits", "light", "L", "2026-09-20", "2026-09-20T22:00:00.125+02:00");
            add("excluded.fits", "light", "Ha", "2026-09-20", "2026-09-21T00:30:00.250", false, true);
            add("bias.fits", "bias", "", "2026-09-20", QJsonValue(), true, false, true);
            add("flat.fits", "flat", "L", "2026-09-20", "not-a-date");
            add("dark.fits", "dark", "L", "2026-09-20", "2026-09-20");
            add("dark-flat.fits", "darkflat", "L", "2026-09-20", 123);
            const auto nextLight = add("next-night.fits", "light", "L", "2026-09-21", "2026-09-21T23:00:00Z");
            add("unassigned.fits", "unknown", "", "", "2026-09-22T01:00:00");
            Window nights;
            nights.open(QString::fromStdString(nightPath.string()));
            nights.navigate(Window::Import);
            nights.show();
            auto rowFor = [&](const QString &night) {
                for (int row = 0; row < nights.importNights.rowCount(); ++row)
                    if (nights.importNights.item(row, 0)->data(Qt::UserRole).toString() == night)
                        return row;
                return -1;
            };
            check(nights.importNights.rowCount() == 3 && nights.importProxy.rowCount() == 8,
                  "night overview includes all assignments");
            auto row = rowFor("2026-09-20");
            check(row >= 0 && nights.importNights.item(row, 1)->text() == "6",
                  "night counts include excluded frames and masters");
            const auto types = nights.importNights.item(row, 2)->text();
            check(types.contains("2 light") && types.contains("1 bias (1 master)") &&
                      types.contains("1 flat") && types.contains("1 dark") && types.contains("1 dark-flat"),
                  "night overview distinguishes frame types and masters");
            check(nights.importNights.item(row, 3)->text() == "Ha, L, Unassigned",
                  "night overview lists filters");
            check(nights.importNights.item(row, 4)->text() == "2026-09-20 20:00:00 – 2026-09-21 00:30:00",
                  "capture range honors offsets and unzoned UTC times across midnight");
            const auto timeDetails = nights.importNights.item(row, 4)->toolTip();
            check(timeDetails.contains("2 valid capture times · 1 missing · 3 invalid") &&
                      timeDetails.contains("20:00:00.125Z") && timeDetails.contains("00:30:00.250Z"),
                  "capture range reports invalid or missing times and retains precision");
            check(nights.importNights.item(row, 5)->text() == "shared (2 folders)" &&
                      nights.importNights.item(row, 5)->toolTip().contains("/first/shared") &&
                      nights.importNights.item(row, 5)->toolTip().contains("/second/shared"),
                  "same-name source folders retain distinct full paths");
            const auto dated = *nights.project->frame(nextLight);
            auto undated = dated;
            undated.descriptor.header.remove("DATE-OBS");
            nights.project->save(undated);
            nights.reload();
            check(nights.importNights.item(rowFor("2026-09-21"), 4)->text() == "No valid capture times" &&
                      nights.importNights.item(rowFor("2026-09-21"), 4)->toolTip().contains("1 missing"),
                  "an undated night reports missing times without inventing a range");
            undated = dated;
            nights.project->save(undated);
            nights.reload();
            nights.filter.setCurrentIndex(nights.filter.findData("L"));
            nights.nightFilter.setCurrentIndex(nights.nightFilter.findData("2026-09-21"));
            const auto reviewCount = nights.proxy.rowCount();
            nights.importTable.sortByColumn(2, Qt::DescendingOrder);
            nights.importNights.setCurrentCell(row, 0);
            check(nights.importProxy.rowCount() == 6 && nights.proxy.rowCount() == reviewCount &&
                      nights.proxy.night == "2026-09-21",
                  "night inspection filters Import independently of Review");
            nights.selectShown.click();
            check(nights.selectedRows().size() == 6, "Select shown selects every visible frame");
            nights.importNights.setCurrentCell(row, 5);
            check(nights.selectedRows().size() == 6,
                  "inspecting another column in the same night preserves frame selection");
            nights.importNights.setCurrentCell(rowFor(""), 0);
            check(nights.importProxy.exactNight && nights.importProxy.night.isEmpty() &&
                      nights.importProxy.rowCount() == 1 && nights.selectedRows().empty(),
                  "Unassigned filters exactly and switching nights clears selection");
            nights.selectShown.click();
            check(nights.selectedRows().size() == 1, "Unassigned frames can be selected for correction");
            nights.allImportNights.click();
            check(!nights.importProxy.exactNight && nights.importProxy.rowCount() == 8 &&
                      nights.selectedRows().empty(),
                  "All nights restores the list and clears selection");
            nights.importNights.setCurrentCell(rowFor("2026-09-20"), 0);
            nights.setBusy(true);
            nights.selectShown.click();
            check(nights.selectedRows().size() == 6 &&
                      !nights.model.setData(nights.model.index(0, 5), "changed", Qt::EditRole),
                  "night inspection remains available while metadata edits are disabled");
            nights.allImportNights.click();
            nights.setBusy(false);
            nights.importNights.setCurrentCell(rowFor("2026-09-20"), 0);
            nights.reload();
            check(nights.importProxy.exactNight && nights.importProxy.rowCount() == 6,
                  "project reload retains the selected night");
            const auto index =
                nights.importProxy.mapFromSource(nights.model.index(nights.model.rowsById.value(light), 0));
            nights.importTable.selectionModel()->select(index, QItemSelectionModel::ClearAndSelect |
                                                                   QItemSelectionModel::Rows);
            const auto settings = nights.project->settings().toJson();
            editNight(nights, "2026-09-21");
            check(nights.project->frame(light)->session == "2026-09-21" &&
                      nights.importProxy.night == "2026-09-20" && nights.importProxy.rowCount() == 5 &&
                      nights.selectedRows().empty(),
                  "single correction preserves a remaining night and removes its moved selection");
            nights.selectShown.click();
            std::vector<int64_t> corrected;
            for (auto index : nights.selectedRows())
                corrected.push_back(nights.model.frames[index].id);
            editNight(nights, "2026-09-19");
            for (auto id : corrected)
                check(nights.project->frame(id)->session == "2026-09-19",
                      "bulk night correction persists every selected identity");
            check(!nights.importProxy.exactNight && nights.importProxy.rowCount() == 8 &&
                      nights.selectedRows().empty() && rowFor("2026-09-20") < 0 &&
                      nights.importNights.item(rowFor("2026-09-19"), 1)->text() == "5" &&
                      nights.project->frame(light)->session == "2026-09-21" &&
                      nights.project->settings().toJson() == settings,
                  "bulk correction updates groups and safely resets a disappearing night");
            capture(nights, "import-nights");
            nights.hide();
            nights.resize(900, 600);
            nights.show();
            QTest::qWait(100);
            check(nights.size() == QSize(900, 600) && nights.importNights.height() >= 62 &&
                      nights.importTable.height() >= nights.importTable.minimumSizeHint().height(),
                  "night overview and frames fit the minimum window size");
            capture(nights, "import-nights-small");
            nights.importNights.setCurrentCell(rowFor("2026-09-19"), 0);
            nights.open(QString::fromStdString(nightPath.string()));
            check(!nights.importProxy.exactNight && nights.importProxy.rowCount() == 8,
                  "opening a project resets the night filter");
            const auto emptyPath = root / "empty.stella";
            ss::Project empty(emptyPath, true);
            nights.importNights.setCurrentCell(rowFor("2026-09-19"), 0);
            nights.open(QString::fromStdString(emptyPath.string()));
            check(nights.importNights.rowCount() == 0 && nights.importProxy.rowCount() == 0 &&
                      !nights.selectShown.isEnabled() && !nights.allImportNights.isEnabled(),
                  "opening an empty project clears the previous overview and filter");
        }
        const auto projectPath = root / "scale.stella";
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
        window.navigate(Window::Review);
        QTest::qWait(100);
        check(window.model.rowCount() == 10000, "10,000 frame model");
        check(window.importNights.rowCount() == 4 && window.importNights.item(0, 1)->text() == "2500",
              "10,000 frame night overview");
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
        window.filter.setCurrentIndex(window.filter.findData("L"));
        check(window.proxy.rowCount() == 5000, "filter matches the filter field exactly");
        window.nightFilter.setCurrentIndex(window.nightFilter.findData("night-1"));
        check(window.proxy.rowCount() == 2500, "night and filter predicates combine");
        window.metric.setCurrentText("HFR");
        window.plot.repaint();
        check(window.plot.points.size() == 2500, "plot contains only visible exposures");
        window.filter.setCurrentIndex(0);
        window.nightFilter.setCurrentIndex(0);
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
        int overviewFrames = 0;
        for (int row = 0; row < window.importNights.rowCount(); ++row)
            overviewFrames += window.importNights.item(row, 1)->text().toInt();
        check(overviewFrames == 10001, "night overview refreshes after supervised imports");
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
        const auto reviewPath = root / "review.stella";
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
        review.resize(1450, 900);
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
        review.navigate(Window::Review);
        review.table.clearSelection();
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
        elapsed.restart();
        while (review.previewThread && elapsed.elapsed() < 10000)
            QTest::qWait(20);
        capture(review, "review");
        review.navigate(Window::Calibration);
        capture(review, "calibration");
        review.prepare();
        finish();
        check(!review.preparationSequence && review.pages->currentIndex() == Window::Review,
              "Prepare frames chains calibration and analysis then opens Review");
        check(review.readiness.canStack, "current prepared exposures are ready for stacking");
        review.navigate(Window::Import);
        review.importNights.setCurrentCell(0, 0);
        review.selectShown.click();
        std::vector<ss::Frame> beforeNightEdit;
        for (auto row : review.selectedRows())
            beforeNightEdit.push_back(review.model.frames[row]);
        editNight(review, "corrected-night");
        check(!review.readiness.prepared && !review.readiness.canStack,
              "night corrections refresh calibration readiness and invalidate old preparation");
        review.project->transaction([&] {
            for (auto frame : beforeNightEdit)
                review.project->save(frame);
        });
        review.reload();
        check(review.readiness.canStack, "restoring original nights restores current preparation");

        // Assignment edits act on the page's group, independently of the review selection.
        review.navigate(Window::Calibration);
        review.assignments.selectRow(0);
        review.updateCalibrationGroup();
        review.calibrationChoices[0].setCurrentIndex(review.calibrationChoices[0].findData("none"));
        review.applyCalibrationGroup();
        for (const auto &frame : review.model.frames)
            if (frame.kind == "light")
                check(ss::str(frame.descriptor.header, "SS_BIAS") == "none",
                      "override targets every group member");
        check(!review.readiness.canPrepare && !review.readiness.canStack,
              "missing calibration blocks preparation and stacking");
        review.calibrationChoices[0].setCurrentIndex(0);
        review.applyCalibrationGroup();
        check(review.readiness.canPrepare, "automatic assignments can be restored");
        review.prepare();
        finish();

        // An included but unusable exposure blocks stacking even with manual inclusion.
        auto original = *std::find_if(review.model.frames.begin(), review.model.frames.end(),
                                      [](const auto &frame) { return frame.kind == "light"; });
        auto unusable = original;
        unusable.transform.valid = false;
        unusable.selection = 1;
        review.project->save(unusable);
        review.model.updateFrame(unusable.id);
        review.updateReadiness();
        check(!review.readiness.canStack && review.readiness.frameProblems.contains(unusable.id),
              "manual inclusion never bypasses alignment failures");
        {
            Window reopened;
            reopened.open(QString::fromStdString(reviewPath.string()));
            check(reopened.pages->currentIndex() == Window::Review,
                  "reopening analyzed frames with alignment failures returns to review");
        }
        unusable.selection = -1;
        review.project->save(unusable);
        review.model.updateFrame(unusable.id);
        review.updateReadiness();
        check(review.readiness.canStack, "excluding an unusable frame resolves its blocker");
        review.project->save(original);
        review.model.updateFrame(original.id);
        review.updateReadiness();

        review.navigate(Window::Stack);
        review.stackDirectory.setText(QString::fromStdString((root / "gui-stack").string()));
        capture(review, "stack");
        review.runPrimary();
        finish();
        check(review.pages->currentIndex() == Window::Results && review.products.size() == 1 &&
                  review.readiness.resultsCurrent,
              "completed stack opens current results");
        elapsed.restart();
        while (review.resultThread && elapsed.elapsed() < 10000)
            QTest::qWait(20);
        check(review.resultView.pixels, "completed master has an asynchronous preview");
        capture(review, "results");
        auto completed = review.project->record("results");
        auto checkpoint = review.project->record("checkpoint");
        auto masterResult = completed["masters"].toArray()[0].toObject();
        review.project->record("results", {});
        review.project->record("checkpoint",
                               {{"stage", "stack"}, {"key", masterResult["key"]}, {"filter", "L"}});
        review.open(QString::fromStdString(reviewPath.string()));
        check(review.readiness.resumable && review.pages->currentIndex() == Window::Stack &&
                  review.primary.text() == "Resume stack",
              "reopening an interrupted stack offers compatible checkpoints");
        review.project->record("results", completed);
        review.project->record("checkpoint", checkpoint);
        review.updateReadiness();
        review.refreshResults();
        review.navigate(Window::Results);
        review.exportDirectory.setText(QString::fromStdString((root / "gui-export").string()));
        review.exportFormat.setCurrentIndex(3);
        review.exportResults();
        finish();
        check(ss::fs::exists(root / "gui-export" / "L-master.xisf"),
              "GUI exports completed masters in the chosen format");
        auto excluded = original;
        excluded.selection = -1;
        review.project->save(excluded);
        review.model.updateFrame(excluded.id);
        review.updateReadiness();
        check(!review.readiness.resultsCurrent, "selection changes mark saved outputs as a previous stack");
        review.project->save(original);
        review.model.updateFrame(original.id);
        review.updateReadiness();

        // Control a worker deterministically to exercise failure and cancellation boundaries.
        const auto fakePath = temp.path() + "/controlled-worker";
        const auto modePath = temp.path() + "/worker-mode";
        const auto callsPath = temp.path() + "/worker-calls";
        QFile controlled(fakePath);
        check(controlled.open(QIODevice::WriteOnly), "create controlled worker");
        QByteArray program = "#!/usr/bin/python3\nimport sys, signal, time, json\n";
        program += "mode = open(" +
                   QJsonDocument(QJsonArray{modePath}).toJson(QJsonDocument::Compact).mid(1).chopped(1) +
                   ").read()\n";
        program += "with open(" +
                   QJsonDocument(QJsonArray{callsPath}).toJson(QJsonDocument::Compact).mid(1).chopped(1) +
                   ", 'a') as log: log.write(sys.argv[1] + '\\n')\n";
        program +=
            "signal.signal(signal.SIGINT, lambda *_: sys.exit(0))\n"
            "if mode == 'failure':\n print(json.dumps({'event': 'error', 'message': 'Calibration failed'}), "
            "flush=True)\n sys.exit(1)\n"
            "if mode == 'cancel':\n print(json.dumps({'event': 'progress', 'stage': 'calibrate', 'done': 0, "
            "'total': 1, 'message': 'Waiting for cancellation'}), flush=True)\n time.sleep(30)\n"
            "time.sleep(.1)\nprint(json.dumps({'event': 'complete'}), flush=True)\n";
        check(controlled.write(program) == program.size(), "write controlled worker");
        controlled.close();
        check(controlled.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner),
              "controlled worker executable");
        auto configureWorker = [&](const char *mode) {
            QFile config(modePath);
            check(config.open(QIODevice::WriteOnly | QIODevice::Truncate), "worker scenario");
            config.write(mode);
            config.close();
            QFile calls(callsPath);
            check(calls.open(QIODevice::WriteOnly | QIODevice::Truncate), "clear worker history");
            review.workerBinary = fakePath;
            review.navigate(Window::Calibration);
        };
        auto calls = [&] {
            QFile file(callsPath);
            check(file.open(QIODevice::ReadOnly), "worker command history");
            return file.readAll();
        };
        configureWorker("failure");
        review.prepare();
        elapsed.restart();
        while (review.model.busy && elapsed.elapsed() < 10000)
            QTest::qWait(20);
        check(!review.model.busy && !review.preparationSequence && calls() == "calibrate\n",
              "calibration failure never launches analysis");
        check(!review.recoverJob.isHidden(), "failed preparation offers recovery");
        configureWorker("cancel");
        review.prepare();
        elapsed.restart();
        while (!review.jobMessage.contains("Waiting for cancellation") && elapsed.elapsed() < 10000)
            QTest::qWait(20);
        check(review.jobMessage.contains("Waiting for cancellation"), "controlled worker ready to cancel");
        review.cancelProcessing();
        elapsed.restart();
        while (review.model.busy && elapsed.elapsed() < 10000)
            QTest::qWait(20);
        check(!review.model.busy && !review.preparationSequence && calls() == "calibrate\n",
              "cancellation prevents analysis even when the worker exits successfully");
        configureWorker("success");
        review.workerBinary = temp.path() + "/missing-worker";
        review.prepare();
        elapsed.restart();
        while (review.model.busy && elapsed.elapsed() < 10000)
            QTest::qWait(20);
        check(!review.model.busy && !review.preparationSequence && calls().isEmpty(),
              "failed startup clears the preparation sequence");
        configureWorker("success");
        review.prepare();
        review.navigate(Window::Stack);
        finish();
        check(calls() == "calibrate\nanalyze\n" && review.pages->currentIndex() == Window::Stack &&
                  !review.viewCompletion.isHidden(),
              "completion respects navigation during preparation");
        review.workerBinary.clear();

        // Keep grading previews transactional, and exercise all contextual dialogs.
        for (auto section : {Window::Resources, Window::Registration, Window::Grading, Window::Integration}) {
            auto before = review.project->settings().toJson();
            QTimer::singleShot(20, [&] {
                for (auto *widget : QApplication::topLevelWidgets())
                    if (auto *dialog = qobject_cast<QDialog *>(widget))
                        dialog->reject();
            });
            review.settings(section);
            check(review.project->settings().toJson() == before,
                  "cancelled settings preserve processing configuration");
        }
        auto preciseBudgets = review.project->settings();
        preciseBudgets.memory += 123;
        preciseBudgets.scratch += 321;
        review.project->settings(preciseBudgets);
        auto beforeGrading = review.project->settings().toJson();
        QTimer::singleShot(20, [&] {
            for (auto *widget : QApplication::topLevelWidgets())
                if (auto *dialog = qobject_cast<QDialog *>(widget))
                    dialog->accept();
        });
        review.settings(Window::Grading);
        check(review.project->settings().toJson() == beforeGrading,
              "saving grading leaves hidden resource and registration settings unchanged");
        // Wayland compositors can retain the configured size of a mapped toplevel.
        // Remap it with the requested small size before testing each page's layout.
        review.hide();
        review.resize(900, 600);
        review.show();
        QTest::qWait(200);
        for (int stage = Window::Import; stage <= Window::Results; ++stage) {
            review.navigate(Window::Stage(stage));
            QTest::qWait(20);
            check(review.size() == QSize(900, 600),
                  qPrintable(QString("stage %1 fits the minimum window size (actual %2 × %3)")
                                 .arg(stage)
                                 .arg(review.width())
                                 .arg(review.height())));
            if (stage == Window::Review)
                check(review.plot.mapToGlobal(QPoint()).y() >=
                          review.view.mapToGlobal(QPoint(0, review.view.height())).y(),
                      "small-window quality controls do not overlap the preview");
            capture(review, QString("small-%1").arg(stage));
        }
        review.resize(1450, 900);
        auto systemPalette = app.palette();
        QPalette dark;
        dark.setColor(QPalette::Window, QColor("#252a32"));
        dark.setColor(QPalette::WindowText, QColor("#edf0f5"));
        dark.setColor(QPalette::Base, QColor("#1d2229"));
        dark.setColor(QPalette::AlternateBase, QColor("#303640"));
        dark.setColor(QPalette::Text, QColor("#edf0f5"));
        dark.setColor(QPalette::Button, QColor("#303640"));
        dark.setColor(QPalette::ButtonText, QColor("#edf0f5"));
        dark.setColor(QPalette::Highlight, QColor("#376b9e"));
        dark.setColor(QPalette::HighlightedText, Qt::white);
        app.setPalette(dark);
        QTest::qWait(20);
        check(review.pageTitle.palette().color(QPalette::WindowText) == dark.color(QPalette::WindowText),
              "system palette changes reach the workspace text");
        review.navigate(Window::Review);
        capture(review, "review-dark");
        app.setPalette(systemPalette);
        review.navigate(Window::Review);
        review.raise();
        review.activateWindow();
        QTest::qWait(50);
        QTest::keyClick(&review, Qt::Key_4, Qt::AltModifier);
        QTest::qWait(20);
        check(review.pages->currentIndex() == Window::Stack, "keyboard shortcut changes stages");
        review.close();
        {
            Window reopened;
            reopened.open(QString::fromStdString(reviewPath.string()));
            check(reopened.pages->currentIndex() == Window::Results && reopened.readiness.resultsCurrent,
                  "reopening a completed project restores results without a schema migration");
        }

        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
