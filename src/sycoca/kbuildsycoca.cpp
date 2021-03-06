/*  This file is part of the KDE libraries
 *  Copyright (C) 1999 David Faure <faure@kde.org>
 *  Copyright (C) 2002-2003 Waldo Bastian <bastian@kde.org>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License version 2 as published by the Free Software Foundation;
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 *  You should have received a copy of the GNU Library General Public License
 *  along with this library; see the file COPYING.LIB.  If not, write to
 *  the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *  Boston, MA 02110-1301, USA.
 **/

#include "kbuildsycoca_p.h"
#include "ksycoca_p.h"
#include "ksycocaresourcelist_p.h"
#include "vfolder_menu_p.h"
#include "ksycocautils_p.h"
#include "sycocadebug.h"

#include <config-ksycoca.h>
#include <kservicegroup.h>
#include <kservice.h>
#include "kbuildservicetypefactory_p.h"
#include "kbuildmimetypefactory_p.h"
#include "kbuildservicefactory_p.h"
#include "kbuildservicegroupfactory_p.h"
#include "kctimefactory_p.h"
#include <QDataStream>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QLocale>
#include <QTimer>
#include <QDebug>
#include <QDirIterator>
#include <QDateTime>
#include <qsavefile.h>

#include <kmemfile_p.h>

#include <qplatformdefs.h>
#include <time.h>
#include <memory> // auto_ptr
#include <qstandardpaths.h>
#include <QLockFile>

static const char *s_cSycocaPath = nullptr;

KBuildSycocaInterface::~KBuildSycocaInterface() {}

KBuildSycoca::KBuildSycoca(bool globalDatabase)
    : KSycoca(true),
      m_allEntries(nullptr),
      m_currentFactory(nullptr),
      m_ctimeFactory(nullptr),
      m_ctimeDict(nullptr),
      m_currentEntryDict(nullptr),
      m_serviceGroupEntryDict(nullptr),
      m_vfolder(nullptr),
      m_newTimestamp(0),
      m_globalDatabase(globalDatabase),
      m_menuTest(false),
      m_changed(false)
{
}

KBuildSycoca::~KBuildSycoca()
{
    // Delete the factories while we exist, so that the virtual isBuilding() still works
    qDeleteAll(*factories());
    factories()->clear();
}

KSycocaEntry::Ptr KBuildSycoca::createEntry(const QString &file, bool addToFactory)
{
    quint32 timeStamp = m_ctimeFactory->dict()->ctime(file, m_resource);
    if (!timeStamp) {
        timeStamp = calcResourceHash(m_resourceSubdir, file);
    }
    KSycocaEntry::Ptr entry;
    if (m_allEntries) {
        Q_ASSERT(m_ctimeDict);
        quint32 oldTimestamp = m_ctimeDict->ctime(file, m_resource);
        if (file.contains(QLatin1String("fake"))) {
            qCDebug(SYCOCA) << "m_ctimeDict->ctime(" << file << ") = " << oldTimestamp << "compared with" << timeStamp;
        }

        if (timeStamp && (timeStamp == oldTimestamp)) {
            // Re-use old entry
            if (m_currentFactory == d->m_serviceFactory) { // Strip .directory from service-group entries
                entry = m_currentEntryDict->value(file.left(file.length() - 10));
            } else {
                entry = m_currentEntryDict->value(file);
            }
            // remove from m_ctimeDict; if m_ctimeDict is not empty
            // after all files have been processed, it means
            // some files were removed since last time
            if (file.contains(QLatin1String("fake"))) {
                qCDebug(SYCOCA) << "reusing (and removing) old entry for:" << file << "entry=" << entry;
            }
            m_ctimeDict->remove(file, m_resource);
        } else if (oldTimestamp) {
            m_changed = true;
            m_ctimeDict->remove(file, m_resource);
            qCDebug(SYCOCA) << "modified:" << file;
        } else {
            m_changed = true;
            qCDebug(SYCOCA) << "new:" << file;
        }
    }
    m_ctimeFactory->dict()->addCTime(file, m_resource, timeStamp);
    if (!entry) {
        // Create a new entry
        entry = m_currentFactory->createEntry(file);
    }
    if (entry && entry->isValid()) {
        if (addToFactory) {
            m_currentFactory->addEntry(entry);
        } else {
            m_tempStorage.append(entry);
        }
        return entry;
    }
    return KSycocaEntry::Ptr();
}

