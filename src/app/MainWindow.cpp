// SPDX-License-Identifier: CC-BY-SA-3.0
#include "MainWindow.hpp"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QTabWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include "czd/decompile.hpp"

namespace {

std::filesystem::path toPath(const QString& s) { return std::filesystem::path(s.toStdString()); }
QString fromPath(const std::filesystem::path& p) { return QString::fromStdString(p.string()); }

QWidget* withBrowse(QWidget* field, QWidget* parent, const std::function<void()>& browse) {
  auto* row = new QWidget(parent);
  auto* layout = new QHBoxLayout(row);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(field, 1);
  auto* button = new QPushButton(QObject::tr("Browse…"), row);
  QObject::connect(button, &QPushButton::clicked, row, browse);
  layout->addWidget(button);
  return row;
}

// The File / Folder / Folder and subfolders choice in front of an input path.
QComboBox* inputModeBox(QWidget* parent) {
  auto* box = new QComboBox(parent);
  box->addItems({QObject::tr("File"), QObject::tr("Folder"), QObject::tr("Folder and subfolders")});
  return box;
}

// A row holding the mode box, the path and a Browse button that asks for a file or a folder to suit the mode.
QWidget* inputRow(QWidget* parent, QComboBox* mode, QLineEdit* path, const QString& fileTitle, const QString& filter, const std::function<void(const QString&)>& chosen,
                  const std::function<QString()>& startDir) {
  auto* row = new QWidget(parent);
  auto* layout = new QHBoxLayout(row);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(mode);
  layout->addWidget(path, 1);
  auto* button = new QPushButton(QObject::tr("Browse…"), row);
  QObject::connect(button, &QPushButton::clicked, row, [=] {
    const QString picked = mode->currentIndex() == 0 ? QFileDialog::getOpenFileName(parent, fileTitle, startDir(), filter)
                                                     : QFileDialog::getExistingDirectory(parent, QObject::tr("Choose a folder"), startDir());
    if (!picked.isEmpty()) chosen(picked);
  });
  layout->addWidget(button);
  return row;
}

// The folder a path points into: itself if it is a folder, else the one that holds it.
QString folderOf(const QString& path) {
  const QFileInfo info(path);
  return info.isDir() ? info.absoluteFilePath() : info.absolutePath();
}

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
  setWindowTitle(tr("CrowbarZD"));
  setAcceptDrops(true);
  resize(940, 720);

  tabs_ = new QTabWidget(this);
  tabs_->addTab(buildCompileTab(), tr("Compile"));
  tabs_->addTab(buildDecompileTab(), tr("Decompile"));

  auto* top = new QWidget(this);
  auto* topLayout = new QVBoxLayout(top);
  topLayout->addWidget(buildSetup());
  topLayout->addWidget(tabs_);

  auto* bottom = new QWidget(this);
  auto* bottomLayout = new QVBoxLayout(bottom);
  log_ = new QPlainTextEdit(bottom);
  log_->setReadOnly(true);
  log_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
  log_->setMaximumBlockCount(50000);
  log_->setPlaceholderText(tr("Compiler and decompiler output appears here."));
  auto* clear = new QPushButton(tr("Clear log"), bottom);
  connect(clear, &QPushButton::clicked, log_, &QPlainTextEdit::clear);
  bottomLayout->addWidget(log_, 1);
  bottomLayout->addWidget(clear, 0, Qt::AlignRight);

  auto* splitter = new QSplitter(Qt::Vertical, this);
  splitter->addWidget(top);
  splitter->addWidget(bottom);
  splitter->setStretchFactor(1, 1);
  setCentralWidget(splitter);

  auto* help = menuBar()->addMenu(tr("&Help"));
  help->addAction(tr("&About CrowbarZD"), this, &MainWindow::showAbout);

  process_ = new QProcess(this);
  process_->setProcessChannelMode(QProcess::MergedChannels);
  connect(process_, &QProcess::readyRead, this, &MainWindow::readCompileOutput);
  connect(process_, &QProcess::finished, this, &MainWindow::compileFinished);
  connect(process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
    if (error != QProcess::FailedToStart) return;
    log(tr("Could not start %1: %2").arg(process_->program(), process_->errorString()), true);
    if (redirect_) redirect_->finish(false, {});
    redirect_.reset();
    queue_.clear();  // the compiler itself is the problem; the other files would fail the same way
    setBusy(false);
  });

  loadSettings();
  setBusy(false);
}

