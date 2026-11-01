/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneFuzz.h"

#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>

#include <cstdio>

/// Replaces libFuzzer for compilers without it: runs the target once for each file of the corpus directories and
/// files given as arguments. Options of libFuzzer, starting with a dash, are ignored.
int main(int argc, char *argv[])
{
    (void) LLVMFuzzerInitialize(&argc, &argv);

    QStringList files;
    for (int i = 1; i < argc; i++) {
        const QString path = QString::fromLocal8Bit(argv[i]);
        if (path.startsWith(QLatin1Char('-'))) {
            continue;
        }
        if (QFileInfo(path).isDir()) {
            QDirIterator it(path, QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                files.append(it.next());
            }
        } else {
            files.append(path);
        }
    }
    files.sort();

    for (const QString &fileName : std::as_const(files)) {
        QFile file(fileName);
        if (!file.open(QIODevice::ReadOnly)) {
            std::fprintf(stderr, "Unable to read %s\n", qPrintable(fileName));
            return 1;
        }
        const QByteArray data = file.readAll();
        (void) LLVMFuzzerTestOneInput(reinterpret_cast<const uint8_t*>(data.constData()), static_cast<size_t>(data.size()));
    }
    std::printf("Executed %lld inputs\n", static_cast<long long>(files.count()));
    return files.isEmpty() ? 1 : 0;
}
