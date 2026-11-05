add_executable(cui_tests test_main.c)
target_link_libraries(cui_tests PRIVATE cui::cui)
add_test(NAME cui_tests COMMAND cui_tests)

add_executable(cui_screenshots screenshots.c ../examples/gallery_ui.c)
target_link_libraries(cui_screenshots PRIVATE cui::cui)
add_test(NAME cui_screenshots COMMAND cui_screenshots ${CMAKE_CURRENT_BINARY_DIR})
