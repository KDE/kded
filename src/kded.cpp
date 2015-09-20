/*  This file is part of the KDE libraries
 *  Copyright (C) 1999 David Faure <faure@kde.org>
 *  Copyright (C) 2000 Waldo Bastian <bastian@kde.org>
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

#include "kded.h"
#include "kdedadaptor.h"
#include "kded_version.h"

#include <kcrash.h>

#include <qplatformdefs.h>

#include <QDebug>
#include <QLoggingCategory>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QTimer>
#include <QApplication>

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusServiceWatcher>
#include <QDBusPendingCall>

#include <KDBusService>
#include <kconfiggroup.h>
#include <ksharedconfig.h>
#include <KDirWatch>
#include <KServiceTypeTrader>
#include <KToolInvocation>
#include <KPluginInfo>
#include <KPluginMetaData>

Q_DECLARE_LOGGING_CATEGORY(KDED);

#if QT_VERSION >= QT_VERSION_CHECK(5, 4, 0)
// logging category for this framework, default: log stuff >= warning
Q_LOGGING_CATEGORY(KDED, "kf5.kded", QtWarningMsg)
#else
Q_LOGGING_CATEGORY(KDED, "kf5.kded")
#endif

Kded *Kded::_self = 0;

static bool delayedCheck;
static bool bCheckSycoca;
static bool bCheckUpdates;

#ifdef Q_DBUS_EXPORT
extern Q_DBUS_EXPORT void qDBusAddSpyHook(void (*)(const QDBusMessage &));
#else
extern QDBUS_EXPORT void qDBusAddSpyHook(void (*)(const QDBusMessage &));
#endif

static void runKonfUpdate()
{
    KToolInvocation::kdeinitExecWait(QStringLiteral(KCONF_UPDATE_EXE),
            QStringList(), 0, 0, "0" /*no startup notification*/);
}

Kded::Kded()
    : m_pDirWatch(0)
    , m_pTimer(new QTimer(this))
    , m_needDelayedCheck(false)
{
    _self = this;

    m_serviceWatcher = new QDBusServiceWatcher(this);
    m_serviceWatcher->setConnection(QDBusConnection::sessionBus());
    m_serviceWatcher->setWatchMode(QDBusServiceWatcher::WatchForUnregistration);
    QObject::connect(m_serviceWatcher, &QDBusServiceWatcher::serviceUnregistered,
                     this, &Kded::slotApplicationRemoved);

    new KBuildsycocaAdaptor(this);
    new KdedAdaptor(this);

    QDBusConnection session = QDBusConnection::sessionBus();
    session.registerObject(QStringLiteral("/kbuildsycoca"), this);
    session.registerObject(QStringLiteral("/kded"), this);

    qDBusAddSpyHook(messageFilter);

    m_pTimer->setSingleShot(true);
    connect(m_pTimer, &QTimer::timeout, this, static_cast<void(Kded::*)()>(&Kded::recreate));
}

Kded::~Kded()
{
    _self = 0;
    m_pTimer->stop();
    delete m_pTimer;
    delete m_pDirWatch;

    for (QHash<QString, KDEDModule *>::const_iterator
            it(m_modules.constBegin()), itEnd(m_modules.constEnd());
            it != itEnd; ++it) {
        KDEDModule *module(it.value());

        // first disconnect otherwise slotKDEDModuleRemoved() is called
        // and changes m_modules while we're iterating over it
        disconnect(module, &KDEDModule::moduleDeleted,
                   this, &Kded::slotKDEDModuleRemoved);

        delete module;
    }
}

// on-demand module loading
// this function is called by the D-Bus message processing function before
// calls are delivered to objects
void Kded::messageFilter(const QDBusMessage &message)
{
    // This happens when kded goes down and some modules try to clean up.
    if (!self()) {
        return;
    }

    QString obj = KDEDModule::moduleForMessage(message);
    if (obj.isEmpty() || obj == QLatin1String("ksycoca")) {
        return;
    }

    if (self()->m_dontLoad.value(obj, 0)) {
        return;
    }

    KDEDModule *module = self()->loadModule(obj, true);
    if (!module) {
        qCWarning(KDED) << "Failed to load module for " << obj;
    }
    Q_UNUSED(module);
}