KService::Ptr KBuildSycoca::createService(const QString &path)
{
    KSycocaEntry::Ptr entry = createEntry(path, false);
    return KService::Ptr(static_cast<KService*>(entry.data()));
}

// returns false if the database is up to date, true if it needs to be saved
bool KBuildSycoca::build()
{
    typedef QList<KBSEntryDict *> KBSEntryDictList;
    KBSEntryDictList entryDictList;
    KBSEntryDict *serviceEntryDict = nullptr;

    // Convert for each factory the entryList to a Dict.
    entryDictList.reserve(factories()->size());
    int i = 0;
    // For each factory
    Q_FOREACH (KSycocaFactory* factory, *factories()) {
        KBSEntryDict *entryDict = new KBSEntryDict;
        if (m_allEntries) { // incremental build
            Q_FOREACH (const KSycocaEntry::Ptr &entry, (*m_allEntries)[i++]) {
                //if (entry->entryPath().contains("fake"))
                //    qCDebug(SYCOCA) << "inserting into entryDict:" << entry->entryPath() << entry;
                entryDict->insert(entry->entryPath(), entry);
            }
        }
        if (factory == d->m_serviceFactory) {
            serviceEntryDict = entryDict;
        } else if (factory == m_buildServiceGroupFactory) {
            m_serviceGroupEntryDict = entryDict;
        }
        entryDictList.append(entryDict);
    }

    // Save the mtime of each dir, just before we list them
    // ## should we convert to UTC to avoid surprises when summer time kicks in?
    Q_FOREACH (const QString &dir, factoryResourceDirs()) {
        qint64 stamp = 0;
        KSycocaUtilsPrivate::visitResourceDirectory(dir, [&stamp] (const QFileInfo &info) {
            stamp = qMax(stamp, info.lastModified().toMSecsSinceEpoch());
            return true;
        });
        m_allResourceDirs.insert(dir, stamp);
    }

    QMap<QString, QByteArray> allResourcesSubDirs; // dirs, kstandarddirs-resource-name
    // For each factory
    Q_FOREACH (KSycocaFactory* factory, *factories()) {
        // For each resource the factory deals with
        const KSycocaResourceList *list = factory->resourceList();
        if (!list) {
            continue;
        }
        Q_FOREACH (const KSycocaResource &res, *list) {
            // With this we would get dirs, but not a unique list of relative files (for global+local merging to work)
            //const QStringList dirs = QStandardPaths::locateAll(QStandardPaths::GenericDataLocation, res.subdir, QStandardPaths::LocateDirectory);
            //allResourcesSubDirs[res.resource] += dirs;
            allResourcesSubDirs.insert(res.subdir, res.resource);
        }
    }

    m_ctimeFactory = new KCTimeFactory(this); // This is a build factory too, don't delete!!
    for (QMap<QString, QByteArray>::ConstIterator it1 = allResourcesSubDirs.constBegin();
            it1 != allResourcesSubDirs.constEnd();
            ++it1) {
        m_changed = false;
        m_resourceSubdir = it1.key();
        m_resource = it1.value();

        QSet<QString> relFiles;
        const QStringList dirs = QStandardPaths::locateAll(QStandardPaths::GenericDataLocation, m_resourceSubdir, QStandardPaths::LocateDirectory);
        qCDebug(SYCOCA) << "Looking for subdir" << m_resourceSubdir << "=>" << dirs;
        Q_FOREACH (const QString &dir, dirs) {
            QDirIterator it(dir, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                const QString filePath = it.next();
                Q_ASSERT(filePath.startsWith(dir)); // due to the line below...
                const QString relPath = filePath.mid(dir.length() + 1);
                relFiles.insert(relPath);
            }
        }
        // Now find all factories that use this resource....
        // For each factory
        KBSEntryDictList::const_iterator ed_it = entryDictList.constBegin();
        const KBSEntryDictList::const_iterator ed_end = entryDictList.constEnd();
        KSycocaFactoryList::const_iterator it = factories()->constBegin();
        const KSycocaFactoryList::const_iterator end = factories()->constEnd();
        for (; it != end; ++it, ++ed_it) {
            m_currentFactory = (*it);
            // m_ctimeInfo gets created after the initial loop, so it has no entryDict.
            m_currentEntryDict = ed_it == ed_end ? nullptr : *ed_it;
            // For each resource the factory deals with
            const KSycocaResourceList *list = m_currentFactory->resourceList();
            if (!list) {
                continue;
            }

            Q_FOREACH (const KSycocaResource &res, *list) {
                if (res.resource != (*it1)) {
                    continue;
                }

                // For each file in the resource
                for (auto entryPath = relFiles.constBegin();
                         entryPath != relFiles.constEnd();
                        ++entryPath) {
                    // Check if file matches filter
                    if ((*entryPath).endsWith(res.extension)) {
                        createEntry(*entryPath, true);
                    }
                }
            }
        }
        if (m_changed || !m_allEntries) {
            //qCDebug(SYCOCA) << "CHANGED:" << m_resource;
            m_changedResources.append(QString::fromLatin1(m_resource));
        }
    }

    bool result = true;
    const bool createVFolder = true; // we need to always run the VFolderMenu code
    if (createVFolder || m_menuTest) {
        m_resource = "apps";
        m_resourceSubdir = QStringLiteral("applications");
        m_currentFactory = d->m_serviceFactory;
        m_currentEntryDict = serviceEntryDict;
        m_changed = false;

        m_vfolder = new VFolderMenu(d->m_serviceFactory, this);
        if (!m_trackId.isEmpty()) {
            m_vfolder->setTrackId(m_trackId);
        }

        VFolderMenu::SubMenu *kdeMenu = m_vfolder->parseMenu(QStringLiteral(APPLICATIONS_MENU_NAME));

        KServiceGroup::Ptr entry = m_buildServiceGroupFactory->addNew(QStringLiteral("/"), kdeMenu->directoryFile, KServiceGroup::Ptr(), false);
        entry->setLayoutInfo(kdeMenu->layoutList);
        createMenu(QString(), QString(), kdeMenu);

        // Storing the mtime *after* looking at these dirs is a tiny race condition,
        // but I'm not sure how to get the vfolder dirs upfront...
        Q_FOREACH (QString dir, m_vfolder->allDirectories()) {
            if (dir.endsWith(QLatin1Char('/'))) {
                dir.chop(1); // remove trailing slash, to avoid having ~/.local/share/applications twice
            }
            if (!m_allResourceDirs.contains(dir)) {
                qint64 stamp = 0;
                KSycocaUtilsPrivate::visitResourceDirectory(dir, [&stamp] (const QFileInfo &info) {
                    stamp = qMax(stamp, info.lastModified().toMSecsSinceEpoch());
                    return true;
                });
                m_allResourceDirs.insert(dir, stamp);
            }
        }

        if (m_changed || !m_allEntries) {
            //qCDebug(SYCOCA) << "CHANGED:" << m_resource;
            m_changedResources.append(QString::fromLatin1(m_resource));
        }
        if (m_menuTest) {
            result = false;
        }
    }

    if (m_ctimeDict && !m_ctimeDict->isEmpty()) {
        qCDebug(SYCOCA) << "Still in time dict:";
        m_ctimeDict->dump();

        // Get the list of resources from which some files were deleted
        QStringList resources = m_ctimeDict->remainingResourceList();
        qCDebug(SYCOCA) << "Still in the time dict (i.e. deleted files)" << resources;
        m_changedResources += resources;
    }

    qDeleteAll(entryDictList);
    return result;
}

