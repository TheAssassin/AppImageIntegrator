// system includes
#include <iostream>
#include <memory>
#include <sstream>
extern "C" {
    #include <appimage/appimage.h>
    #include <glib.h>
    // #include <libgen.h>
    #include <sys/stat.h>
    #include <stdio.h>
    #include <unistd.h>
}

// library includes
#include <QDebug>
#include <QIcon>
#include <QtDBus>
#include <QDirIterator>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLibraryInfo>
#include <QMap>
#include <QMapIterator>
#include <QMessageBox>
#include <QObject>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#ifdef ENABLE_UPDATE_HELPER
#include <appimage/update.h>
#endif

// local headers
#include "shared.h"
#include "translationmanager.h"

static void gKeyFileDeleter(GKeyFile* ptr) {
    if (ptr != nullptr)
        g_key_file_free(ptr);
}

static void gErrorDeleter(GError* ptr) {
    if (ptr != nullptr)
        g_error_free(ptr);
}

bool makeExecutable(const QString& path) {
    struct stat fileStat{};

    if (stat(path.toStdString().c_str(), &fileStat) != 0) {
        std::cerr << "Failed to call stat() on " << path.toStdString() << std::endl;
        return false;
    }

    // no action required when file is executable already
    // this could happen in scenarios when an AppImage is in a read-only location
    if ((fileStat.st_uid == getuid() && fileStat.st_mode & 0100) ||
        (fileStat.st_gid == getgid() && fileStat.st_mode & 0010) ||
        (fileStat.st_mode & 0001)) {
        return true;
    }

    return chmod(path.toStdString().c_str(), fileStat.st_mode | 0111) == 0;
}

bool makeNonExecutable(const QString& path) {
    struct stat fileStat{};

    if (stat(path.toStdString().c_str(), &fileStat) != 0) {
        std::cerr << "Failed to call stat() on " << path.toStdString() << std::endl;
        return false;
    }

    auto permissions = fileStat.st_mode;

    // remove executable permissions
    for (const auto permPart : {0100, 0010, 0001}) {
        if (permissions & permPart)
            permissions -= permPart;
    }

    return chmod(path.toStdString().c_str(), permissions) == 0;
}

QString expandTilde(QString path) {
    if (path[0] == '~') {
        path.remove(0, 1);
        path.prepend(QDir::homePath());
    }

    return path;
}

// calculate path to config file
QString getConfigFilePath() {
    const auto configPath = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    const auto configFilePath = configPath + "/appimagelauncher.cfg";
    return configFilePath;
}

void createConfigFile(int askToMove, QString destination, int enableDaemon) {
    auto configFilePath = getConfigFilePath();

    QFile file(configFilePath);
    file.open(QIODevice::WriteOnly);

    // cannot use QSettings because it doesn't support comments
    // let's do it manually and hope for the best
    file.write("[AppImageLauncher]\n");

    if (askToMove < 0) {
        file.write("# ask_to_move = true\n");
    } else {
        file.write("ask_to_move = ");
        if (askToMove == 0) {
            file.write("false");
        } else {
            file.write("true");
        }
        file.write("\n");
    }

    if (destination.isEmpty()) {
        file.write("# destination = ~/Applications\n");
    } else {
        file.write("destination = ");
        file.write(destination.toUtf8());
        file.write("\n");
    }

    if (enableDaemon < 0) {
        file.write("# enable_daemon = true\n");
    } else {
        file.write("enable_daemon = ");
        if (enableDaemon == 0) {
            file.write("false");
        } else {
            file.write("true");
        }
        file.write("\n");
    }
}


std::shared_ptr<QSettings> getConfig() {
    auto configFilePath = getConfigFilePath();

    // if the file does not exist, we'll just use the standard location
    // while in theory it would have been possible to just write the default location to the file, if we'd ever change
    // it again, we'd leave a lot of systems in the old state, and would have to write some complex code to resolve
    // the situation
    // therefore, the file is simply created, but left empty intentionally
    if (!QFileInfo::exists(configFilePath)) {
        return nullptr;
    }

    auto rv = std::make_shared<QSettings>(configFilePath, QSettings::IniFormat);

    // expand ~ in paths in the config file with $HOME
    for (const QString& keyContainingPath : {"destination"}){
        QString fullKey = "AppImageLauncher/" + keyContainingPath;

        if (rv->contains(fullKey)) {
            auto newValue = expandTilde(rv->value(fullKey).toString());
            rv->setValue(fullKey, newValue);
        }
    }

    return rv;
}

