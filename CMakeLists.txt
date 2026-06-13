cmake_minimum_required(VERSION 3.20)
project(WebViewCpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 本地依赖路径
set(LOCAL_PACKAGES_DIR "${CMAKE_SOURCE_DIR}/third_party")
set(WEBVIEW2_DIR "${LOCAL_PACKAGES_DIR}/microsoft.web.webview2.1.0.3967.48")
set(NUGET_PACKAGES_DIR "${CMAKE_BINARY_DIR}/_nuget_packages")
set(NUGET_CONFIG "${CMAKE_SOURCE_DIR}/nuget.config")

# ============================================================
# NuGet: nlohmann.json + WIL
# ============================================================
find_program(NUGET_EXE NAMES nuget)
if(NOT NUGET_EXE)
    set(NUGET_EXE "${CMAKE_BINARY_DIR}/nuget.exe")
    if(NOT EXISTS "${NUGET_EXE}")
        message(STATUS "Downloading nuget.exe...")
        file(DOWNLOAD
            "https://dist.nuget.org/win-x86-commandline/latest/nuget.exe"
            "${NUGET_EXE}"
            SHOW_PROGRESS STATUS DOWNLOAD_STATUS)
        list(GET DOWNLOAD_STATUS 0 STATUS_CODE)
        if(NOT STATUS_CODE EQUAL 0)
            message(FATAL_ERROR "Failed to download nuget.exe")
        endif()
    endif()
endif()

macro(install_nuget_package PKG_NAME VERSION)
    if(NOT EXISTS "${NUGET_PACKAGES_DIR}/${PKG_NAME}")
        message(STATUS "Installing ${PKG_NAME} ${VERSION}...")
        execute_process(
            COMMAND ${NUGET_EXE} install ${PKG_NAME} -Version ${VERSION}
                    -ExcludeVersion -OutputDirectory ${NUGET_PACKAGES_DIR}
                    -ConfigFile ${NUGET_CONFIG}
            RESULT_VARIABLE NUGET_RESULT)
        if(NOT NUGET_RESULT EQUAL 0)
            message(FATAL_ERROR "Failed to install ${PKG_NAME}")
        endif()
    else()
        message(STATUS "Found ${PKG_NAME}")
    endif()
endmacro()

install_nuget_package("nlohmann.json" "3.12.0")
install_nuget_package("Microsoft.Windows.ImplementationLibrary" "1.0.240122.1")

# ============================================================
# webview library (FetchContent - 官方推荐方式)
# ============================================================
include(FetchContent)
FetchContent_Declare(
    webview_lib
    GIT_REPOSITORY https://github.com/webview/webview
    GIT_TAG 0.12.0
)

# MSVC: treat source files as UTF-8, suppress C4819 warnings
if(MSVC)
    add_compile_options(/utf-8)
endif()

# 禁用 webview 的 examples/tests/docs
set(WEBVIEW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WEBVIEW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WEBVIEW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(WEBVIEW_BUILD_AMALGAMATION OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(webview_lib)

# ============================================================
# 主目标
# ============================================================
add_executable(${PROJECT_NAME} WIN32
    src/main.cpp
    src/WebViewWrapper.cpp
)

set_target_properties(${PROJECT_NAME} PROPERTIES
    VS_GLOBAL_RestorePackagesConfig "false"
    VS_GLOBAL_NuGetPack "false"
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${NUGET_PACKAGES_DIR}/nlohmann.json/build/native/include
    ${NUGET_PACKAGES_DIR}/Microsoft.Windows.ImplementationLibrary/include
)

# 链接: webview::core_static + 本地 WebView2 Loader + Windows 系统库
# 链接: webview::core_static + Windows 系统库
# 注意：使用 core_static 需要确保 WEBVIEW_STATIC 只在 webview 库编译时定义
target_link_libraries(${PROJECT_NAME} PRIVATE
    webview::core_static
)

# 复制 WebView2Loader.dll 到输出目录
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${WEBVIEW2_DIR}/runtimes/win-x64/native/WebView2Loader.dll"
        $<TARGET_FILE_DIR:${PROJECT_NAME}>/WebView2Loader.dll
)

# 复制 demo.html 到输出目录（与 exe 同目录）
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/src/demo.html"
        $<TARGET_FILE_DIR:${PROJECT_NAME}>/demo.html
)
