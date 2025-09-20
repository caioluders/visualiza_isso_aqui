cmake_minimum_required(VERSION 3.20)

project(visualiza_isso_aqui VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Allow older subprojects (e.g., glad) to configure with modern CMake
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

include(FetchContent)
include(ExternalProject)

# Dependencies: GLFW, ImGui, glad, RtAudio, KissFFT

set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  glfw
  GIT_REPOSITORY https://github.com/glfw/glfw.git
  GIT_TAG 3.4
)
FetchContent_MakeAvailable(glfw)

FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG docking
)
FetchContent_MakeAvailable(imgui)

FetchContent_Declare(
  glad
  GIT_REPOSITORY https://github.com/Dav1dde/glad.git
  GIT_TAG v0.1.36
)
FetchContent_MakeAvailable(glad)

FetchContent_Declare(
  rtaudio
  GIT_REPOSITORY https://github.com/thestk/rtaudio.git
  GIT_TAG 6.0.1
)
FetchContent_GetProperties(rtaudio)
if(NOT rtaudio_POPULATED)
  FetchContent_Populate(rtaudio)
endif()

set(KISSFFT_TEST OFF CACHE BOOL "" FORCE)
set(KISSFFT_TOOLS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  kissfft
  GIT_REPOSITORY https://github.com/mborgerding/kissfft.git
  GIT_TAG 131.1.0
)
FetchContent_GetProperties(kissfft)
if(NOT kissfft_POPULATED)
  FetchContent_Populate(kissfft)
endif()

find_package(OpenGL REQUIRED)

