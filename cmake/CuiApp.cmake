# cui_app_target(<target>): what a window app built on CUI wants.
#   - Windows: no console window, and int main(void) stays the entry point; MinGW links
#     its own runtime pieces in, so the exe needs no DLLs from the compiler
#   - release builds keep only the library code the app calls, which is what makes a
#     small app about 250 KB instead of about 1 MB (see docs/integration.md)
function(cui_app_target name)
    if(WIN32)
        set_target_properties(${name} PROPERTIES WIN32_EXECUTABLE ON)
        if(MSVC)
            target_link_options(${name} PRIVATE /ENTRY:mainCRTStartup)
        else()
            target_link_options(${name} PRIVATE -mwindows)
        endif()
        if(MINGW)
            # libgcc and winpthread go into the exe: thread-local variables (stb_image's, an
            # app's) otherwise need libwinpthread-1.dll, which Windows looks for along PATH
            # when the app starts, and which other machines do not have
            target_link_options(${name} PRIVATE -static)
        endif()
    endif()
    if(APPLE)
        target_link_options(${name} PRIVATE $<$<CONFIG:Release,MinSizeRel>:-Wl,-dead_strip>)
    elseif(MSVC)
        target_link_options(${name} PRIVATE $<$<CONFIG:Release,MinSizeRel>:/OPT:REF /OPT:ICF>)
    elseif(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_link_options(${name} PRIVATE $<$<CONFIG:Release,MinSizeRel>:-Wl,--gc-sections>)
        if(MINGW)   # MinGW drops unused code only through link-time optimization (see the top CMakeLists.txt)
            target_link_options(${name} PRIVATE $<$<CONFIG:Release,MinSizeRel>:-flto=auto>)
        endif()
    endif()
endfunction()