MainWindow::~MainWindow() = default;

QWidget* MainWindow::buildSetup() {
  auto* box = new QGroupBox(tr("Setup"), this);
  auto* form = new QFormLayout(box);
  studiomdl_ = new QLineEdit(box);
  studiomdl_->setPlaceholderText(tr("Path to studiomdl++ (or another studiomdl build)"));
  form->addRow(tr("Model compiler:"), withBrowse(studiomdl_, box, [this] {
    const QString f = QFileDialog::getOpenFileName(this, tr("Choose the model compiler"), QFileInfo(studiomdl_->text()).path());
    if (!f.isEmpty()) studiomdl_->setText(f);
  }));
  gameDir_ = new QComboBox(box);
  gameDir_->setEditable(true);
  gameDir_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  gameDir_->lineEdit()->setPlaceholderText(tr("Folder that contains gameinfo.txt"));
  form->addRow(tr("Game folder:"), withBrowse(gameDir_, box, [this] {
    const QString d = QFileDialog::getExistingDirectory(this, tr("Choose the game folder (it contains gameinfo.txt)"), gameDir_->currentText());
    if (!d.isEmpty()) gameDir_->setEditText(d);
  }));
  return box;
}

QWidget* MainWindow::buildCompileTab() {
  auto* page = new QWidget(this);
  auto* form = new QFormLayout(page);

  qcMode_ = inputModeBox(page);
  qcMode_->setObjectName("qcMode");
  qc_ = new QLineEdit(page);
  qc_->setPlaceholderText(tr("QC file, or a folder of QC files (you can also drop one on the window)"));
  form->addRow(tr("QC input:"), inputRow(page, qcMode_, qc_, tr("Choose a QC file"), tr("QC files (*.qc);;All files (*)"), [this](const QString& p) { qc_->setText(p); },
                                         [this] { return qc_->text().isEmpty() ? QString() : folderOf(qc_->text()); }));

  outputMode_ = new QComboBox(page);
  outputMode_->setObjectName("outputMode");
  outputMode_->addItems({tr("Game's \"models\" folder"), tr("Work folder"), tr("Subfolder (of QC input)")});
  outputFolder_ = new QLineEdit(page);
  outputBrowse_ = new QPushButton(tr("Browse…"), page);
  auto* goTo = new QPushButton(tr("Go to"), page);
  goTo->setToolTip(tr("Open the folder the compiled model is written to."));
  connect(outputMode_, &QComboBox::currentIndexChanged, this, &MainWindow::outputModeChanged);
  connect(outputBrowse_, &QPushButton::clicked, this, [this] {
    const QString d = QFileDialog::getExistingDirectory(this, tr("Choose the work folder"), outputFolder_->text());
    if (!d.isEmpty()) outputFolder_->setText(d);
  });
  connect(goTo, &QPushButton::clicked, this, [this] {
    czd::CompileOptions opt = compileOptions();
    if (qcMode_->currentIndex() != 0) opt.qc = toPath(folderOf(qc_->text().trimmed())) / "x.qc";  // only its folder matters here
    std::filesystem::path dir = czd::outputRoot(opt);
    if (dir.empty()) dir = opt.gameDir / "models";
    std::error_code ec;
    if (std::filesystem::is_directory(dir, ec)) QDesktopServices::openUrl(QUrl::fromLocalFile(fromPath(dir)));
    else log(tr("The output folder does not exist yet: %1").arg(fromPath(dir)), true);
  });
  auto* outputRow = new QHBoxLayout;
  outputRow->addWidget(outputMode_);
  outputRow->addWidget(outputFolder_, 1);
  outputRow->addWidget(outputBrowse_);
  outputRow->addWidget(goTo);
  form->addRow(tr("Output to:"), outputRow);

  noP4_ = new QCheckBox(tr("Skip Perforce (-nop4)"), page);
  noP4_->setChecked(true);
  verbose_ = new QCheckBox(tr("Verbose output (-verbose)"), page);
  auto* checks = new QHBoxLayout;
  checks->addWidget(noP4_);
  checks->addWidget(verbose_);
  checks->addStretch(1);
  form->addRow(tr("Options:"), checks);
  extraArgs_ = new QLineEdit(page);
  extraArgs_->setPlaceholderText(tr("Extra compiler arguments, for example -fastbuild"));
  form->addRow(tr("Extra arguments:"), extraArgs_);

  compile_ = new QPushButton(tr("Compile"), page);
  compile_->setDefault(true);
  stopCompile_ = new QPushButton(tr("Stop"), page);
  connect(compile_, &QPushButton::clicked, this, &MainWindow::startCompile);
  connect(stopCompile_, &QPushButton::clicked, this, &MainWindow::stopCompile);
  auto* buttons = new QHBoxLayout;
  buttons->addWidget(compile_);
  buttons->addWidget(stopCompile_);
  buttons->addStretch(1);
  form->addRow(QString(), buttons);
  return page;
}