void KBuildSycoca::createMenu(const QString &caption_, const QString &name_, VFolderMenu::SubMenu *menu)
{
    QString caption = caption_;
    QString name = name_;
    foreach (VFolderMenu::SubMenu *subMenu, menu->subMenus) {
        QString subName = name + subMenu->name + QLatin1Char('/');

        QString directoryFile = subMenu->directoryFile;
        if (directoryFile.isEmpty()) {
            directoryFile = subName + QStringLiteral(".directory");
        }
        quint32 timeStamp = m_ctimeFactory->dict()->ctime(directoryFile, m_resource);
        if (!timeStamp) {
            timeStamp = calcResourceHash(m_resourceSubdir, directoryFile);
        }

        KServiceGroup::Ptr entry;
        if (m_allEntries) {
            const quint32 oldTimestamp = m_ctimeDict->ctime(directoryFile, m_resource);

            if (timeStamp && (timeStamp == oldTimestamp)) {
                KSycocaEntry::Ptr group = m_serviceGroupEntryDict->value(subName);
                if (group) {
                    entry = KServiceGroup::Ptr(static_cast<KServiceGroup*>(group.data()));
                    if (entry->directoryEntryPath() != directoryFile) {
                        entry = nullptr;    // Can't reuse this one!
                    }
                }
            }
        }
        if (timeStamp) { // bug? (see calcResourceHash). There might not be a .directory file...
            m_ctimeFactory->dict()->addCTime(directoryFile, m_resource, timeStamp);
        }

        entry = m_buildServiceGroupFactory->addNew(subName, subMenu->directoryFile, entry, subMenu->isDeleted);
        entry->setLayoutInfo(subMenu->layoutList);
        if (!(m_menuTest && entry->noDisplay())) {
            createMenu(caption + entry->caption() + QLatin1Char('/'), subName, subMenu);
        }
    }
    if (caption.isEmpty()) {
        caption += QLatin1Char('/');
    }
    if (name.isEmpty()) {
        name += QLatin1Char('/');
    }
    foreach (const KService::Ptr &p, menu->items) {
        if (m_menuTest) {
            if (!menu->isDeleted && !p->noDisplay())
                printf("%s\t%s\t%s\n", qPrintable(caption), qPrintable(p->menuId()),
                       qPrintable(QStandardPaths::locate(QStandardPaths::ApplicationsLocation, p->entryPath())));
        } else {
            m_buildServiceGroupFactory->addNewEntryTo(name, p);
        }
    }
}

