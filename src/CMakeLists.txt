add_executable(kded5)

target_sources(kded5 PRIVATE
    kded.cpp
    kdedadaptor.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/org.kde.kded5.xml # just so that it gets generated
)

ecm_qt_export_logging_category(
    IDENTIFIER KDED
    CATEGORY_NAME kf.kded
    OLD_CATEGORY_NAMES kf5.kded
    DEFAULT_SEVERITY Warning
    DESCRIPTION "KDED"
    EXPORT KDED
)

ecm_mark_nongui_executable(kded5)

target_link_libraries(kded5
    Qt5::Widgets # QApplication
    KF5::Service # Needed for ksycoca.h
    KF5::CoreAddons # Needed for KDirWatch
    KF5::DBusAddons # Needed for kdedmodule.h
    KF5::Crash # Sets it as autostart
)
if(APPLE)
    target_link_libraries(kded5 "-framework CoreFoundation")
endif()
target_compile_definitions(kded5 PRIVATE
    KCONF_UPDATE_EXE="$<TARGET_FILE:KF5::kconf_update>"
)

install(TARGETS kded5 ${KF5_INSTALL_TARGETS_DEFAULT_ARGS} )

configure_file(plasma-kded.service.in
               ${CMAKE_CURRENT_BINARY_DIR}/plasma-kded.service @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/plasma-kded.service
        DESTINATION ${SYSTEMD_USER_UNIT_INSTALL_DIR})

qt_generate_dbus_interface( kdedadaptor.h org.kde.kded5.xml )
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.kde.kded5.xml DESTINATION ${KDE_INSTALL_DBUSINTERFACEDIR})

kdbusaddons_generate_dbus_service_file(kded5 org.kde.kded5 ${KDE_INSTALL_FULL_BINDIR})

install( FILES kdedmodule.desktop DESTINATION  ${KDE_INSTALL_KSERVICETYPES5DIR} )

ecm_qt_install_logging_categories(
    EXPORT KDED
    FILE kded.categories
    DESTINATION ${KDE_INSTALL_LOGGINGCATEGORIESDIR}
)
