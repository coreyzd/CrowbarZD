// SPDX-License-Identifier: CC-BY-SA-3.0
#include <QApplication>

#include "MainWindow.hpp"

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QApplication::setOrganizationName("CrowbarZD");
  QApplication::setApplicationName("CrowbarZD");
  QApplication::setApplicationVersion(CZD_VERSION);
  MainWindow window;
  window.show();
  const QStringList args = QApplication::arguments();
  if (args.size() > 1) window.openPath(args.at(1));
  return QApplication::exec();
}
