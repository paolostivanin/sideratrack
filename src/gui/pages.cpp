#include "window.hpp"

static void initializeIcons() {
    Q_INIT_RESOURCE(icons);
}

namespace ss::gui {
namespace {
const QStringList stageNames{"Import", "Calibration", "Review", "Stack", "Results"};
const QStringList stageDescriptions{
    "Add your lights and calibration frames. Check how each exposure was identified.",
    "Match calibration frames to your lights, then prepare them for review.",
    "Inspect your exposures, compare quality, and choose which frames to stack.",
    "Review the selection and output settings before creating your linear masters.",
    "Your linear masters are ready for color composition and finishing in your image editor."};
const QStringList formatNames{"FITS · uncompressed", "FITS · lossless compressed", "XISF · uncompressed",
                              "XISF · Zstandard + shuffle"};
QIcon icon(const QString &name) {
    return QIcon::fromTheme(name, QIcon(":/icons/" + name + ".svg"));
}
QLabel *label(const QString &text, int size = 0, bool bold = false) {
    auto *result = new QLabel(text);
    result->setWordWrap(true);
    if (size || bold) {
        auto font = result->font();
        if (size)
            font.setPointSize(size);
        if (bold)
            font.setWeight(QFont::DemiBold);
        result->setFont(font);
    }
    return result;
}
QWidget *scrollable(QWidget *content) {
    auto *area = new QScrollArea;
    area->setWidgetResizable(true);
    area->setFrameShape(QFrame::NoFrame);
    area->setWidget(content);
    return area;
}
void setupTable(QTableView &table) {
    table.setItemDelegate(new FrameDelegate(&table));
    table.setSelectionBehavior(QAbstractItemView::SelectRows);
    table.setSelectionMode(QAbstractItemView::ExtendedSelection);
    table.setSortingEnabled(true);
    table.setAlternatingRowColors(true);
    table.setWordWrap(false);
    table.setShowGrid(false);
    table.verticalHeader()->hide();
    table.verticalHeader()->setDefaultSectionSize(32);
    table.horizontalHeader()->setHighlightSections(false);
    table.setColumnWidth(0, 42);
    table.setColumnWidth(2, 230);
    table.setColumnWidth(16, 200);
}
QWidget *directoryField(QLineEdit &edit, QWidget *parent, const QString &title) {
    auto *row = new QWidget;
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *browse = new QPushButton(icon("folder-open"), "Browse…");
    layout->addWidget(&edit, 1);
    layout->addWidget(browse);
    QObject::connect(browse, &QPushButton::clicked, parent, [&edit, parent, title] {
        auto path = QFileDialog::getExistingDirectory(parent, title, edit.text());
        if (!path.isEmpty())
            edit.setText(path);
    });
    return row;
}
QString joined(const std::vector<std::string> &messages) {
    QStringList unique;
    for (const auto &message : messages)
        if (!unique.contains(q(message)))
            unique << q(message);
    return unique.join("\n");
}
} // namespace

void Window::buildUi() {
    initializeIcons();
    auto baseFont = font();
    baseFont.setPointSizeF(std::max(10.0, baseFont.pointSizeF()));
    setFont(baseFont);
    setWindowTitle("Stellastack");
    setWindowIcon(QIcon(":/org.stellastack.Stellastack.svg"));
    resize(1450, 900);
    setMinimumSize(900, 600);
    restoreGeometry(preferences.value("window/geometry").toByteArray());
    auto *file = menuBar()->addMenu("&Project");
    newAction = file->addAction(icon("list-add"), "&New project…", this, [this] { createProject(); });
    newAction->setShortcut(QKeySequence::New);
    openAction = file->addAction(icon("folder-open"), "&Open project…", this, [this] { chooseProject(); });
    openAction->setShortcut(QKeySequence::Open);
    file->addSeparator();
    auto *importFiles = file->addAction("Add &files…", this, [this] { chooseImport(false); });
    importFiles->setShortcut(QKeySequence("Ctrl+I"));
    auto *importFolder = file->addAction("Add f&older…", this, [this] { chooseImport(true); });
    jobActions << importFiles << importFolder;
    file->addSeparator();
    file->addAction("&Quit", QKeySequence::Quit, this, &QWidget::close);
    auto *settingsMenu = menuBar()->addMenu("&Settings");
    auto addSettings = [&](const QString &text, SettingsSection section) {
        auto *action = settingsMenu->addAction(text, this, [this, section] { settings(section); });
        jobActions << action;
    };
    addSettings("Resource preferences…", Resources);
    addSettings("Registration & calibration…", Registration);
    addSettings("Frame grading…", Grading);
    addSettings("Integration settings…", Integration);
    auto *advanced = menuBar()->addMenu("&Advanced");
    calibrateAction = advanced->addAction("Calibrate only", this, [this] { start("calibrate"); });
    analyzeAction = advanced->addAction("Analyze only", this, [this] { start("analyze"); });
    analyzeAction->setToolTip("Measure calibrated lights using the current registration settings.");
    jobActions << calibrateAction << analyzeAction;
    auto *help = menuBar()->addMenu("&Help");
    help->addAction("About Stellastack", this, [this] {
        QMessageBox::about(this, "Stellastack",
                           "<h3>Stellastack " STELLASTACK_DISPLAY_VERSION "</h3>"
                           "<p>From camera exposures to linear masters.</p>"
                           "<p>Linux astrophotography stacking · GPL-3.0-or-later</p>");
    });
    welcomeOrProject = new QStackedWidget;
    welcomeOrProject->addWidget(buildWelcome());
    projectWorkspace = new QWidget;
    auto *workspace = new QHBoxLayout(projectWorkspace);
    workspace->setContentsMargins(0, 0, 0, 0);
    workspace->setSpacing(0);
    auto *sidebar = new QFrame;
    sidebar->setObjectName("sidebar");
    sidebar->setFixedWidth(190);
    auto *side = new QVBoxLayout(sidebar);
    side->setContentsMargins(16, 24, 16, 16);
    side->setSpacing(16);
    side->addWidget(label("Stellastack", 17, true));
    projectName.setWordWrap(true);
    projectName.setTextFormat(Qt::PlainText);
    side->addWidget(&projectName);
    navigation.setObjectName("stageNavigation");
    navigation.setFrameShape(QFrame::NoFrame);
    navigation.setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    for (int i = 0; i < stageNames.size(); ++i) {
        auto *item = new QListWidgetItem(
            icon(QStringList{"document-import", "adjustments", "view-preview", "layers", "task-complete"}[i]),
            stageNames[i]);
        item->setSizeHint(QSize(150, 66));
        navigation.addItem(item);
    }
    side->addWidget(&navigation, 1);
    auto *resources = new QPushButton(icon("preferences-system"), "Preferences");
    editableWidgets << resources;
    connect(resources, &QPushButton::clicked, this, [this] { settings(Resources); });
    side->addWidget(resources);
    workspace->addWidget(sidebar);
    auto *content = new QWidget;
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(24, 24, 24, 16);
    layout->setSpacing(16);
    auto *heading = new QHBoxLayout;
    auto font = pageTitle.font();
    font.setPointSize(23);
    font.setWeight(QFont::DemiBold);
    pageTitle.setFont(font);
    heading->addWidget(&pageTitle, 1);
    primary.setMinimumHeight(36);
    primary.setAutoDefault(false);
    primary.setObjectName("primaryAction");
    heading->addWidget(&primary);
    layout->addLayout(heading);
    pageDescription.setWordWrap(true);
    layout->addWidget(&pageDescription);
    pageHint.setWordWrap(true);
    pageHint.setTextFormat(Qt::RichText);
    pageHint.setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
    layout->addWidget(&pageHint);
    connect(&pageHint, &QLabel::linkActivated, this, [this](const QString &target) {
        if (target == "resources")
            settings(Resources);
        else if (target == "registration")
            settings(Registration);
        else
            navigate(Stage(target.toInt()));
    });
    pages = new QStackedWidget;
    pages->addWidget(buildImport());
    pages->addWidget(buildCalibration());
    pages->addWidget(buildReview());
    pages->addWidget(buildStack());
    pages->addWidget(buildResults());
    layout->addWidget(pages, 1);
    jobPanel = new QFrame;
    jobPanel->setObjectName("jobPanel");
    auto *jobLayout = new QVBoxLayout(jobPanel);
    jobLayout->setContentsMargins(12, 12, 12, 12);
    auto *jobHeading = new QHBoxLayout;
    auto jobFont = jobTitle.font();
    jobFont.setWeight(QFont::DemiBold);
    jobTitle.setFont(jobFont);
    jobHeading->addWidget(&jobTitle, 1);
    cancelJob.setText("Cancel");
    cancelJob.setToolTip("Stop safely at the next processing checkpoint");
    recoverJob.setText("Retry preparation");
    viewCompletion.setText("View results");
    logsToggle.setText("Details");
    logsToggle.setCheckable(true);
    jobHeading->addWidget(&viewCompletion);
    jobHeading->addWidget(&recoverJob);
    jobHeading->addWidget(&cancelJob);
    jobHeading->addWidget(&logsToggle);
    jobLayout->addLayout(jobHeading);
    jobDetail.setWordWrap(true);
    jobDetail.setTextFormat(Qt::PlainText);
    jobLayout->addWidget(&jobDetail);
    progress.setRange(0, 1000);
    progress.setTextVisible(false);
    progress.setMaximumHeight(6);
    jobLayout->addWidget(&progress);
    log.setReadOnly(true);
    log.setMaximumBlockCount(500);
    log.setMaximumHeight(110);
    jobLayout->addWidget(&log);
    log.hide();
    connect(&logsToggle, &QToolButton::toggled, &log, &QWidget::setVisible);
    connect(&cancelJob, &QPushButton::clicked, this, [this] { cancelProcessing(); });
    connect(&recoverJob, &QPushButton::clicked, this, [this] {
        if (lastCommand == "calibrate" || lastCommand == "analyze")
            prepare();
        else if (lastCommand == "stack" || lastCommand == "resume")
            start("resume", lastArguments);
        else
            start(lastCommand, lastArguments);
    });
    connect(&viewCompletion, &QPushButton::clicked, this, [this] {
        if (completionStage >= 0)
            navigate(Stage(completionStage));
    });
    layout->addWidget(jobPanel);
    jobPanel->hide();
    recoverJob.hide();
    viewCompletion.hide();
    elapsedTimer.setInterval(1000);
    connect(&elapsedTimer, &QTimer::timeout, this, [this] {
        if (model.busy)
            jobDetail.setText(jobMessage + QString(" · %1:%2 elapsed")
                                               .arg((preparationElapsed + jobElapsed.elapsed()) / 60000)
                                               .arg(((preparationElapsed + jobElapsed.elapsed()) / 1000) % 60,
                                                    2, 10, QChar('0')));
    });
    workspace->addWidget(content, 1);
    welcomeOrProject->addWidget(projectWorkspace);
    setCentralWidget(welcomeOrProject);
    statusBar()->addWidget(&status, 1);
    status.setText("Create or open a project to begin");
    connect(&navigation, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row >= 0)
            navigate(Stage(row));
    });
    connect(&primary, &QPushButton::clicked, this, [this] { runPrimary(); });
    for (int i = 0; i < 5; ++i) {
        auto *shortcut = new QShortcut(QKeySequence(QString("Alt+%1").arg(i + 1)), this);
        connect(shortcut, &QShortcut::activated, this, [this, i] {
            if (project)
                navigate(Stage(i));
        });
    }
    setStyleSheet("QFrame#sidebar { background: palette(alternate-base); }"
                  "QListWidget#stageNavigation { background: transparent; }"
                  "QListWidget#stageNavigation::item { padding: 8px; border-radius: 6px; }"
                  "QListWidget#stageNavigation::item:selected { background: palette(highlight); color: "
                  "palette(highlighted-text); }"
                  "QFrame#dropArea { border: 1px dashed palette(mid); border-radius: 8px; }"
                  "QFrame#jobPanel { border: 1px solid palette(mid); border-radius: 6px; }"
                  "QPushButton#primaryAction { padding: 7px 18px; font-weight: 600; }");
    updateRecentProjects();
    setBusy(false);
}

