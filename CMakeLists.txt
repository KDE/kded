cmake_minimum_required(VERSION 3.16)

set(KF_VERSION "5.103.0") # handled by release scripts
set(KF_DEP_VERSION "5.103.0") # handled by release scripts
project(KDED VERSION ${KF_VERSION})

include(FeatureSummary)
find_package(ECM 5.103.0  NO_MODULE)
set_package_properties(ECM PROPERTIES TYPE REQUIRED DESCRIPTION "Extra CMake Modules." URL "https://commits.kde.org/extra-cmake-modules")
feature_summary(WHAT REQUIRED_PACKAGES_NOT_FOUND FATAL_ON_MISSING_REQUIRED_PACKAGES)

set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})

set(REQUIRED_QT_VERSION 5.15.2)

include(KDEInstallDirs)
include(KDEFrameworkCompilerSettings NO_POLICY_SCOPE)
include(KDECMakeSettings)
include(KDEGitCommitHooks)
include(ECMDeprecationSettings)

find_package(Qt${QT_MAJOR_VERSION} "${REQUIRED_QT_VERSION}" CONFIG REQUIRED DBus Widgets)
find_package(KF5Config ${KF_DEP_VERSION} REQUIRED)
find_package(KF5CoreAddons ${KF_DEP_VERSION} REQUIRED)
find_package(KF5Crash ${KF_DEP_VERSION} REQUIRED)
find_package(KF5DBusAddons ${KF_DEP_VERSION} REQUIRED)
find_package(KF5DocTools ${KF_DEP_VERSION})
find_package(KF5Service ${KF_DEP_VERSION} REQUIRED)


include(CMakePackageConfigHelpers)
include(ECMQtDeclareLoggingCategory)
include(ECMSetupVersion)
include(ECMMarkNonGuiExecutable)

ecm_setup_version(
  PROJECT
  VARIABLE_PREFIX KDED
  VERSION_HEADER "${CMAKE_CURRENT_BINARY_DIR}/src/kded_version.h"
  PACKAGE_VERSION_FILE "${CMAKE_CURRENT_BINARY_DIR}/KDEDConfigVersion.cmake"
  SOVERSION 5)

# We don't install kded_version.h, it's only useful internally
# (given that this framework produces no library)

ecm_set_disabled_deprecation_versions(
    QT 5.15.2
    KF 5.97.0
    KSERVICE 5.89 # We need KServiceTypeTrader for compatibility plugin loading
)

if (KF5DocTools_FOUND)
    kdoctools_install(po)
    add_subdirectory(docs)
endif()

add_subdirectory(src)

# Create a Config.cmake and a ConfigVersion.cmake file and install them
set(CMAKECONFIG_INSTALL_DIR "${KDE_INSTALL_CMAKEPACKAGEDIR}/KDED")

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/KDEDConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/KDEDConfig.cmake"
  PATH_VARS KDE_INSTALL_DBUSINTERFACEDIR
  INSTALL_DESTINATION "${CMAKECONFIG_INSTALL_DIR}"
)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/KDEDConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/KDEDConfigVersion.cmake"
  DESTINATION "${CMAKECONFIG_INSTALL_DIR}"
  COMPONENT Devel
)

install(FILES org.kde.kded5.desktop DESTINATION ${KDE_INSTALL_APPDIR})

feature_summary(WHAT ALL   FATAL_ON_MISSING_REQUIRED_PACKAGES)

kde_configure_git_pre_commit_hook(CHECKS CLANG_FORMAT)
