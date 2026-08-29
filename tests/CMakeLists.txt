add_executable(tm_core_tests
    test_main.cpp
    test_deci3_codec.cpp
    test_dfmp_codec.cpp
    test_drfp_codec.cpp
    test_dbgp_codec.cpp
    test_debug_controller.cpp
    test_ppc_disasm.cpp
    test_ppc_stack.cpp
    test_elf_symbols.cpp
    test_dwarf_line.cpp
    test_tsmp_codec.cpp
    test_target_type.cpp
    test_sft_settings.cpp
    test_load_options_codec.cpp
    test_target_record_codec.cpp
    test_wol.cpp
    test_cp_client.cpp
    test_dcmp_codec.cpp
    test_host_path_policy.cpp
    ${CMAKE_SOURCE_DIR}/src/tm_ui/target_properties_xml.cpp
    test_target_properties_xml.cpp
    test_icons_resource.cpp
    ${CMAKE_SOURCE_DIR}/src/tm_ui/icons.qrc
)

set_target_properties(tm_core_tests PROPERTIES AUTORCC ON)

target_link_libraries(tm_core_tests PRIVATE
    tm_core
    tm_session
    Qt6::Core
    Qt6::Gui
    Catch2::Catch2WithMain
)

target_include_directories(tm_core_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

opentm_target_speedup(tm_core_tests GUI CATCH2)

include(Catch)
catch_discover_tests(tm_core_tests)