QWidget *Window::buildWelcome() {
    auto *page = new QWidget;
    auto *outer = new QVBoxLayout(page);
    outer->setContentsMargins(48, 32, 48, 32);
    outer->addStretch();
    auto *body = new QWidget;
    body->setMinimumWidth(560);
    body->setMaximumWidth(660);
    auto *layout = new QVBoxLayout(body);
    layout->setSpacing(20);
    auto *logo = new QLabel;
    logo->setPixmap(QIcon(":/org.stellastack.Stellastack.svg").pixmap(72, 72));
    layout->addWidget(logo);
    layout->addWidget(label("A clearer path to your\nnext deep-sky image.", 28, true));
    layout->addWidget(label("Bring your nights together. Calibrate your exposures, review their quality, "
                            "and stack them into linear masters ready for your image editor.",
                            12));
    auto *actions = new QHBoxLayout;
    auto *create = new QPushButton(icon("list-add"), "Create project…");
    auto *open = new QPushButton(icon("folder-open"), "Open project…");
    create->setMinimumHeight(42);
    open->setMinimumHeight(42);
    actions->addWidget(create);
    actions->addWidget(open);
    actions->addStretch();
    layout->addLayout(actions);
    connect(create, &QPushButton::clicked, this, [this] { createProject(); });
    connect(open, &QPushButton::clicked, this, [this] { chooseProject(); });
    layout->addWidget(label("One project for each target, camera, and optical setup. Add exposures from as "
                            "many nights as you need."));
    layout->addWidget(label("Recent projects", 11, true));
    recentProjects.setFrameShape(QFrame::NoFrame);
    recentProjects.setStyleSheet("background: transparent;");
    recentProjects.setMaximumHeight(150);
    layout->addWidget(&recentProjects);
    connect(&recentProjects, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
        auto path = item->data(Qt::UserRole).toString();
        if (!path.isEmpty())
            this->open(path);
    });
    outer->addWidget(body, 0, Qt::AlignHCenter);
    outer->addStretch();
    return scrollable(page);
}

