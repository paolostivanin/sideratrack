#include "window.hpp"
#include <signal.h>
#include <sys/types.h>

namespace ss::gui {
Window::Window() {
    buildUi();
    model.changed = [this] {
        blinkButton->setChecked(false);
        previewCache.clear();
        black = white = NAN;
        previewCalibration = q(ss::calibrationRevision(model.frames, project->settings()));
        updateFilters();
        showCurrent();
        plot.update();
        summary();
        updateReadiness();
    };
    plot.select = [this](int64_t id) {
        for (int i = 0; i < model.rowCount(); ++i)
            if (model.frames[size_t(i)].id == id) {
                auto index = proxy.mapFromSource(model.index(i, 0));
                table.selectRow(index.row());
                table.scrollTo(index);
                break;
            }
    };
    connect(&metric, &QComboBox::currentTextChanged, this, [this](const QString &s) {
        plot.metric = s;
        plot.update();
    });
    connect(table.selectionModel(), &QItemSelectionModel::currentRowChanged, this, [this] { showCurrent(); });
    connect(table.selectionModel(), &QItemSelectionModel::selectionChanged, this, [this] {
        blinkButton->setChecked(false);
        blinkButton->setEnabled(project && table.selectionModel()->selectedRows().size() >= 2);
        summary();
    });
    connect(table.horizontalHeader(), &QHeaderView::sortIndicatorChanged, this,
            [this] { blinkButton->setChecked(false); });
    connect(blinkButton, &QPushButton::toggled, this, [this](bool on) {
        blink.stop();
        ++previewGeneration;
        pendingPreview = 0;
        if (previewThread)
            previewThread->requestInterruption();
        blinkButton->setText(on ? "Stop blink" : "Start blink");
        blinkIds.clear();
        if (on) {
            failedPreviewKeys.clear();
            auto rows = table.selectionModel()->selectedRows();
            std::sort(rows.begin(), rows.end(),
                      [](const auto &a, const auto &b) { return a.row() < b.row(); });
            for (auto row : rows)
                blinkIds.push_back(model.frames[size_t(proxy.mapToSource(row).row())].id);
            if (blinkIds.size() < 2) {
                blinkButton->setChecked(false);
                return;
            }
            auto current = proxy.mapToSource(table.currentIndex());
            auto id = current.isValid() ? model.frames[size_t(current.row())].id : blinkIds.front();
            auto it = std::find(blinkIds.begin(), blinkIds.end(), id);
            blinkCursor = it == blinkIds.end() ? 0 : size_t(it - blinkIds.begin());
        }
        showCurrent();
    });
    connect(&blinkInterval, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double seconds) {
        if (blink.isActive())
            blink.start(qRound(seconds * 1000));
    });
    connect(&blink, &QTimer::timeout, this, [this] {
        blink.stop();
        if (!blinkButton->isChecked() || blinkIds.size() < 2)
            return;
        blinkCursor = (blinkCursor + 1) % blinkIds.size();
        auto it = model.rowsById.find(blinkIds[blinkCursor]);
        if (it == model.rowsById.end()) {
            blinkButton->setChecked(false);
            return;
        }
        auto next = proxy.mapFromSource(model.index(*it, 0));
        if (next == table.currentIndex())
            showCurrent();
        else
            table.selectionModel()->setCurrentIndex(next, QItemSelectionModel::NoUpdate);
    });
    updateTimer.setSingleShot(true);
    updateTimer.setInterval(100);
    connect(&updateTimer, &QTimer::timeout, this, [this] { updateFrames(); });
    connect(&worker, &QProcess::readyReadStandardOutput, this, [this] { readWorkerOutput(); });
    connect(&worker, &QProcess::readyReadStandardError, this,
            [this] { log.appendPlainText(QString::fromUtf8(worker.readAllStandardError())); });
    connect(&worker, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus exit) { finishJob(code, exit); });
    connect(&worker, &QProcess::started, this, [this] {
        if (cancellationRequested)
            ::kill(pid_t(worker.processId()), SIGINT);
    });
    connect(&worker, &QProcess::errorOccurred, this, [this](QProcess::ProcessError errorCode) {
        if (errorCode != QProcess::FailedToStart)
            return;
        preparationSequence = false;
        workerHadError = true;
        setBusy(false);
        jobTitle.setText("Could not start processing");
        jobDetail.setText(worker.errorString());
        log.appendPlainText(worker.errorString());
        recoverJob.show();
    });
}
Window::~Window() {
    disconnect(&worker, nullptr, this, nullptr);
    preparationSequence = false;
    updateTimer.stop();
    elapsedTimer.stop();
    if (worker.state() != QProcess::NotRunning) {
        ::kill(pid_t(worker.processId()), SIGINT);
        worker.waitForFinished(3000);
        if (worker.state() != QProcess::NotRunning) {
            worker.kill();
            worker.waitForFinished();
        }
    }
    if (resultThread) {
        resultThread->requestInterruption();
        resultThread->wait();
        delete resultThread;
        resultThread = nullptr;
    }
    if (previewThread) {
        previewThread->requestInterruption();
        previewThread->wait();
        delete previewThread;
        previewThread = nullptr;
    }
}
void Window::error(const QString &message) {
    QMessageBox::critical(this, "Stellastack", message);
}
void Window::setBusy(bool busy) {
    model.busy = busy;
    for (auto *a : jobActions)
        a->setEnabled(project && !busy);
    for (auto *widget : editableWidgets)
        widget->setEnabled(project && !busy);
    newAction->setEnabled(!busy);
    openAction->setEnabled(!busy);
    cancelJob.setVisible(busy);
    cancelJob.setEnabled(busy && !cancellationRequested);
    recoverJob.setVisible(false);
    if (busy)
        elapsedTimer.start();
    else
        elapsedTimer.stop();
    updateReadiness();
}

