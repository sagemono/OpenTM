add_library(tm_session STATIC
    frame_dispatcher.cpp
    frame_dispatcher.h
    session_controller.cpp
    session_controller.h
    target_actions.cpp
    target_actions.h
    target_discovery.cpp
    target_discovery.h
    file_explorer_controller.cpp
    file_explorer_controller.h
    host_file_server.cpp
    host_file_server.h
    kernel_explorer_controller.cpp
    kernel_explorer_controller.h
    debug_controller.cpp
    debug_controller.h
    tsmp_helpers.h
    session_api.h
    rpc_client.cpp
    rpc_client.h
    remote_session.cpp
    remote_session.h
    target_session.cpp
    target_session.h
    session_factory.cpp
    session_factory.h
    target_record.h
    load_options.h
    load_options_codec.cpp
    load_options_codec.h
    target_record_codec.cpp
    target_record_codec.h
)

set_target_properties(tm_session PROPERTIES AUTOMOC ON)

target_include_directories(tm_session PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_libraries(tm_session
    PUBLIC
        tm_core
        Qt6::Core
        Qt6::Network
)

opentm_target_speedup(tm_session NETWORK)