QWidget* MainWindow::buildDecompileTab() {
  auto* page = new QWidget(this);
  auto* form = new QFormLayout(page);

  mdlMode_ = inputModeBox(page);
  mdlMode_->setObjectName("mdlMode");
  mdl_ = new QLineEdit(page);
  mdl_->setPlaceholderText(tr("MDL file, or a folder of models (you can also drop one on the window)"));
  connect(mdl_, &QLineEdit::editingFinished, this, &MainWindow::suggestOutDir);
  connect(mdlMode_, &QComboBox::currentIndexChanged, this, &MainWindow::suggestOutDir);
  form->addRow(tr("MDL input:"), inputRow(page, mdlMode_, mdl_, tr("Choose an MDL file"), tr("Source models (*.mdl);;All files (*)"), [this](const QString& p) { setMdlInput(p); }, [this] {
    if (!mdl_->text().isEmpty()) return folderOf(mdl_->text());
    return gameDir_->currentText().isEmpty() ? QString() : gameDir_->currentText() + "/models";
  }));

  outDir_ = new QLineEdit(page);
  outDir_->setPlaceholderText(tr("Folder for the QC and SMD files"));
  connect(outDir_, &QLineEdit::textEdited, this, [this] { outDirEdited_ = true; });
  form->addRow(tr("Output to:"), withBrowse(outDir_, page, [this] {
    const QString d = QFileDialog::getExistingDirectory(this, tr("Choose the output folder"), outDir_->text());
    if (!d.isEmpty()) { outDir_->setText(d); outDirEdited_ = true; }
  }));

  defineBones_ = new QCheckBox(tr("$definebone lines"), page);
  defineBones_->setToolTip(tr("Keeps the skeleton exactly as it is when the model is compiled again."));
  lods_ = new QCheckBox(tr("LOD meshes"), page);
  anims_ = new QCheckBox(tr("Animations"), page);
  physics_ = new QCheckBox(tr("Collision model"), page);
  procedural_ = new QCheckBox(tr("Procedural bones (VRD)"), page);
  flexes_ = new QCheckBox(tr("Eyes and flexes (VTA)"), page);
  auto* checks = new QHBoxLayout;
  for (QCheckBox* c : {defineBones_, lods_, anims_, physics_, procedural_, flexes_}) {
    c->setChecked(true);
    checks->addWidget(c);
  }
  checks->addStretch(1);
  form->addRow(tr("Write:"), checks);

  folderPerModel_ = new QCheckBox(tr("Folder for each model"), page);
  folderPerModel_->setChecked(true);
  folderPerModel_->setToolTip(tr("Puts each model's files in a folder named after the model, inside the output folder.\n"
                                 "Without it, models decompiled together share one folder and mesh files get the model's name in front."));
  form->addRow(tr("Options:"), folderPerModel_);

  decompile_ = new QPushButton(tr("Decompile"), page);
  stopDecompile_ = new QPushButton(tr("Stop"), page);
  openOut_ = new QPushButton(tr("Go to"), page);
  openOut_->setToolTip(tr("Open the output folder."));
  connect(decompile_, &QPushButton::clicked, this, &MainWindow::startDecompile);
  connect(stopDecompile_, &QPushButton::clicked, this, [this] {
    cancelDecompile_ = true;
    stopDecompile_->setEnabled(false);
  });
  connect(openOut_, &QPushButton::clicked, this, [this] {
    if (QDir(outDir_->text()).exists()) QDesktopServices::openUrl(QUrl::fromLocalFile(outDir_->text()));
    else log(tr("The output folder does not exist yet: %1").arg(outDir_->text()), true);
  });
  auto* buttons = new QHBoxLayout;
  buttons->addWidget(decompile_);
  buttons->addWidget(stopDecompile_);
  buttons->addWidget(openOut_);
  buttons->addStretch(1);
  form->addRow(QString(), buttons);
  return page;
}

