set(DMN_BLUETOOTH_GTK_SCHEMA_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/schemas"
)

set(DMN_BLUETOOTH_GTK_SCHEMA_FILE
    "${DMN_BLUETOOTH_GTK_SCHEMA_DIR}/gschemas.compiled"
)
set(DMN_BLUETOOTH_GTK_INSTALL_SCHEMA_DIR
    "${CMAKE_INSTALL_FULL_DATADIR}/dmn-bluetooth/schemas"
)
set(DMN_BLUETOOTH_GTK_DBUS_SERVICE_FILE
    "${CMAKE_CURRENT_BINARY_DIR}/com.cheebinhoh.dmn-bluetooth.service"
)
set(DMN_BLUETOOTH_GTK_DESKTOP_FILE
    "${CMAKE_CURRENT_BINARY_DIR}/com.cheebinhoh.dmn-bluetooth.desktop"
)

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/data/com.cheebinhoh.dmn-bluetooth.service.in"
  "${DMN_BLUETOOTH_GTK_DBUS_SERVICE_FILE}"
  @ONLY
)
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/data/com.cheebinhoh.dmn-bluetooth.desktop.in"
  "${DMN_BLUETOOTH_GTK_DESKTOP_FILE}"
  @ONLY
)

add_custom_command(OUTPUT "${DMN_BLUETOOTH_GTK_SCHEMA_FILE}"
                     COMMAND "${CMAKE_COMMAND}" -E make_directory
                             "${DMN_BLUETOOTH_GTK_SCHEMA_DIR}"
                     COMMAND "${GLIB_COMPILE_SCHEMAS_EXECUTABLE}" --strict
                             --targetdir "${DMN_BLUETOOTH_GTK_SCHEMA_DIR}"
                             "${CMAKE_CURRENT_SOURCE_DIR}/data"
                     DEPENDS
                       "${CMAKE_CURRENT_SOURCE_DIR}/data/com.cheebinhoh.DmnBluetooth.gschema.xml"
                     VERBATIM
)

add_custom_target(dmn-bluetooth-gtk-schemas
                    DEPENDS "${DMN_BLUETOOTH_GTK_SCHEMA_FILE}"
)

add_executable(dmn-bluetooth-gtk
                 main.cpp
)

add_dependencies(dmn-bluetooth-gtk dmn-bluetooth-gtk-schemas)

target_compile_definitions(dmn-bluetooth-gtk
                             PRIVATE
                             DMN_VERSION="${PROJECT_VERSION}"
                             DMN_BLUETOOTH_GTK_SCHEMA_DIR="${DMN_BLUETOOTH_GTK_SCHEMA_DIR}"
                             DMN_BLUETOOTH_GTK_INSTALL_SCHEMA_DIR="${DMN_BLUETOOTH_GTK_INSTALL_SCHEMA_DIR}"
)

target_compile_options(dmn-bluetooth-gtk
                         PRIVATE -Wall -Wextra -Wpedantic
)

set_target_properties(dmn-bluetooth-gtk PROPERTIES INSTALL_RPATH "")

target_link_libraries(dmn-bluetooth-gtk
                        PRIVATE
                        dmn-bluetooth-gui-core
                        dmn-bluez
                        dmn-ancs-bluez-monitor
                        PkgConfig::GTK4
                        PkgConfig::ADWAITA
)

install(TARGETS dmn-bluetooth-gtk
          RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES
          "${DMN_BLUETOOTH_GTK_DESKTOP_FILE}"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/applications"
)

install(FILES
          "${DMN_BLUETOOTH_GTK_DBUS_SERVICE_FILE}"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/dbus-1/services"
)

install(FILES
          "${CMAKE_CURRENT_SOURCE_DIR}/data/com.cheebinhoh.dmn-bluetooth.metainfo.xml"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/metainfo"
)

install(FILES
          "${CMAKE_CURRENT_SOURCE_DIR}/data/icons/hicolor/scalable/apps/com.cheebinhoh.dmn-bluetooth.svg"
        DESTINATION
          "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps"
)

install(FILES
          "${CMAKE_CURRENT_SOURCE_DIR}/data/icons/hicolor/128x128/apps/com.cheebinhoh.dmn-bluetooth.png"
        DESTINATION
          "${CMAKE_INSTALL_DATADIR}/icons/hicolor/128x128/apps"
)

install(FILES
          "${CMAKE_CURRENT_SOURCE_DIR}/data/icons/hicolor/symbolic/apps/com.cheebinhoh.dmn-bluetooth-symbolic.svg"
        DESTINATION
          "${CMAKE_INSTALL_DATADIR}/icons/hicolor/symbolic/apps"
)

if (GTK4_UPDATE_ICON_CACHE_EXECUTABLE)
  set(DMN_BLUETOOTH_GTK_ICON_CACHE_INSTALL_CODE [=[
set(icon_theme_dir "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/@CMAKE_INSTALL_DATADIR@/icons/hicolor")
execute_process(
  COMMAND "@GTK4_UPDATE_ICON_CACHE_EXECUTABLE@" --force --ignore-theme-index
          "${icon_theme_dir}"
  RESULT_VARIABLE icon_cache_status
)
if(NOT icon_cache_status EQUAL 0)
  message(FATAL_ERROR "Failed to update the installed Dmn Bluetooth icon cache")
endif()
]=])
  string(CONFIGURE
           "${DMN_BLUETOOTH_GTK_ICON_CACHE_INSTALL_CODE}"
           DMN_BLUETOOTH_GTK_ICON_CACHE_INSTALL_CODE
           @ONLY
  )
  install(CODE "${DMN_BLUETOOTH_GTK_ICON_CACHE_INSTALL_CODE}")
else()
  message(WARNING
            "gtk4-update-icon-cache was not found; refresh the hicolor icon "
            "cache after installing dmn-bluetooth-gtk"
  )
endif()

install(FILES
          "${CMAKE_CURRENT_SOURCE_DIR}/data/com.cheebinhoh.DmnBluetooth.gschema.xml"
          "${DMN_BLUETOOTH_GTK_SCHEMA_FILE}"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/dmn-bluetooth/schemas"
)
