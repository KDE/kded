/*
    This file is part of the KDE libraries
    SPDX-FileCopyrightText: 1999 David Faure <faure@kde.org>
    SPDX-FileCopyrightText: 2000 Waldo Bastian <bastian@kde.org>

    SPDX-License-Identifier: LGPL-2.0-only
*/

#include "kded.h"
#include "kded_version.h"
#include "kdedadaptor.h"

#include <KCrash>

#include <qplatformdefs.h>

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QLoggingCategory>
#include <QProcess>

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusServiceWatcher>

#include <KConfigGroup>
#include <KDBusService>
#include <KDirWatch>
#include <KPluginFactory>
#include <KPluginInfo>
#include <KPluginMetaData>
#include <KServiceTypeTrader>
#include <KSharedConfig>

#ifdef Q_OS_OSX
#include <CoreFoundation/CoreFoundation.h>
#endif

#include <memory>

Q_DECLARE_LOGGING_CATEGORY(KDED)

Q_LOGGING_CATEGORY(KDED, "kf.kded", QtWarningMsg)

Kded *Kded::_self = nullptr;

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
    QProcess kconfUpdate;
    kconfUpdate.start(QStringLiteral(KCONF_UPDATE_EXE), QStringList());
    kconfUpdate.waitForFinished();
}

Kded::Kded()
    : m_pDirWatch(new KDirWatch(this))
    , m_pTimer(new QTimer(this))
    , m_needDelayedCheck(false)
{
    _self = this;

    m_serviceWatcher = new QDBusServiceWatcher(this);
    m_serviceWatcher->setConnection(QDBusConnection::sessionBus());
    m_serviceWatcher->setWatchMode(QDBusServiceWatcher::WatchForUnregistration);
    QObject::connect(m_serviceWatcher, &QDBusServiceWatcher::serviceUnregistered, this, &Kded::slotApplicationRemoved);

    new KBuildsycocaAdaptor(this);
    new KdedAdaptor(this);

    QDBusConnection session = QDBusConnection::sessionBus();
    session.registerObject(QStringLiteral("/kbuildsycoca"), this);
    session.registerObject(QStringLiteral("/kded"), this);

    qDBusAddSpyHook(messageFilter);

    m_pTimer->setSingleShot(true);
    connect(m_pTimer, &QTimer::timeout, this, static_cast<void (Kded::*)()>(&Kded::recreate));
}