QWidget *Window::buildImport() {
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(16);
    dropArea = new ImportDropArea;
    dropArea->setObjectName("dropArea");
    auto *drop = new QHBoxLayout(dropArea);
    drop->setContentsMargins(20, 18, 20, 18);
    auto *copy = new QVBoxLayout;
    copy->addWidget(label("Drop your exposures here", 13, true));
    copy->addWidget(
        label("FITS and XISF files, or folders containing lights, darks, flats, biases, and dark-flats."));
    drop->addLayout(copy, 1);
    auto *actions = new QVBoxLayout;
    auto *files = new QPushButton(icon("document-import"), "Add files…");
    auto *folder = new QPushButton(icon("folder-open"), "Add folder…");
    actions->addWidget(files);
    actions->addWidget(folder);
    drop->addLayout(actions);
    connect(files, &QPushButton::clicked, this, [this] { chooseImport(false); });
    connect(folder, &QPushButton::clicked, this, [this] { chooseImport(true); });
    dropArea->import = [this](const QStringList &paths) { start("import", paths); };
    editableWidgets << dropArea;
    layout->addWidget(dropArea);
    importSummary.setWordWrap(true);
    importSummary.setTextFormat(Qt::PlainText);
    layout->addWidget(&importSummary);
    importProxy.setSourceModel(&model);
    importTable.setModel(&importProxy);
    setupTable(importTable);
    for (int i = 0; i < columns.size(); ++i)
        importTable.setColumnHidden(i, !QList<int>{0, 2, 3, 4, 5, 6, 7, 16}.contains(i));
    importTable.horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(&importTable, 1);
    auto *row = new QHBoxLayout;
    auto *edit = new QPushButton("Edit metadata…");
    auto *exclude = new QPushButton("Exclude selected");
    editableWidgets << edit << exclude;
    connect(edit, &QPushButton::clicked, this, [this] { editSelected(); });
    connect(exclude, &QPushButton::clicked, this, [this] { select(-1); });
    row->addWidget(edit);
    row->addWidget(exclude);
    row->addStretch();
    row->addWidget(label("Double-click Type, Filter, or Night to correct a value."));
    layout->addLayout(row);
    return page;
}

