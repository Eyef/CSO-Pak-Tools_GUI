#include <QApplication>

#include "MainWindow.h"

int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	QApplication::setApplicationName("CSO Pak Browser");
	QApplication::setOrganizationName("cso-pak-tool");

	MainWindow window;
	window.show();

	return QApplication::exec();
}
