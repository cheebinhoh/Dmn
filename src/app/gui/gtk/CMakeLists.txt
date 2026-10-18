set(DMN_BLUETOOTH_GTK_SCHEMA_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/schemas"
)

set(DMN_BLUETOOTH_GTK_SCHEMA_FILE
    "${DMN_BLUETOOTH_GTK_SCHEMA_DIR}/gschemas.compiled"
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
)

target_compile_options(dmn-bluetooth-gtk
                         PRIVATE -Wall -Wextra -Wpedantic
)

target_link_libraries(dmn-bluetooth-gtk
                        PRIVATE
                        dmn-bluetooth-gui-core
                        dmn-bluez
                        PkgConfig::GTK4
                        PkgConfig::ADWAITA
)

install(TARGETS dmn-bluetooth-gtk
          RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES
          "${CMAKE_CURRENT_SOURCE_DIR}/data/com.cheebinhoh.DmnBluetooth.desktop"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/applications"
)

install(FILES
          "${CMAKE_CURRENT_SOURCE_DIR}/data/com.cheebinhoh.DmnBluetooth.gschema.xml"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/glib-2.0/schemas"
)
