#! [cmake]
find_package(KF6 REQUIRED COMPONENTS CoreAddons DBusAddons)

kcoreaddons_add_plugin(mymodule SOURCES module.cpp INSTALL_NAMESPACE "kf6/kded")

target_link_libraries(mymodule PRIVATE KF6::DBusAddons)
#! [cmake]
