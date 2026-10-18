// A smoke test of the Qt app (SmokeTest.cpp).
#pragma once

#include <filesystem>
#include <memory>

#include <QObject>
#include <QTemporaryDir>

#include "MainWindow.h"

class SmokeTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void scansAndDraws();
    void drawsTheMapPixelForPixel();
    void selectsWithTheMouse();
    void zoomsInAndOut();
    void movesWithTheKeys();
    void togglesThePieChart();
    void showsTips();
    void showsTheContextMenu();
    void reloadsWhatChanged();
    void rescansAFolderOrEverything();
    void scansWhatIsDroppedOnIt();
    void opensSettings();
    void switchesThemes();
    void previewsWhatIsPicked();
    void playsTheBackgroundAnimation();
    void movesToTheTrash();
    void trashesNamesThatAreNotUtf8();
    void remembersWhereItWas();
    void closesWhileScanning();
    void cleanupTestCase();

private:  // NOLINT(readability-redundant-access-specifiers): Q_SLOTS is not one to clang-tidy
    QTemporaryDir folder_;
    std::filesystem::path root_;
    std::unique_ptr<sm::qt::MainWindow> window_;
};