static int phaseForModule(const KPluginMetaData &module)
{
    const QVariant phasev = module.rawData().value("X-KDE-Kded-phase").toVariant();
    return phasev.isValid() ? phasev.toInt() : 2;
}

QVector<KPluginMetaData> Kded::availableModules() const
{
    QVector<KPluginMetaData> plugins = KPluginLoader::findPlugins("kf5/kded");
    QSet<QString> moduleIds;
    foreach (const KPluginMetaData &md, plugins) {
        moduleIds.insert(md.pluginId());
    }
    // also search for old .desktop based kded modules
    KPluginInfo::List oldStylePlugins = KPluginInfo::fromServices(KServiceTypeTrader::self()->query("KDEDModule"));
    foreach (const KPluginInfo &info, oldStylePlugins) {
        if (moduleIds.contains(info.pluginName())) {
            qCWarning(KDED).nospace() << "kded module " << info.pluginName() << " has already been found using "
                "JSON metadata, please don't install the now unneeded .desktop file (" << info.entryPath() << ").";
        } else {
            qCDebug(KDED).nospace() << "kded module " << info.pluginName() << " still uses .desktop files ("
                << info.entryPath() << "). Please port it to JSON metadata.";
            plugins.append(info.toMetaData());
        }
    }
    return plugins;
}

static KPluginMetaData findModule(const QString &id)
{
    KPluginMetaData module(QStringLiteral("kf5/kded/") + id);
    if (module.isValid()) {
        return module;
    }
    // TODO KF6: remove the .desktop fallback code
    KService::Ptr oldStyleModule = KService::serviceByDesktopPath(QStringLiteral("kded/") + id + QStringLiteral(".desktop"));
    if (oldStyleModule) {
        qCDebug(KDED).nospace() << "kded module " << oldStyleModule->desktopEntryName()
            << " still uses .desktop files (" << oldStyleModule->entryPath() << "). Please port it to JSON metadata.";
        return KPluginInfo(oldStyleModule).toMetaData();
    }
    qCWarning(KDED) << "could not find kded module with id" << id;
    return KPluginMetaData();
}

void Kded::initModules()
{
    m_dontLoad.clear();
    bool kde_running = !qgetenv("KDE_FULL_SESSION").isEmpty();
    if (kde_running) {
#ifndef Q_OS_WIN
        // not the same user like the one running the session (most likely we're run via sudo or something)
        const QByteArray sessionUID = qgetenv("KDE_SESSION_UID");
        if (!sessionUID.isEmpty() && uid_t(sessionUID.toInt()) != getuid()) {
            kde_running = false;
        }
#endif
        //TODO: Change 5 to KDED_VERSION_MAJOR the moment KF5 are stable
        // not the same kde version as the current desktop
        const QByteArray kdeSession = qgetenv("KDE_SESSION_VERSION");
        if (kdeSession.toInt() != 5) {
            kde_running = false;
        }
    }

    // There will be a "phase 2" only if we're in the KDE startup.
    // If kded is restarted by its crashhandled or by hand,
    // then there will be no second phase autoload, so load
    // these modules now, if in a KDE session.
    const bool loadPhase2Now = (kde_running && qgetenv("KDED_STARTED_BY_KDEINIT").toInt() == 0);

    // Preload kded modules.
    const QVector<KPluginMetaData> kdedModules = availableModules();
    foreach (const KPluginMetaData &module, kdedModules) {
        // Should the service load on startup?
        const bool autoload = isModuleAutoloaded(module);

        // see ksmserver's README for description of the phases
        bool prevent_autoload = false;
        switch (phaseForModule(module)) {
        case 0: // always autoload
            break;
        case 1: // autoload only in KDE
            if (!kde_running) {
                prevent_autoload = true;
            }
            break;
        case 2: // autoload delayed, only in KDE
        default:
            if (!loadPhase2Now) {
                prevent_autoload = true;
            }
            break;
        }

        // Load the module if necessary and allowed
        if (autoload && !prevent_autoload) {
            if (!loadModule(module, false)) {
                continue;
            }
        }

        // Remember if the module is allowed to load on demand
        bool loadOnDemand = isModuleLoadedOnDemand(module);
        if (!loadOnDemand) {
            noDemandLoad(module.pluginId());
        }

        // In case of reloading the configuration it is possible for a module
        // to run even if it is now allowed to. Stop it then.
        if (!loadOnDemand && !autoload) {
            unloadModule(module.pluginId());
        }
    }
}