Kded::~Kded()
{
    _self = nullptr;
    m_pTimer->stop();

    for (auto it = m_modules.cbegin(); it != m_modules.cend(); ++it) {
        KDEDModule *module(it.value());

        // first disconnect otherwise slotKDEDModuleRemoved() is called
        // and changes m_modules while we're iterating over it
        disconnect(module, &KDEDModule::moduleDeleted, this, &Kded::slotKDEDModuleRemoved);

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

    if (self()->m_dontLoad.value(obj, nullptr)) {
        return;
    }

    self()->loadModule(obj, true);
}

static int phaseForModule(const KPluginMetaData &module)
{
    return module.value(QStringLiteral("X-KDE-Kded-phase"), 2);
}

QVector<KPluginMetaData> Kded::availableModules() const
{
    QVector<KPluginMetaData> plugins = KPluginMetaData::findPlugins(QStringLiteral("kf" QT_STRINGIFY(QT_VERSION_MAJOR) "/kded"));
    QSet<QString> moduleIds;
    for (const KPluginMetaData &md : std::as_const(plugins)) {
        moduleIds.insert(md.pluginId());
    }
#if KSERVICE_BUILD_DEPRECATED_SINCE(5, 0)
    // also search for old .desktop based kded modules
    QT_WARNING_PUSH
    QT_WARNING_DISABLE_CLANG("-Wdeprecated-declarations")
    QT_WARNING_DISABLE_GCC("-Wdeprecated-declarations")
    const KPluginInfo::List oldStylePlugins = KPluginInfo::fromServices(KServiceTypeTrader::self()->query(QStringLiteral("KDEDModule")));
    QT_WARNING_POP
    for (const KPluginInfo &info : oldStylePlugins) {
        if (moduleIds.contains(info.pluginName())) {
            qCWarning(KDED).nospace() << "kded module " << info.pluginName()
                                      << " has already been found using "
                                         "JSON metadata, please don't install the now unneeded .desktop file ("
                                      << info.entryPath() << ").";
        } else {
            qCDebug(KDED).nospace() << "kded module " << info.pluginName() << " still uses .desktop files (" << info.entryPath()
                                    << "). Please port it to JSON metadata.";
            plugins.append(info.toMetaData());
        }
    }
#endif
    return plugins;
}

static KPluginMetaData findModule(const QString &id)
{
    KPluginMetaData module(QStringLiteral("kf" QT_STRINGIFY(QT_VERSION_MAJOR) "/kded/") + id);
    if (module.isValid()) {
        return module;
    }
#if KSERVICE_BUILD_DEPRECATED_SINCE(5, 0)
    // TODO KF6: remove the .desktop fallback code
    KService::Ptr oldStyleModule = KService::serviceByDesktopPath(QStringLiteral("kded/") + id + QStringLiteral(".desktop"));
    if (oldStyleModule) {
        qCDebug(KDED).nospace() << "kded module " << oldStyleModule->desktopEntryName() << " still uses .desktop files (" << oldStyleModule->entryPath()
                                << "). Please port it to JSON metadata.";
        QT_WARNING_PUSH
        QT_WARNING_DISABLE_CLANG("-Wdeprecated-declarations")
        QT_WARNING_DISABLE_GCC("-Wdeprecated-declarations")
        return KPluginInfo(oldStyleModule).toMetaData();
        QT_WARNING_POP
    }
#endif
    qCWarning(KDED) << "could not find kded module with id" << id;
    return KPluginMetaData();
}

void Kded::initModules()
{
    m_dontLoad.clear();
#ifdef Q_OS_OSX
    // it seems there is no reason to honour KDE_FULL_SESSION on OS X even if it's set for some reason.
    // That way kdeinit5 is always auto-started if required (and the kded is as good a candidate
    // for starting this service as any other application).
    bool kde_running = false;
#else
    bool kde_running = !qEnvironmentVariableIsEmpty("KDE_FULL_SESSION");
#endif
    if (kde_running) {
#ifndef Q_OS_WIN
        // not the same user like the one running the session (most likely we're run via sudo or something)
        const QByteArray sessionUID = qgetenv("KDE_SESSION_UID");
        if (!sessionUID.isEmpty() && uid_t(sessionUID.toInt()) != getuid()) {
            kde_running = false;
        }
#endif
        // TODO: Change 5 to KDED_VERSION_MAJOR the moment KF5 are stable
        // not the same kde version as the current desktop
        const QByteArray kdeSession = qgetenv("KDE_SESSION_VERSION");
        if (kdeSession.toInt() != 5) {
            kde_running = false;
        }
    }

    // Preload kded modules.
    const QVector<KPluginMetaData> kdedModules = availableModules();
    for (const KPluginMetaData &module : kdedModules) {
        // Should the service load on startup?
        const bool autoload = isModuleAutoloaded(module);
        if (!platformSupportsModule(module)) {
            continue;
        }

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
            if (!kde_running) {
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
    qCDebug(KDED) << "Second phase autoload is deprecated";
}

void Kded::noDemandLoad(const QString &obj)
{
    m_dontLoad.insert(obj, this);
}

void Kded::setModuleAutoloading(const QString &obj, bool autoload)
{
    KSharedConfig::Ptr config = KSharedConfig::openConfig();
    // Ensure the service exists.
    KPluginMetaData module = findModule(obj);
    if (!module.isValid()) {
        return;
    }
    KConfigGroup cg(config, QStringLiteral("Module-").append(module.pluginId()));
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
    bool autoload = module.value(QStringLiteral("X-KDE-Kded-autoload"), false);
    KConfigGroup cg(config, QStringLiteral("Module-").append(module.pluginId()));
    autoload = cg.readEntry("autoload", autoload);
    return autoload;
}

bool Kded::platformSupportsModule(const KPluginMetaData &module) const
{
    const QStringList supportedPlatforms = module.value(QStringLiteral("X-KDE-OnlyShowOnQtPlatforms"), QStringList());

    return supportedPlatforms.isEmpty() || supportedPlatforms.contains(qApp->platformName());
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
    return module.value(QStringLiteral("X-KDE-Kded-load-on-demand"), true);
}

KDEDModule *Kded::loadModule(const QString &obj, bool onDemand)
{
    // Make sure this method is only called with valid module names.
    if (obj.contains(QLatin1Char('/'))) {
        qCWarning(KDED) << "attempting to load invalid kded module name:" << obj;
        return nullptr;
    }
    KDEDModule *module = m_modules.value(obj, nullptr);
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
    KDEDModule *oldModule = m_modules.value(moduleId, nullptr);
    if (oldModule) {
        qCDebug(KDED) << "kded module" << moduleId << "is already loaded.";
        return oldModule;
    }

    if (onDemand) {
        if (!module.value(QStringLiteral("X-KDE-Kded-load-on-demand"), true)) {
            noDemandLoad(moduleId);
            return nullptr;
        }
    }

    KDEDModule *kdedModule = nullptr;

    auto factoryResult = KPluginFactory::loadFactory(module);
    if (factoryResult) {
        kdedModule = factoryResult.plugin->create<KDEDModule>(this);
    } else {
        // TODO: remove this fallback code, the kded modules should all be fixed instead
        factoryResult = KPluginFactory::loadFactory(KPluginMetaData(QStringLiteral("kded_") + module.fileName()));
        if (factoryResult) {
            qCWarning(KDED).nospace() << "found kded module " << moduleId << " by prepending 'kded_' to the library path, please fix your metadata.";
            kdedModule = factoryResult.plugin->create<KDEDModule>(this);
        } else {
            qCWarning(KDED).nospace() << "Could not load kded module " << moduleId << ":" << factoryResult.errorText
                                      << " (library path was:" << module.fileName() << ")";
        }
    }

    if (kdedModule) {
        kdedModule->setModuleName(moduleId);
        m_modules.insert(moduleId, kdedModule);
        // m_libs.insert(moduleId, lib);
        connect(kdedModule, &KDEDModule::moduleDeleted, this, &Kded::slotKDEDModuleRemoved);
        qCDebug(KDED) << "Successfully loaded module" << moduleId;
        return kdedModule;
    }
    return nullptr;
}

bool Kded::unloadModule(const QString &obj)
{
    KDEDModule *module = m_modules.value(obj, nullptr);
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
    for (const auto id : windowIds) {
        m_globalWindowIdList.remove(id);
        for (KDEDModule *module : std::as_const(m_modules)) {
            Q_EMIT module->windowUnregistered(id);
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
    m_pDirWatch = new KDirWatch(this);

    QObject::connect(m_pDirWatch, &KDirWatch::dirty, this, &Kded::update);
    QObject::connect(m_pDirWatch, &KDirWatch::created, this, &Kded::update);
    QObject::connect(m_pDirWatch, &KDirWatch::deleted, this, &Kded::dirDeleted);

    // For each resource
    for (const QString &dir : std::as_const(m_allResourceDirs)) {
        readDirectory(dir);
    }

    QStringList dataDirs = QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation);
    for (auto &dir : dataDirs) {
        dir += QLatin1String("/icons");
        if (!m_pDirWatch->contains(dir)) {
            m_pDirWatch->addDir(dir, KDirWatch::WatchDirOnly);
        }
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
    for (const auto &dir : dirs) {
        if (!m_allResourceDirs.contains(dir)) {
            m_allResourceDirs.append(dir);
            readDirectory(dir);
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
            updateDirWatch(); // this would search all the directories
        }
        if (bCheckSycoca) {
            KSycoca::self()->ensureCacheValid();
        }
        recreateDone();
        if (delayedCheck) {
            // do a proper ksycoca check after a delay
            QTimer::singleShot(60000, this, &Kded::runDelayedCheck);
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

void Kded::update(const QString &path)
{
    if (path.endsWith(QLatin1String("/icons")) && m_pDirWatch->contains(path)) {
        // If the dir was created or updated there could be new folders to merge into the active theme(s)
        QDBusMessage message = QDBusMessage::createSignal(QStringLiteral("/KIconLoader"), QStringLiteral("org.kde.KIconLoader"), QStringLiteral("iconChanged"));
        message << 0;
        QDBusConnection::sessionBus().send(message);
    } else {
        m_pTimer->start(1000);
    }
}

void Kded::readDirectory(const QString &_path)
{
    QString path(_path);
    if (!path.endsWith(QLatin1Char('/'))) {
        path += QLatin1Char('/');
    }

    if (m_pDirWatch->contains(path)) { // Already seen this one?
        return;
    }

    Q_ASSERT(path != QDir::homePath());
    m_pDirWatch->addDir(path, KDirWatch::WatchFiles | KDirWatch::WatchSubDirs); // add watch on this dir
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

    for (KDEDModule *module : std::as_const(m_modules)) {
        qCDebug(KDED) << module->moduleName();
        Q_EMIT module->windowRegistered(windowId);
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

    for (KDEDModule *module : std::as_const(m_modules)) {
        qCDebug(KDED) << module->moduleName();
        Q_EMIT module->windowUnregistered(windowId);
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
    QObject::connect(m_pDirWatch, &KDirWatch::dirty, this, &KUpdateD::slotNewUpdateFile);

    QStringList dirs = QStandardPaths::locateAll(QStandardPaths::GenericDataLocation, QStringLiteral("kconf_update"), QStandardPaths::LocateDirectory);
    for (auto &path : dirs) {
        Q_ASSERT(path != QDir::homePath());
        if (!path.endsWith(QLatin1Char('/'))) {
            path += QLatin1Char('/');
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
void KBuildsycocaAdaptor::setTestModeEnabled()
{
    QStandardPaths::setTestModeEnabled(true);
}

static void setupAppInfo(QApplication *app)
{
    app->setApplicationName(QStringLiteral("kded5"));
    app->setApplicationDisplayName(QStringLiteral("KDE Daemon"));
    app->setOrganizationDomain(QStringLiteral("kde.org"));
    app->setApplicationVersion(QStringLiteral(KDED_VERSION_STRING));
}

static bool detectPlatform(int argc, char **argv)
{
    if (qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
        return false;
    }
    for (int i = 0; i < argc; i++) {
        /* clang-format off */
        if (qstrcmp(argv[i], "-platform") == 0
            || qstrcmp(argv[i], "--platform") == 0
            || QByteArray(argv[i]).startsWith("-platform=")
            || QByteArray(argv[i]).startsWith("--platform=")) { /* clang-format on */
            return false;
        }
    }
    const QByteArray sessionType = qgetenv("XDG_SESSION_TYPE");
    if (sessionType.isEmpty()) {
        return false;
    }
    if (qstrcmp(sessionType.data(), "wayland") == 0) {
        qputenv("QT_QPA_PLATFORM", "wayland");
        return true;
    } else if (qstrcmp(sessionType.data(), "x11") == 0) {
        qputenv("QT_QPA_PLATFORM", "xcb");
        return true;
    }
    return false;
}

int main(int argc, char *argv[])
{
#ifdef Q_OS_OSX
    CFBundleRef mainBundle = CFBundleGetMainBundle();
    if (mainBundle) {
        // get the application's Info Dictionary. For app bundles this would live in the bundle's Info.plist,
        // for regular executables it is obtained in another way.
        CFMutableDictionaryRef infoDict = (CFMutableDictionaryRef)CFBundleGetInfoDictionary(mainBundle);
        if (infoDict) {
            // Add or set the "LSUIElement" key with/to value "1". This can simply be a CFString.
            CFDictionarySetValue(infoDict, CFSTR("LSUIElement"), CFSTR("1"));
            // That's it. We're now considered as an "agent" by the window server, and thus will have
            // neither menubar nor presence in the Dock or App Switcher.
        }
    }
#endif
    // options.add("check", qi18n("Check Sycoca database only once"));

    // WABA: Make sure not to enable session management.
    qunsetenv("SESSION_MANAGER");

    const bool unsetQpa = detectPlatform(argc, argv);

    // In older versions, QApplication creation was postponed until after
    // testing for --check, in which case, only a QCoreApplication was created.
    // Since that option is no longer used at startup, we removed that speed
    // optimization for code clarity and easier support of standard parameters.

    // Fixes blurry icons with Fractional scaling
    QGuiApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
    QApplication app(argc, argv);
    if (unsetQpa) {
        qunsetenv("QT_QPA_PLATFORM");
    }
    setupAppInfo(&app);
    app.setQuitOnLastWindowClosed(false);

    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption(QCommandLineOption(QStringLiteral("check"), QStringLiteral("Check cache validity")));
    QCommandLineOption replaceOption({QStringLiteral("replace")}, QStringLiteral("Replace an existing instance"));
    parser.addOption(replaceOption);
    parser.process(app);

    // Parse command line before checking D-Bus
    if (parser.isSet(QStringLiteral("check"))) {
        // KDBusService not wanted here.
        KSycoca::self()->ensureCacheValid();
        runKonfUpdate();
        return 0;
    }

    KDBusService service(KDBusService::Unique | KDBusService::StartupOption(parser.isSet(replaceOption) ? KDBusService::Replace : 0));

    QDBusConnectionInterface *bus = QDBusConnection::sessionBus().interface();
    // Also register as all the names we should respond to (org.kde.kcookiejar, org.kde.khotkeys etc.)
    // so that the calling code is independent from the physical "location" of the service.
    const QVector<KPluginMetaData> plugins = KPluginMetaData::findPlugins(QStringLiteral("kf" QT_STRINGIFY(QT_VERSION_MAJOR) "/kded"));
    for (const KPluginMetaData &metaData : plugins) {
        const QString serviceName = metaData.value(QStringLiteral("X-KDE-DBus-ServiceName"));
        if (serviceName.isEmpty()) {
            continue;
        }
        if (!bus->registerService(serviceName)) {
            qCWarning(KDED) << "Couldn't register name" << serviceName << "with DBUS - another process owns it already!";
        }
    }

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

    std::unique_ptr<Kded> kded = std::make_unique<Kded>();

    kded->recreate(true); // initial

    if (bCheckUpdates) {
        (void)new KUpdateD; // Watch for updates
    }

    runKonfUpdate(); // Run it once.

    return app.exec(); // keep running
}