// TODO: check if this works with Wayland
bool isHeadless() {
    bool isHeadless = true;

    // not really clean to abuse env vars as "global storage", but hey, it works
    if (getenv("_FORCE_HEADLESS")) {
        return true;
    }

    QProcess proc;
    proc.setProgram("xhost");
    proc.setStandardOutputFile(QProcess::nullDevice());
    proc.setStandardErrorFile(QProcess::nullDevice());

    proc.start();
    proc.waitForFinished();

    switch (proc.exitCode()) {
        case 255: {
            // program not found, using fallback method
            isHeadless = (getenv("DISPLAY") == nullptr);
            break;
        }
        case 0:
        case 1:
            isHeadless = proc.exitCode() == 1;
            break;
        default:
            throw std::runtime_error("Headless detection failed: unexpected exit code from xhost");
    }

    return isHeadless;
}

// avoids code duplication, and works for both graphical and non-graphical environments
void displayMessageBox(const QString& title, const QString& message, const QMessageBox::Icon icon) {
    if (isHeadless()) {
        std::cerr << title.toStdString() << ": " << message.toStdString() << std::endl;
    } else {
        // little complex, can't use QMessageBox::{critical,warning,...} for the same reason as in main()
        auto* mb = new QMessageBox(icon, title, message, QMessageBox::Ok, nullptr);
        mb->show();
        QApplication::exec();
    }
}

void displayError(const QString& message) {
    displayMessageBox(QObject::tr("Error"), message, QMessageBox::Critical);
}

void displayWarning(const QString& message) {
    displayMessageBox(QObject::tr("Warning"), message, QMessageBox::Warning);
}

QDir integratedAppImagesDestination() {
    auto config = getConfig();

    if (config == nullptr)
        return DEFAULT_INTEGRATION_DESTINATION;

    static const QString keyName("AppImageLauncher/destination");
    if (config->contains(keyName))
        return config->value(keyName).toString();

    return DEFAULT_INTEGRATION_DESTINATION;
}

QString buildPathToIntegratedAppImage(const QString& pathToAppImage) {
    // if type 2 AppImage, we can build a "content-aware" filename
    // see #7 for details
    auto digest = getAppImageDigestMd5(pathToAppImage);

    const QFileInfo appImageInfo(pathToAppImage);

    QString baseName = appImageInfo.completeBaseName();

    // if digest is available, append a separator
    if (!digest.isEmpty()) {
        const auto digestSuffix = "_" + digest;

        // check whether digest is already contained in filename
        if (!pathToAppImage.contains(digestSuffix))
            baseName += "_" + digest;
    }

    auto fileName = baseName;

    // must not use completeSuffix() in combination with completeBasename(), otherwise the final filename is composed
    // incorrectly
    if (!appImageInfo.suffix().isEmpty()) {
        fileName += "." + appImageInfo.suffix();
    }

    return integratedAppImagesDestination().path() + "/" + fileName;
}