void Window::open(const QString &path) {
    if (model.busy || worker.state() != QProcess::NotRunning)
        return;
    try {
        auto opened = std::make_unique<ss::Project>(path.toStdString());
        blinkButton->setChecked(false);
        table.clearSelection();
        importTable.clearSelection();
        setImportNight();
        changedFrameIds.clear();
        updateTimer.stop();
        previewCache.clear();
        displayedFrame = 0;
        ++previewGeneration;
        ++resultGeneration;
        if (previewThread)
            previewThread->requestInterruption();
        if (resultThread)
            resultThread->requestInterruption();
        pendingPreview = 0;
        project = std::move(opened);
        model.project = project.get();
        setWindowTitle("Stellastack · " + QFileInfo(path).completeBaseName());
        projectName.setText(QFileInfo(path).completeBaseName());
        projectName.setToolTip(QFileInfo(path).absoluteFilePath());
        lastOutput = q(ss::str(project->record("output"), "directory"));
        black = white = NAN;
        jobPanel->hide();
        preparationSequence = cancellationRequested = false;
        welcomeOrProject->setCurrentWidget(projectWorkspace);
        preparedDirectory.setText(q(
            ss::str(project->record("calibration"), "directory", project->path().string() + ".calibrated")));
        {
            QSignalBlocker blocker(stackDirectory);
            stackDirectory.setText(lastOutput.isEmpty() ? QFileInfo(path).absolutePath() + "/" +
                                                              QFileInfo(path).completeBaseName() + "-masters"
                                                        : lastOutput);
        }
        {
            QSignalBlocker blocker(stackFormat);
            stackFormat.setCurrentIndex(int(project->settings().format));
        }
        products = {};
        resultList.clear();
        resultView.scene.clear();
        resultView.pixels = nullptr;
        resultView.markers.clear();
        reload();
        setBusy(false);
        refreshResults();
        updateRecentProjects(path);
        if (model.frames.empty() || readiness.unknown)
            navigate(Import);
        else if (!readiness.lights && !products.empty())
            navigate(Results);
        else if (readiness.resultsCurrent)
            navigate(Results);
        else if (readiness.resumable)
            navigate(Stack);
        else if (readiness.analyzed)
            navigate(Review);
        else
            navigate(Calibration);
    } catch (const std::exception &e) {
        error(e.what());
    }
}

void Window::reload() {
    try {
        QList<qlonglong> reviewIds, importIds;
        const auto importNight = importProxy.night;
        const bool exactImportNight = importProxy.exactNight;
        for (auto index : table.selectionModel()->selectedRows())
            reviewIds << model.frames[size_t(proxy.mapToSource(index).row())].id;
        for (auto index : importTable.selectionModel()->selectedRows())
            importIds << model.frames[size_t(importProxy.mapToSource(index).row())].id;
        model.reload();
        previewCache.clear();
        failedPreviewKeys.clear();
        black = white = NAN;
        previewCalibration = q(ss::calibrationRevision(model.frames, project->settings()));
        previewCache.setMaxCost(
            int(std::min<uint64_t>(256 * ss::MiB, project->settings().memory / 16) / 1024));
        updateFilters();
        updateReadiness();
        auto restore = [&](QTableView &view, FrameProxy &mapping, const QList<qlonglong> &ids) {
            for (auto id : ids) {
                auto row = model.rowsById.value(id, -1);
                if (row < 0)
                    continue;
                auto index = mapping.mapFromSource(model.index(row, 0));
                if (!index.isValid())
                    continue;
                view.selectionModel()->select(index, QItemSelectionModel::Select | QItemSelectionModel::Rows);
                view.selectionModel()->setCurrentIndex(index, QItemSelectionModel::NoUpdate);
            }
        };
        restore(table, proxy, reviewIds);
        if (importNight == importProxy.night && exactImportNight == importProxy.exactNight)
            restore(importTable, importProxy, importIds);
        plot.update();
        summary();
        showDetails();
    } catch (const std::exception &e) {
        error(e.what());
    }
}