bool KBuildSycoca::recreate(bool incremental)
{
    QFileInfo fi(KSycoca::absoluteFilePath(m_globalDatabase ? KSycoca::GlobalDatabase : KSycoca::LocalDatabase));
    if (!QDir().mkpath(fi.absolutePath())) {
        qCWarning(SYCOCA) << "Couldn't create" << fi.absolutePath();
        return false;
    }
    QString path(fi.absoluteFilePath());

    QLockFile lockFile(path + QLatin1String(".lock"));
    if (!lockFile.tryLock()) {
        qCDebug(SYCOCA) <<  "Waiting for already running" << KBUILDSYCOCA_EXENAME << "to finish.";
        if (!lockFile.lock()) {
            qCWarning(SYCOCA) << "Couldn't lock" << path + QStringLiteral(".lock");
            return false;
        }
        if (!needsRebuild()) {
            //qCDebug(SYCOCA) << "Up-to-date, skipping.";
            return true;
        }
    }

    QByteArray qSycocaPath = QFile::encodeName(path);
    s_cSycocaPath = qSycocaPath.data();

    m_allEntries = nullptr;
    m_ctimeDict = nullptr;
    if (incremental && checkGlobalHeader()) {
        qCDebug(SYCOCA) << "Reusing existing ksycoca";
        KSycoca *oldSycoca = KSycoca::self();
        m_allEntries = new KSycocaEntryListList;
        m_ctimeDict = new KCTimeDict;

        // Must be in same order as in KBuildSycoca::recreate()!
        m_allEntries->append(KSycocaPrivate::self()->serviceTypeFactory()->allEntries());
        m_allEntries->append(KSycocaPrivate::self()->mimeTypeFactory()->allEntries());
        m_allEntries->append(KSycocaPrivate::self()->serviceGroupFactory()->allEntries());
        m_allEntries->append(KSycocaPrivate::self()->serviceFactory()->allEntries());

        KCTimeFactory *ctimeInfo = new KCTimeFactory(oldSycoca);
        *m_ctimeDict = ctimeInfo->loadDict();
    }
    s_cSycocaPath = nullptr;

    QSaveFile database(path);
    bool openedOK = database.open(QIODevice::WriteOnly);

    if (!openedOK && database.error() == QFile::WriteError && QFile::exists(path)) {
        QFile::remove(path);
        openedOK = database.open(QIODevice::WriteOnly);
    }
    if (!openedOK) {
        qCWarning(SYCOCA) << "ERROR creating database" << path << ":" << database.errorString();
        return false;
    }

    QDataStream *str = new QDataStream(&database);
    str->setVersion(QDataStream::Qt_5_3);

    m_newTimestamp = QDateTime::currentMSecsSinceEpoch();
    qCDebug(SYCOCA).nospace() << "Recreating ksycoca file (" << path << ", version " << KSycoca::version() << ")";

    // It is very important to build the servicetype one first
    KBuildServiceTypeFactory *buildServiceTypeFactory = new KBuildServiceTypeFactory(this);
    d->m_serviceTypeFactory = buildServiceTypeFactory;
    KBuildMimeTypeFactory *buildMimeTypeFactory = new KBuildMimeTypeFactory(this);
    d->m_mimeTypeFactory = buildMimeTypeFactory;
    m_buildServiceGroupFactory = new KBuildServiceGroupFactory(this);
    d->m_serviceGroupFactory = m_buildServiceGroupFactory;
    d->m_serviceFactory = new KBuildServiceFactory(buildServiceTypeFactory, buildMimeTypeFactory, m_buildServiceGroupFactory);

    if (build()) { // Parse dirs
        save(str); // Save database
        if (str->status() != QDataStream::Ok) { // Probably unnecessary now in Qt5, since QSaveFile detects write errors
            database.cancelWriting();    // Error
        }
        delete str;
        str = nullptr;

        //if we are currently via sudo, preserve the original owner
        //as $HOME may also be that of another user rather than /root
#ifdef Q_OS_UNIX
        if (qEnvironmentVariableIsSet("SUDO_UID")) {
            const int uid = qEnvironmentVariableIntValue("SUDO_UID");
            const int gid = qEnvironmentVariableIntValue("SUDO_GID");
            if (uid && gid) {
                fchown(database.handle(), uid, gid);
            }
        }
#endif

        if (!database.commit()) {
            qCWarning(SYCOCA) << "ERROR writing database" << database.fileName() << ". Disk full?";
            return false;
        }

        if (!m_globalDatabase) {
            // Compatibility code for KF < 5.15: provide a ksycoca5 symlink after the filename change, for old apps to keep working during the upgrade
            const QString oldSycoca = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + QLatin1String("/ksycoca5");
            if (QFile::exists(oldSycoca)) {
                QFile::remove(oldSycoca);
#ifdef Q_OS_UNIX
                if (::link(QFile::encodeName(path).constData(), QFile::encodeName(oldSycoca).constData()) != 0) {
                    QFile::copy(path, oldSycoca);
                }
#else
                QFile::copy(path, oldSycoca);
#endif
            }
        }
    } else {
        delete str;
        str = nullptr;
        database.cancelWriting();
        if (m_menuTest) {
            return true;
        }
        qCDebug(SYCOCA) << "Database is up to date";
    }

    if (m_globalDatabase) {
        // These directories may have been created with 0700 permission
        // better delete them if they are empty
        QString appsDir = QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation);
        QDir().remove(appsDir);
        // was doing the same with servicetypes, but I don't think any of these gets created-by-mistake anymore.
    }
    if (d->m_sycocaStrategy == KSycocaPrivate::StrategyMemFile) {
        KMemFile::fileContentsChanged(path);
    }

    delete m_ctimeDict;
    delete m_allEntries;
    delete m_vfolder;

    return true;
}