void MainWindow::loadSettings() {
  QSettings s;
  restoreGeometry(s.value("geometry").toByteArray());
  QString compiler = s.value("studiomdl").toString();
  if (compiler.isEmpty()) compiler = fromPath(czd::findStudiomdl());
  studiomdl_->setText(compiler);
  for (const auto& dir : czd::findGameDirs()) gameDir_->addItem(fromPath(dir));
  gameDir_->setEditText(s.value("gameDir").toString());

  qc_->setText(s.value("qc").toString());
  qcMode_->setCurrentIndex(qBound(0, s.value("qcMode", 0).toInt(), 2));
  noP4_->setChecked(s.value("noP4", true).toBool());
  verbose_->setChecked(s.value("verbose", false).toBool());
  extraArgs_->setText(s.value("extraArgs").toString());
  workFolder_ = s.value("workFolder").toString();
  subfolder_ = s.value("subfolder", QStringLiteral("compiled")).toString();
  outputMode_->setCurrentIndex(qBound(0, s.value("outputMode", 0).toInt(), 2));
  outputModeChanged();

  outDirEdited_ = true;  // keep the saved folder while the saved input is put back
  mdl_->setText(s.value("mdl").toString());
  mdlMode_->setCurrentIndex(qBound(0, s.value("mdlMode", 0).toInt(), 2));
  outDir_->setText(s.value("outDir").toString());
  outDirEdited_ = s.value("outDirEdited", false).toBool();
  defineBones_->setChecked(s.value("defineBones", true).toBool());
  lods_->setChecked(s.value("lods", true).toBool());
  anims_->setChecked(s.value("anims", true).toBool());
  physics_->setChecked(s.value("physics", true).toBool());
  procedural_->setChecked(s.value("procedural", true).toBool());
  flexes_->setChecked(s.value("flexes", true).toBool());
  folderPerModel_->setChecked(s.value("folderPerModel", true).toBool());
  tabs_->setCurrentIndex(s.value("tab", 0).toInt());
}

void MainWindow::saveSettings() {
  QSettings s;
  s.setValue("geometry", saveGeometry());
  s.setValue("studiomdl", studiomdl_->text());
  s.setValue("gameDir", gameDir_->currentText());
  s.setValue("qc", qc_->text());
  s.setValue("qcMode", qcMode_->currentIndex());
  s.setValue("noP4", noP4_->isChecked());
  s.setValue("verbose", verbose_->isChecked());
  s.setValue("extraArgs", extraArgs_->text());
  if (shownOutputMode_ == 1) workFolder_ = outputFolder_->text();
  if (shownOutputMode_ == 2) subfolder_ = outputFolder_->text();
  s.setValue("workFolder", workFolder_);
  s.setValue("subfolder", subfolder_);
  s.setValue("outputMode", outputMode_->currentIndex());
  s.setValue("mdl", mdl_->text());
  s.setValue("mdlMode", mdlMode_->currentIndex());
  s.setValue("outDir", outDir_->text());
  s.setValue("outDirEdited", outDirEdited_);
  s.setValue("defineBones", defineBones_->isChecked());
  s.setValue("lods", lods_->isChecked());
  s.setValue("anims", anims_->isChecked());
  s.setValue("physics", physics_->isChecked());
  s.setValue("procedural", procedural_->isChecked());
  s.setValue("flexes", flexes_->isChecked());
  s.setValue("folderPerModel", folderPerModel_->isChecked());
  s.setValue("tab", tabs_->currentIndex());
}