void Kded::loadSecondPhase()
{
    qCDebug(KDED) << "Loading second phase autoload";
    KSharedConfig::Ptr config = KSharedConfig::openConfig();
    QVector<KPluginMetaData> kdedModules = availableModules();
    foreach (const KPluginMetaData &module, kdedModules) {
        const bool autoload = isModuleAutoloaded(module);
        if (autoload && phaseForModule(module) == 2) {
            qCDebug(KDED) << "2nd phase: loading" << module.pluginId();
            loadModule(module, false);
        }
    }
}

void Kded::noDemandLoad(const QString &obj)
{
    m_dontLoad.insert(obj.toLatin1(), this);
}

void Kded::setModuleAutoloading(const QString &obj, bool autoload)
{
    KSharedConfig::Ptr config = KSharedConfig::openConfig();
    // Ensure the service exists.
    KPluginMetaData module = findModule(obj);
    if (!module.isValid()) {
        return;
    }
    KConfigGroup cg(config, QString("Module-").append(module.pluginId()));
    cg.writeEntry("autoload", autoload);
    cg.sync();
}

bool Kded::isModuleAutoloaded(const QString &obj) const
{
    return isModuleAutoloaded(findModule(obj));
}

bool Kded::isModuleAutoloaded(const KPluginMetaData &module) const
{
    if (!module.isValid()) {
        return false;
    }
    KSharedConfig::Ptr config = KSharedConfig::openConfig();
    bool autoload = module.rawData().value(QStringLiteral("X-KDE-Kded-autoload")).toVariant().toBool();
    KConfigGroup cg(config, QString("Module-").append(module.pluginId()));
    autoload = cg.readEntry("autoload", autoload);
    return autoload;
}

bool Kded::isModuleLoadedOnDemand(const QString &obj) const
{
    return isModuleLoadedOnDemand(findModule(obj));
}

bool Kded::isModuleLoadedOnDemand(const KPluginMetaData &module) const
{
    if (!module.isValid()) {
        return false;
    }
    KSharedConfig::Ptr config = KSharedConfig::openConfig();
    bool loadOnDemand = true;
    // use toVariant() since it could be string or bool in the json and QJsonObject does not convert
    QVariant p = module.rawData().value(QStringLiteral("X-KDE-Kded-load-on-demand")).toVariant();
    if (p.isValid() && (p.toBool() == false)) {
        loadOnDemand = false;
    }
    return loadOnDemand;
}

KDEDModule *Kded::loadModule(const QString &obj, bool onDemand)
{
    // Make sure this method is only called with valid module names.
    if (obj.contains('/')) {
        qCWarning(KDED) << "attempting to load invalid kded module name:" << obj;
        return nullptr;
    }
    KDEDModule *module = m_modules.value(obj, 0);
    if (module) {
        return module;
    }
    return loadModule(findModule(obj), onDemand);
}

