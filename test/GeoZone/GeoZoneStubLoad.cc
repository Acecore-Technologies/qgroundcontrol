/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

/// GeoZoneStubLoad checks the stub of the QGroundControl.GeoZone QML module which builds without geozones use
/// (QGC_DISABLE_GEOZONE, src/GeoZone/Stub) with the QML engine, where GeoZoneStubCheck.cmake compares text. It loads
/// the stub types from the files of src/GeoZone/Stub/CMakeLists.txt and, for each QML file outside src/GeoZone which
/// imports the module:
/// - compiles each object of a type of the module which the file places, with each property it sets and each signal
///   it handles on it, as the engine does when it loads the file: a type the stub lacks, or a property or signal which
///   neither the stub nor its base type has, is an error;
/// - makes those objects and reads the members the file uses through their ids, the members of the module's
///   singletons and the enumeration values of its types.
/// The files themselves can't be loaded without the application, whose types they use, so the uses are read from
/// their text. The values the file assigns aren't evaluated: they depend on the rest of the file.
///
///   GeoZoneStubLoad <source dir>
///
/// It exits with 1 and lists what the stub lacks. The stub of GeoZoneToolStripAction derives from ToolStripAction of
/// QGroundControl.Controls, which is registered here from its source. A stub deriving from another type of the
/// application needs that type registered here too.

#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QMap>
#include <QtCore/QMetaMethod>
#include <QtCore/QRegularExpression>
#include <QtCore/QSet>
#include <QtCore/QTextStream>
#include <QtGui/QGuiApplication>
#include <QtQml/QQmlComponent>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlEngine>
#include <QtQml/QQmlExpression>
#include <QtQml/qqml.h>

#include <memory>

#include "ToolStripAction.h"