void Window::readWorkerOutput() {
    output += worker.readAllStandardOutput();
    for (;;) {
        auto pos = output.indexOf('\n');
        if (pos < 0)
            break;
        auto line = output.left(pos);
        output.remove(0, pos + 1);
        try {
            auto event = ss::parseJson(line);
            if (ss::str(event, "event") == "error") {
                workerHadError = true;
                jobMessage = event["message"].toString();
                log.appendPlainText(jobMessage);
                jobDetail.setText(jobMessage);
            } else if (ss::str(event, "event") == "frame-updated") {
                changedFrameIds.insert(event["id"].toInteger());
                if (!updateTimer.isActive())
                    updateTimer.start();
            } else if (ss::str(event, "event") == "progress") {
                double total = ss::number(event, "total", 0), done = ss::number(event, "done", 0);
                progress.setRange(0, total > 0 ? 1000 : 0);
                progress.setValue(total > 0 ? int(1000 * done / total) : 0);
                static const QHash<QString, QString> stages{
                    {"verify", "Checking files"},        {"import", "Adding frames"},
                    {"calibrate", "Calibrating frames"}, {"analyze", "Measuring stars"},
                    {"register", "Aligning frames"},     {"normalize", "Normalizing frames"},
                    {"stack", "Stacking frames"},        {"checkpoint", "Saving progress"},
                    {"export", "Writing masters"},       {"publication", "Saving outputs"}};
                auto stage = event["stage"].toString();
                jobMessage = stages.value(stage, stage) + " · " + event["message"].toString();
                if (total > 0)
                    jobMessage += QString(" · %1 / %2").arg(done).arg(total);
                jobDetail.setText(jobMessage);
            }
        } catch (...) {
            log.appendPlainText(QString::fromUtf8(line));
        }
    }
}

void Window::updateReadiness() {
    if (!project)
        return;
    auto settings = project->settings();
    previewCache.setMaxCost(int(std::min<uint64_t>(256 * ss::MiB, settings.memory / 16) / 1024));
    auto revision = q(ss::calibrationRevision(model.frames, settings));
    if (revision != previewCalibration) {
        previewCalibration = revision;
        previewCache.clear();
        failedPreviewKeys.clear();
        black = white = NAN;
        showCurrent();
    }
    readiness = ss::workflowReport(*project, model.frames, settings);
    model.decisions.clear();
    for (const auto &decision : ss::evaluateSelection(model.frames, settings))
        model.decisions.insert(decision.id, decision);
    model.problems.clear();
    for (const auto &[id, reason] : readiness.frameProblems)
        model.problems.insert(id, q(reason));
    if (model.rowCount())
        Q_EMIT model.dataChanged(model.index(0, 0), model.index(model.rowCount() - 1, columns.size() - 1));
    calibrateAction->setEnabled(!model.busy && readiness.canPrepare);
    analyzeAction->setEnabled(!model.busy && readiness.prepared);
    analyzeAction->setToolTip(readiness.prepared ? "Analyze calibrated lights"
                                                 : "Prepare current calibrated images first");
    refreshCalibration();
    stackGroups.setRowCount(readiness.groups.size());
    for (int row = 0; row < readiness.groups.size(); ++row) {
        const auto group = readiness.groups[row].toObject();
        QStringList cells{group["filter"].toString(), group["night"].toString(),
                          QString::number(group["accepted"].toInt()),
                          QString::number(group["excluded"].toInt()),
                          QString::number(group["seconds"].toDouble() / 60, 'f', 1) + " min"};
        QStringList calibrations;
        for (auto value : readiness.assignments) {
            auto assignment = value.toObject();
            if (assignment["filter"] != group["filter"] || assignment["session"] != group["night"])
                continue;
            for (const auto &kind : QStringList{"dark", "bias", "flat"}) {
                QStringList files;
                for (auto id : assignment[kind].toArray()) {
                    auto index = model.rowsById.value(id.toInteger(), -1);
                    if (index >= 0)
                        files << q(model.frames[size_t(index)].path.filename().string());
                }
                auto description = kind + ": " + (files.empty() ? "none" : files.join(", "));
                if (!calibrations.contains(description))
                    calibrations << description;
            }
        }
        cells << (calibrations.empty() ? "None" : calibrations.join(" · "));
        for (int col = 0; col < cells.size(); ++col)
            stackGroups.setItem(row, col, new QTableWidgetItem(cells[col]));
        stackGroups.item(row, 5)->setToolTip(calibrations.join("\n"));
    }
    QString reference = "Automatic reference after preparation";
    auto referenceRow = model.rowsById.value(settings.reference, -1);
    if (referenceRow >= 0) {
        const auto &frame = model.frames[size_t(referenceRow)];
        reference = QString("Reference: %1 · %2 × %3 pixels")
                        .arg(q(frame.path.filename().string()))
                        .arg(frame.descriptor.width)
                        .arg(frame.descriptor.height);
    }
    preflightSummary.setText(reference +
                             QString("\nMemory budget %1 GiB · Scratch budget %2 GiB · %3 CPU threads")
                                 .arg(double(settings.memory) / ss::GiB, 0, 'f', 1)
                                 .arg(double(settings.scratch) / ss::GiB, 0, 'f', 1)
                                 .arg(settings.threads));
    refreshPages();
    refreshImportNights();
    summary();
    showDetails();
}