QWidget *Window::buildCalibration() {
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(16);
    calibrationHint.setWordWrap(true);
    calibrationHint.setTextFormat(Qt::PlainText);
    auto *groupDetails = scrollable(&calibrationHint);
    groupDetails->setMaximumHeight(110);
    groupDetails->setMinimumHeight(45);
    layout->addWidget(groupDetails);
    assignments.setColumnCount(7);
    assignments.setHorizontalHeaderLabels({"Night", "Filter", "Lights", "Dark", "Bias", "Flat", "Status"});
    assignments.setSelectionBehavior(QAbstractItemView::SelectRows);
    assignments.setSelectionMode(QAbstractItemView::SingleSelection);
    assignments.setEditTriggers(QAbstractItemView::NoEditTriggers);
    assignments.setAlternatingRowColors(true);
    assignments.setShowGrid(false);
    assignments.verticalHeader()->hide();
    assignments.horizontalHeader()->setStretchLastSection(true);
    assignments.setMinimumHeight(130);
    layout->addWidget(&assignments, 1);
    connect(&assignments, &QTableWidget::itemSelectionChanged, this, [this] { updateCalibrationGroup(); });
    auto *editor = new QGroupBox("Assignments for the selected group");
    auto *form = new QFormLayout(editor);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    for (int i = 0; i < 4; ++i) {
        calibrationChoices[i].setMinimumContentsLength(15);
        calibrationChoices[i].setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        calibrationDescriptions[i].setWordWrap(true);
        calibrationDescriptions[i].setTextFormat(Qt::PlainText);
        auto *field = new QWidget;
        auto *row = new QVBoxLayout(field);
        row->setContentsMargins(0, 0, 0, 0);
        row->addWidget(&calibrationChoices[i]);
        row->addWidget(&calibrationDescriptions[i]);
        form->addRow(QStringList{"Bias", "Dark", "Flat", "Dark-flat for raw flats"}[i], field);
        connect(&calibrationChoices[i], &QComboBox::currentIndexChanged, this, [this, i] {
            calibrationDescriptions[i].setText(calibrationChoices[i].currentData(Qt::ToolTipRole).toString());
        });
        editableWidgets << &calibrationChoices[i];
    }
    applyAssignment.setText("Apply to this group");
    form->addRow(&applyAssignment);
    connect(&applyAssignment, &QPushButton::clicked, this, [this] { applyCalibrationGroup(); });
    editableWidgets << &applyAssignment;
    form->addRow("Prepared light images",
                 directoryField(preparedDirectory, this, "Prepared light images directory"));
    layout->addWidget(scrollable(editor));
    preparedDirectory.setToolTip("Verified calibrated images are kept separately from the scratch cache. "
                                 "Original inputs are preserved.");
    connect(&preparedDirectory, &QLineEdit::textChanged, this, [this] { refreshPages(); });
    connect(&preparedDirectory, &QLineEdit::editingFinished, this, [this] {
        if (!project || model.busy || preparedDirectory.text().trimmed().isEmpty())
            return;
        try {
            project->record("calibration", {{"directory", preparedDirectory.text().trimmed()}});
        } catch (const std::exception &e) {
            error(e.what());
        }
    });
    auto *actions = new QHBoxLayout;
    masterButton.setText("Create calibration masters…");
    masterButton.setIcon(icon("layers"));
    auto *registration = new QPushButton("Registration & calibration options…");
    editableWidgets << &masterButton << registration << editor;
    connect(&masterButton, &QPushButton::clicked, this, [this] { createMasters(); });
    connect(registration, &QPushButton::clicked, this, [this] { settings(Registration); });
    actions->addWidget(&masterButton);
    actions->addStretch();
    actions->addWidget(registration);
    layout->addLayout(actions);
    return page;
}