namespace {

constexpr const char *moduleUri = "QGroundControl.GeoZone";

/// A QML file using the module
struct Document {
    QString name;       ///< Relative to the source directory
    QString code;       ///< Its text with the comments and the content of the strings blanked, of the same length
    QString import;     ///< Its import statement of the module
    QString qualifier;  ///< Of the import, with its dot, empty without
};

/// An object of a type of the module placed by a document
struct Block {
    QString type;
    int line = 0;
    QString id;
    QList<QPair<QString, int>> members;     ///< The properties set and the handlers, like anchors.fill and onDone, with their lines
};

QStringList errors;
int useCount = 0;

QString readFile(const QString &fileName)
{
    QFile file(fileName);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
}

/// @return The text with its comments and the content of its strings replaced by spaces, so that positions and lines
///         stay the same
QString blanked(const QString &text)
{
    enum class State { Code, LineComment, BlockComment, String };
    QString code = text;
    State state = State::Code;
    QChar quote;
    for (qsizetype i = 0; i < code.size(); i++) {
        const QChar c = code[i];
        const QChar next = ((i + 1) < code.size()) ? code[i + 1] : QChar();
        switch (state) {
        case State::Code:
            if ((c == QLatin1Char('/')) && (next == QLatin1Char('/'))) {
                state = State::LineComment;
                code[i] = QLatin1Char(' ');
            } else if ((c == QLatin1Char('/')) && (next == QLatin1Char('*'))) {
                state = State::BlockComment;
                code[i] = QLatin1Char(' ');
            } else if ((c == QLatin1Char('"')) || (c == QLatin1Char('\'')) || (c == QLatin1Char('`'))) {
                state = State::String;
                quote = c;
            }
            break;
        case State::LineComment:
            if (c == QLatin1Char('\n')) {
                state = State::Code;
            } else {
                code[i] = QLatin1Char(' ');
            }
            break;
        case State::BlockComment:
            if ((c == QLatin1Char('*')) && (next == QLatin1Char('/'))) {
                code[i] = QLatin1Char(' ');
                code[++i] = QLatin1Char(' ');
                state = State::Code;
            } else if (c != QLatin1Char('\n')) {
                code[i] = QLatin1Char(' ');
            }
            break;
        case State::String:
            if (c == QLatin1Char('\\')) {
                code[i] = QLatin1Char(' ');
                if (next != QLatin1Char('\n')) {
                    code[++i] = QLatin1Char(' ');
                }
            } else if (c == quote) {
                state = State::Code;
            } else if (c != QLatin1Char('\n')) {
                code[i] = QLatin1Char(' ');
            }
            break;
        }
    }
    return code;
}

int lineAt(const QString &text, qsizetype position)
{
    return static_cast<int>(text.left(position).count(QLatin1Char('\n'))) + 1;
}

/// Registers the types of the stub module from the files its CMakeLists.txt lists, like the build does
/// @return false when the list can't be read
bool registerStubs(const QString &stubDir)
{
    const QString list = readFile(QDir(stubDir).filePath(QStringLiteral("CMakeLists.txt")));
    static const QRegularExpression filesExpression(QStringLiteral(R"(QML_FILES\s+((?:[\w./-]+\.qml\s+)+))"));
    const QRegularExpressionMatch files = filesExpression.match(list);
    if (!files.hasMatch()) {
        errors.append(QStringLiteral("%1/CMakeLists.txt lists no QML_FILES").arg(stubDir));
        return false;
    }
    static const QRegularExpression singletonExpression(QStringLiteral(R"(set_source_files_properties\(\s*([\w./-]+\.qml)\s+PROPERTIES\s+QT_QML_SINGLETON_TYPE\s+TRUE)"));
    QSet<QString> singletons;
    for (const QRegularExpressionMatch &match : singletonExpression.globalMatch(list)) {
        singletons.insert(match.captured(1));
    }
    static const QRegularExpression space(QStringLiteral(R"(\s+)"));
    for (const QString &name : files.captured(1).split(space, Qt::SkipEmptyParts)) {
        const QString fileName = QDir(stubDir).filePath(name);
        if (!QFileInfo::exists(fileName)) {
            errors.append(QStringLiteral("%1 is listed in CMakeLists.txt and doesn't exist").arg(fileName));
            continue;
        }
        const QByteArray type = QFileInfo(name).completeBaseName().toLatin1();
        const QUrl url = QUrl::fromLocalFile(fileName);
        if (singletons.contains(name)) {
            (void) qmlRegisterSingletonType(url, moduleUri, 1, 0, type.constData());
        } else {
            (void) qmlRegisterType(url, moduleUri, 1, 0, type.constData());
        }
    }
    return true;
}

/// @return The source of a QML document with the imports of the layouts and of the module like the document has it,
///         and the body
QByteArray qmlSource(const Document &document, const QString &body)
{
    return QStringLiteral("import QtQuick\nimport QtQuick.Layouts\n%1\n%2\n").arg(document.import, body).toUtf8();
}

/// @return The errors of compiling the body in the document, empty when it compiles
QString compileErrors(QQmlEngine &engine, const Document &document, const QString &body)
{
    QQmlComponent component(&engine);
    component.setData(qmlSource(document, body), QUrl::fromLocalFile(QStringLiteral("/GeoZoneStubLoad/") + document.name));
    if (component.isReady()) {
        return QString();
    }
    QStringList messages;
    for (const QQmlError &error : component.errors()) {
        messages.append(error.description());
    }
    return messages.isEmpty() ? QStringLiteral("not loaded") : messages.join(QStringLiteral("; "));
}

/// @return The objects of the module's types the document places, with the members assigned directly in them
QList<Block> blocksOf(const Document &document)
{
    QList<Block> blocks;
    const QString &code = document.code;
    const QRegularExpression blockExpression(QStringLiteral(R"((?<![\w./])%1(GeoZone\w*)\s*\{)").arg(QRegularExpression::escape(document.qualifier)));
    // A member assigned at the start of a statement: a name, with the names of a grouped or attached property
    static const QRegularExpression memberExpression(QStringLiteral(R"(([A-Za-z_]\w*(?:\.[A-Za-z_]\w*)*)[ \t]*:(?!:))"));
    for (const QRegularExpressionMatch &match : blockExpression.globalMatch(code)) {
        Block block;
        block.type = match.captured(1);
        block.line = lineAt(code, match.capturedStart());

        // Statements start after the opening brace, a line break or a semicolon, outside nested braces, parentheses
        // and brackets. A line break within an expression, after or before an operator, starts none.
        int depth = 0;
        bool statementStart = true;
        QChar lastCode = QLatin1Char('{');
        for (qsizetype i = match.capturedEnd(); (i < code.size()) && (depth >= 0); i++) {
            const QChar c = code[i];
            if ((depth == 0) && statementStart && !c.isSpace()) {
                statementStart = false;
                const QRegularExpressionMatch member = memberExpression.match(code, i, QRegularExpression::NormalMatch, QRegularExpression::AnchorAtOffsetMatchOption);
                static const QString continuing = QStringLiteral("?:+-*/%&|=<>,.([");
                if (member.hasMatch() && !continuing.contains(lastCode)) {
                    const QString name = member.captured(1);
                    if (name == QStringLiteral("id")) {
                        static const QRegularExpression idExpression(QStringLiteral(R"([ \t]*([A-Za-z_]\w*))"));
                        block.id = idExpression.match(code, member.capturedEnd(), QRegularExpression::NormalMatch, QRegularExpression::AnchorAtOffsetMatchOption).captured(1);
                    } else {
                        block.members.append({ name, lineAt(code, i) });
                    }
                    i = member.capturedEnd() - 1;
                    lastCode = QLatin1Char(':');
                    continue;
                }
            }
            if ((c == QLatin1Char('{')) || (c == QLatin1Char('(')) || (c == QLatin1Char('['))) {
                depth++;
            } else if ((c == QLatin1Char('}')) || (c == QLatin1Char(')')) || (c == QLatin1Char(']'))) {
                depth--;
            } else if ((depth == 0) && ((c == QLatin1Char('\n')) || (c == QLatin1Char(';')))) {
                statementStart = true;
            }
            if (!c.isSpace()) {
                lastCode = c;
            }
        }
        blocks.append(block);
    }
    return blocks;
}

/// @return true when the object has a property, function or signal with the name
bool hasMember(const QObject *object, const QString &name)
{
    const QMetaObject *const metaObject = object->metaObject();
    const QByteArray member = name.toLatin1();
    if (metaObject->indexOfProperty(member.constData()) >= 0) {
        return true;
    }
    for (int i = 0; i < metaObject->methodCount(); i++) {
        if (metaObject->method(i).name() == member) {
            return true;
        }
    }
    return false;
}

/// Checks the members of the chain, like GeoZoneManager.settings.visible, whose first name is the id of an object
/// of the root or a type of the module
void checkChain(QObject *root, const Document &document, const QString &chain, bool throughId, const QString &idType = QString())
{
    useCount++;
    const auto evaluate = [root](const QString &expressionText, bool &failed) {
        QQmlExpression expression(qmlContext(root), root, expressionText);
        const QVariant value = expression.evaluate();
        failed = expression.hasError();
        return value;
    };
    const QStringList names = chain.split(QLatin1Char('.'));
    // The qualifier belongs to the type
    const int first = (!throughId && !document.qualifier.isEmpty()) ? 2 : 1;
    QString expression = names.mid(0, first).join(QLatin1Char('.'));
    bool failed = false;
    QObject *object = evaluate(expression, failed).value<QObject*>();
    for (int i = first; (i < names.count()) && object && !failed; i++) {
        if (!hasMember(object, names[i])) {
            errors.append(throughId ? QStringLiteral("%1: %2.%3 (%4.%3) isn't a member of the stub").arg(document.name, expression, names[i], idType)
                                    : QStringLiteral("%1: %2.%3 isn't a member of the stub").arg(document.name, expression, names[i]));
            return;
        }
        expression += QLatin1Char('.') + names[i];
        // What the file assigns to the properties of its objects isn't known here
        if (throughId) {
            return;
        }
        object = evaluate(expression, failed).value<QObject*>();
        if (!object && ((i + 1) < names.count())) {
            break;
        }
    }
    if (expression == chain) {
        return;
    }
    // The rest are values the engine reads, like the values of an enumeration
    const QVariant type = evaluate(QStringLiteral("typeof (%1)").arg(chain), failed);
    if (failed || (type.toString() == QStringLiteral("undefined"))) {
        errors.append(QStringLiteral("%1: %2 isn't declared by the stub").arg(document.name, chain));
    }
}

void checkDocument(QQmlEngine &engine, const Document &document)
{
    const QString &code = document.code;
    QString objects;
    QMap<QString, QString> ids;
    for (const Block &block : blocksOf(document)) {
        const QString where = QStringLiteral("%1:%2").arg(document.name).arg(block.line);
        const QString type = document.qualifier + block.type;
        useCount++;
        if (const QString typeErrors = compileErrors(engine, document, type + QStringLiteral(" { }")); !typeErrors.isEmpty()) {
            errors.append(QStringLiteral("%1: type %2: %3").arg(where, block.type, typeErrors));
            continue;
        }
        for (const auto &[member, line] : block.members) {
            // Attached properties, like Layout.fillWidth, belong to their own type
            if (member.at(0).isUpper()) {
                continue;
            }
            useCount++;
            static const QRegularExpression handlerExpression(QStringLiteral("^on[A-Z]"));
            const QString value = handlerExpression.match(member).hasMatch() ? QStringLiteral("{ }") : QStringLiteral("undefined");
            const QString memberErrors = compileErrors(engine, document, QStringLiteral("%1 { %2: %3 }").arg(type, member, value));
            if (!memberErrors.isEmpty()) {
                errors.append(QStringLiteral("%1:%2: %3.%4: %5").arg(document.name).arg(line).arg(block.type, member, memberErrors));
            }
        }
        if (!block.id.isEmpty()) {
            ids.insert(block.id, block.type);
            objects += QStringLiteral("    %1 { id: %2 }\n").arg(type, block.id);
        }
    }

    // The objects with an id, to read the members used through it, and the types of the module
    QQmlComponent component(&engine);
    component.setData(qmlSource(document, QStringLiteral("Item {\n%1}").arg(objects)), QUrl::fromLocalFile(QStringLiteral("/GeoZoneStubLoad/") + document.name));
    const std::unique_ptr<QObject> root(component.create());
    if (!root) {
        errors.append(QStringLiteral("%1: the stubs of its objects can't be made: %2").arg(document.name, component.errorString().trimmed()));
        return;
    }

    QSet<QString> chains;
    for (auto it = ids.cbegin(); it != ids.cend(); ++it) {
        const QRegularExpression useExpression(QStringLiteral(R"((?<![\w.])%1\.[A-Za-z_]\w*)").arg(QRegularExpression::escape(it.key())));
        for (const QRegularExpressionMatch &match : useExpression.globalMatch(code)) {
            if (!chains.contains(match.captured(0))) {
                chains.insert(match.captured(0));
                checkChain(root.get(), document, match.captured(0), true, it.value());
            }
        }
    }
    const QRegularExpression typeExpression(QStringLiteral(R"((?<![\w./])%1GeoZone\w*(?:\.[A-Za-z_]\w*)+)").arg(QRegularExpression::escape(document.qualifier)));
    for (const QRegularExpressionMatch &match : typeExpression.globalMatch(code)) {
        if (!chains.contains(match.captured(0))) {
            chains.insert(match.captured(0));
            checkChain(root.get(), document, match.captured(0), false);
        }
    }
}

} // namespace