file(GLOB IMGUI_SOURCES
  ${imgui_SOURCE_DIR}/*.cpp
)

set(IMGUI_BACKENDS
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)

set(KISSFFT_SOURCES
  ${kissfft_SOURCE_DIR}/kiss_fft.c
  ${kissfft_SOURCE_DIR}/kiss_fftr.c
)

add_executable(${PROJECT_NAME}
  src/main.cpp
  src/ShaderProgram.cpp
  src/FFglitchPlayer.cpp
  src/AudioInput.cpp
  src/FeatureExtractor.cpp
  src/AudioAnalysis.cpp
  src/AudioFilePlayer.cpp
  src/FileDialog.mm
  ${IMGUI_SOURCES}
  ${IMGUI_BACKENDS}
  ${KISSFFT_SOURCES}
  ${rtaudio_SOURCE_DIR}/RtAudio.cpp
)

target_include_directories(${PROJECT_NAME} PRIVATE
  ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends
  ${kissfft_SOURCE_DIR}
  ${rtaudio_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${glad_BINARY_DIR}/include
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
  IMGUI_IMPL_OPENGL_LOADER_GLAD
  __MACOSX_CORE__
)

target_link_libraries(${PROJECT_NAME}
  PRIVATE
    glfw
    glad
    OpenGL::GL
    z
    bz2
    iconv
)

# Discover and link SDL2 if available (needed by ffglitch/FFmpeg build)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(SDL2 QUIET sdl2)
  if(SDL2_FOUND)
    target_include_directories(${PROJECT_NAME} PRIVATE ${SDL2_INCLUDE_DIRS})
    if(SDL2_LIBRARY_DIRS)
      target_link_directories(${PROJECT_NAME} PRIVATE ${SDL2_LIBRARY_DIRS})
    endif()
    target_link_libraries(${PROJECT_NAME} PRIVATE ${SDL2_LIBRARIES})
  endif()
endif()

# ffglitch-core integration (always on, auto-fetch unless user provides paths)
# Users can override with an install prefix:
#   -DFFGLITCH_CORE_INCLUDE_DIR=/path/to/include
#   -DFFGLITCH_CORE_LIBDIR=/path/to/lib
set(FFGLITCH_CORE_INCLUDE_DIR "" CACHE PATH "Path to ffglitch-core public headers")
set(FFGLITCH_CORE_LIBDIR "" CACHE PATH "Path to directory with ffglitch/FFmpeg libs (libavformat, libavcodec, etc)")

if(FFGLITCH_CORE_INCLUDE_DIR AND FFGLITCH_CORE_LIBDIR AND EXISTS "${FFGLITCH_CORE_INCLUDE_DIR}" AND EXISTS "${FFGLITCH_CORE_LIBDIR}")
  message(STATUS "ffglitch-core: using provided prefix ${FFGLITCH_CORE_LIBDIR}")
  target_include_directories(${PROJECT_NAME} PRIVATE ${FFGLITCH_CORE_INCLUDE_DIR})
  target_link_directories(${PROJECT_NAME} PRIVATE ${FFGLITCH_CORE_LIBDIR})
  target_link_libraries(${PROJECT_NAME} PRIVATE avformat avcodec avutil swresample swscale avfilter avdevice)
  target_compile_definitions(${PROJECT_NAME} PRIVATE HAVE_FFGLITCH_CORE=1)
else()
  message(STATUS "ffglitch-core: auto-fetching and building")
  set(FFG_INSTALL_DIR ${CMAKE_BINARY_DIR}/_deps/ffglitch-core)
  ExternalProject_Add(ffglitch_core
    PREFIX ${CMAKE_BINARY_DIR}/_deps/ffglitch_core_ep
    GIT_REPOSITORY https://github.com/ramiropolla/ffglitch-core.git
    GIT_TAG ffedit-0.10
    CONFIGURE_COMMAND ./configure --enable-static --disable-shared --prefix=${FFG_INSTALL_DIR}
    BUILD_COMMAND $(MAKE)
    INSTALL_COMMAND $(MAKE) install
    BUILD_IN_SOURCE 1
    UPDATE_DISCONNECTED 1
  )
  add_dependencies(${PROJECT_NAME} ffglitch_core)
  target_include_directories(${PROJECT_NAME} PRIVATE ${FFG_INSTALL_DIR}/include)
  target_link_directories(${PROJECT_NAME} PRIVATE ${FFG_INSTALL_DIR}/lib)
  target_link_libraries(${PROJECT_NAME} PRIVATE avformat avcodec avutil swresample swscale avfilter avdevice)
  # Common extra libs used by FFmpeg builds
  target_link_libraries(${PROJECT_NAME} PRIVATE z bz2 iconv)
  # Common extra libs used by FFmpeg builds
  target_link_libraries(${PROJECT_NAME} PRIVATE z bz2 iconv)
  target_compile_definitions(${PROJECT_NAME} PRIVATE HAVE_FFGLITCH_CORE=1)
endif()

if(APPLE)
  # Build as a proper macOS bundle and attach an app icon if present
  set_target_properties(${PROJECT_NAME} PROPERTIES MACOSX_BUNDLE TRUE)
  set_target_properties(${PROJECT_NAME} PROPERTIES MACOSX_BUNDLE_GUI_IDENTIFIER "com.visualiza.isso.aqui")
  set_target_properties(${PROJECT_NAME} PROPERTIES MACOSX_BUNDLE_BUNDLE_NAME "visualiza_isso_aqui")
  set_target_properties(${PROJECT_NAME} PROPERTIES MACOSX_BUNDLE_ICON_FILE "app.icns")
  set_target_properties(${PROJECT_NAME} PROPERTIES
    RESOURCE "${CMAKE_SOURCE_DIR}/assets/Info.plist"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/assets/Info.plist")
  if(EXISTS ${CMAKE_SOURCE_DIR}/assets/app.icns)
    target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/assets/app.icns)
    set_source_files_properties(${CMAKE_SOURCE_DIR}/assets/app.icns PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
  else()
    message(WARNING "App icon not found: assets/app.icns. Place an .icns file there to set the Dock icon.")
  endif()

  # Copy assets into the app bundle Resources so shaders/textures are found at runtime
  add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${PROJECT_NAME}>/../Resources"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/assets" "$<TARGET_FILE_DIR:${PROJECT_NAME}>/../Resources/assets"
  )

  target_link_libraries(${PROJECT_NAME} PRIVATE
    "-framework Cocoa"
    "-framework IOKit"
    "-framework CoreVideo"
    "-framework CoreAudio"
    "-framework AudioToolbox"
    "-framework CoreMedia"
    "-framework VideoToolbox"
    "-framework Security"
    "-framework CoreMIDI"
    "-framework Cocoa" # for glfwGetPlatform symbol resolution in newer GLFW
  )
endif()

# Ensure glad headers are generated before building this target
add_dependencies(${PROJECT_NAME} glad)

if(MSVC)
  target_compile_options(${PROJECT_NAME} PRIVATE /W4)
else()
  target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_custom_target(run
  COMMAND ${PROJECT_NAME}
  DEPENDS ${PROJECT_NAME}
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT ${PROJECT_NAME})

# Copy assets to build dir for runtime
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
  ${CMAKE_SOURCE_DIR}/assets ${CMAKE_CURRENT_BINARY_DIR}/assets)