void MainWindow::closeEvent(QCloseEvent* event) {
  saveSettings();
  queue_.clear();
  if (process_->state() != QProcess::NotRunning) {
    process_->kill();
    process_->waitForFinished(1000);
  }
  // The decompile worker posts its log lines to this window, so let it wind down first.
  cancelDecompile_ = true;
  while (decompiling_) QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  event->accept();
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
  if (event->mimeData()->hasUrls()) event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* event) {
  for (const QUrl& url : event->mimeData()->urls())
    if (url.isLocalFile()) { openPath(url.toLocalFile()); break; }
}

void MainWindow::openPath(const QString& path) {
  const QFileInfo info(path);
  const QString suffix = info.suffix().toLower();
  if (info.isDir()) {  // a folder goes to the tab that is showing
    if (tabs_->currentIndex() == 0) {
      if (qcMode_->currentIndex() == 0) qcMode_->setCurrentIndex(1);
      qc_->setText(path);
    } else {
      if (mdlMode_->currentIndex() == 0) mdlMode_->setCurrentIndex(1);
      setMdlInput(path);
    }
  } else if (suffix == "qc") {
    qcMode_->setCurrentIndex(0);
    qc_->setText(path);
    tabs_->setCurrentIndex(0);
  } else if (suffix == "mdl") {
    mdlMode_->setCurrentIndex(0);
    setMdlInput(path);
    tabs_->setCurrentIndex(1);
  } else {
    log(tr("Not a .qc file, a .mdl file or a folder: %1").arg(path), true);
  }
}

void MainWindow::log(const QString& line, bool problem) {
  if (problem) log_->appendHtml(QStringLiteral("<span style=\"color:#c0392b\">%1</span>").arg(line.toHtmlEscaped()));
  else log_->appendPlainText(line);
}

void MainWindow::logLine(std::string_view line) {
  const QString text = QString::fromUtf8(line.data(), static_cast<qsizetype>(line.size()));
  log(text, text.startsWith("WARNING") || text.startsWith("ERROR"));
}

void MainWindow::setBusy(bool busy) {
  compile_->setEnabled(!busy);
  decompile_->setEnabled(!busy);
  stopCompile_->setEnabled(busy && !decompiling_);
  stopDecompile_->setEnabled(busy && decompiling_);
}

// ---- Compile -------------------------------------------------------------------

// The folder field means something different per output mode, so each mode keeps its own text.
void MainWindow::outputModeChanged() {
  if (shownOutputMode_ == 1) workFolder_ = outputFolder_->text();
  if (shownOutputMode_ == 2) subfolder_ = outputFolder_->text();
  shownOutputMode_ = outputMode_->currentIndex();
  outputFolder_->setEnabled(shownOutputMode_ != 0);
  outputBrowse_->setEnabled(shownOutputMode_ == 1);
  if (shownOutputMode_ == 0) {
    outputFolder_->clear();
    outputFolder_->setPlaceholderText(tr("<game folder>/models"));
  } else if (shownOutputMode_ == 1) {
    outputFolder_->setText(workFolder_);
    outputFolder_->setPlaceholderText(tr("Folder to move the compiled model into (a \"models\" folder is made inside it)"));
  } else {
    outputFolder_->setText(subfolder_);
    outputFolder_->setPlaceholderText(tr("Name of a folder next to the QC file"));
  }
}

czd::CompileOptions MainWindow::compileOptions() const {
  czd::CompileOptions opt;
  opt.studiomdl = toPath(studiomdl_->text().trimmed());
  opt.gameDir = toPath(gameDir_->currentText().trimmed());
  opt.qc = toPath(qc_->text().trimmed());
  opt.noP4 = noP4_->isChecked();
  opt.verbose = verbose_->isChecked();
  for (const QString& a : QProcess::splitCommand(extraArgs_->text())) opt.extraArgs.push_back(a.toStdString());
  opt.outputMode = static_cast<czd::OutputMode>(outputMode_->currentIndex());
  opt.outputFolder = toPath(outputFolder_->text().trimmed());
  return opt;
}