std::map<std::string, std::string> findCollisions(const QString& currentNameEntry) {
    std::map<std::string, std::string> collisions;

    // default locations of desktop files on systems
    const auto directories = {QString("/usr/share/applications/"), QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/applications/"};

    for (const auto& directory : directories) {
        QDirIterator iterator(directory, QDirIterator::FollowSymlinks);

        while (iterator.hasNext()) {
            const auto filename = iterator.next();

            if (!QFileInfo(filename).isFile() || !filename.endsWith(".desktop"))
                continue;

            std::shared_ptr<GKeyFile> desktopFile(g_key_file_new(), gKeyFileDeleter);
            std::shared_ptr<GError*> error(nullptr, gErrorDeleter);

            // if the key file parser can't load the file, it's most likely not a valid desktop file, so we just skip this file
            if (!g_key_file_load_from_file(desktopFile.get(), filename.toStdString().c_str(), G_KEY_FILE_KEEP_TRANSLATIONS, error.get()))
                continue;

            auto* nameEntry = g_key_file_get_string(desktopFile.get(), G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_NAME, error.get());

            // invalid desktop file, needs to be skipped
            if (nameEntry == nullptr)
                continue;

            if (QString(nameEntry).trimmed().startsWith(currentNameEntry.trimmed())) {
                collisions[filename.toStdString()] = nameEntry;
            }
        }
    }

    return collisions;
}

bool updateDesktopDatabaseAndIconCaches() {
    const std::map<std::string, std::string> commands = {
        {"update-desktop-database", "~/.local/share/applications"},
        {"gtk-update-icon-cache-3.0", "~/.local/share/icons/hicolor/ -t"},
        {"gtk-update-icon-cache", "~/.local/share/icons/hicolor/ -t"},
        {"xdg-desktop-menu", "forceupdate"},
    };

    for (const auto& command : commands) {
        // only call if the command exists
        if (system(("which " + command.first + " 2>&1 1>/dev/null").c_str()) == 0) {
            // exit codes are not evaluated intentionally
            system((command.first + " " + command.second).c_str());
        }
    }

    return true;
}

std::shared_ptr<char> getOwnBinaryPath() {
    auto path = std::shared_ptr<char>(realpath("/proc/self/exe", nullptr));

    if (path == nullptr)
        throw std::runtime_error("Could not detect path to own binary; something must be horribly broken");

    return path;
}

bool installDesktopFileAndIcons(const QString& pathToAppImage, bool resolveCollisions) {
    if (appimage_register_in_system(pathToAppImage.toStdString().c_str(), false) != 0) {
        displayError(QObject::tr("Failed to register AppImage in system via libappimage"));
        return false;
    }

    const auto* desktopFilePath = appimage_registered_desktop_file_path(pathToAppImage.toStdString().c_str(), nullptr, false);

    // sanity check -- if the file doesn't exist, the function returns NULL
    if (desktopFilePath == nullptr) {
        displayError(QObject::tr("Failed to find integrated desktop file"));
        return false;
    }

    // check that file exists
    if (!QFile(desktopFilePath).exists()) {
        displayError(QObject::tr("Couldn't find integrated AppImage's desktop file"));
        return false;
    }

    /* write AppImageLauncher specific entries to desktop file
     *
     * unfortunately, QSettings doesn't work as a desktop file reader/writer, and libqtxdg isn't really meant to be
     * used by projects via add_subdirectory/ExternalProject
     * a system dependency is not an option for this project, and we link to glib already anyway, so let's just use
     * glib, which is known to work
     */

    std::shared_ptr<GKeyFile> desktopFile(g_key_file_new(), gKeyFileDeleter);

    std::shared_ptr<GError*> error(nullptr, gErrorDeleter);

    const auto flags = GKeyFileFlags(G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS);

    auto handleError = [error, desktopFile]() {
        std::ostringstream ss;
        ss << QObject::tr("Failed to load desktop file:").toStdString() << std::endl << (*error)->message;
        displayError(QString::fromStdString(ss.str()));
    };

    if (!g_key_file_load_from_file(desktopFile.get(), desktopFilePath, flags, error.get())) {
        handleError();
        return false;
    }

    const auto* nameEntry = g_key_file_get_string(desktopFile.get(), G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_NAME, error.get());

    if (nameEntry == nullptr) {
        displayWarning(QObject::tr("AppImage has invalid desktop file"));
    }

    if (resolveCollisions) {
        // TODO: support multilingual collisions
        auto collisions = findCollisions(nameEntry);

        // make sure to remove own entry
        collisions.erase(collisions.find(desktopFilePath));

        if (!collisions.empty()) {
            // collisions are resolved like in the filesystem: a monotonically increasing number in brackets is
            // appended to the Name in order to keep the number monotonically increasing, we look for the highest
            // number in brackets in the existing entries, add 1 to it, and append it in brackets to the current
            // desktop file's Name entry

            unsigned int currentNumber = 1;

            QRegularExpression regex(R"(^.*\(([0-9]+)\)$)");

            for (const auto& collision : collisions) {
                const auto& currentNameEntry = collision.second;

                auto match = regex.match(QString::fromStdString(currentNameEntry));

                if (match.hasMatch()) {
                    const unsigned int num = match.captured(0).toUInt();
                    if (num >= currentNumber)
                        currentNumber = num + 1;
                }
            }

            auto newName = QString(nameEntry) + " (" + QString::number(currentNumber) + ")";
            g_key_file_set_string(desktopFile.get(), G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_NAME, newName.toStdString().c_str());
        }
    }

    auto convertToCharPointerList = [](const std::vector<std::string>& stringList) {
        std::vector<const char*> pointerList;

        // reserve space to increase efficiency
        pointerList.reserve(stringList.size());

        // convert string list to list of const char pointers
        for (const auto& action : stringList) {
            pointerList.push_back(action.c_str());
        }

        return pointerList;
    };

    std::vector<std::string> desktopActions = {"Remove"};

    // load translations from JSON file(s)
    QMap<QString, QString> removeActionNameTranslations;

#ifdef ENABLE_UPDATE_HELPER
    QMap<QString, QString> updateActionNameTranslations;

    {
        QDirIterator i18nDirIterator(TranslationManager::getTranslationDir());

        while(i18nDirIterator.hasNext()) {
            const auto& filePath = i18nDirIterator.next();
            const auto& fileName = QFileInfo(filePath).fileName();

            if (!QFileInfo(filePath).isFile() || !(fileName.startsWith("desktopfiles.") && fileName.endsWith(".json")))
                continue;

            // check whether filename's format is alright, otherwise parsing the locale might try to access a
            // non-existing (or the wrong) member
            auto splitFilename = fileName.split(".");

            if (splitFilename.size() != 3)
                continue;

            // parse locale from filename
            auto locale = splitFilename[1];

            QFile jsonFile(filePath);

            if (!jsonFile.open(QIODevice::ReadOnly)) {
                displayWarning(QMessageBox::tr("Could not parse desktop file translations:\nCould not open file for reading:\n\n%1").arg(fileName));
            }

            // TODO: need to make sure that this doesn't try to read huge files at once
            auto data = jsonFile.readAll();

            QJsonParseError parseError{};
            auto jsonDoc = QJsonDocument::fromJson(data, &parseError);

            // show warning on syntax errors and continue
            if (parseError.error != QJsonParseError::NoError || jsonDoc.isNull() || !jsonDoc.isObject()) {
                displayWarning(QMessageBox::tr("Could not parse desktop file translations:\nInvalid syntax:\n\n%1").arg(parseError.errorString()));
            }

            auto jsonObj = jsonDoc.object();

            for (const auto& key : jsonObj.keys()) {
                auto value = jsonObj[key].toString();
                auto splitKey = key.split("/");

                if (key.startsWith("Desktop Action update")) {
                    qDebug() << "update: adding" << value << "for locale" << locale;
                    updateActionNameTranslations[locale] = value;
                } else if (key.startsWith("Desktop Action remove")) {
                    qDebug() << "remove: adding" << value << "for locale" << locale;
                    removeActionNameTranslations[locale] = value;
                }
            }
        }
    }
#endif

#ifndef BUILD_LITE
    // PRIVATE_LIBDIR will be a relative path most likely
    // therefore, we need to detect the install prefix based on our own binary path, and then calculate the path to
    // the helper tools based on that
    const QString ownBinaryDirPath = QFileInfo(getOwnBinaryPath().get()).dir().absolutePath();
    const QString installPrefixPath = QFileInfo(ownBinaryDirPath).dir().absolutePath();
    QString privateLibDirPath = installPrefixPath + "/" + PRIVATE_LIBDIR;

    // the following lines make things work during development: here, the build dir path is inserted instead, which
    // allows for testing with the latest changes
    if (!QDir(privateLibDirPath).exists()) {
        // this makes sure that when we're running from a local dev build, we end up in the right directory
        // very important when running this code from the daemon, since it's not in the same directory as the helpers
        privateLibDirPath = ownBinaryDirPath + "/../ui";
    }

    const char helperIconName[] = "AppImageLauncher";
#else
    const char helperIconName[] = "AppImageLauncher-Lite";
#endif

    // add Remove action
    {
        const auto removeSectionName = "Desktop Action Remove";

        g_key_file_set_string(desktopFile.get(), removeSectionName, "Name", "Remove application from system");
        g_key_file_set_string(desktopFile.get(), removeSectionName, "Icon", helperIconName);

        std::ostringstream removeExecPath;

#ifndef BUILD_LITE
        removeExecPath << privateLibDirPath.toStdString() << "/remove";
#else
        removeExecPath << getenv("HOME") << "/.local/lib/appimagelauncher-lite/appimagelauncher-lite.AppImage remove";
#endif

        removeExecPath << " \"" << pathToAppImage.toStdString() << "\"";

        g_key_file_set_string(desktopFile.get(), removeSectionName, "Exec", removeExecPath.str().c_str());

        // install translations
        auto it = QMapIterator<QString, QString>(removeActionNameTranslations);
        while (it.hasNext()) {
            auto entry = it.next();
            g_key_file_set_locale_string(desktopFile.get(), removeSectionName, "Name", entry.key().toStdString().c_str(), entry.value().toStdString().c_str());
        }
    }

#ifdef ENABLE_+_HELPER
    // add Update action
    {
        appimage::update::Updater updater(pathToAppImage.toStdString());

        // but only if there's update information
        if (!updater.updateInformation().empty()) {
            // section needs to be announced in desktop actions list
            desktopActions.emplace_back("Update");

            const auto updateSectionName = "Desktop Action Update";

            g_key_file_set_string(desktopFile.get(), updateSectionName, "Name", "Update application");
            g_key_file_set_string(desktopFile.get(), updateSectionName, "Icon", helperIconName);

            std::ostringstream updateExecPath;

#ifndef BUILD_LITE
            updateExecPath << privateLibDirPath.toStdString() << "/update";
#else
            updateExecPath << getenv("HOME") << "/.local/lib/appimagelauncher-lite/appimagelauncher-lite.AppImage update";
#endif
            updateExecPath << " \"" << pathToAppImage.toStdString() << "\"";

            g_key_file_set_string(desktopFile.get(), updateSectionName, "Exec", updateExecPath.str().c_str());

            // install translations
            auto it = QMapIterator<QString, QString>(updateActionNameTranslations);
            while (it.hasNext()) {
                auto entry = it.next();
                g_key_file_set_locale_string(desktopFile.get(), updateSectionName, "Name", entry.key().toStdString().c_str(), entry.value().toStdString().c_str());
            }
        }
    }
#endif

    // add desktop actions key
    g_key_file_set_string_list(
            desktopFile.get(),
            G_KEY_FILE_DESKTOP_GROUP,
            G_KEY_FILE_DESKTOP_KEY_ACTIONS,
            convertToCharPointerList(desktopActions).data(),
            desktopActions.size()
    );

    // add version key
    const auto version = QApplication::applicationVersion().replace("version ", "").toStdString();
    g_key_file_set_string(desktopFile.get(), G_KEY_FILE_DESKTOP_GROUP, "X-AppImageLauncher-Version", version.c_str());

    // save desktop file to disk
    if (!g_key_file_save_to_file(desktopFile.get(), desktopFilePath, error.get())) {
        handleError();
        return false;
    }

    // make desktop file executable ("trustworthy" to some DEs)
    // TODO: handle this in libappimage
    makeExecutable(desktopFilePath);

    // notify KDE/Plasma about icon change
    {
        auto message = QDBusMessage::createSignal(QStringLiteral("/KIconLoader"), QStringLiteral("org.kde.KIconLoader"), QStringLiteral("iconChanged"));
        message.setArguments({0});
        QDBusConnection::sessionBus().send(message);
    }

    return true;
}

bool updateDesktopFileAndIcons(const QString& pathToAppImage) {
    return installDesktopFileAndIcons(pathToAppImage, true);
}

IntegrationState integrateAppImage(const QString& pathToAppImage, const QString& pathToIntegratedAppImage) {
    // need std::strings to get working pointers with .c_str()
    const auto oldPath = pathToAppImage.toStdString();
    const auto newPath = pathToIntegratedAppImage.toStdString();

    // create target directory
    QDir().mkdir(QFileInfo(QFile(pathToIntegratedAppImage)).dir().absolutePath());

    // check whether AppImage is in integration directory already
    if (QFileInfo(pathToAppImage).absoluteFilePath() != QFileInfo(pathToIntegratedAppImage).absoluteFilePath()) {
        // need to check whether file exists
        // if it does, the existing AppImage needs to be removed before rename can be called
        if (QFile(pathToIntegratedAppImage).exists()) {
            std::ostringstream message;
            message << QObject::tr("AppImage with same filename has already been integrated.").toStdString() << std::endl
                    << std::endl
                    << QObject::tr("Do you wish to overwrite the existing AppImage?").toStdString() << std::endl
                    << QObject::tr("Choosing No will run the AppImage once, and leave the system in its current state.").toStdString();

            auto* messageBox = new QMessageBox(
                QMessageBox::Warning,
                QObject::tr("Warning"),
                QString::fromStdString(message.str()),
                QMessageBox::Yes | QMessageBox::No
            );

            messageBox->setDefaultButton(QMessageBox::No);
            messageBox->show();

            QApplication::exec();

            if (messageBox->clickedButton() == messageBox->button(QMessageBox::No)) {
                return INTEGRATION_ABORTED;
            }

            QFile(pathToIntegratedAppImage).remove();
        }

        if (!QFile(pathToAppImage).rename(pathToIntegratedAppImage)) {
            auto* messageBox = new QMessageBox(
                QMessageBox::Critical,
                QObject::tr("Error"),
                QObject::tr("Failed to move AppImage to target location.\n"
                            "Try to copy AppImage instead?"),
                QMessageBox::Ok | QMessageBox::Cancel
            );

            messageBox->setDefaultButton(QMessageBox::Ok);
            messageBox->show();

            QApplication::exec();

            if (messageBox->clickedButton() == messageBox->button(QMessageBox::Cancel))
                return INTEGRATION_FAILED;

            if (!QFile(pathToAppImage).copy(pathToIntegratedAppImage)) {
                displayError("Failed to copy AppImage to target location");
                return INTEGRATION_FAILED;
            }
        }
    }

    if (!installDesktopFileAndIcons(pathToIntegratedAppImage))
        return INTEGRATION_FAILED;

    return INTEGRATION_SUCCESSFUL;
}


QString getAppImageDigestMd5(const QString& path) {
    // try to read embedded MD5 digest
    unsigned long offset = 0, length = 0;

    // first of all, digest calculation is supported only for type 2
    if (appimage_get_type(path.toStdString().c_str(), false) != 2)
        return "";

    auto rv = appimage_get_elf_section_offset_and_length(path.toStdString().c_str(), ".digest_md5", &offset, &length);

    QByteArray buffer(16, '\0');

    if (rv && offset != 0 && length != 0) {
        // open file and read digest from ELF header section
        QFile file(path);

        if (!file.open(QFile::ReadOnly))
            return "";

        if (!file.seek(static_cast<qint64>(offset)))
            return "";

        if (!file.read(buffer.data(), buffer.size()))
            return "";

        file.close();
    } else {
        // calculate digest
        if (!appimage_type2_digest_md5(path.toStdString().c_str(), buffer.data()))
            return "";
    }

    // create hexadecimal representation
    auto hexDigest = appimage_hexlify(buffer, static_cast<size_t>(buffer.size()));

    QString hexDigestStr(hexDigest);

    free(hexDigest);

    return hexDigestStr;
}

bool hasAlreadyBeenIntegrated(const QString& pathToAppImage) {
    return appimage_is_registered_in_system(pathToAppImage.toStdString().c_str());
}

bool isInDirectory(const QString& pathToAppImage, const QDir& directory) {
    return directory == QFileInfo(pathToAppImage).absoluteDir();
}

bool cleanUpOldDesktopIntegrationResources(bool verbose) {
    auto dirPath = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/applications";

    auto directory = QDir(dirPath);

    QStringList filters;
    filters << "appimagekit_*.desktop";

    directory.setNameFilters(filters);

    for (auto desktopFilePath : directory.entryList()) {
        desktopFilePath = dirPath + "/" + desktopFilePath;

        std::shared_ptr<GKeyFile> desktopFile(g_key_file_new(), [](GKeyFile* p) {
            g_key_file_free(p);
        });

        if (!g_key_file_load_from_file(desktopFile.get(), desktopFilePath.toStdString().c_str(), G_KEY_FILE_NONE, nullptr)) {
            continue;
        }

        std::shared_ptr<char> execValue(g_key_file_get_string(desktopFile.get(), G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_EXEC, nullptr), [](char* p) {
            free(p);
        });

        // if there is no Exec value in the file, the desktop file is apparently broken, therefore we skip the file
        if (execValue == nullptr) {
            continue;
        }

        std::shared_ptr<char> tryExecValue(g_key_file_get_string(desktopFile.get(), G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_TRY_EXEC, nullptr), [](char* p) {
            free(p);
        });

        // TryExec is optional, although recently the desktop integration functions started to force add such keys
        // with a path to the desktop file
        // (before, if it existed, the key was replaced with the AppImage's path)
        // If it exists, we assume its value is the full path to the AppImage, which can be used to check the existence
        // of the AppImage
        QString appImagePath;

        if (tryExecValue != nullptr) {
            appImagePath = QString(tryExecValue.get());
        } else {
            appImagePath = QString(execValue.get()).split(" ").first();
        }

        // now, check whether AppImage exists
        // FIXME: the split command for the Exec value might not work if there's a space in the filename
        // we really need a parser that understands the desktop file escaping
        if (!QFile(appImagePath).exists()) {
            if (verbose)
                std::cout << "AppImage no longer exists, cleaning up resources: " << appImagePath.toStdString() << std::endl;

            if (verbose)
                std::cout << "Removing desktop file: " << desktopFilePath.toStdString() << std::endl;

            QFile(desktopFilePath).remove();

            // TODO: clean up related resources such as icons or MIME definitions

            auto* iconValue = g_key_file_get_string(desktopFile.get(), G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_ICON, nullptr);

            if (iconValue != nullptr) {
                for (QDirIterator it("~/.local/share/icons/", QDirIterator::Subdirectories); it.hasNext();) {
                    auto path = it.next();

                    if (QFileInfo(path).completeBaseName().startsWith(iconValue)) {
                        QFile::remove(path);
                    }
                }
            }
        }
    }

    return true;
}

time_t getMTime(const QString& path) {
    struct stat st{};
    if (stat(path.toStdString().c_str(), &st) != 0) {
        displayError(QObject::tr("Failed to call stat() on path:\n\n%1").arg(path));
        return -1;
    }

    return st.st_mtim.tv_sec;
}

bool desktopFileHasBeenUpdatedSinceLastUpdate(const QString& pathToAppImage) {
    const auto ownBinaryPath = getOwnBinaryPath();

    const auto desktopFilePath = appimage_registered_desktop_file_path(pathToAppImage.toStdString().c_str(), nullptr, false);
    
    auto ownBinaryMTime = getMTime(ownBinaryPath.get());
    auto desktopFileMTime = getMTime(desktopFilePath);

    // check if something has failed horribly
    if (desktopFileMTime < 0 || ownBinaryMTime < 0)
        return false;

    return desktopFileMTime > ownBinaryMTime;
}

bool fsDaemonHasBeenRestartedSinceLastUpdate() {
    const auto ownBinaryPath = getOwnBinaryPath();

    auto ownBinaryMTime = getMTime(ownBinaryPath.get());

    auto getServiceStartTime = []() -> long long {
        auto fp = popen("systemctl --user show appimagelauncherfs.service --property=ActiveEnterTimestampMonotonic", "r");

        if (fp == nullptr) {
            return -1;
        }

        std::vector<char> buffer(512);

        if (fread(buffer.data(), sizeof(char), buffer.size(), fp) < 0)
            return 1;

        std::string strbuf(buffer.data());

        auto equalsPos = strbuf.find('=');
        auto lfPos = strbuf.find('\n');
        if (lfPos == std::string::npos)
            lfPos = strbuf.size();

        auto timestamp = strbuf.substr(equalsPos + 1, lfPos - equalsPos - 1);

        auto monotonicRuntime = static_cast<long long>(std::stoll(timestamp) / 1e6);

        timespec currentMonotonicTime{};
        timespec currentRealTime{};

        clock_gettime(CLOCK_MONOTONIC, &currentMonotonicTime);
        clock_gettime(CLOCK_REALTIME, &currentRealTime);

        auto offset = currentRealTime.tv_sec - currentMonotonicTime.tv_sec;

        return monotonicRuntime + offset;
    };

    auto serviceStartTime = getServiceStartTime();

    // check if something has failed horribly
    if (serviceStartTime < 0 || ownBinaryMTime < 0)
        return false;

    return serviceStartTime > ownBinaryMTime;
}

bool isAppImage(const QString& path) {
    const auto type = appimage_get_type(path.toUtf8(), false);
    return type > 0 && type <= 2;
}