KDEDModule *Kded::loadModule(const KPluginMetaData &module, bool onDemand)
{
    if (!module.isValid() || module.fileName().isEmpty()) {
        qCWarning(KDED) << "attempted to load an invalid module.";
        return nullptr;
    }
    const QString moduleId = module.pluginId();
    KDEDModule *oldModule = m_modules.value(moduleId, 0);
    if (oldModule) {
        qCDebug(KDED) << "kded module" << moduleId << "is already loaded.";
        return oldModule;
    }

    if (onDemand) {
        // use toVariant() since it could be string or bool in the json and QJsonObject does not convert
        QVariant p = module.rawData().value(QStringLiteral("X-KDE-Kded-load-on-demand")).toVariant();
        if (p.isValid() && (p.toBool() == false)) {
            noDemandLoad(moduleId);
            return nullptr;
        }
    }

    KDEDModule *kdedModule = nullptr;

    KPluginLoader loader(module.fileName());
    KPluginFactory *factory = loader.factory();
    if (factory) {
        kdedModule = factory->create<KDEDModule>(this);
    } else {
        // TODO: remove this fallback code, the kded modules should all be fixed instead
        KPluginLoader loader2("kded_" + module.fileName());
        factory = loader2.factory();
        if (factory) {
            qCWarning(KDED).nospace() << "found kded module " << moduleId
                << " by prepending 'kded_' to the library path, please fix your metadata.";
            kdedModule = factory->create<KDEDModule>(this);
        } else {
            qCWarning(KDED).nospace() << "Could not load kded module " << moduleId << ":"
                << loader.errorString() << " (library path was:" << module.fileName() << ")";
        }
    }

    if (kdedModule) {
        kdedModule->setModuleName(moduleId);
        m_modules.insert(moduleId, kdedModule);
        //m_libs.insert(moduleId, lib);
        connect(kdedModule, &KDEDModule::moduleDeleted, this, &Kded::slotKDEDModuleRemoved);
        qCDebug(KDED) << "Successfully loaded module" << moduleId;
        return kdedModule;
    }
    return nullptr;
}

bool Kded::unloadModule(const QString &obj)
{
    KDEDModule *module = m_modules.value(obj, 0);
    if (!module) {
        return false;
    }
    qCDebug(KDED) << "Unloading module" << obj;
    m_modules.remove(obj);
    delete module;
    return true;
}

QStringList Kded::loadedModules()
{
    return m_modules.keys();
}

void Kded::slotKDEDModuleRemoved(KDEDModule *module)
{
    m_modules.remove(module->moduleName());
}

void Kded::slotApplicationRemoved(const QString &name)
{
#if 0 // see kdedmodule.cpp (KDED_OBJECTS)
    foreach (KDEDModule *module, m_modules) {
        module->removeAll(appId);
    }
#endif
    m_serviceWatcher->removeWatchedService(name);
    const QList<qlonglong> windowIds = m_windowIdList.value(name);
    for (QList<qlonglong>::ConstIterator it = windowIds.begin();
            it != windowIds.end(); ++it) {
        qlonglong windowId = *it;
        m_globalWindowIdList.remove(windowId);
        foreach (KDEDModule *module, m_modules) {
            emit module->windowUnregistered(windowId);
        }
    }
    m_windowIdList.remove(name);
}

void Kded::updateDirWatch()
{
    if (!bCheckUpdates) {
        return;
    }

    delete m_pDirWatch;
    m_pDirWatch = new KDirWatch;

    QObject::connect(m_pDirWatch, &KDirWatch::dirty,
                     this, &Kded::update);
    QObject::connect(m_pDirWatch, &KDirWatch::created,
                     this, &Kded::update);
    QObject::connect(m_pDirWatch, &KDirWatch::deleted,
                     this, &Kded::dirDeleted);

    // For each resource
    for (QStringList::ConstIterator it = m_allResourceDirs.constBegin();
            it != m_allResourceDirs.constEnd();
            ++it) {
        readDirectory(*it);
    }
}

void Kded::updateResourceList()
{
    KSycoca::clearCaches();

    if (!bCheckUpdates) {
        return;
    }

    if (delayedCheck) {
        return;
    }

    const QStringList dirs = KSycoca::self()->allResourceDirs();
    // For each resource
    for (QStringList::ConstIterator it = dirs.begin();
            it != dirs.end();
            ++it) {
        if (!m_allResourceDirs.contains(*it)) {
            m_allResourceDirs.append(*it);
            readDirectory(*it);
        }
    }
}