void MainWindow::startCompile() {
  runOptions_ = compileOptions();
  const auto mode = static_cast<czd::InputMode>(qcMode_->currentIndex());
  const auto files = czd::collectInputs(runOptions_.qc, mode, ".qc");
  if (files.empty()) {
    log(mode == czd::InputMode::File ? tr("ERROR: QC file not found: %1").arg(qc_->text().trimmed()) : tr("ERROR: no QC files found in %1").arg(folderOf(qc_->text().trimmed())), true);
    return;
  }
  queue_.clear();
  for (const auto& f : files) queue_ << fromPath(f);
  compileTotal_ = static_cast<int>(queue_.size());
  compileFailed_ = 0;
  if (compileTotal_ > 1) log(tr("—— Compiling %1 QC files ——").arg(compileTotal_));
  saveSettings();
  setBusy(true);
  compileNext();
}

void MainWindow::compileNext() {
  while (!queue_.isEmpty()) {
    runOptions_.qc = toPath(queue_.takeFirst());
    log(tr("—— Compile %1 ——").arg(fromPath(runOptions_.qc.filename())));
    const czd::Preflight pf = czd::preflight(runOptions_);
    for (const auto& w : pf.warnings) log(tr("WARNING: %1").arg(QString::fromStdString(w)), true);
    for (const auto& e : pf.errors) log(tr("ERROR: %1").arg(QString::fromStdString(e)), true);
    if (!pf.errors.empty()) {
      ++compileFailed_;
      continue;
    }
    QStringList args;
    for (const auto& a : czd::compileArgs(runOptions_)) args << QString::fromStdString(a);
    log(fromPath(runOptions_.studiomdl) + " " + args.join(' '));
    pending_.clear();
    sawError_ = false;
    redirect_ = std::make_unique<czd::OutputRedirect>(runOptions_);
    redirect_->begin([this](std::string_view line) { logLine(line); });
    process_->setWorkingDirectory(fromPath(runOptions_.qc.parent_path()));
    process_->start(fromPath(runOptions_.studiomdl), args);
    return;  // compileFinished() carries on with the queue
  }
  if (compileTotal_ > 1) log(tr("Compiled %1 of %2 QC files.").arg(compileTotal_ - compileFailed_).arg(compileTotal_), compileFailed_ > 0);
  setBusy(false);
}

void MainWindow::readCompileOutput() {
  pending_ += process_->readAll();
  for (qsizetype nl; (nl = pending_.indexOf('\n')) >= 0; pending_.remove(0, nl + 1)) {
    const QString line = QString::fromStdString(czd::stripAnsi(std::string_view(pending_.constData(), static_cast<size_t>(nl))));
    sawError_ = sawError_ || line.startsWith("ERROR");
    log(line, line.startsWith("WARNING") || line.startsWith("ERROR"));
  }
}

void MainWindow::compileFinished(int exitCode, QProcess::ExitStatus status) {
  readCompileOutput();
  if (!pending_.isEmpty()) log(QString::fromStdString(czd::stripAnsi(pending_.toStdString())));
  pending_.clear();
  const bool ok = status == QProcess::NormalExit && exitCode == 0 && !sawError_;
  if (redirect_) {
    redirect_->finish(ok, [this](std::string_view line) { logLine(line); });
    if (ok && !redirect_->modelFile().empty()) log(tr("Model: %1").arg(fromPath(redirect_->modelFile())));
    redirect_.reset();
  }
  if (!ok) ++compileFailed_;
  if (status == QProcess::CrashExit) log(tr("The compiler was stopped or crashed."), true);
  else if (exitCode != 0) log(tr("The compiler exited with code %1.").arg(exitCode), true);
  else if (sawError_) log(tr("The compiler reported an error; see above."), true);
  else log(tr("Compile finished."));
  // Back to the event loop first: starting the next process from inside this signal is not allowed.
  QTimer::singleShot(0, this, &MainWindow::compileNext);
}

void MainWindow::stopCompile() {
  if (!queue_.isEmpty()) {
    log(tr("Stopped; %1 QC file(s) were not compiled.").arg(queue_.size()), true);
    compileFailed_ += static_cast<int>(queue_.size());
    queue_.clear();
  }
  if (process_->state() == QProcess::NotRunning) return;
  process_->terminate();
  if (!process_->waitForFinished(2000)) process_->kill();
}

// ---- Decompile -----------------------------------------------------------------

void MainWindow::setMdlInput(const QString& path) {
  mdl_->setText(path);
  suggestOutDir();
}

