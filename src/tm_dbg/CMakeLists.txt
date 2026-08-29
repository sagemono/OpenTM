qt_add_executable(opentm_dbg
    main.cpp
    debugger_window.cpp
    debugger_window.h
    debugger_panel.cpp
    debugger_panel.h
)

set_target_properties(opentm_dbg PROPERTIES
    AUTOMOC ON
    WIN32_EXECUTABLE ON
)

target_include_directories(opentm_dbg PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_libraries(opentm_dbg PRIVATE
    tm_core
    tm_session
    tm_launch
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
)

opentm_target_speedup(opentm_dbg GUI)
