cmake_minimum_required(VERSION 3.21)
project(cso_pak_tool LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

# ---------------------------------------------------------------------------
# Core library: the pak format + SnowCipher, shared by the CLI and the GUI.
# Untouched apart from a small public API addition (Entries()/ExtractEntry())
# in PakArchive.h/.cpp needed so the GUI can preview single files.
# ---------------------------------------------------------------------------
add_library(cso_pak_core STATIC
	core/PakArchive.h
	core/PakArchive.cpp
	core/SnowCipher.h
	core/SnowCipher.cpp
	core/CsoDecoder.h
	core/CsoDecoder.cpp
)
target_include_directories(cso_pak_core PUBLIC core)

# ---------------------------------------------------------------------------
# Original console tool (unpack/pack/patch). Unchanged.
# ---------------------------------------------------------------------------
add_executable(cso-pak-cli core/Main.cpp)
target_link_libraries(cso-pak-cli PRIVATE cso_pak_core)

# ---------------------------------------------------------------------------
# Qt6 GUI browser (Explorer-style tree + text/image/properties preview).
# ---------------------------------------------------------------------------
find_package(Qt6 REQUIRED COMPONENTS Widgets Multimedia MultimediaWidgets)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

add_executable(cso-pak-gui WIN32
	gui/src/main.cpp
	gui/src/MainWindow.h
	gui/src/MainWindow.cpp
	gui/src/PakTreeModel.h
	gui/src/PakTreeModel.cpp
	gui/src/TgaImage.h
	gui/src/TgaImage.cpp
)
target_link_libraries(cso-pak-gui PRIVATE cso_pak_core Qt6::Widgets Qt6::Multimedia Qt6::MultimediaWidgets)

# EUC-KR/CP949 (Korean) decoding for .cso table previews uses the Win32
# codepage-conversion API directly, so it's Windows-only. On other platforms
# MainWindow falls back to plain UTF-8/Latin-1 decoding for .cso content
# (guarded by #ifdef _WIN32 in MainWindow.cpp).
if(WIN32)
	target_sources(cso-pak-gui PRIVATE gui/src/EucKrCodec.h gui/src/EucKrCodec.cpp)
endif()

# App icon + version/author info shown in Explorer and the exe's Properties
# dialog. Resource compilation (.rc) is an MSVC/Windows-only step.
if(WIN32)
	target_sources(cso-pak-gui PRIVATE gui/resources/app.rc)
endif()

# windeployqt is a Windows-only Qt tool; guard it so the project still
# builds cleanly on other platforms (e.g. during development/testing).
if(WIN32 AND TARGET Qt6::windeployqt)
	add_custom_command(TARGET cso-pak-gui POST_BUILD
		COMMAND Qt6::windeployqt $<TARGET_FILE:cso-pak-gui>
	)
endif()

# ===========================================================================
# Automatically build a clean distribution (Deployment)
# ===========================================================================
if(CMAKE_BUILD_TYPE STREQUAL "Release")

    # Path for clean release in external project folder
    set(CMAKE_INSTALL_PREFIX "${CMAKE_SOURCE_DIR}/out/install")

    # 1. Copy program's executable files (.exe)
    install(TARGETS cso-pak-cli cso-pak-gui
        RUNTIME DESTINATION .
    )

    # 2. Find and copy DLL strictly at the time of project installation
    install(CODE [[
        file(GLOB ROOT_DLL_FILES "${CMAKE_BINARY_DIR}/*.dll")
        if(ROOT_DLL_FILES)
            file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}" TYPE FILE FILES ${ROOT_DLL_FILES})
        endif()
    ]])

    # 3. Copy only necessary Qt resource service folders
    install(DIRECTORY 
        "${CMAKE_BINARY_DIR}/platforms"
        "${CMAKE_BINARY_DIR}/styles"
        "${CMAKE_BINARY_DIR}/translations"
        "${CMAKE_BINARY_DIR}/iconengines"
        "${CMAKE_BINARY_DIR}/imageformats"
        "${CMAKE_BINARY_DIR}/networkinformation"
        "${CMAKE_BINARY_DIR}/tls"
        "${CMAKE_BINARY_DIR}/generic"
        "${CMAKE_BINARY_DIR}/multimedia"
        DESTINATION .
        OPTIONAL
    )
endif()