// Until the user picks an output folder, offer "decompiled" next to the input.
void MainWindow::suggestOutDir() {
  if (mdl_->text().trimmed().isEmpty() || (outDirEdited_ && !outDir_->text().isEmpty())) return;
  const QString input = mdl_->text().trimmed();
  outDir_->setText((mdlMode_->currentIndex() == 0 ? QFileInfo(input).absolutePath() : folderOf(input)) + "/decompiled");
  outDirEdited_ = false;
}

void MainWindow::startDecompile() {
  const QString input = mdl_->text().trimmed(), out = outDir_->text().trimmed();
  if (input.isEmpty() || out.isEmpty()) {
    log(tr("ERROR: choose an MDL input and an output folder first."), true);
    return;
  }
  const auto mode = static_cast<czd::InputMode>(mdlMode_->currentIndex());
  const auto models = czd::collectInputs(toPath(input), mode, ".mdl");
  if (models.empty()) {
    log(mode == czd::InputMode::File ? tr("ERROR: MDL file not found: %1").arg(input) : tr("ERROR: no MDL files found in %1").arg(folderOf(input)), true);
    return;
  }
  czd::DecompileOptions opt;
  opt.defineBones = defineBones_->isChecked();
  opt.lods = lods_->isChecked();
  opt.animations = anims_->isChecked();
  opt.physics = physics_->isChecked();
  opt.proceduralBones = procedural_->isChecked();
  opt.flexes = flexes_->isChecked();
  const bool folderPerModel = folderPerModel_->isChecked();
  opt.prefixMeshNames = models.size() > 1 && !folderPerModel;  // shared folder: keep "ref.smd" files apart

  if (models.size() > 1) log(tr("—— Decompiling %1 models ——").arg(models.size()));
  saveSettings();
  cancelDecompile_ = false;
  decompiling_ = true;
  setBusy(true);

  auto* watcher = new QFutureWatcher<void>(this);
  connect(watcher, &QFutureWatcher<void>::finished, this, [this, watcher] {
    watcher->deleteLater();
    decompiling_ = false;
    setBusy(false);
  });
  watcher->setFuture(QtConcurrent::run([this, models, opt, mode, folderPerModel, inputPath = toPath(input), outRoot = toPath(out)] {
    // Log lines are handed to the window's thread; `post` keeps them in order.
    auto post = [this](const QString& text, bool problem) { QMetaObject::invokeMethod(this, [this, text, problem] { log(text, problem); }, Qt::QueuedConnection); };
    size_t done = 0, failed = 0;
    for (const auto& mdl : models) {
      if (cancelDecompile_) break;
      post(tr("—— Decompile %1 ——").arg(fromPath(mdl.filename())), false);
      try {
        czd::decompile(mdl, czd::decompileOutputDir(outRoot, inputPath, mode, mdl, folderPerModel), opt, [&post](std::string_view line) {
          const QString text = QString::fromUtf8(line.data(), static_cast<qsizetype>(line.size()));
          post(text, text.startsWith("WARNING"));
        });
      } catch (const std::exception& e) {
        post(tr("ERROR: %1: %2").arg(fromPath(mdl.filename()), QString::fromUtf8(e.what())), true);
        ++failed;
      }
      ++done;
    }
    if (cancelDecompile_ && done < models.size()) post(tr("Stopped; %1 model(s) were not decompiled.").arg(models.size() - done), true);
    if (models.size() > 1) post(tr("Decompiled %1 of %2 models.").arg(done - failed).arg(models.size()), failed > 0);
  }));
}

void MainWindow::showAbout() {
  QMessageBox::about(this, tr("About CrowbarZD"),
                     tr("<h3>CrowbarZD %1</h3>"
                        "<p>A Linux-native compiler front end and decompiler for Source engine models.</p>"
                        "<p>The decompiler is a C++ port of code from <b>Crowbar</b> by <b>ZeqMacaw</b> "
                        "(<a href=\"https://steamcommunity.com/groups/CrowbarTool\">steamcommunity.com/groups/CrowbarTool</a>). "
                        "CrowbarZD is a separate, modified work and is not the original Crowbar.</p>"
                        "<p>Modified by: CoreyZD</p>"
                        "<p>Licensed under the Creative Commons Attribution-ShareAlike 3.0 Unported License.</p>")
                         .arg(QStringLiteral(CZD_VERSION)));
}
