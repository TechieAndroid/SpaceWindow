// SpaceWindow (Qt) - the Qt versions the app builds with.
//
// Qt 6.4 is the oldest it supports (Debian 12, Ubuntu 24.04); what later
// versions added is used where they have it, behind
//
//     #if SPACEWINDOW_QT_API >= QT_VERSION_CHECK(6, 6, 0)
//
// Configuring with -DSPACEWINDOW_QT_BASELINE=ON builds only what Qt 6.4 has,
// whatever Qt is installed: the way to build and test the fallbacks.
#pragma once

#include <QtGlobal>

#if defined(SPACEWINDOW_QT_BASELINE)
#define SPACEWINDOW_QT_API QT_VERSION_CHECK(6, 4, 0)
#else
#define SPACEWINDOW_QT_API QT_VERSION
#endif
