#include "window.hpp"
#include <signal.h>
#include <sys/types.h>

namespace ss::gui {
void Window::prepare() {
    if (!project || model.busy || !readiness.canPrepare)
        return;
    auto directory = preparedDirectory.text().trimmed();
    if (directory.isEmpty()) {
        error("Choose a directory for prepared light images.");
        return;
    }
    project->record("calibration", {{"directory", directory}});
    preparationElapsed = 0;
    preparationSequence = true;
    start("calibrate");
}

void Window::cancelProcessing() {
    preparationSequence = false;
    cancellationRequested = true;
    cancelJob.setEnabled(false);
    jobTitle.setText("Stopping safely…");
    if (worker.state() == QProcess::Running)
        ::kill(pid_t(worker.processId()), SIGINT);
}

void Window::finishJob(int code, QProcess::ExitStatus exit) {
    readWorkerOutput();
    updateFrames();
    auto command = jobCommand;
    bool successful = code == 0 && exit == QProcess::NormalExit && !workerHadError && !cancellationRequested;
    auto origin = jobOrigin;
    setBusy(false);
    if (successful && preparationSequence && command == "calibrate") {
        preparationElapsed += jobElapsed.elapsed();
        start("analyze");
        jobOrigin = origin;
        return;
    }
    preparationSequence = false;
    progress.setRange(0, 1000);
    refreshResults();
    if (successful) {
        progress.setValue(1000);
        jobTitle.setText(command == "analyze" ? "Frames prepared for review" : "Processing complete");
        jobDetail.setText(QString("Completed in %1 s. %2")
                              .arg((preparationElapsed + jobElapsed.elapsed()) / 1000)
                              .arg(command == "analyze" && !readiness.canStack
                                       ? "Review the frame statuses before stacking."
                                       : ""));
        completionStage =
            command == "stack" || command == "resume" || command == "masters" || command == "export" ? Results
            : command == "analyze"                                                                   ? Review
            : command == "import" ? Calibration
                                  : Review;
        viewCompletion.setText(completionStage == Results  ? "View results"
                               : completionStage == Review ? "Review frames"
                                                           : "Review calibration");
        viewCompletion.show();
        if (pages->currentIndex() == origin && command != "import" && command != "calibrate")
            navigate(Stage(completionStage));
    } else {
        jobTitle.setText(cancellationRequested || code == 130 ? "Processing cancelled"
                                                              : "Processing needs attention");
        jobDetail.setText(
            (workerHadError ? jobMessage + "\n" : QString()) +
            (command == "stack" || command == "resume"
                 ? "Completed work is saved. Resume into the same output directory."
             : command == "calibrate" || command == "analyze"
                 ? "Completed preparation is saved. Retry preparation to continue."
                 : "Completed work is saved. Inspect the details, correct the problem, and retry."));
        recoverJob.show();
        log.appendPlainText("Worker exit code " + QString::number(code));
    }
    refreshPages();
}

void Window::refreshCalibration() {
    QJsonArray groups = readiness.assignments;
    std::map<std::string, int> flatGroups;
    for (const auto &f : model.frames) {
        if (f.kind != "flat" || f.master || f.selection < 0)
            continue;
        QJsonObject identity{{"night", q(f.session)},
                             {"filter", q(f.filter)},
                             {"width", f.descriptor.width},
                             {"height", f.descriptor.height},
                             {"cfa", q(f.descriptor.cfa)}};
        for (const char *key : {"INSTRUME", "EXPTIME", "EXPOSURE", "GAIN", "OFFSET", "XBINNING", "YBINNING",
                                "CCD-TEMP", "READOUTM", "SS_BIAS", "SS_DARKFLAT"})
            identity[key] = f.descriptor.header[key];
        auto signature = ss::json(identity).toStdString();
        if (!flatGroups.contains(signature)) {
            flatGroups[signature] = groups.size();
            groups.append(QJsonObject{{"frame", qint64(f.id)},
                                      {"frameIds", QJsonArray{}},
                                      {"kind", "flat"},
                                      {"session", q(f.session)},
                                      {"filter", q(f.filter)},
                                      {"count", 0}});
        }
        auto group = groups[flatGroups[signature]].toObject();
        auto ids = group["frameIds"].toArray();
        ids.append(qint64(f.id));
        group["frameIds"] = ids;
        group["count"] = ids.size();
        groups[flatGroups[signature]] = group;
    }
    if (groups == calibrationGroups) {
        updateCalibrationGroup();
        return;
    }
    auto previous = assignments.currentRow() >= 0 && assignments.currentRow() < calibrationGroups.size()
                        ? calibrationGroups[assignments.currentRow()].toObject()["frame"].toInteger()
                        : 0;
    calibrationGroups = groups;
    QSignalBlocker blocker(assignments);
    assignments.setRowCount(groups.size());
    assignments.setHorizontalHeaderLabels({"Night", "Filter", "Frames", "Dark", "Bias", "Flat", "Status"});
    int selected = -1;
    for (int row = 0; row < groups.size(); ++row) {
        auto group = groups[row].toObject();
        if (group["frame"].toInteger() == previous)
            selected = row;
        bool flat = group["kind"] == "flat";
        QStringList cells{group["session"].toString(), group["filter"].toString(),
                          QString::number(group["count"].toInt()) + (flat ? " flats" : " lights")};
        for (auto kind : {"dark", "bias", "flat"}) {
            QStringList files;
            for (auto id : group[kind].toArray()) {
                auto index = model.rowsById.value(id.toInteger(), -1);
                if (index >= 0)
                    files << q(model.frames[size_t(index)].path.filename().string());
            }
            cells << (files.empty()       ? flat ? "See assignments" : "None"
                      : files.size() == 1 ? files.front()
                                          : QString("%1 raw frames").arg(files.size()));
        }
        cells << (flat                                    ? "Raw-flat correction"
                  : group.contains("error")               ? group["error"].toString()
                  : group["warnings"].toArray().isEmpty() ? "Matched"
                                                          : "Check details");
        for (int column = 0; column < cells.size(); ++column) {
            auto *item = new QTableWidgetItem(cells[column]);
            item->setToolTip(cells[column]);
            assignments.setItem(row, column, item);
        }
    }
    if (!groups.empty())
        assignments.selectRow(selected >= 0 ? selected : 0);
    updateCalibrationGroup();
}

void Window::updateCalibrationGroup() {
    if (!project)
        return;
    int row = assignments.currentRow();
    bool valid = row >= 0 && row < calibrationGroups.size();
    applyAssignment.setEnabled(valid && !model.busy);
    if (!valid) {
        calibrationHint.setText(readiness.lights ? "Select an acquisition group to review its assignments."
                                                 : "Import calibration frames to create reusable masters. "
                                                   "Lights are optional for this workflow.");
        for (auto &combo : calibrationChoices) {
            combo.clear();
            combo.setEnabled(false);
        }
        for (auto &text : calibrationDescriptions)
            text.clear();
        return;
    }
    auto group = calibrationGroups[row].toObject();
    auto representative = model.rowsById.value(group["frame"].toInteger(), -1);
    if (representative < 0)
        return;
    const auto &target = model.frames[size_t(representative)];
    const QStringList kinds{"bias", "dark", "flat", "darkflat"};
    for (int i = 0; i < 4; ++i) {
        auto &combo = calibrationChoices[i];
        QSignalBlocker blocker(combo);
        combo.clear();
        combo.addItem("Automatic matching", "");
        combo.addItem("None · explicitly skip", "none");
        combo.setItemData(
            0, "Uses compatible acquisition metadata. Ambiguous matches need an explicit assignment.",
            Qt::ToolTipRole);
        combo.setItemData(1, "No correction of this type will be applied.", Qt::ToolTipRole);
        std::map<std::string, std::vector<const ss::Frame *>> candidates;
        for (const auto &frame : model.frames) {
            if (q(frame.kind) != kinds[i] || frame.selection < 0)
                continue;
            QJsonObject acquisition{{"night", q(frame.session)},
                                    {"filter", q(frame.filter)},
                                    {"width", frame.descriptor.width},
                                    {"height", frame.descriptor.height},
                                    {"channels", frame.descriptor.channels},
                                    {"cfa", q(frame.descriptor.cfa)}};
            for (const char *field : {"INSTRUME", "EXPTIME", "EXPOSURE", "CCD-TEMP", "GAIN", "OFFSET",
                                      "XBINNING", "YBINNING", "READOUTM", "ROWORDER"})
                acquisition[field] = frame.descriptor.header[field];
            auto signature =
                frame.master ? "master:" + std::to_string(frame.id) : ss::json(acquisition).toStdString();
            candidates[signature].push_back(&frame);
        }
        for (const auto &[key, frames] : candidates) {
            QStringList ids;
            for (const auto *frame : frames)
                ids << QString::number(frame->id);
            const auto &first = *frames.front();
            QString name = first.master ? q(first.path.filename().string())
                                        : QString("%1 · %2 · %3 s · %4 raw frames")
                                              .arg(q(first.session), q(first.filter),
                                                   value(ss::number(first.descriptor.header, "EXPTIME")))
                                              .arg(frames.size());
            combo.addItem(name, ids.join(','));
            QString detail =
                QString("%1 · %2 × %3 · Gain %4 · Offset %5 · %6 °C\n%7")
                    .arg(q(ss::str(first.descriptor.header, "INSTRUME")))
                    .arg(first.descriptor.width)
                    .arg(first.descriptor.height)
                    .arg(value(ss::number(first.descriptor.header, "GAIN")),
                         value(ss::number(first.descriptor.header, "OFFSET")),
                         value(ss::number(first.descriptor.header, "CCD-TEMP")), q(first.path.string()));
            if (first.master)
                detail += first.biasSubtracted ? "\nMaster: bias already removed"
                                               : "\nMaster: bias not marked as removed";
            if (!first.metadataSources.empty())
                detail += "\nMetadata sources: " + QString::fromUtf8(ss::json(first.metadataSources));
            combo.setItemData(combo.count() - 1, detail, Qt::ToolTipRole);
            if (first.descriptor.width != target.descriptor.width ||
                first.descriptor.height != target.descriptor.height ||
                first.descriptor.channels != target.descriptor.channels ||
                first.descriptor.cfa != target.descriptor.cfa)
                static_cast<QStandardItemModel *>(combo.model())->item(combo.count() - 1)->setEnabled(false);
        }
        auto current =
            q(ss::str(target.descriptor.header, ("SS_" + kinds[i].toUpper()).toStdString().c_str()));
        int index = combo.findData(current);
        if (index < 0) {
            combo.addItem("Current explicit assignment", current);
            index = combo.count() - 1;
        }
        combo.setCurrentIndex(index);
        combo.setEnabled(!model.busy && (target.kind == "flat" ? i == 0 || i == 3 : i != 3));
        calibrationDescriptions[i].setText(combo.isEnabled() ? combo.currentData(Qt::ToolTipRole).toString()
                                           : i == 3 && target.kind != "flat"
                                               ? "Select a raw-flat group to assign its dark-flat correction."
                                               : QString());
    }
    QStringList notes;
    notes << QString("Selected: %1 %2 · %3 · %4 s · Camera %5")
                 .arg(group["count"].toInt())
                 .arg(target.kind == "flat" ? "flats" : "lights", q(target.filter),
                      value(ss::number(target.descriptor.header, "EXPTIME")),
                      q(ss::str(target.descriptor.header, "INSTRUME")));
    if (group.contains("error"))
        notes << "Resolve: " + group["error"].toString();
    for (auto warning : group["warnings"].toArray())
        notes << warning.toString();
    for (auto detail : group["details"].toArray()) {
        auto item = detail.toObject();
        QStringList missing;
        for (auto value : item["missing"].toArray())
            missing << value.toString();
        if (!missing.empty())
            notes << item["file"].toString() + ": missing " + missing.join(", ");
        auto sources = item["inferred"].toObject();
        for (auto it = sources.begin(); it != sources.end(); ++it)
            if (it.value() == "filename")
                notes << item["file"].toString() + ": " + it.key() + " inferred from filename";
    }
    calibrationHint.setText(notes.join("\n"));
}

void Window::applyCalibrationGroup() {
    if (!project || model.busy)
        return;
    int row = assignments.currentRow();
    if (row < 0 || row >= calibrationGroups.size())
        return;
    auto group = calibrationGroups[row].toObject();
    QStringList kinds{"bias", "dark", "flat", "darkflat"};
    try {
        project->transaction([&] {
            for (auto value : group["frameIds"].toArray()) {
                auto frame = project->frame(value.toInteger());
                if (!frame)
                    throw ss::Error("An assignment target is missing. Refresh the project.");
                for (int i = 0; i < 4; ++i) {
                    if (!calibrationChoices[i].isEnabled())
                        continue;
                    auto choice = calibrationChoices[i].currentData().toString();
                    auto key = "SS_" + kinds[i].toUpper();
                    if (choice.isEmpty())
                        frame->descriptor.header.remove(key);
                    else
                        frame->descriptor.header[key] = choice;
                }
                project->save(*frame);
            }
        });
        reload();
    } catch (const std::exception &e) {
        error(e.what());
    }
}

void Window::createMasters() {
    if (!project || model.busy)
        return;
    QDialog dialog(this);
    dialog.setWindowTitle("Create calibration masters");
    dialog.resize(560, 300);
    QFormLayout form(&dialog);
    QLineEdit directory;
    QPushButton browse("Browse…");
    auto *path = new QWidget;
    auto *row = new QHBoxLayout(path);
    row->setContentsMargins(0, 0, 0, 0);
    row->addWidget(&directory, 1);
    row->addWidget(&browse);
    form.addRow("Output directory", path);
    connect(&browse, &QPushButton::clicked, &dialog, [&] {
        auto selected = QFileDialog::getExistingDirectory(&dialog, "Choose the calibration master directory");
        if (!selected.isEmpty())
            directory.setText(selected);
    });
    QComboBox policy, format;
    policy.addItems({"Bias", "Dark-flat", "Automatic · prefer dark-flat", "None · uncalibrated"});
    format.addItems({"FITS · uncompressed", "FITS · lossless compressed", "XISF · uncompressed",
                     "XISF · Zstandard + shuffle"});
    format.setCurrentIndex(int(project->settings().format));
    form.addRow("Calibrate raw flats using", &policy);
    form.addRow("File format", &format);
    QSet<qlonglong> selected;
    for (auto *view : {&importTable, &table}) {
        auto *mapping = static_cast<FrameProxy *>(view->model());
        for (auto index : view->selectionModel()->selectedRows()) {
            const auto &frame = model.frames[size_t(mapping->mapToSource(index).row())];
            if (frame.kind != "light" && frame.kind != "unknown" && !frame.master && frame.selection >= 0)
                selected.insert(frame.id);
        }
    }
    auto selectedIds = selected.values();
    std::sort(selectedIds.begin(), selectedIds.end());
    QStringList ids;
    for (auto id : selectedIds)
        ids << QString::number(id);
    QComboBox source;
    source.addItem("All included raw calibration frames");
    if (!ids.empty()) {
        source.addItem(QString("Selected raw calibration frames (%1)").arg(ids.size()));
        source.setCurrentIndex(1);
    }
    form.addRow("Frames to use", &source);
    QLabel explanation("Lights and imported masters are not stacked into calibration masters. Selection can "
                       "be made in Import or Review.");
    explanation.setWordWrap(true);
    form.addRow(&explanation);
    QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons.button(QDialogButtonBox::Ok)->setText("Create masters");
    buttons.button(QDialogButtonBox::Ok)->setEnabled(false);
    connect(&directory, &QLineEdit::textChanged, &dialog, [&](const QString &text) {
        buttons.button(QDialogButtonBox::Ok)->setEnabled(!text.trimmed().isEmpty());
    });
    form.addRow(&buttons);
    connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return;
    QStringList args{directory.text().trimmed(), "--flat-calibration",
                     QStringList{"bias", "darkflat", "auto", "none"}[policy.currentIndex()], "--format",
                     QStringList{"fits", "fits.fz", "xisf", "xisf-zstd"}[format.currentIndex()]};
    if (source.currentIndex() == 1 && !ids.empty())
        args << "--ids" << ids.join(',');
    start("masters", args);
}

void Window::refreshResults() {
    if (!project)
        return;
    auto combined = project->record("results")["masters"].toArray();
    for (auto value : project->record("calibration-results")["masters"].toArray())
        combined.append(value);
    if (products == combined)
        return;
    auto selected = resultList.currentRow() >= 0 && resultList.currentRow() < products.size()
                        ? products[resultList.currentRow()].toObject()["path"].toString()
                        : QString();
    products = combined;
    ++resultGeneration;
    QSignalBlocker blocker(resultList);
    resultList.clear();
    bool calibration = false;
    int current = 0;
    for (int i = 0; i < products.size(); ++i) {
        auto row = products[i].toObject();
        QString name = row.contains("kind") ? "Calibration · " + row["kind"].toString()
                                            : "Light master · " + row["filter"].toString();
        calibration |= row.contains("kind");
        auto *item = new QListWidgetItem(name + QString("\n%1 frames").arg(row["frames"].toInt()));
        item->setSizeHint(QSize(220, 60));
        item->setToolTip(row["path"].toString());
        resultList.addItem(item);
        if (row["path"] == selected)
            current = i;
    }
    importMasterButton.setVisible(calibration);
    if (!products.empty())
        resultList.setCurrentRow(current);
    if (products.empty())
        resultDetails.setText(
            "No completed masters yet. Prepare and stack your lights, or create calibration masters.");
    showResult();
    refreshPages();
}

void Window::showResult() {
    if (!project)
        return;
    ++resultGeneration;
    if (resultThread) {
        resultThread->requestInterruption();
        return;
    }
    int row = resultList.currentRow();
    if (row < 0 || row >= products.size())
        return;
    auto product = products[row].toObject();
    auto path = product["path"].toString();
    auto generation = resultGeneration;
    auto memory = project->settings().memory;
    resultDetails.setText("Loading " + QFileInfo(path).fileName() + "…");
    resultThread = QThread::create([this, path, generation, memory, product] {
        try {
            auto image = ss::readImage(path.toStdString(), false, memory / 2);
            if (QThread::currentThread()->isInterruptionRequested())
                return;
            double lo = NAN, hi = NAN;
            auto display = preview(image, lo, hi, false, 2048);
            QString details =
                QFileInfo(path).fileName() +
                QString(" · %1 × %2 · %3 frames\n%4 · display stretch only")
                    .arg(image.width)
                    .arg(image.height)
                    .arg(product["frames"].toInt())
                    .arg(product.contains("kind") ? "Calibration master" : "Linear light master");
            QMetaObject::invokeMethod(
                this,
                [this, display, details, generation] {
                    if (generation != resultGeneration)
                        return;
                    resultView.display(display, {});
                    resultDetails.setText(details);
                },
                Qt::QueuedConnection);
        } catch (const std::exception &e) {
            auto message = QString::fromUtf8(e.what());
            QMetaObject::invokeMethod(
                this,
                [this, message, generation] {
                    if (generation != resultGeneration)
                        return;
                    resultView.scene.clear();
                    resultView.pixels = nullptr;
                    resultView.markers.clear();
                    resultDetails.setText("Could not preview this master: " + message);
                },
                Qt::QueuedConnection);
        }
    });
    resultThread->setParent(this);
    connect(resultThread, &QThread::finished, this, [this, generation] {
        auto *finished = resultThread;
        resultThread = nullptr;
        finished->deleteLater();
        if (generation != resultGeneration)
            showResult();
    });
    resultThread->start();
}

void Window::exportResults() {
    if (!project || model.busy || project->record("results")["masters"].toArray().empty())
        return;
    if (exportDirectory.text().trimmed().isEmpty()) {
        auto path = QFileDialog::getExistingDirectory(this, "Choose the export directory");
        if (path.isEmpty())
            return;
        exportDirectory.setText(path);
    }
    start("export", {exportDirectory.text().trimmed(), "--format",
                     QStringList{"fits", "fits.fz", "xisf", "xisf-zstd"}[exportFormat.currentIndex()]});
}
} // namespace ss::gui