QWidget *Window::buildReview() {
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);
    auto *filters = new QHBoxLayout;
    typeFilter.addItem("All types", "");
    for (const auto &type : QStringList{"light", "dark", "flat", "bias", "darkflat", "unknown"})
        typeFilter.addItem(type == "darkflat" ? "Dark-flat" : type.left(1).toUpper() + type.mid(1), type);
    typeFilter.setCurrentIndex(1);
    filter.addItem("All filters", "");
    nightFilter.addItem("All nights", "");
    stateFilter.addItems({"All statuses", "Included", "Excluded", "Needs attention"});
    search.setPlaceholderText("Find a frame…");
    search.setClearButtonEnabled(true);
    filters->addWidget(&typeFilter);
    filters->addWidget(&filter);
    filters->addWidget(&nightFilter);
    filters->addWidget(&stateFilter);
    filters->addWidget(&search, 1);
    layout->addLayout(filters);
    proxy.setSourceModel(&model);
    proxy.kind = "light";
    table.setModel(&proxy);
    setupTable(table);
    for (int i = 0; i < columns.size(); ++i)
        table.setColumnHidden(i, !QList<int>{0, 2, 8, 16}.contains(i));
    table.setColumnWidth(2, 140);
    table.setColumnWidth(8, 60);
    table.setColumnWidth(16, 120);
    auto saved = preferences.value("review/tableHeader").toByteArray();
    if (!saved.isEmpty())
        table.horizontalHeader()->restoreState(saved);
    table.horizontalHeader()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(table.horizontalHeader(), &QWidget::customContextMenuRequested, this,
            [this](const QPoint &position) {
                QMenu menu;
                for (int column = 1; column < columns.size(); ++column) {
                    auto *action = menu.addAction(columns[column]);
                    action->setCheckable(true);
                    action->setChecked(!table.isColumnHidden(column));
                    connect(action, &QAction::toggled, this,
                            [this, column](bool visible) { table.setColumnHidden(column, !visible); });
                }
                menu.exec(table.horizontalHeader()->mapToGlobal(position));
            });
    auto *left = new QWidget;
    auto *leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->addWidget(&table, 1);
    selectionSummary.setWordWrap(true);
    leftLayout->addWidget(&selectionSummary);
    auto *selection = new QHBoxLayout;
    for (auto item : {std::pair{"Include", 1}, std::pair{"Exclude", -1}, std::pair{"Automatic", 0}}) {
        auto *button = new QPushButton(item.first);
        button->setToolTip(item.second == 0 ? "Reset selected frames to automatic grading"
                                            : "Override grading for the selected frames");
        selection->addWidget(button);
        editableWidgets << button;
        connect(button, &QPushButton::clicked, this, [this, value = item.second] { select(value); });
    }
    leftLayout->addLayout(selection);
    auto *edit = new QPushButton("Edit selected metadata…");
    editableWidgets << edit;
    connect(edit, &QPushButton::clicked, this, [this] { editSelected(); });
    leftLayout->addWidget(edit);
    auto *right = new QWidget;
    auto *rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    auto *tools = new QHBoxLayout;
    blinkButton = new QPushButton("Start blink");
    blinkButton->setCheckable(true);
    blinkButton->setEnabled(false);
    blinkInterval.setRange(0.1, 60);
    blinkInterval.setSingleStep(0.1);
    blinkInterval.setDecimals(1);
    blinkInterval.setValue(preferences.value("review/blinkInterval", 1).toDouble());
    blinkInterval.setSuffix(" s");
    blinkInterval.setToolTip("Seconds per frame during blink playback");
    auto *stars = new QCheckBox("Stars");
    stars->setChecked(true);
    stars->setToolTip("Overlay detected stars on the image");
    auto *common = new QCheckBox("Match stretch");
    common->setChecked(true);
    common->setToolTip("Use the same display stretch when comparing exposures. Image data is unchanged.");
    auto *fit = new QToolButton;
    fit->setText("Fit");
    fit->setToolTip("Fit the image to the canvas");
    auto *actual = new QToolButton;
    actual->setText("1:1");
    actual->setToolTip("View image at full pixel scale");
    detailsToggle.setText("Details");
    detailsToggle.setCheckable(true);
    tools->addWidget(blinkButton);
    tools->addWidget(&blinkInterval);
    tools->addStretch();
    tools->addWidget(&detailsToggle);
    rightLayout->addLayout(tools);
    auto *displayTools = new QHBoxLayout;
    displayTools->addWidget(stars);
    displayTools->addWidget(common);
    displayTools->addStretch();
    displayTools->addWidget(fit);
    displayTools->addWidget(actual);
    rightLayout->addLayout(displayTools);
    previewStatus.setWordWrap(true);
    previewStatus.setTextFormat(Qt::PlainText);
    previewStatus.setText("Select a frame to inspect it. Scroll to zoom; drag to pan.");
    rightLayout->addWidget(&previewStatus);
    view.setMinimumHeight(80);
    rightLayout->addWidget(&view, 1);
    inspector = new QScrollArea;
    static_cast<QScrollArea *>(inspector)->setWidgetResizable(true);
    static_cast<QScrollArea *>(inspector)->setFrameShape(QFrame::NoFrame);
    frameDetails.setWordWrap(true);
    frameDetails.setTextFormat(Qt::PlainText);
    frameDetails.setTextInteractionFlags(Qt::TextSelectableByMouse);
    static_cast<QScrollArea *>(inspector)->setWidget(&frameDetails);
    inspector->setMaximumHeight(140);
    rightLayout->addWidget(inspector);
    inspector->hide();
    connect(&detailsToggle, &QToolButton::toggled, inspector, &QWidget::setVisible);
    connect(stars, &QCheckBox::toggled, &view, &ImageView::toggleStars);
    connect(common, &QCheckBox::toggled, this, [this](bool enabled) {
        blinkButton->setChecked(false);
        commonStretch = enabled;
        ++previewGeneration;
        pendingPreview = 0;
        black = white = NAN;
        showCurrent();
    });
    connect(fit, &QToolButton::clicked, &view, &ImageView::fitImage);
    connect(actual, &QToolButton::clicked, this, [this] {
        view.autoFit = false;
        view.resetTransform();
    });
    auto *quality = new QHBoxLayout;
    quality->addWidget(label("Compare quality"));
    metric.addItems({"FWHM", "HFR", "Eccentricity", "Stars", "Background", "Noise", "Transparency"});
    metric.setToolTip("FWHM and HFR measure star size; eccentricity measures elongation. Lower values "
                      "usually indicate better frames.");
    quality->addWidget(&metric);
    quality->addStretch();
    auto *grading = new QPushButton("Grading rules…");
    editableWidgets << grading;
    connect(grading, &QPushButton::clicked, this, [this] { settings(Grading); });
    quality->addWidget(grading);
    rightLayout->addLayout(quality);
    plot.visible = &proxy;
    plot.setMinimumHeight(65);
    plot.setMaximumHeight(160);
    rightLayout->addWidget(&plot);
    reviewSplit = new QSplitter;
    left->setMinimumWidth(230);
    right->setMinimumWidth(330);
    reviewSplit->addWidget(left);
    reviewSplit->addWidget(scrollable(right));
    reviewSplit->setStretchFactor(0, 3);
    reviewSplit->setStretchFactor(1, 7);
    reviewSplit->setSizes({380, 750});
    reviewSplit->restoreState(preferences.value("review/splitter").toByteArray());
    layout->addWidget(reviewSplit, 1);
    auto applyFilters = [this] {
        blinkButton->setChecked(false);
        proxy.kind = typeFilter.currentData().toString();
        proxy.band = filter.currentData().toString();
        proxy.night = nightFilter.currentData().toString();
        proxy.state = stateFilter.currentIndex() ? stateFilter.currentText() : QString();
        proxy.search = search.text();
        proxy.refresh();
        plot.update();
        showCurrent();
        summary();
    };
    for (auto *combo : {&typeFilter, &filter, &nightFilter, &stateFilter})
        connect(combo, &QComboBox::currentIndexChanged, this, [applyFilters](int) { applyFilters(); });
    connect(&search, &QLineEdit::textChanged, this, [applyFilters](const QString &) { applyFilters(); });
    connect(table.selectionModel(), &QItemSelectionModel::currentRowChanged, this, [this] { showDetails(); });
    return page;
}