void KBuildSycoca::save(QDataStream *str)
{
    // Write header (#pass 1)
    str->device()->seek(0);

    (*str) << qint32(KSycoca::version());
    //KSycocaFactory * servicetypeFactory = 0;
    //KBuildMimeTypeFactory * mimeTypeFactory = 0;
    KBuildServiceFactory *serviceFactory = nullptr;
    Q_FOREACH (KSycocaFactory* factory, *factories()) {
        qint32 aId;
        qint32 aOffset;
        aId = factory->factoryId();
        //if ( aId == KST_KServiceTypeFactory )
        //   servicetypeFactory = factory;
        //else if ( aId == KST_KMimeTypeFactory )
        //   mimeTypeFactory = static_cast<KBuildMimeTypeFactory *>( factory );
        if (aId == KST_KServiceFactory) {
            serviceFactory = static_cast<KBuildServiceFactory *>(factory);
        }
        aOffset = factory->offset(); // not set yet, so always 0
        (*str) << aId;
        (*str) << aOffset;
    }
    (*str) << qint32(0); // No more factories.
    // Write XDG_DATA_DIRS
    (*str) << QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation).join(QString(QLatin1Char(':')));
    (*str) << m_newTimestamp;
    (*str) << QLocale().bcp47Name();
    // This makes it possible to trigger a ksycoca update for all users (KIOSK feature)
    (*str) << calcResourceHash(QStringLiteral("kservices5"), QStringLiteral("update_ksycoca"));
    (*str) << m_allResourceDirs.keys();
    for (auto it = m_allResourceDirs.constBegin(); it != m_allResourceDirs.constEnd(); ++it) {
        (*str) << it.value();
    }

    // Calculate per-servicetype/mimetype data
    if (serviceFactory) serviceFactory->postProcessServices();

    // Here so that it's the last debug message
    qCDebug(SYCOCA) << "Saving";

    // Write factory data....
    Q_FOREACH (KSycocaFactory* factory, *factories()) {
        factory->save(*str);
        if (str->status() != QDataStream::Ok) { // ######## TODO: does this detect write errors, e.g. disk full?
            return;    // error
        }
    }

    qint64 endOfData = str->device()->pos();

    // Write header (#pass 2)
    str->device()->seek(0);

    (*str) << qint32(KSycoca::version());
    Q_FOREACH (KSycocaFactory* factory, *factories()) {
        qint32 aId;
        qint32 aOffset;
        aId = factory->factoryId();
        aOffset = factory->offset();
        (*str) << aId;
        (*str) << aOffset;
    }
    (*str) << qint32(0); // No more factories.

    // Jump to end of database
    str->device()->seek(endOfData);
}

