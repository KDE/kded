set(kded_KDEINIT_SRCS kded.cpp kdedadaptor.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/org.kde.kded5.xml # just so that it gets generated
)

kf5_add_kdeinit_executable(kded5 ${kded_KDEINIT_SRCS})

if (APPLE)
    set_target_properties(kded5 PROPERTIES MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.template)
    set_target_properties(kded5 PROPERTIES MACOSX_BUNDLE_GUI_IDENTIFIER "org.kded.kded5")
    set_target_properties(kded5 PROPERTIES MACOSX_BUNDLE_BUNDLE_NAME "KDE Daemon")
endif ()

target_link_libraries(kdeinit_kded5
    Qt5::Widgets # QApplication
    KF5::Service # Needed for ksycoca.h
    KF5::CoreAddons # Needed for KDirWatch
    KF5::DBusAddons # Needed for kdedmodule.h
    KF5::Crash # Sets it as autostart
)
target_compile_definitions(kdeinit_kded5 PRIVATE
    KCONF_UPDATE_EXE="$<TARGET_FILE:KF5::kconf_update>"
)
target_compile_definitions(kded5 PRIVATE
    KCONF_UPDATE_EXE="$<TARGET_FILE:KF5::kconf_update>"
)

install(TARGETS kdeinit_kded5 EXPORT KDEDTargets ${INSTALL_TARGETS_DEFAULT_ARGS})

target_link_libraries( kded5 kdeinit_kded5)
install(TARGETS kded5 EXPORT KDEDTargets ${INSTALL_TARGETS_DEFAULT_ARGS} )


qt5_generate_dbus_interface( kdedadaptor.h org.kde.kded5.xml )
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.kde.kded5.xml DESTINATION ${DBUS_INTERFACES_INSTALL_DIR})

configure_file(org.kde.kded5.service.in
               ${CMAKE_CURRENT_BINARY_DIR}/org.kde.kded5.service)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.kde.kded5.service
        DESTINATION ${DBUS_SERVICES_INSTALL_DIR})

install( FILES kdedmodule.desktop DESTINATION  ${SERVICETYPES_INSTALL_DIR} )