QWidget *Window::buildStack() {
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(16);
    preflightSummary.setWordWrap(true);
    preflightSummary.setTextFormat(Qt::PlainText);
    layout->addWidget(&preflightSummary);
    stackGroups.setColumnCount(6);
    stackGroups.setHorizontalHeaderLabels(
        {"Filter", "Night", "Included", "Excluded", "Exposure time", "Calibration"});
    stackGroups.setEditTriggers(QAbstractItemView::NoEditTriggers);
    stackGroups.verticalHeader()->hide();
    stackGroups.setShowGrid(false);
    stackGroups.setAlternatingRowColors(true);
    stackGroups.horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    layout->addWidget(&stackGroups, 1);
    auto *output = new QGroupBox("Output");
    auto *form = new QFormLayout(output);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->addRow("Master directory",
                 directoryField(stackDirectory, this, "Choose the master output directory"));
    stackDirectory.setPlaceholderText("Choose a new directory for this stack");
    stackFormat.addItems(formatNames);
    form->addRow("File format", &stackFormat);
    form->addRow(label("Masters contain linear float32 data. Statistical rejection requires at least ten "
                       "valid exposures at each pixel."));
    auto *integration = new QPushButton("Integration settings…");
    form->addRow(integration);
    editableWidgets << output;
    connect(integration, &QPushButton::clicked, this, [this] { settings(Integration); });
    connect(&stackDirectory, &QLineEdit::textChanged, this, [this] { refreshPages(); });
    connect(&stackFormat, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (!project || model.busy || index < 0)
            return;
        auto s = project->settings();
        s.format = ss::Format(index);
        project->settings(s);
        updateReadiness();
    });
    layout->addWidget(output);
    return page;
}

QWidget *Window::buildResults() {
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(16);
    auto *split = new QSplitter;
    auto *left = new QWidget;
    auto *leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    resultList.setMinimumWidth(220);
    leftLayout->addWidget(&resultList, 1);
    auto *folder = new QPushButton(icon("folder-open"), "Open folder");
    leftLayout->addWidget(folder);
    importMasterButton.setText("Import calibration masters");
    leftLayout->addWidget(&importMasterButton);
    importMasterButton.hide();
    connect(folder, &QPushButton::clicked, this, [this] {
        int row = resultList.currentRow();
        if (row >= 0 && row < products.size())
            QDesktopServices::openUrl(
                QUrl::fromLocalFile(QFileInfo(products[row].toObject()["path"].toString()).absolutePath()));
    });
    connect(&importMasterButton, &QPushButton::clicked, this, [this] {
        QStringList paths;
        for (const auto &value : products) {
            auto row = value.toObject();
            if (row.contains("kind"))
                paths << row["path"].toString();
        }
        if (!paths.empty())
            start("import", paths);
    });
    editableWidgets << &importMasterButton;
    auto *right = new QWidget;
    auto *rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    resultDetails.setWordWrap(true);
    resultDetails.setTextFormat(Qt::PlainText);
    rightLayout->addWidget(&resultDetails);
    resultView.setMinimumHeight(80);
    rightLayout->addWidget(&resultView, 1);
    split->addWidget(left);
    split->addWidget(right);
    split->setStretchFactor(0, 2);
    split->setStretchFactor(1, 8);
    layout->addWidget(split, 1);
    connect(&resultList, &QListWidget::currentRowChanged, this, [this] { showResult(); });
    auto *output = new QGroupBox("Export completed light masters");
    auto *form = new QFormLayout(output);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->addRow("Export directory", directoryField(exportDirectory, this, "Choose the export directory"));
    exportFormat.addItems(formatNames);
    form->addRow("File format", &exportFormat);
    form->addRow(label("Exports all completed light masters and their available diagnostic maps. Display "
                       "stretching does not change exported data."));
    editableWidgets << output;
    layout->addWidget(output);
    return page;
}

void Window::navigate(Stage stage) {
    if (!project || stage < Import || stage > Results)
        return;
    if (blinkButton && pages->currentIndex() != stage)
        blinkButton->setChecked(false);
    pages->setCurrentIndex(stage);
    QSignalBlocker blocker(navigation);
    navigation.setCurrentRow(stage);
    pageTitle.setText(stageNames[stage]);
    pageDescription.setText(stageDescriptions[stage]);
    if (stage == Review && !table.currentIndex().isValid() && proxy.rowCount()) {
        table.selectRow(0);
        table.setCurrentIndex(proxy.index(0, 0));
    }
    if (stage == Results)
        refreshResults();
    refreshPages();
}