void Kded::recreate()
{
    recreate(false);
}

void Kded::runDelayedCheck()
{
    if (m_needDelayedCheck) {
        recreate(false);
    }
    m_needDelayedCheck = false;
}

void Kded::recreate(bool initial)
{
    // Using KLauncher here is difficult since we might not have a
    // database

    if (!initial) {
        updateDirWatch(); // Update tree first, to be sure to miss nothing.
        KSycoca::self()->ensureCacheValid();
        recreateDone();
    } else {
        if (!delayedCheck) {
            updateDirWatch();    // this would search all the directories
        }
        if (bCheckSycoca) {
            KSycoca::self()->ensureCacheValid();
        }
        recreateDone();
        if (delayedCheck) {
            // do a proper ksycoca check after a delay
            QTimer::singleShot(60000, this, SLOT(runDelayedCheck()));
            m_needDelayedCheck = true;
            delayedCheck = false;
        } else {
            m_needDelayedCheck = false;
        }
    }
}

void Kded::recreateDone()
{
    updateResourceList();

    initModules();
}

void Kded::dirDeleted(const QString &path)
{
    update(path);
}

void Kded::update(const QString &)
{
    m_pTimer->start(10000);
}

void Kded::readDirectory(const QString &_path)
{
    QString path(_path);
    if (!path.endsWith(QLatin1Char('/'))) {
        path += QLatin1Char('/');
    }

    if (m_pDirWatch->contains(path)) {   // Already seen this one?
        return;
    }

    Q_ASSERT(path != QDir::homePath());
    m_pDirWatch->addDir(path, KDirWatch::WatchFiles | KDirWatch::WatchSubDirs);       // add watch on this dir
}

void Kded::registerWindowId(qlonglong windowId, const QString &sender)
{
    if (!m_windowIdList.contains(sender)) {
        m_serviceWatcher->addWatchedService(sender);
    }

    m_globalWindowIdList.insert(windowId);
    QList<qlonglong> windowIds = m_windowIdList.value(sender);
    windowIds.append(windowId);
    m_windowIdList.insert(sender, windowIds);

    foreach (KDEDModule *module, m_modules) {
        qCDebug(KDED) << module->moduleName();
        emit module->windowRegistered(windowId);
    }
}

void Kded::unregisterWindowId(qlonglong windowId, const QString &sender)
{
    m_globalWindowIdList.remove(windowId);
    QList<qlonglong> windowIds = m_windowIdList.value(sender);
    if (!windowIds.isEmpty()) {
        windowIds.removeAll(windowId);
        if (windowIds.isEmpty()) {
            m_serviceWatcher->removeWatchedService(sender);
            m_windowIdList.remove(sender);
        } else {
            m_windowIdList.insert(sender, windowIds);
        }
    }

    foreach (KDEDModule *module, m_modules) {
        qCDebug(KDED) << module->moduleName();
        emit module->windowUnregistered(windowId);
    }
}

static void sighandler(int /*sig*/)
{
    if (qApp) {
        qApp->quit();
    }
}

KUpdateD::KUpdateD()
{
    m_pDirWatch = new KDirWatch(this);
    m_pTimer = new QTimer(this);
    m_pTimer->setSingleShot(true);
    connect(m_pTimer, &QTimer::timeout, this, &KUpdateD::runKonfUpdate);
    QObject::connect(m_pDirWatch, &KDirWatch::dirty,
                     this, &KUpdateD::slotNewUpdateFile);

    const QStringList dirs = QStandardPaths::locateAll(QStandardPaths::GenericDataLocation, "kconf_update", QStandardPaths::LocateDirectory);
    for (QStringList::ConstIterator it = dirs.begin();
            it != dirs.end();
            ++it) {
        QString path = *it;
        Q_ASSERT(path != QDir::homePath());
        if (path[path.length() - 1] != '/') {
            path += '/';
        }

        if (!m_pDirWatch->contains(path)) {
            m_pDirWatch->addDir(path, KDirWatch::WatchFiles);
        }
    }
}

