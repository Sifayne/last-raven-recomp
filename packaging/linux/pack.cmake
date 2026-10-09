# Armored Core's checks in the player's package build (psprecomp
# player/linux/CMakeLists.txt includes this file, for pack.json's
# build.cmake). The pack itself is source: the app builds its host code and
# its launcher part when a player adds it. The variables and targets used
# are the player's: TOOLKIT, PACK, PLAYER_SETTINGS, PLAYER_HOST,
# PLAYER_LAUNCHER, player_imgui, runtime and PkgConfig::SDL.
set(AC_SETTINGS "${PACK}/host/settings.c" "${PLAYER_SETTINGS}")

# psprecomp's launcher with this pack linked in: its pages drawn, and a
# session. It includes launcher.c for its internals.
add_executable(launcher-tests "${PACK}/host/launcher_tests.c" "${PACK}/host/launcher_info.c"
    "${TOOLKIT}/src/host/launcher_one.c" $<TARGET_OBJECTS:player_imgui> ${AC_SETTINGS})
target_include_directories(launcher-tests PRIVATE "${TOOLKIT}/src/host")
target_compile_definitions(launcher-tests PRIVATE HAVE_SDL2)
target_link_libraries(launcher-tests PRIVATE runtime PkgConfig::SDL ${CMAKE_DL_LIBS})
set_target_properties(launcher-tests PROPERTIES ENABLE_EXPORTS ON)
add_executable(settings-tests "${PACK}/host/settings_tests.c" ${AC_SETTINGS})
target_include_directories(settings-tests PRIVATE "${TOOLKIT}/include")
target_link_libraries(settings-tests PRIVATE m pthread)
# Assertions are the fixtures' checks; Release must not compile them out.
target_compile_options(launcher-tests PRIVATE -UNDEBUG)
target_compile_options(settings-tests PRIVATE -UNDEBUG)
foreach(name fps-tests fps-pose-tests)
    string(REPLACE "-" "_" source ${name})
    add_executable(${name} "${PACK}/host/${source}.c")
    target_compile_options(${name} PRIVATE -UNDEBUG -Wall -Wextra -Werror)
    target_link_libraries(${name} PRIVATE m)
endforeach()
add_executable(save-dialog-tests "${PACK}/host/save_dialog_tests.c"
    "${PACK}/host/settings.c" ${PLAYER_HOST})
target_compile_definitions(save-dialog-tests PRIVATE HAVE_SDL2)
target_compile_options(save-dialog-tests PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_link_libraries(save-dialog-tests PRIVATE runtime PkgConfig::SDL)