void Window::updateFrames() {
    updateTimer.stop();
    auto ids = std::exchange(changedFrameIds, {});
    if (!project)
        return;
    try {
        for (auto id : ids) {
            auto previousRow = model.rowsById.value(id, -1);
            auto previousKey =
                previousRow >= 0 ? model.frames[size_t(previousRow)].calibrationKey : std::string{};
            model.updateFrame(id);
            const auto row = model.rowsById.value(id, -1);
            if (row >= 0) {
                const auto &frame = model.frames[size_t(row)];
                auto name = q(frame.filter);

                if (id == displayedFrame && frame.calibrationKey != previousKey) {
                    black = white = NAN;
                    showCurrent();
                } else if (id == displayedFrame && view.pixels) {
                    // Updating metrics/catalogs must not decode or stretch the image again.
                    auto image = view.pixels->pixmap().toImage();
                    view.display(image, displayStars(frame, image), true);
                }
            }
        }
        auto calibration = q(ss::calibrationRevision(model.frames, project->settings()));
        if (calibration != previewCalibration) {
            previewCalibration = calibration;
            previewCache.clear();
            black = white = NAN;
            showCurrent();
        }
        plot.update();
        updateFilters();
        updateReadiness();
    } catch (const std::exception &e) {
        log.appendPlainText(QString::fromUtf8(e.what()));
    }
}
std::vector<ss::Star> Window::displayStars(const ss::Frame &frame, const QImage &image) {
    auto stars = frame.stars;
    for (auto &star : stars) {
        star.x *= double(image.width()) / std::max(1, frame.descriptor.width);
        star.y *= double(image.height()) / std::max(1, frame.descriptor.height);
    }
    return stars;
}
void Window::presentPreview(const ss::Frame &frame, const CachedPreview &preview) {
    black = preview.black;
    white = preview.white;
    view.display(preview.image, displayStars(frame, preview.image),
                 blinkButton->isChecked() || displayedFrame == frame.id);
    displayedFrame = frame.id;
    previewStatus.setText(q(frame.path.filename().string()) +
                          (preview.calibrated ? " · calibrated" : " · uncalibrated") +
                          " · preview stretch only");
    if (blinkButton->isChecked())
        blink.start(qRound(blinkInterval.value() * 1000));
}
void Window::prefetchNext() {
    if (!project || previewThread || pendingPreview || !blinkButton->isChecked() || blinkIds.size() < 2)
        return;
    for (size_t offset = 1; offset <= std::min<size_t>(2, blinkIds.size() - 1); ++offset) {
        auto id = blinkIds[(blinkCursor + offset) % blinkIds.size()];
        auto row = model.rowsById.value(id, -1);
        if (row < 0)
            continue;
        const auto &frame = model.frames[size_t(row)];
        auto size = QSize(frame.descriptor.width, frame.descriptor.height);
        if (std::max(size.width(), size.height()) > 2048)
            size.scale(2048, 2048, Qt::KeepAspectRatio);
        auto cost = (int64_t(size.width()) * size.height() * 4 + 1023) / 1024;
        if (cost * int64_t(offset + 1) > previewCache.maxCost())
            continue;
        auto key = previewKey(frame, project->path(), black, white, commonStretch, previewCalibration);
        if (!previewCache.contains(key) && !failedPreviewKeys.contains(key)) {
            pendingPreview = id;
            pendingIsPrefetch = true;
            loadPreview();
            return;
        }
    }
}
void Window::summary() {
    int excluded = 0, ready = 0;
    QMap<QString, int> counts;
    QSet<QString> bands, nights;
    for (const auto &f : model.frames) {
        ++counts[q(f.kind)];
        excluded += !model.included(f);
        ready += f.kind == "light" && !f.master && f.transform.valid && !model.problems.contains(f.id);
        if (!f.filter.empty())
            bands.insert(q(f.filter));
        if (!f.session.empty())
            nights.insert(q(f.session));
    }
    status.setText(
        QString("%1 frames · %2 excluded · %3 aligned").arg(model.frames.size()).arg(excluded).arg(ready));
    QStringList breakdown;
    for (auto it = counts.begin(); it != counts.end(); ++it)
        breakdown << QString("%1 %2").arg(it.value()).arg(it.key());
    importSummary.setText(model.frames.empty()
                              ? "No frames yet. Add files or a folder to get started."
                              : breakdown.join(" · ") +
                                    QString("\n%1 filters · %2 nights").arg(bands.size()).arg(nights.size()));
    selectionSummary.setText(QString("%1 visible · %2 selected")
                                 .arg(proxy.rowCount())
                                 .arg(table.selectionModel()->selectedRows().size()));
}