QStringList KBuildSycoca::factoryResourceDirs()
{
    static QStringList *dirs = nullptr;
    if (dirs != nullptr) {
        return *dirs;
    }
    dirs = new QStringList;
    // these are all resource dirs cached by ksycoca
    *dirs += KServiceTypeFactory::resourceDirs();
    *dirs += KMimeTypeFactory::resourceDirs();
    *dirs += KServiceFactory::resourceDirs();

    return *dirs;
}

QStringList KBuildSycoca::existingResourceDirs()
{
    static QStringList *dirs = nullptr;
    if (dirs != nullptr) {
        return *dirs;
    }
    dirs = new QStringList(factoryResourceDirs());

    for (QStringList::Iterator it = dirs->begin();
            it != dirs->end();) {
        QFileInfo inf(*it);
        if (!inf.exists() || !inf.isReadable()) {
            it = dirs->erase(it);
        } else {
            ++it;
        }
    }
    return *dirs;
}

static quint32 updateHash(const QString &file, quint32 hash)
{
    QFileInfo fi(file);
    if (fi.isReadable() && fi.isFile()) {
        // This was using buff.st_ctime (in Waldo's initial commit to kstandarddirs.cpp in 2001), but that looks wrong?
        // Surely we want to catch manual editing, while a chmod doesn't matter much?
        hash += fi.lastModified().toTime_t();
    }
    return hash;
}

quint32 KBuildSycoca::calcResourceHash(const QString &resourceSubDir, const QString &filename)
{
    quint32 hash = 0;
    if (!QDir::isRelativePath(filename)) {
        return updateHash(filename, hash);
    }
    const QStringList files = QStandardPaths::locateAll(QStandardPaths::GenericDataLocation, resourceSubDir + QLatin1Char('/') + filename);
    Q_FOREACH (const QString &file, files) {
        hash = updateHash(file, hash);
    }
    if (hash == 0 && !filename.endsWith(QLatin1String("update_ksycoca"))
            && !filename.endsWith(QLatin1String(".directory")) // bug? needs investigation from someone who understands the VFolder spec
       ) {
        qCWarning(SYCOCA) << "File not found or not readable:" << filename << "found:" << files;
        Q_ASSERT(hash != 0);
    }
    return hash;
}

bool KBuildSycoca::checkGlobalHeader()
{
    // Since it's part of the filename, we are 99% sure that the locale and prefixes will match.
    const QString current_language = QLocale().bcp47Name();
    const quint32 current_update_sig = KBuildSycoca::calcResourceHash(QStringLiteral("kservices5"), QStringLiteral("update_ksycoca"));
    const QString current_prefixes = QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation).join(QString(QLatin1Char(':')));

    const KSycocaHeader header = KSycocaPrivate::self()->readSycocaHeader();
    Q_ASSERT(!header.prefixes.split(QLatin1Char(':')).contains(QDir::homePath()));

    return (current_update_sig == header.updateSignature) &&
            (current_language == header.language) &&
            (current_prefixes == header.prefixes) &&
            (header.timeStamp != 0);
}

const char *KBuildSycoca::sycocaPath()
{
    return s_cSycocaPath;
}
