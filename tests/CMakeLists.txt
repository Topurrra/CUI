add_executable(cui_tests test_main.c)
target_link_libraries(cui_tests PRIVATE cui::cui)
add_test(NAME cui_tests COMMAND cui_tests)

add_executable(cui_screenshots screenshots.c ../examples/gallery_ui.c)
target_link_libraries(cui_screenshots PRIVATE cui::cui)
add_test(NAME cui_screenshots COMMAND cui_screenshots ${CMAKE_CURRENT_BINARY_DIR})

if(CUI_WITH_APP)
    add_executable(cui_app_smoke app_smoke.c ../examples/gallery_ui.c)
    target_link_libraries(cui_app_smoke PRIVATE cui::cui)
endif()

add_executable(cui_bench bench.c ../examples/gallery_ui.c)
target_link_libraries(cui_bench PRIVATE cui::cui)

add_executable(cui_component_shots component_shots.c)
target_link_libraries(cui_component_shots PRIVATE cui::cui)

include(CheckLanguage)
check_language(CXX)
if(CMAKE_CXX_COMPILER)
    enable_language(CXX)
    add_executable(cui_cpp_compat cpp_compat.cpp)
    target_link_libraries(cui_cpp_compat PRIVATE cui::cui)
    set_target_properties(cui_cpp_compat PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON)
    add_test(NAME cui_cpp_compat COMMAND cui_cpp_compat)
endif()

add_executable(cui_fuzz fuzz_gallery.c ../examples/gallery_ui.c)
target_link_libraries(cui_fuzz PRIVATE cui::cui)
add_test(NAME cui_fuzz COMMAND cui_fuzz 3000)