void Window::start(const QString &command, QStringList extra) {
    if (!project || model.busy || worker.state() != QProcess::NotRunning)
        return;
    output.clear();
    jobCommand = lastCommand = command;
    lastArguments = extra;
    cancellationRequested = workerHadError = false;
    completionStage = -1;
    viewCompletion.hide();
    jobMessage = "Starting " + command + "…";
    static const QHash<QString, QString> titles{
        {"import", "Adding frames"},     {"masters", "Creating calibration masters"},
        {"stack", "Stacking frames"},    {"resume", "Resuming stack"},
        {"export", "Exporting masters"}, {"calibrate", "Calibrating frames"},
        {"analyze", "Analyzing frames"}};
    jobTitle.setText(preparationSequence ? "Preparing frames" : titles.value(command, command));
    jobDetail.setText(jobMessage);
    jobPanel->show();
    recoverJob.setText(command == "stack" || command == "resume"        ? "Resume stack"
                       : command == "calibrate" || command == "analyze" ? "Retry preparation"
                                                                        : "Retry");
    jobOrigin = pages->currentIndex();
    if (!preparationSequence)
        preparationElapsed = 0;
    jobElapsed.restart();
    progress.setRange(0, 0);
    setBusy(true);
    log.appendPlainText(command + " started");
    QStringList args{command, q(project->path().string()), "--json"};
    args.append(extra);
    worker.start(workerBinary.isEmpty() ? QCoreApplication::applicationDirPath() + "/stellastack-cli"
                                        : workerBinary,
                 args);
}

std::vector<size_t> Window::selectedRows() {
    std::vector<size_t> rows;
    auto &active = activeTable();
    auto &mapping = pages->currentIndex() == Import ? importProxy : proxy;
    for (auto index : active.selectionModel()->selectedRows())
        rows.push_back(size_t(mapping.mapToSource(index).row()));
    return rows;
}