void Window::runPrimary() {
    if (!project || model.busy)
        return;
    switch (pages->currentIndex()) {
    case Import:
        navigate(Calibration);
        break;
    case Calibration:
        readiness.lights ? prepare() : createMasters();
        break;
    case Review:
        navigate(Stack);
        break;
    case Stack:
        if (!readiness.canStack || stackDirectory.text().trimmed().isEmpty())
            return;
        {
            auto target = stackDirectory.text().trimmed();
            bool resume = readiness.resumable &&
                          QFileInfo(target).absoluteFilePath() == QFileInfo(lastOutput).absoluteFilePath();
            lastOutput = target;
            project->record("output", {{"directory", lastOutput}});
            start(resume ? "resume" : "stack", {lastOutput});
        }
        break;
    case Results:
        exportResults();
        break;
    }
}

void Window::createProject() {
    if (model.busy)
        return;
    auto path =
        QFileDialog::getSaveFileName(this, "Create Stellastack project", {}, "Stellastack (*.stella)");
    if (path.isEmpty())
        return;
    if (!path.endsWith(".stella"))
        path += ".stella";
    try {
        if (ss::fs::exists(path.toStdString()))
            throw ss::Error("Choose a new filename for this project.");
        {
            ss::Project fresh(path.toStdString(), true);
        }
        open(path);
    } catch (const std::exception &e) {
        error(e.what());
    }
}

void Window::chooseProject() {
    if (model.busy)
        return;
    auto path = QFileDialog::getOpenFileName(this, "Open Stellastack project", {}, "Stellastack (*.stella)");
    if (!path.isEmpty())
        open(path);
}

void Window::chooseImport(bool folder) {
    if (!project || model.busy)
        return;
    QStringList paths;
    if (folder) {
        auto path = QFileDialog::getExistingDirectory(this, "Add exposures from a folder");
        if (!path.isEmpty())
            paths << path;
    } else
        paths = QFileDialog::getOpenFileNames(this, "Add exposures", {},
                                              "Astro images (*.fits *.fit *.fts *.fz *.xisf);;All files (*)");
    if (!paths.empty())
        start("import", paths);
}

void Window::updateRecentProjects(const QString &path) {
    auto recent = preferences.value("recentProjects").toStringList();
    if (!path.isEmpty()) {
        auto absolute = QFileInfo(path).absoluteFilePath();
        recent.removeAll(absolute);
        recent.prepend(absolute);
    }
    recent.removeIf([](const QString &entry) { return !QFileInfo::exists(entry); });
    recent = recent.mid(0, 8);
    preferences.setValue("recentProjects", recent);
    recentProjects.clear();
    for (const auto &entry : recent) {
        auto *item = new QListWidgetItem(icon("folder-open"), QFileInfo(entry).completeBaseName());
        item->setData(Qt::UserRole, entry);
        item->setToolTip(entry);
        recentProjects.addItem(item);
    }
    if (recent.empty()) {
        auto *item = new QListWidgetItem("Your recent projects will appear here.");
        item->setFlags(Qt::ItemIsEnabled);
        recentProjects.addItem(item);
    }
}

void Window::updateFilters() {
    QSet<QString> bands, nights;
    for (const auto &f : model.frames) {
        if (!f.filter.empty())
            bands.insert(q(f.filter));
        if (!f.session.empty())
            nights.insert(q(f.session));
    }
    auto populate = [](QComboBox &combo, const QSet<QString> &values, const QString &all) {
        auto previous = combo.currentData();
        QSignalBlocker blocker(combo);
        auto sorted = values.values();
        sorted.sort();
        combo.clear();
        combo.addItem(all, "");
        for (const auto &value : sorted)
            combo.addItem(value, value);
        combo.setCurrentIndex(std::max(0, combo.findData(previous)));
    };
    populate(filter, bands, "All filters");
    populate(nightFilter, nights, "All nights");
    proxy.band = filter.currentData().toString();
    proxy.night = nightFilter.currentData().toString();
    proxy.refresh();
}

