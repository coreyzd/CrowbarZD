// SPDX-License-Identifier: CC-BY-SA-3.0
// CrowbarZD main window: Compile and Decompile tabs over a shared log.
#pragma once

#include <QMainWindow>
#include <QProcess>
#include <QStringList>

#include <atomic>
#include <memory>

#include "czd/batch.hpp"
#include "czd/compile.hpp"

class QCheckBox;
class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTabWidget;

class MainWindow : public QMainWindow {
  Q_OBJECT
 public:
  explicit MainWindow(QWidget* parent = nullptr);
  ~MainWindow() override;
  // Routes a .qc to the Compile tab, a .mdl to the Decompile tab and a folder to the tab that is showing.
  void openPath(const QString& path);

 protected:
  void closeEvent(QCloseEvent* event) override;
  void dragEnterEvent(QDragEnterEvent* event) override;
  void dropEvent(QDropEvent* event) override;

 private:
  QWidget* buildSetup();
  QWidget* buildCompileTab();
  QWidget* buildDecompileTab();
  void loadSettings();
  void saveSettings();
  void log(const QString& line, bool problem = false);
  void logLine(std::string_view line);
  void setBusy(bool busy);
  void showAbout();

  // Compile: the QC files of one run are queued and compiled one after another.
  czd::CompileOptions compileOptions() const;
  void outputModeChanged();
  void startCompile();
  void compileNext();
  void readCompileOutput();
  void compileFinished(int exitCode, QProcess::ExitStatus status);
  void stopCompile();

  // Decompile: runs on a worker thread; `cancelDecompile_` stops it between models.
  void setMdlInput(const QString& path);
  void suggestOutDir();
  void startDecompile();

  QTabWidget* tabs_ = nullptr;
  QLineEdit* studiomdl_ = nullptr;
  QComboBox* gameDir_ = nullptr;
  QPlainTextEdit* log_ = nullptr;

  QComboBox* qcMode_ = nullptr;
  QLineEdit* qc_ = nullptr;
  QComboBox* outputMode_ = nullptr;
  QLineEdit* outputFolder_ = nullptr;
  QPushButton* outputBrowse_ = nullptr;
  QCheckBox* noP4_ = nullptr;
  QCheckBox* verbose_ = nullptr;
  QLineEdit* extraArgs_ = nullptr;
  QPushButton* compile_ = nullptr;
  QPushButton* stopCompile_ = nullptr;
  QString workFolder_, subfolder_ = QStringLiteral("compiled");
  int shownOutputMode_ = 0;
  QProcess* process_ = nullptr;
  QByteArray pending_;
  std::unique_ptr<czd::OutputRedirect> redirect_;
  czd::CompileOptions runOptions_;  // settings of the run in progress
  QStringList queue_;               // QC files still to compile
  int compileTotal_ = 0, compileFailed_ = 0;
  bool sawError_ = false;

  QComboBox* mdlMode_ = nullptr;
  QLineEdit* mdl_ = nullptr;
  QLineEdit* outDir_ = nullptr;
  QCheckBox* defineBones_ = nullptr;
  QCheckBox* lods_ = nullptr;
  QCheckBox* anims_ = nullptr;
  QCheckBox* physics_ = nullptr;
  QCheckBox* procedural_ = nullptr;
  QCheckBox* flexes_ = nullptr;
  QCheckBox* folderPerModel_ = nullptr;
  QPushButton* decompile_ = nullptr;
  QPushButton* stopDecompile_ = nullptr;
  QPushButton* openOut_ = nullptr;
  bool outDirEdited_ = false;
  bool decompiling_ = false;
  std::atomic<bool> cancelDecompile_{false};
};