void Window::select(int selection) {
    if (!project || model.busy || selectedRows().empty())
        return;
    try {
        project->transaction([&] {
            for (auto row : selectedRows()) {
                auto f = model.frames[row];
                f.selection = selection;
                project->save(f);
            }
        });
        QList<qlonglong> changed;
        for (auto row : selectedRows())
            changed << model.frames[row].id;
        for (auto id : changed)
            model.updateFrame(id);
        model.dataChanged(model.index(0, 0), model.index(model.rowCount() - 1, 16));
        plot.update();
        summary();
        updateReadiness();
    } catch (const std::exception &e) {
        error(e.what());
    }
}
void Window::editSelected() {
    if (!project || model.busy || selectedRows().empty())
        return;
    QDialog dialog(this);
    dialog.setWindowTitle("Edit selected frames");
    QFormLayout form(&dialog);
    QComboBox kind;
    kind.addItems({"Keep current type", "light", "dark", "flat", "bias", "darkflat", "unknown"});
    QLineEdit band, night, camera, exposure, temperature, gain, offset, xbin, ybin, sampleScale;
    night.setObjectName("metadataNight");
    QComboBox cfa, rowOrder;
    cfa.addItems({"Keep current", "Mono / RGB", "RGGB", "BGGR", "GRBG", "GBRG"});
    rowOrder.addItems({"Keep current", "TOP-DOWN", "BOTTOM-UP"});
    form.addRow("Frame type", &kind);
    form.addRow("Filter (blank keeps current)", &band);
    form.addRow("Night (blank keeps current)", &night);
    form.addRow("Camera (blank keeps current)", &camera);
    form.addRow("Exposure seconds (blank keeps current)", &exposure);
    form.addRow("Temperature °C (blank keeps current)", &temperature);
    form.addRow("Gain (blank keeps current)", &gain);
    form.addRow("Offset (blank keeps current)", &offset);
    form.addRow("Horizontal binning (blank keeps current)", &xbin);
    form.addRow("Vertical binning (blank keeps current)", &ybin);
    form.addRow("Bayer pattern", &cfa);
    form.addRow("Stored row orientation", &rowOrder);
    form.addRow("Scale to relative units (blank uses metadata)", &sampleScale);
    QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    form.addRow(&buttons);
    connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return;
    try {
        project->transaction([&] {
            for (auto row : selectedRows()) {
                auto f = model.frames[row];
                QJsonObject changes, header;
                if (kind.currentIndex())
                    changes["kind"] = kind.currentText();
                if (!band.text().isEmpty())
                    changes["filter"] = band.text();
                if (!night.text().isEmpty())
                    changes["session"] = night.text();
                if (!camera.text().isEmpty())
                    header["INSTRUME"] = camera.text();
                const std::array<std::pair<QLineEdit *, const char *>, 7> numbers{
                    {{&exposure, "EXPTIME"},
                     {&temperature, "CCD-TEMP"},
                     {&gain, "GAIN"},
                     {&offset, "OFFSET"},
                     {&xbin, "XBINNING"},
                     {&ybin, "YBINNING"},
                     {&sampleScale, "SS_SAMPLE_SCALE"}}};
                for (const auto &[field, key] : numbers)
                    if (!field->text().isEmpty()) {
                        bool ok = false;
                        auto n = field->text().toDouble(&ok);
                        if (!ok || !std::isfinite(n))
                            throw ss::Error("Enter valid numeric acquisition metadata");
                        header[key] = n;
                    }
                if (cfa.currentIndex()) {
                    header["BAYERPAT"] =
                        cfa.currentIndex() == 1 ? QJsonValue() : QJsonValue(cfa.currentText());
                    header["XBAYROFF"] = header["YBAYROFF"] = 0;
                }
                if (rowOrder.currentIndex())
                    header["ROWORDER"] = rowOrder.currentText();
                changes["header"] = header;
                ss::editFrame(f, changes);
                project->save(f);
            }
        });
        reload();
    } catch (const std::exception &e) {
        error(e.what());
    }
}
void Window::showCurrent() {
    if (!project)
        return;
    auto index = proxy.mapToSource(table.currentIndex());
    if (!index.isValid()) {
        ++previewGeneration;
        pendingPreview = 0;
        if (previewThread)
            previewThread->requestInterruption();
        view.scene.clear();
        view.markers.clear();
        view.pixels = nullptr;
        displayedFrame = 0;
        previewStatus.setText("Select a frame to inspect it. Scroll to zoom; drag to pan.");
        return;
    }
    ++previewGeneration;
    const auto &frame = model.frames[size_t(index.row())];
    if (blinkButton->isChecked()) {
        auto key = previewKey(frame, project->path(), black, white, commonStretch, previewCalibration);
        if (auto cached = previewCache.object(key)) {
            pendingPreview = 0;
            presentPreview(frame, *cached);
            prefetchNext();
            return;
        }
    }
    pendingPreview = frame.id;
    pendingIsPrefetch = false;
    if (!previewThread)
        loadPreview();
}
void Window::loadPreview() {
    if (!project || !pendingPreview)
        return;
    auto row = model.rowsById.value(pendingPreview, -1);
    if (row < 0) {
        pendingPreview = 0;
        return;
    }
    auto frame = model.frames[size_t(row)];
    const auto prefetch = pendingIsPrefetch;
    pendingPreview = 0;
    const bool reduced = blinkButton->isChecked();
    if (!prefetch)
        previewStatus.setText("Loading " + q(frame.path.filename().string()));
    double lo = black, hi = white;
    bool common = commonStretch;
    const auto generation = previewGeneration;
    const auto projectPath = project->path();
    const auto settings = project->settings();
    const auto calibration = previewCalibration;
    const auto key = previewKey(frame, projectPath, lo, hi, common, calibration);
    previewThread = QThread::create([this, frame, lo, hi, common, projectPath, generation, prefetch, reduced,
                                     settings, key, calibration]() mutable {
        try {
            auto preview = reduced ? readPreviewCache(settings, key) : CachedPreview{};
            bool rendered = preview.image.isNull();
            if (rendered) {
                ss::Project snapshot(projectPath);
                auto im = ss::previewFrame(snapshot, frame.id, preview.calibrated);
                preview.image = ss::gui::preview(im, lo, hi, common, reduced ? 2048 : 0);
                preview.black = lo;
                preview.white = hi;
                if (reduced)
                    writePreviewCache(
                        settings,
                        previewKey(frame, projectPath, preview.black, preview.white, common, calibration),
                        preview);
            }
            if (QThread::currentThread()->isInterruptionRequested())
                return;
            QMetaObject::invokeMethod(
                this,
                [this, preview, frame, generation, prefetch, reduced, key, projectPath, rendered, calibration,
                 common] {
                    if (!project || project->path() != projectPath)
                        return;
                    previewRenders += rendered;
                    if (reduced) {
                        int cost = int((preview.image.sizeInBytes() + 1023) / 1024);
                        auto resolved =
                            previewKey(frame, projectPath, preview.black, preview.white, common, calibration);
                        previewCache.insert(resolved, new CachedPreview(preview), cost);
                    }
                    if (!prefetch && generation == previewGeneration) {
                        auto row = model.rowsById.value(frame.id, -1);
                        if (row >= 0)
                            presentPreview(model.frames[size_t(row)], preview);
                    }
                },
                Qt::QueuedConnection);
        } catch (const std::exception &e) {
            auto message = QString::fromUtf8(e.what());
            QMetaObject::invokeMethod(
                this,
                [this, message, generation, prefetch, key] {
                    if (prefetch)
                        failedPreviewKeys.insert(key);
                    if (!prefetch && generation == previewGeneration) {
                        previewStatus.setText(message);
                        blinkButton->setChecked(false);
                    }
                },
                Qt::QueuedConnection);
        }
    });
    previewThread->setParent(this);
    connect(previewThread, &QThread::finished, this, [this] {
        auto *done = previewThread;
        previewThread = nullptr;
        done->deleteLater();
        if (pendingPreview)
            loadPreview();
        else
            prefetchNext();
    });
    previewThread->start();
}
void Window::settings(SettingsSection section) {
    if (!project || model.busy)
        return;
    auto s = project->settings();
    QDialog dialog(this);
    dialog.setMinimumWidth(480);
    dialog.setWindowTitle(QStringList{"Resource preferences", "Registration & calibration", "Frame grading",
                                      "Integration settings"}[section]);
    QFormLayout form(&dialog);
    QDoubleSpinBox memory, scratch, low, high, fwhm, ecc, keep;
    QSpinBox threads, iterations;
    QLineEdit cache(q(s.cacheDirectory.string()));
    QCheckBox rejection, poly, uncal, diagnostics;
    QComboBox reference;
    memory.setRange(64, 1048576);
    memory.setValue(double(s.memory / ss::MiB));
    memory.setSuffix(" MiB");
    scratch.setRange(0, 100000);
    scratch.setValue(double(s.scratch) / ss::GiB);
    scratch.setSuffix(" GiB");
    threads.setRange(1, 1024);
    threads.setValue(s.threads);
    iterations.setRange(1, 10);
    iterations.setValue(s.iterations);
    low.setRange(0.1, 20);
    high.setRange(0.1, 20);
    low.setValue(s.lowSigma);
    high.setValue(s.highSigma);
    fwhm.setRange(0, 100);
    fwhm.setValue(s.maxFwhm);
    ecc.setRange(0, 1);
    ecc.setSingleStep(.05);
    ecc.setValue(s.maxEccentricity);
    keep.setRange(.1, 100);
    keep.setValue(s.keepPercent);
    keep.setSuffix(" %");
    rejection.setChecked(s.rejection);
    poly.setChecked(s.polynomial);
    uncal.setChecked(s.allowUncalibrated);
    diagnostics.setChecked(s.diagnostics);
    reference.addItem("Automatic sharp reference", qlonglong(0));
    for (const auto &f : model.frames)
        if (f.kind == "light")
            reference.addItem(QString::number(f.id) + " · " + q(f.path.filename().string()), qlonglong(f.id));
    reference.setCurrentIndex(std::max(0, reference.findData(qlonglong(s.reference))));
    QLabel gradingPreview;
    gradingPreview.setWordWrap(true);
    if (section == Resources) {
        form.addRow("Memory budget", &memory);
        form.addRow("Scratch budget", &scratch);
        form.addRow("Scratch directory", &cache);
        form.addRow("CPU threads", &threads);
        auto *note = new QLabel("Prepared lights are kept separately from the evictable scratch cache.");
        note->setWordWrap(true);
        form.addRow(note);
    } else if (section == Registration) {
        form.addRow("Reference frame", &reference);
        form.addRow("Second-order distortion correction", &poly);
        form.addRow("Allow missing calibration", &uncal);
        auto *note = new QLabel("Automatic calibration is recommended. Allow missing calibration only when "
                                "you intend to prepare lights without matching correction frames.");
        note->setWordWrap(true);
        form.addRow(note);
    } else if (section == Grading) {
        form.addRow("Maximum FWHM (0 disables)", &fwhm);
        form.addRow("Maximum eccentricity (0 disables)", &ecc);
        form.addRow("Keep best FWHM per filter", &keep);
        form.addRow(&gradingPreview);
        auto update = [&] {
            auto proposed = s;
            proposed.maxFwhm = fwhm.value();
            proposed.maxEccentricity = ecc.value();
            proposed.keepPercent = keep.value();
            auto choices = ss::evaluateSelection(model.frames, proposed);
            int accepted = 0, excluded = 0;
            for (size_t i = 0; i < choices.size(); ++i)
                if (model.frames[i].kind == "light" && !model.frames[i].master)
                    choices[i].included ? ++accepted : ++excluded;
            gradingPreview.setText(
                QString("With these rules: %1 lights included · %2 excluded.\nManual inclusion overrides "
                        "grading. Processing errors must still be fixed or excluded.")
                    .arg(accepted)
                    .arg(excluded));
        };
        for (auto *control : {&fwhm, &ecc, &keep})
            connect(control, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog,
                    [update](double) { update(); });
        update();
    } else {
        form.addRow("Sigma rejection", &rejection);
        form.addRow("Low sigma", &low);
        form.addRow("High sigma", &high);
        form.addRow("Rejection iterations", &iterations);
        form.addRow("Coverage, weights, and rejection maps", &diagnostics);
        auto toggle = [&](bool enabled) {
            low.setEnabled(enabled);
            high.setEnabled(enabled);
            iterations.setEnabled(enabled);
        };
        connect(&rejection, &QCheckBox::toggled, &dialog, toggle);
        toggle(rejection.isChecked());
        auto *note =
            new QLabel("Statistical rejection runs only where at least ten valid exposures contribute. "
                       "Inverse-variance weighting and same-filter normalization are applied automatically.");
        note->setWordWrap(true);
        form.addRow(note);
    }
    QDialogButtonBox buttons(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    form.addRow(&buttons);
    connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return;
    if (section == Resources) {
        s.memory = uint64_t(memory.value()) * ss::MiB;
        s.scratch = uint64_t(scratch.value() * ss::GiB);
        s.cacheDirectory = cache.text().toStdString();
        s.threads = threads.value();
    } else if (section == Registration) {
        s.reference = reference.currentData().toLongLong();
        s.polynomial = poly.isChecked();
        s.allowUncalibrated = uncal.isChecked();
    } else if (section == Grading) {
        s.maxFwhm = fwhm.value();
        s.maxEccentricity = ecc.value();
        s.keepPercent = keep.value();
    } else {
        s.iterations = iterations.value();
        s.lowSigma = low.value();
        s.highSigma = high.value();
        s.rejection = rejection.isChecked();
        s.diagnostics = diagnostics.isChecked();
    }
    try {
        project->settings(s);
        updateReadiness();
        summary();
        showDetails();
    } catch (const std::exception &e) {
        error(e.what());
    }
}
void Window::calibration() {
    navigate(Calibration);
}
void Window::closeEvent(QCloseEvent *event) {
    if (worker.state() != QProcess::NotRunning) {
        auto answer = QMessageBox::question(this, "Processing is running",
                                            "Cancel processing and close? Completed stages are saved.");
        if (answer != QMessageBox::Yes) {
            event->ignore();
            return;
        }
    }
    saveUiState();
    event->accept();
}
bool Window::event(QEvent *event) {
    bool handled = QMainWindow::event(event);
    if (event->type() == QEvent::ApplicationPaletteChange) {
        setPalette(QApplication::palette());
        for (auto *widget : findChildren<QWidget *>())
            widget->setPalette(QApplication::palette());
        setStyleSheet(styleSheet());
    }
    return handled;
}
} // namespace ss::gui
