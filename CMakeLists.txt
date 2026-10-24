# The Warriors (original Xbox), statically recompiled

cmake_minimum_required(VERSION 3.25)
project(game LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# the SDK: sdk/ here, two folders up inside the SDK's templates, a checkout beside this repository, else downloaded
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/sdk/cmake/XbeRecomp.cmake)
    set(SDK ${CMAKE_CURRENT_SOURCE_DIR}/sdk)
elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/XbeRecomp.cmake)
    set(SDK ${CMAKE_CURRENT_SOURCE_DIR}/../..)
elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../XboxXBE-Recomp/cmake/XbeRecomp.cmake)
    set(SDK ${CMAKE_CURRENT_SOURCE_DIR}/../XboxXBE-Recomp)
else()
    include(FetchContent)
    FetchContent_Declare(xbe_sdk GIT_REPOSITORY https://github.com/TGP482/XboxXBE-Recomp.git GIT_TAG main)
    FetchContent_MakeAvailable(xbe_sdk)
    set(SDK ${xbe_sdk_SOURCE_DIR})
endif()
# The game's x87 FPREM1 recompiles to std::remainder, which the SDK's freestanding <cmath> does not declare yet.
# Added to the SDK checkout here, so the headers a release ships for building on the player's PC have it too.
set(SDK_CMATH ${SDK}/runtime/cpu/freestanding/cmath)
file(READ ${SDK_CMATH} SDK_CMATH_TEXT)
if(NOT SDK_CMATH_TEXT MATCHES "remainder")
    string(REPLACE "X86_MATH2(fmod)" "X86_MATH2(fmod)\nX86_MATH2(remainder)" SDK_CMATH_TEXT "${SDK_CMATH_TEXT}")
    file(WRITE ${SDK_CMATH} "${SDK_CMATH_TEXT}")
endif()

include(${SDK}/cmake/XbeRecomp.cmake)

# DirectSound is the SDK's with what XACT needs in this game (src/audio/dsound.cpp), built in place of the SDK's
list(REMOVE_ITEM XBE_RUNTIME_SOURCES ${SDK}/runtime/xdk/dsound.cpp)

file(GLOB XBE_HOST_SOURCES CONFIGURE_DEPENDS src/*/*.cpp)
list(FILTER XBE_HOST_SOURCES EXCLUDE REGEX "/src/game/")

# partitions as xbe_symbols wrote them into config/game.toml
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS config/game.toml)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/config/game.toml)
    file(STRINGS config/game.toml GAME_PARTITIONS REGEX "^partitions *=")
    string(REGEX MATCH "[0-9]+" GAME_PARTITIONS "${GAME_PARTITIONS}")
endif()
# no config yet: build.bat writes it with xbe_symbols, the change reconfigures
if(NOT GAME_PARTITIONS)
    set(GAME_PARTITIONS 8)
endif()

# target name = GAME_EXE in build.bat, xbe = GAME_XBE
add_xbe_image(Warriors default.xbe game.toml ${GAME_PARTITIONS} game)
