# Armored Core's targets in the player's package build (psprecomp
# player/linux/CMakeLists.txt includes this file; pack.json names it): the
# player's launcher with this pack's settings, and what has not moved into
# psprecomp yet -- boot.c -- with the checks. The variables and targets used
# are the player's: PACK, RECOMP, PLAYER_SETTINGS, PLAYER_HOST (psprecomp's
# GL backend among it), PLAYER_LAUNCHER, runtime and PkgConfig::SDL.
set(AC_SETTINGS "${PACK}/host/settings.c" "${PLAYER_SETTINGS}")

add_executable(launcher ${PLAYER_LAUNCHER} "${PACK}/host/launcher_info.c" ${AC_SETTINGS})
target_link_libraries(launcher PRIVATE runtime PkgConfig::SDL)
target_compile_options(launcher PRIVATE -Wall -Wextra -Werror)
# The tests include the player's launcher.c for its internals.
add_executable(launcher-tests "${PACK}/host/launcher_tests.c" "${PACK}/host/launcher_info.c" ${AC_SETTINGS})
target_include_directories(launcher-tests PRIVATE "${TOOLKIT}/src/host")
target_link_libraries(launcher-tests PRIVATE runtime PkgConfig::SDL)
foreach(name settings-tool settings-tests)
    string(REPLACE "-" "_" source ${name})
    add_executable(${name} "${PACK}/host/${source}.c" ${AC_SETTINGS})
    target_include_directories(${name} PRIVATE "${TOOLKIT}/include")
    target_link_libraries(${name} PRIVATE m pthread)
endforeach()
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

foreach(slug ${PACK_SLUGS})
    # The window title comes from the title's psp_title_info, compiled on the
    # device with its replacements, so these archives are the same objects.
    # They are original host objects only: the user's generated game objects
    # and the replacements that need generated headers link on their device.
    add_library(host-${slug} STATIC "${PACK}/host/boot.c" "${PACK}/host/settings.c"
        ${PLAYER_HOST} "${RECOMP}/loader.c" "${RECOMP}/container.c" "${RECOMP}/decode.c")
    target_include_directories(host-${slug} PRIVATE "${RECOMP}")
    target_compile_definitions(host-${slug} PRIVATE HAVE_SDL2)
    target_link_libraries(host-${slug} PRIVATE runtime PkgConfig::SDL)
endforeach()
