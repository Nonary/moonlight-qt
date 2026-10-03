#pragma once
#include <QDir>
#include <QString>

// The trace is already compressed. Store files in an ordinary ZIP without an
// external archiver, private Qt APIs, or loading the whole trace into memory.
bool writeDiagnosticZip(const QDir& source, const QString& destination, QString& error);