KUpdateD::~KUpdateD()
{
}

void KUpdateD::runKonfUpdate()
{
    ::runKonfUpdate();
}

void KUpdateD::slotNewUpdateFile(const QString &dirty)
{
    Q_UNUSED(dirty);
    qCDebug(KDED) << dirty;
    m_pTimer->start(500);
}

KBuildsycocaAdaptor::KBuildsycocaAdaptor(QObject *parent)
    : QDBusAbstractAdaptor(parent)
{
}

void KBuildsycocaAdaptor::recreate()
{
    Kded::self()->recreate();
}

// KF6: remove
bool KBuildsycocaAdaptor::isTestModeEnabled()
{
    return QStandardPaths::isTestModeEnabled();
}

// KF6: remove
void KBuildsycocaAdaptor::enableTestMode()
{
    QStandardPaths::enableTestMode(true);
}

static void setupAppInfo(QCoreApplication *app)
{
    app->setApplicationName("kded5");
    app->setOrganizationDomain("kde.org");
}

extern "C" Q_DECL_EXPORT int kdemain(int argc, char *argv[])
{
    //options.add("check", qi18n("Check Sycoca database only once"));

    // WABA: Make sure not to enable session management.
    putenv(qstrdup("SESSION_MANAGER="));

    // Parse command line before checking D-Bus
    if (argc > 1 && QByteArray(argv[1]) == "--check") {
        // KDBusService not wanted here.
        QCoreApplication app(argc, argv);
        setupAppInfo(&app);

        KSycoca::self()->ensureCacheValid();
        runKonfUpdate();
        return 0;
    }

    QApplication app(argc, argv);
    setupAppInfo(&app);
    app.setApplicationDisplayName("KDE Daemon");
    app.setQuitOnLastWindowClosed(false);

    KDBusService service(KDBusService::Unique);

    KSharedConfig::Ptr config = KSharedConfig::openConfig();
    KConfigGroup cg(config, "General");

    bCheckSycoca = cg.readEntry("CheckSycoca", true);
    bCheckUpdates = cg.readEntry("CheckUpdates", true);
    delayedCheck = cg.readEntry("DelayedCheck", false);

#ifndef Q_OS_WIN
    signal(SIGTERM, sighandler);
    signal(SIGHUP, sighandler);
#endif

    KCrash::setFlags(KCrash::AutoRestart);

    Kded *kded = new Kded();

    kded->recreate(true); // initial

    if (bCheckUpdates) {
        (void) new KUpdateD;    // Watch for updates
    }

//NOTE: We are going to change how KDE starts and this certanly doesn't fit on the new design.
#ifdef Q_OS_LINUX
    // Tell KSplash that KDED has started
    QDBusMessage ksplashProgressMessage = QDBusMessage::createMethodCall(QStringLiteral("org.kde.KSplash"),
                                          QStringLiteral("/KSplash"),
                                          QStringLiteral("org.kde.KSplash"),
                                          QStringLiteral("setStage"));
    ksplashProgressMessage.setArguments(QList<QVariant>() << QStringLiteral("kded"));
    QDBusConnection::sessionBus().asyncCall(ksplashProgressMessage);
#endif

    runKonfUpdate(); // Run it once.

#ifdef Q_OS_LINUX
    ksplashProgressMessage = QDBusMessage::createMethodCall(QStringLiteral("org.kde.KSplash"),
                             QStringLiteral("/KSplash"),
                             QStringLiteral("org.kde.KSplash"),
                             QStringLiteral("setStage"));
    ksplashProgressMessage.setArguments(QList<QVariant>() << QStringLiteral("confupdate"));
    QDBusConnection::sessionBus().asyncCall(ksplashProgressMessage);
#endif

    int result = app.exec(); // keep running

    delete kded;

    return result;
}