void Window::refreshPages() {
    if (!project)
        return;
    auto stage = pages->currentIndex();
    const bool idle = !model.busy;
    bool hasCalibrationFrames = std::any_of(model.frames.begin(), model.frames.end(), [](const auto &frame) {
        return frame.kind != "light" && frame.kind != "unknown" && !frame.master && frame.selection >= 0;
    });
    masterButton.setEnabled(idle && hasCalibrationFrames);
    QString hint;
    primary.setEnabled(idle);
    if (stage == Import) {
        primary.setText("Review calibration");
        primary.setEnabled(idle && !model.frames.empty() && !readiness.unknown);
        hint = model.frames.empty()
                   ? "Start by adding your lights and calibration frames. Your original files stay untouched."
               : readiness.unknown
                   ? QString("%1 frames need a type. Edit their metadata or exclude them before processing.")
                         .arg(readiness.unknown)
                   : "Next: check the calibration assignments for your imported frames.";
    } else if (stage == Calibration) {
        primary.setText(readiness.lights ? "Prepare frames" : "Create masters…");
        primary.setEnabled(
            idle && (readiness.lights ? readiness.canPrepare && !preparedDirectory.text().trimmed().isEmpty()
                                      : hasCalibrationFrames));
        hint =
            readiness.canPrepare ? "Prepare frames calibrates your lights, then measures stars and aligns "
                                   "exposures for review."
            : !readiness.lights && !model.frames.empty()
                ? "Calibration-only project: create reusable masters from your included calibration frames."
                : joined(readiness.prepareBlockers).toHtmlEscaped().replace("\n", "<br>") +
                      " <a href=\"0\">Check imported frames</a> · <a href=\"registration\">Calibration "
                      "options</a> · <a href=\"resources\">Resources</a>";
        if (readiness.lights && preparedDirectory.text().trimmed().isEmpty())
            hint = "Choose a directory for prepared light images below before preparing frames.";
    } else if (stage == Review) {
        primary.setText("Continue to stack");
        primary.setEnabled(idle && readiness.canStack);
        hint = readiness.canStack
                   ? "Inspect the frames, adjust your selection, then continue to the stack summary."
                   : "Some included frames are not ready. Check their status or <a href=\"1\">prepare "
                     "frames</a> before stacking.";
    } else if (stage == Stack) {
        bool resume = readiness.resumable && !lastOutput.isEmpty() &&
                      QFileInfo(stackDirectory.text().trimmed()).absoluteFilePath() ==
                          QFileInfo(lastOutput).absoluteFilePath();
        primary.setText(resume ? "Resume stack" : "Stack frames");
        bool pathOk = !stackDirectory.text().trimmed().isEmpty();
        QFileInfo path(stackDirectory.text().trimmed());
        if (path.exists())
            pathOk = path.isDir() && path.isWritable();
        else {
            QFileInfo ancestor(path.absolutePath());
            while (!ancestor.exists()) {
                auto parent = ancestor.absoluteDir();
                if (parent.absolutePath() == ancestor.absoluteFilePath())
                    break;
                ancestor.setFile(parent.absolutePath());
            }
            pathOk &= ancestor.isDir() && ancestor.isWritable();
        }
        primary.setEnabled(idle && readiness.canStack && pathOk);
        hint = !readiness.canStack ? joined(readiness.stackBlockers).toHtmlEscaped().replace("\n", "<br>") +
                                         " <a href=\"2\">Review frames</a> · <a href=\"1\">Prepare frames</a>"
               : !pathOk ? "Choose a writable output directory before stacking."
                         : "Ready to stack. Outputs are verified before publication; existing files are "
                           "never silently overwritten.";
    } else {
        primary.setText("Export masters");
        primary.setEnabled(idle && !project->record("results")["masters"].toArray().empty());
        hint = products.empty()
                   ? "Completed masters will appear here after stacking or creating calibration masters."
               : !readiness.resultsCurrent && !project->record("results")["masters"].toArray().empty()
                   ? "These are saved masters from a previous stack. Current selection or settings may "
                     "differ. <a href=\"3\">Review the next stack</a>"
                   : "Display stretching is for inspection only. Your masters retain their linear data.";
    }
    if (model.busy)
        hint = "Processing is running. You can inspect other pages; edits are available when the operation "
               "finishes.";
    pageHint.setText(hint);
    QStringList state{
        model.frames.empty()
            ? "Add frames"
            : QString("%1 frames%2").arg(model.frames.size()).arg(readiness.unknown ? " · check types" : ""),
        !readiness.lights      ? "Calibration tools"
        : readiness.prepared   ? "Prepared"
        : readiness.canPrepare ? "Ready to prepare"
                               : "Needs attention",
        readiness.canStack ? "Ready to review" : "Needs preparation",
        readiness.resultsCurrent ? "Completed"
        : readiness.canStack     ? "Ready to stack"
                                 : "Not ready",
        QString("%1 saved masters").arg(products.size())};
    for (int i = 0; i < navigation.count(); ++i) {
        navigation.item(i)->setText(stageNames[i] + "\n" + state[i]);
        navigation.item(i)->setToolTip(stageDescriptions[i]);
    }
}

QTableView &Window::activeTable() {
    return pages->currentIndex() == Import ? importTable : table;
}

void Window::showDetails() {
    auto index = proxy.mapToSource(table.currentIndex());
    if (!index.isValid()) {
        frameDetails.setText("Select a frame to see its details.");
        return;
    }
    const auto &f = model.frames[size_t(index.row())];
    QStringList details{
        q(f.path.filename().string()),
        model.state(f),
        model.decisions.contains(f.id) ? q(model.decisions[f.id].reason) : QString(),
        QString("%1 · %2 · %3").arg(q(f.kind), q(f.filter), q(f.session)),
        QString("%1 × %2 · %3 channels · %4 s · %5 °C")
            .arg(f.descriptor.width)
            .arg(f.descriptor.height)
            .arg(f.descriptor.channels)
            .arg(value(ss::number(f.descriptor.header, "EXPTIME")))
            .arg(value(ss::number(f.descriptor.header, "CCD-TEMP"))),
        QString("FWHM %1 px · HFR %2 px · Eccentricity %3 · Stars %4 · RMS %5 px")
            .arg(value(f.metrics.fwhm), value(f.metrics.hfr), value(f.metrics.eccentricity))
            .arg(f.metrics.stars)
            .arg(value(f.metrics.residual)),
        QString("Background %1 · Noise %2 · Transparency %3")
            .arg(value(f.metrics.background), value(f.metrics.noise), value(f.metrics.transparency)),
        QString("Camera %1 · Gain %2 · Offset %3 · CFA %4")
            .arg(q(ss::str(f.descriptor.header, "INSTRUME")), value(ss::number(f.descriptor.header, "GAIN")),
                 value(ss::number(f.descriptor.header, "OFFSET")),
                 f.descriptor.cfa.empty() ? "Mono / RGB" : q(f.descriptor.cfa)),
        f.master ? QString("Imported master · %1")
                       .arg(f.biasSubtracted ? "bias already removed" : "bias not marked as removed")
                 : QString(),
        q(f.path.string())};
    frameDetails.setText(details.join("\n"));
    plot.current = f.id;
    plot.update();
}

void Window::saveUiState() {
    preferences.setValue("window/geometry", saveGeometry());
    preferences.setValue("review/tableHeader", table.horizontalHeader()->saveState());
    preferences.setValue("review/splitter", reviewSplit->saveState());
    preferences.setValue("review/blinkInterval", blinkInterval.value());
}
} // namespace ss::gui
