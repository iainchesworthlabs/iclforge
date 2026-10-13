# Takes one picture of the window the way the documentation's are taken: `crucible --shot` over a
# scripted machine (--app), on a settings store of its own (--setting), with the lyric strip
# started by hand (--lyrics) in a taller window (--size). Passes when the program says it drew
# what was asked for (exit code 0) and the file is a PNG of the size asked for.
#
# Run by ctest as `cmake -DEXE=<crucible> -DLRC=<an .lrc> -DOUT=<a .png> -P capture_smoke.cmake`
# (ui/tests/CMakeLists.txt): the whole path from the options to the picture, which neither the
# Catch2 cases (the options' parts) nor the Qt Quick cases (the window's) take.
cmake_minimum_required(VERSION 3.20)

foreach(required EXE LRC OUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "capture_smoke.cmake needs -D${required}=")
    endif()
endforeach()

file(REMOVE "${OUT}")
set(ENV{QT_QPA_PLATFORM} offscreen)
set(ENV{QT_QUICK_BACKEND} software)
set(ENV{QT_FORCE_STDERR_LOGGING} 1)

execute_process(
    COMMAND "${EXE}" --shot "${OUT}" --page room --size 1480x1280
            --app Hearth "--app" "Microsoft Edge,idle"
            --setting behaviour/showSilentApps=false
            --place "Hearth=0.2,0.15,0.6"
            --lyrics "Hearth=${LRC}@00:05.75"
    RESULT_VARIABLE code
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
    TIMEOUT 150)
if(NOT code EQUAL 0)
    message(FATAL_ERROR "crucible --shot exited with ${code}\n${out}\n${err}")
endif()
if(NOT EXISTS "${OUT}")
    message(FATAL_ERROR "crucible --shot said it drew the picture, and there is no file at ${OUT}")
endif()

# A PNG begins with its signature and then the IHDR chunk: the width and the height are the two
# big-endian 32-bit numbers at bytes 16 and 20.
file(READ "${OUT}" header HEX LIMIT 24)
string(SUBSTRING "${header}" 0 16 signature)
string(SUBSTRING "${header}" 32 8 width)
string(SUBSTRING "${header}" 40 8 height)
if(NOT signature STREQUAL "89504e470d0a1a0a")
    message(FATAL_ERROR "${OUT} is not a PNG (begins ${signature})")
endif()
# 1480 is 0x5c8 and 1280 is 0x500.
if(NOT width STREQUAL "000005c8" OR NOT height STREQUAL "00000500")
    message(FATAL_ERROR "${OUT} is 0x${width} by 0x${height} pixels, not 1480 by 1280: --size was not taken")
endif()
file(SIZE "${OUT}" bytes)
if(bytes LESS 20000)
    message(FATAL_ERROR "${OUT} is ${bytes} bytes, which is a blank window and not the room")
endif()
message(STATUS "the picture is ${bytes} bytes, 1480 by 1280")
