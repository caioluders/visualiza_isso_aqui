cmake_minimum_required(VERSION 3.20)

project(visualiza_isso_aqui VERSION 0.1.0 LANGUAGES C CXX)
if(APPLE)
  enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Allow older subprojects (e.g., glad) to configure with modern CMake.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

include(FetchContent)
include(ExternalProject)
include(CTest)

option(VISUALIZA_BUILD_APP "Build the interactive OpenGL visualizer app" ON)
option(VISUALIZA_BUILD_TEST_ENGINE "Build the headless audio-analysis test engine" ON)
option(VISUALIZA_ENABLE_FFGLITCH "Build FFglitch/FFmpeg-backed video glitch player" OFF)
option(VISUALIZA_ENABLE_GPROF "Build targets with gprof profiling instrumentation" OFF)

set(KISSFFT_TEST OFF CACHE BOOL "" FORCE)
set(KISSFFT_TOOLS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  kissfft
  GIT_REPOSITORY https://github.com/mborgerding/kissfft.git
  GIT_TAG 131.1.0
)
FetchContent_MakeAvailable(kissfft)

set(KISSFFT_SOURCES
  ${kissfft_SOURCE_DIR}/kiss_fft.c
  ${kissfft_SOURCE_DIR}/kiss_fftr.c
)

