# Copyright © 2026 Chee Bin HOH. All rights reserved.

# Static engine (GUI-independent orchestration over the Dmn Bluetooth layers)
add_library(dmn-bluetooth-engine STATIC
              dmn-bluez-backend.cpp
              dmn-demo-backend.cpp
              dmn-engine.cpp
              dmn-json.cpp
)

target_include_directories(dmn-bluetooth-engine
                             PUBLIC ${PROJECT_SOURCE_DIR}/include
)

target_compile_options(dmn-bluetooth-engine
                         PRIVATE -Wall -Wextra -Wpedantic
)

target_link_libraries(dmn-bluetooth-engine
                        PUBLIC dmn-ancs-bluez-monitor dmn-bluetooth-device-core
)