int main(int argc, char *argv[])
{
    // No window is shown
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    const QGuiApplication application(argc, argv);
    QTextStream out(stdout);
    if (application.arguments().count() != 2) {
        out << "Usage: GeoZoneStubLoad <source dir>" << Qt::endl;
        return 2;
    }
    const QDir sourceDir(application.arguments().at(1));
    const QString stubDir = sourceDir.filePath(QStringLiteral("src/GeoZone/Stub"));

    (void) qmlRegisterType<ToolStripAction>("QGroundControl.Controls", 1, 0, "ToolStripAction");
    int fileCount = 0;
    if (registerStubs(stubDir)) {
        QQmlEngine engine;
        QStringList fileNames;
        QDirIterator files(sourceDir.filePath(QStringLiteral("src")), { QStringLiteral("*.qml") }, QDir::Files, QDirIterator::Subdirectories);
        while (files.hasNext()) {
            fileNames.append(sourceDir.relativeFilePath(files.next()));
        }
        fileNames.sort();
        static const QRegularExpression importExpression(QStringLiteral(R"(^[ \t]*(import[ \t]+QGroundControl\.GeoZone(?:[ \t]+[0-9.]+)?(?:[ \t]+as[ \t]+([A-Za-z_]\w*))?)[ \t\r]*$)"),
                                                         QRegularExpression::MultilineOption);
        for (const QString &fileName : std::as_const(fileNames)) {
            if (fileName.startsWith(QStringLiteral("src/GeoZone/"))) {
                continue;
            }
            Document document;
            document.name = fileName;
            document.code = blanked(readFile(sourceDir.filePath(fileName)));
            const QRegularExpressionMatch import = importExpression.match(document.code);
            if (!import.hasMatch()) {
                continue;
            }
            document.import = import.captured(1);
            document.qualifier = import.captured(2).isEmpty() ? QString() : (import.captured(2) + QLatin1Char('.'));
            fileCount++;
            checkDocument(engine, document);
        }
        if (fileCount == 0) {
            errors.append(QStringLiteral("No QML in %1 outside src/GeoZone imports %2").arg(sourceDir.filePath(QStringLiteral("src")), QLatin1String(moduleUri)));
        }
    }

    if (!errors.isEmpty()) {
        out << "src/GeoZone/Stub, used by builds without geozones, lacks what this QML uses:" << Qt::endl;
        for (const QString &error : std::as_const(errors)) {
            out << "  " << error << Qt::endl;
        }
        return 1;
    }
    out << "src/GeoZone/Stub loads and has the " << useCount << " uses of the module in " << fileCount << " QML files" << Qt::endl;
    return 0;
}