if(VISUALIZA_BUILD_TEST_ENGINE)
  add_executable(analysis_probe
    src/analysis_probe.cpp
    src/AudioFilePlayer.cpp
    src/FeatureExtractor.cpp
    src/AudioAnalysis.cpp
    ${KISSFFT_SOURCES}
  )

  target_include_directories(analysis_probe PRIVATE
    ${kissfft_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  if(MSVC)
    target_compile_options(analysis_probe PRIVATE /W4)
  else()
    target_compile_options(analysis_probe PRIVATE -Wall -Wextra -Wpedantic)
    if(VISUALIZA_ENABLE_GPROF)
      target_compile_options(analysis_probe PRIVATE -pg)
      target_link_options(analysis_probe PRIVATE -pg)
    endif()
  endif()
endif()

if(BUILD_TESTING)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(
      NAME shader_validation
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/validate_shaders.py
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    add_test(
      NAME fixture_manifest_validation
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/validate_fixture_manifest.py ${CMAKE_CURRENT_SOURCE_DIR}/assets/test_fixtures/edm_fixtures.json
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    if(VISUALIZA_BUILD_TEST_ENGINE)
      add_test(
        NAME wav_loader_variants
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_wav_loader_variants.py --probe $<TARGET_FILE:analysis_probe> --out-dir ${CMAKE_CURRENT_BINARY_DIR}/wav_loader_variants
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      )
      add_test(
        NAME analysis_nuance_bands
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_analysis_nuance.py --probe $<TARGET_FILE:analysis_probe> --out-dir ${CMAKE_CURRENT_BINARY_DIR}/analysis_nuance
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      )
      add_test(
        NAME edm_fixture_analysis
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/run_edm_fixture_tests.py --build-dir ${CMAKE_CURRENT_BINARY_DIR} --limit-seconds 90 --skip-download
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      )
    endif()
  endif()
endif()

if(VISUALIZA_BUILD_APP)
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
  FetchContent_MakeAvailable(rtaudio)

  find_package(OpenGL REQUIRED)
  find_package(PkgConfig QUIET)

  file(GLOB IMGUI_SOURCES
    ${imgui_SOURCE_DIR}/*.cpp
  )

  set(IMGUI_BACKENDS
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
  )

  set(FILE_DIALOG_SOURCE src/FileDialog.cpp)
  if(APPLE)
    set(FILE_DIALOG_SOURCE src/FileDialog.mm)
  endif()

  set(FFGLITCH_PLAYER_SOURCE src/FFglitchPlayerStub.cpp)
  if(VISUALIZA_ENABLE_FFGLITCH)
    set(FFGLITCH_PLAYER_SOURCE src/FFglitchPlayer.cpp)
  endif()

  add_executable(${PROJECT_NAME}
    src/main.cpp
    src/ShaderProgram.cpp
    ${FFGLITCH_PLAYER_SOURCE}
    src/AudioInput.cpp
    src/FeatureExtractor.cpp
    src/AudioAnalysis.cpp
    src/AudioFilePlayer.cpp
    ${FILE_DIALOG_SOURCE}
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
  )

  target_link_libraries(${PROJECT_NAME}
    PRIVATE
      glfw
      glad
      OpenGL::GL
      z
      bz2
  )

  if(APPLE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE __MACOSX_CORE__)
    target_link_libraries(${PROJECT_NAME} PRIVATE iconv)
  else()
    set(VISUALIZA_LINUX_AUDIO_DEFS __LINUX_ALSA__)
    target_link_libraries(${PROJECT_NAME} PRIVATE X11)
    if(PkgConfig_FOUND)
      pkg_check_modules(ALSA QUIET alsa)
      pkg_check_modules(PULSE QUIET libpulse-simple libpulse)
      if(ALSA_FOUND)
        target_include_directories(${PROJECT_NAME} PRIVATE ${ALSA_INCLUDE_DIRS})
        if(ALSA_LIBRARY_DIRS)
          target_link_directories(${PROJECT_NAME} PRIVATE ${ALSA_LIBRARY_DIRS})
        endif()
        target_link_libraries(${PROJECT_NAME} PRIVATE ${ALSA_LIBRARIES})
      else()
        target_link_libraries(${PROJECT_NAME} PRIVATE asound)
      endif()
      if(PULSE_FOUND)
        list(APPEND VISUALIZA_LINUX_AUDIO_DEFS __LINUX_PULSE__)
        target_include_directories(${PROJECT_NAME} PRIVATE ${PULSE_INCLUDE_DIRS})
        if(PULSE_LIBRARY_DIRS)
          target_link_directories(${PROJECT_NAME} PRIVATE ${PULSE_LIBRARY_DIRS})
        endif()
        target_link_libraries(${PROJECT_NAME} PRIVATE ${PULSE_LIBRARIES})
      endif()
    else()
      target_link_libraries(${PROJECT_NAME} PRIVATE asound)
    endif()
    target_compile_definitions(${PROJECT_NAME} PRIVATE ${VISUALIZA_LINUX_AUDIO_DEFS})
  endif()

  if(VISUALIZA_ENABLE_FFGLITCH)
    target_compile_definitions(${PROJECT_NAME} PRIVATE VISUALIZA_HAS_FFGLITCH=1)
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

    set(FFGLITCH_CORE_INCLUDE_DIR "" CACHE PATH "Path to ffglitch-core public headers")
    set(FFGLITCH_CORE_LIBDIR "" CACHE PATH "Path to directory with ffglitch/FFmpeg libs")

    if(FFGLITCH_CORE_INCLUDE_DIR AND FFGLITCH_CORE_LIBDIR AND EXISTS "${FFGLITCH_CORE_INCLUDE_DIR}" AND EXISTS "${FFGLITCH_CORE_LIBDIR}")
      message(STATUS "ffglitch-core: using provided prefix ${FFGLITCH_CORE_LIBDIR}")
      target_include_directories(${PROJECT_NAME} PRIVATE ${FFGLITCH_CORE_INCLUDE_DIR})
      target_link_directories(${PROJECT_NAME} PRIVATE ${FFGLITCH_CORE_LIBDIR})
      target_link_libraries(${PROJECT_NAME} PRIVATE avformat avcodec avutil swresample swscale avfilter avdevice)
      target_compile_definitions(${PROJECT_NAME} PRIVATE HAVE_FFGLITCH_CORE=1)
    else()
      message(STATUS "ffglitch-core: auto-fetching and building")
      set(FFG_INSTALL_DIR ${CMAKE_BINARY_DIR}/_deps/ffglitch-core)
      find_program(FFG_ASSEMBLER nasm)
      if(NOT FFG_ASSEMBLER)
        find_program(FFG_ASSEMBLER yasm)
      endif()
      set(FFG_X86ASM_OPTION "")
      if(NOT FFG_ASSEMBLER)
        message(WARNING "nasm/yasm not found; configuring ffglitch-core with --disable-x86asm")
        set(FFG_X86ASM_OPTION "--disable-x86asm")
      endif()
      ExternalProject_Add(ffglitch_core
        PREFIX ${CMAKE_BINARY_DIR}/_deps/ffglitch_core_ep
        GIT_REPOSITORY https://github.com/ramiropolla/ffglitch-core.git
        GIT_TAG ffedit-0.10
        CONFIGURE_COMMAND ./configure --enable-static --disable-shared ${FFG_X86ASM_OPTION} --prefix=${FFG_INSTALL_DIR}
        BUILD_COMMAND $(MAKE)
        INSTALL_COMMAND $(MAKE) install
        BUILD_IN_SOURCE 1
        UPDATE_DISCONNECTED 1
      )
      add_dependencies(${PROJECT_NAME} ffglitch_core)
      target_include_directories(${PROJECT_NAME} PRIVATE ${FFG_INSTALL_DIR}/include)
      target_link_directories(${PROJECT_NAME} PRIVATE ${FFG_INSTALL_DIR}/lib)
      target_link_libraries(${PROJECT_NAME} PRIVATE avformat avcodec avutil swresample swscale avfilter avdevice z bz2)
      if(APPLE)
        target_link_libraries(${PROJECT_NAME} PRIVATE iconv)
      endif()
      target_compile_definitions(${PROJECT_NAME} PRIVATE HAVE_FFGLITCH_CORE=1)
    endif()
  else()
    message(STATUS "FFglitch disabled. Reconfigure with -DVISUALIZA_ENABLE_FFGLITCH=ON to build FFmpeg-backed glitch playback.")
  endif()

  if(APPLE)
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
      "-framework Cocoa"
    )
  endif()

  add_dependencies(${PROJECT_NAME} glad)

  if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4)
  else()
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wpedantic)
    if(VISUALIZA_ENABLE_GPROF)
      target_compile_options(${PROJECT_NAME} PRIVATE -pg)
      target_link_options(${PROJECT_NAME} PRIVATE -pg)
    endif()
  endif()

  add_custom_target(run
    COMMAND ${PROJECT_NAME}
    DEPENDS ${PROJECT_NAME}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  )

  set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT ${PROJECT_NAME})

  add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/assets ${CMAKE_CURRENT_BINARY_DIR}/assets)
endif()
