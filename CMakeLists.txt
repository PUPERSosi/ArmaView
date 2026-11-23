cmake_minimum_required(VERSION 3.28)
project(ArmaView VERSION 1.1.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# version header (src/Version.h.in): project version + commit at configure time
execute_process(COMMAND git rev-parse --short HEAD WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                OUTPUT_VARIABLE AV_GIT_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
configure_file(src/Version.h.in ${CMAKE_BINARY_DIR}/generated/Version.h @ONLY)

find_package(imgui CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(unofficial-imgui-node-editor CONFIG REQUIRED)
find_path(CGLTF_INCLUDE_DIRS "cgltf.h" REQUIRED) # header-only glTF 2.0 reader (MIT)

# ---- ArmaViewCore: formats, configs, skeleton, animation, CfgMoves, materials (CPU), strings. No ImGui / Win32 / D3D:
# it links glm only (+ the header-only cgltf for glTF import), and src/core/CoreGuard.h (included last by every core .cpp) stops windows.h / d3d11.h / imgui.h.
add_library(ArmaViewCore STATIC
    src/core/CoreGuard.h
    src/core/FileSystem.h   src/core/FileSystem.cpp
    src/core/FileWatcher.h
    src/core/EventBus.h
    src/core/CommandStack.h
    src/core/Session.h      src/core/Session.cpp
    src/core/Validation.h   src/core/Validation.cpp
    src/core/SelfTest.h     src/core/SelfTest.cpp
    src/core/Fuzz.h         src/core/Fuzz.cpp
    src/core/RetargetJob.h  src/core/RetargetJob.cpp
    src/core/MovesSim.h     src/core/MovesSim.cpp
    src/core/MovesEditor.h  src/core/MovesEditor.cpp
    src/core/LayersJob.h    src/core/LayersJob.cpp
    src/core/ReloadSync.h   src/core/ReloadSync.cpp
    src/core/MaterialSet.h  src/core/MaterialSet.cpp
    src/core/Playback.h     src/core/Playback.cpp
    src/core/MovesLoader.h  src/core/MovesLoader.cpp
    src/core/AssetSession.h src/core/AssetSession.cpp
    src/core/ShaderSurvey.h src/core/ShaderSurvey.cpp
    src/core/Settings.h     src/core/Settings.cpp
    src/core/Paths.h
    src/core/Text.h         src/core/Text.cpp
    src/core/Log.h
    src/core/Perf.h         src/core/Perf.cpp
    src/core/Strings.h      src/core/Strings.cpp
    src/core/ArmaConfig.h   src/core/ArmaConfig.cpp
    src/core/P3D.h          src/core/P3D.cpp
    src/core/Gltf.h         src/core/Gltf.cpp
    src/core/Texture.h      src/core/Texture.cpp
    src/core/RvMaterial.h   src/core/RvMaterial.cpp
    src/core/Skeleton.h     src/core/Skeleton.cpp
    src/core/RTM.h          src/core/RTM.cpp
    src/core/Moves.h        src/core/Moves.cpp
    src/core/MovesEdit.h    src/core/MovesEdit.cpp
    src/core/Retarget.h     src/core/Retarget.cpp
    src/core/AnimProject.h  src/core/AnimProject.cpp
    src/core/Visibility.h   src/core/Visibility.cpp
    src/core/Mesh.h
    src/core/Attachments.h  src/core/Attachments.cpp
    src/core/Unit.h  src/core/Unit.cpp
    src/core/Weapon.h       src/core/Weapon.cpp
    src/core/Lighting.h
    src/core/WorldLight.h   src/core/WorldLight.cpp
    src/core/ModCheck.h     src/core/ModCheck.cpp
    src/core/IconStandards.h src/core/IconStandards.cpp
    src/core/IconImage.h    src/core/IconImage.cpp
)
target_include_directories(ArmaViewCore PUBLIC ${CMAKE_SOURCE_DIR}/src/core)
target_include_directories(ArmaViewCore PRIVATE ${CGLTF_INCLUDE_DIRS})
target_link_libraries(ArmaViewCore PUBLIC glm::glm)
target_compile_definitions(ArmaViewCore PUBLIC GLM_FORCE_LEFT_HANDED GLM_FORCE_DEPTH_ZERO_TO_ONE GLM_ENABLE_EXPERIMENTAL)
target_compile_options(ArmaViewCore PRIVATE /W4 /we4456 /we4457 /we4458 /permissive- /utf-8 /MP /Zc:__cplusplus
    $<$<CONFIG:Release>:/O2 /GL /Gy>)

# ---- ArmaViewViewCore: the view logic and the interfaces another UI builds on (app mode, states graph host, point
# overlay settings, input state, scene interaction).
# No ImGui / Win32 / D3D, like the core: src/view/ViewCoreGuard.h is included last by every .cpp of it.
add_library(ArmaViewViewCore STATIC
    src/view/ViewCoreGuard.h
    src/view/AppMode.h
    src/view/IStatesGraphHost.h
    src/view/ViewCoreHeaders.cpp
    src/view/PointOverlaySettings.h src/view/PointOverlaySettings.cpp
    src/view/InputState.h
    src/view/SceneInteraction.h src/view/SceneInteraction.cpp
    src/view/ViewCoreTest.h src/view/ViewCoreTest.cpp
)
# src/render: Camera.h only (header only, no D3D)
target_include_directories(ArmaViewViewCore PUBLIC ${CMAKE_SOURCE_DIR}/src/view ${CMAKE_SOURCE_DIR}/src/render)
target_link_libraries(ArmaViewViewCore PUBLIC ArmaViewCore)
target_compile_options(ArmaViewViewCore PRIVATE /W4 /permissive- /utf-8 /MP /Zc:__cplusplus $<$<CONFIG:Release>:/O2 /GL /Gy>)

# ---- AV.Render: D3D11 renderer, GPU texture cache, camera. Knows the core's data, not ImGui.
add_library(ArmaViewRender STATIC
    src/render/Renderer.h   src/render/Renderer.cpp
    src/render/Material.h   src/render/Material.cpp
    src/render/RenderSelfTest.h src/render/RenderSelfTest.cpp
    src/render/Camera.h
)
target_include_directories(ArmaViewRender PUBLIC ${CMAKE_SOURCE_DIR}/src/render)
target_link_libraries(ArmaViewRender PUBLIC ArmaViewCore d3d11 dxgi d3dcompiler windowscodecs ole32)
target_compile_definitions(ArmaViewRender PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(ArmaViewRender PRIVATE /W4 /permissive- /utf-8 /MP /Zc:__cplusplus $<$<CONFIG:Release>:/O2 /GL /Gy>)

# ---- AV.PlatformWin: Win32 parts (code page / SUBST hooks of the core, file watcher, local paths, memory, ffmpeg recorder)
add_library(ArmaViewPlatform STATIC
    src/platform/WinPlatform.h src/platform/WinPlatform.cpp
    src/platform/PerfMemory.h  src/platform/PerfMemory.cpp
    src/platform/LocalPaths.h  src/platform/LocalPaths.cpp
    src/platform/FfmpegPipe.h  src/platform/FfmpegPipe.cpp
    src/platform/WinFileSystem.h src/platform/WinFileSystem.cpp
    src/platform/RunProgram.h  src/platform/RunProgram.cpp
    src/platform/WinPaths.h    src/platform/WinPaths.cpp
    src/platform/Input.h       src/platform/Input.cpp
    src/platform/Dialogs.h     src/platform/Dialogs.cpp
    src/platform/ImageFiles.h  src/platform/ImageFiles.cpp
)
target_include_directories(ArmaViewPlatform PUBLIC ${CMAKE_SOURCE_DIR}/src/platform)
target_link_libraries(ArmaViewPlatform PUBLIC ArmaViewCore psapi windowscodecs ole32)
target_compile_definitions(ArmaViewPlatform PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_definitions(ArmaViewPlatform PRIVATE ARMAVIEW_SOURCE_DIR="${CMAKE_SOURCE_DIR}") # LocalPaths: the ini in the source tree
target_compile_options(ArmaViewPlatform PRIVATE /W4 /permissive- /utf-8 /MP /Zc:__cplusplus $<$<CONFIG:Release>:/O2 /GL /Gy>)

# ---- armaview-cli: the core without a window (validate a mod, model info, core self test)
add_executable(armaview-cli src/cli/main.cpp)
target_link_libraries(armaview-cli PRIVATE ArmaViewCore ArmaViewViewCore ArmaViewPlatform)
target_compile_options(armaview-cli PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus $<$<CONFIG:Release>:/O2 /GL /Gy>)
target_link_options(armaview-cli PRIVATE $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF>)

# ---- the viewer (UI: ImGui panels, D3D11 renderer, Win32 platform parts)
add_executable(ArmaView WIN32
    src/main.cpp
    src/App.h       src/App.cpp
    src/StatesGraph.h src/StatesGraph.cpp
    src/view/WeaponScene.h src/view/WeaponScene.cpp
    src/view/SceneView.h src/view/SceneView.cpp
    src/view/IconRenderer.h src/view/IconRenderer.cpp
    src/view/SceneInput.h src/view/SceneInput.cpp
    src/view/SceneOverlay.h src/view/SceneOverlay.cpp
    src/view/UiSettings.h src/view/UiSettings.cpp
    src/view/LayoutStore.h src/view/LayoutStore.cpp
    src/view/Recorder.h src/view/Recorder.cpp
    src/view/AnimationView.h src/view/AnimationView.cpp
    src/automation/ViewerAutomation.h src/automation/ViewerAutomation.cpp
    src/NodeGraph.h src/NodeGraph.cpp
    src/automation/UiScript.h src/automation/UiScript.cpp
    src/PointOverlay.h src/PointOverlay.cpp
    src/Commands.h src/Commands.cpp
    src/Lang.h src/Lang.cpp
    src/panels/LogPanel.h src/panels/LogPanel.cpp
    src/panels/InfoWindows.h src/panels/InfoWindows.cpp
    src/panels/CommandPalette.h src/panels/CommandPalette.cpp
    src/panels/CrewPanel.h src/panels/CrewPanel.cpp
    src/panels/UnitPanel.h src/panels/UnitPanel.cpp
    src/panels/StartScreen.h src/panels/StartScreen.cpp
    src/panels/SourcesPanel.h src/panels/SourcesPanel.cpp
    src/panels/SkeletonPanel.h src/panels/SkeletonPanel.cpp
    src/panels/GettingStarted.h src/panels/GettingStarted.cpp
    src/panels/ValidationPanel.h src/panels/ValidationPanel.cpp
    src/panels/RetargetPanel.h src/panels/RetargetPanel.cpp
    src/panels/MaterialsPanel.h src/panels/MaterialsPanel.cpp
    src/panels/RecordPanel.h src/panels/RecordPanel.cpp
    src/panels/TimelinePanel.h src/panels/TimelinePanel.cpp
    src/panels/LayersPanel.h src/panels/LayersPanel.cpp
    src/panels/ReloadPanel.h src/panels/ReloadPanel.cpp
    src/panels/OpticsPanel.h src/panels/OpticsPanel.cpp
    src/panels/WeaponView.h
    src/panels/MovesPanel.h src/panels/MovesPanel.cpp
    src/panels/StatesPanel.h src/panels/StatesPanel.cpp
    src/panels/MoveEditorPanel.h src/panels/MoveEditorPanel.cpp
    src/panels/InspectorPanel.h src/panels/InspectorPanel.cpp
    src/panels/DisplaySettings.h
    src/panels/DisplayPanel.h src/panels/DisplayPanel.cpp
    src/panels/UiShell.h src/panels/UiShell.cpp
    src/panels/ModelOutliner.h src/panels/ModelOutliner.cpp
    src/panels/ShaderSurveyPanel.h src/panels/ShaderSurveyPanel.cpp
    src/panels/ClosePrompt.h src/panels/ClosePrompt.cpp
    src/panels/ViewHost.h
    src/panels/ModCheckPanel.h src/panels/ModCheckPanel.cpp
    src/panels/IconsPanel.h src/panels/IconsPanel.cpp
    src/ArmaView.rc src/Version.h.in
)

target_link_libraries(ArmaView PRIVATE ArmaViewCore ArmaViewViewCore ArmaViewRender ArmaViewPlatform imgui::imgui unofficial::imgui-node-editor::imgui-node-editor glm::glm d3d11 dxgi d3dcompiler comdlg32 shell32 ole32 windowscodecs dbghelp psapi)

target_compile_definitions(ArmaView PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
    ARMAVIEW_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    $<$<NOT:$<CONFIG:Release>>:ARMAVIEW_DEV>)

target_compile_options(ArmaView PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /permissive- /utf-8 /MP /Zc:__cplusplus>)
target_include_directories(ArmaView PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/src/panels ${CMAKE_SOURCE_DIR}/src/view ${CMAKE_SOURCE_DIR}/src/automation ${CMAKE_BINARY_DIR}/generated ${CMAKE_SOURCE_DIR}/res)

# Release: full optimisation + link-time code generation, unused code removed
target_compile_options(ArmaView PRIVATE $<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:CXX>>:/O2 /GL /Gy>)
target_link_options(ArmaView PRIVATE $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF>)

set_target_properties(ArmaView PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:ArmaView>")

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT ArmaView)

# Parser fuzzing (preset "fuzz"): libFuzzer + AddressSanitizer over the file parsers only (src/Fuzz.h)
option(ARMAVIEW_FUZZ "Build ArmaViewFuzz (libFuzzer + ASan) instead of the viewer" OFF)
if(ARMAVIEW_FUZZ)
    add_executable(ArmaViewFuzz src/FuzzMain.cpp)
    target_link_libraries(ArmaViewFuzz PRIVATE ArmaViewCore)
    target_compile_definitions(ArmaViewFuzz PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    # the core is built with the same sanitizer flags (its parsers are what is fuzzed)
    target_compile_options(ArmaViewCore PRIVATE /Zi /fsanitize=address /fsanitize=fuzzer-no-link)
    set_target_properties(ArmaViewCore PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
    target_compile_options(ArmaViewFuzz PRIVATE /permissive- /utf-8 /Zc:__cplusplus /Zi /fsanitize=address /fsanitize=fuzzer)
    target_link_options(ArmaViewFuzz PRIVATE /DEBUG /INCREMENTAL:NO)
    set_target_properties(ArmaViewFuzz PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
endif()

# ---- the core stays free of the UI / Win32 / D3D: configuring fails when someone links them or adds their headers.
# (CoreGuard.h #errors on windows.h / d3d11.h / imgui.h reaching a core .cpp through any header.)
function(av_check_core_dependencies)
    set(forbidden "imgui|d3d11|d3dcompiler|dxgi|user32|gdi32|comdlg32|shell32|ole32|windowscodecs|node-editor|ArmaViewRender|ArmaViewPlatform")
    foreach(prop LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
        get_target_property(libs ArmaViewCore ${prop})
        if(libs)
            foreach(lib IN LISTS libs)
                if(lib MATCHES "${forbidden}")
                    message(FATAL_ERROR "ArmaViewCore must not link '${lib}' (${prop}): the core knows no UI / Win32 / D3D")
                endif()
            endforeach()
        endif()
    endforeach()
    # include paths: the core's own folder, glm and the header-only cgltf only
    get_target_property(dirs ArmaViewCore INCLUDE_DIRECTORIES)
    foreach(dir IN LISTS dirs)
        if(NOT dir MATCHES "src/core$" AND NOT dir MATCHES "glm" AND NOT dir STREQUAL "${CGLTF_INCLUDE_DIRS}")
            message(FATAL_ERROR "ArmaViewCore include path '${dir}': only src/core, glm and cgltf are allowed")
        endif()
    endforeach()
endfunction()
av_check_core_dependencies()

# ---- the same for the view logic another UI builds on: no ImGui / Win32 / D3D links; include paths of the core,
# src/view and src/render (the header-only Camera.h); these files must not include ImGui themselves either.
# (ViewCoreGuard.h #errors on windows.h / d3d11.h / imgui.h reaching its .cpp files through any header.)
function(av_check_view_core_dependencies)
    set(forbidden "imgui|d3d11|d3dcompiler|dxgi|user32|gdi32|comdlg32|shell32|ole32|windowscodecs|node-editor|ArmaViewRender|ArmaViewPlatform")
    foreach(prop LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
        get_target_property(libs ArmaViewViewCore ${prop})
        if(libs)
            foreach(lib IN LISTS libs)
                if(lib MATCHES "${forbidden}")
                    message(FATAL_ERROR "ArmaViewViewCore must not link '${lib}' (${prop}): the view logic knows no UI / Win32 / D3D")
                endif()
            endforeach()
        endif()
    endforeach()
    get_target_property(dirs ArmaViewViewCore INCLUDE_DIRECTORIES)
    foreach(dir IN LISTS dirs)
        if(NOT dir MATCHES "src/view$" AND NOT dir MATCHES "src/render$" AND NOT dir MATCHES "src/core$" AND NOT dir MATCHES "glm")
            message(FATAL_ERROR "ArmaViewViewCore include path '${dir}': only src/view, src/render (Camera.h), src/core and glm are allowed")
        endif()
    endforeach()
    foreach(f AppMode.h IStatesGraphHost.h InputState.h SceneInteraction.h SceneInteraction.cpp PointOverlaySettings.h PointOverlaySettings.cpp ViewCoreTest.cpp)
        file(READ ${CMAKE_SOURCE_DIR}/src/view/${f} text)
        if(text MATCHES "#include[ ]*[<\"][^>\"]*(imgui|windows\\.h|d3d11)")
            message(FATAL_ERROR "src/view/${f} includes ImGui / Win32 / D3D: the view logic must stay UI-free")
        endif()
    endforeach()
    # Camera.h is the only file of src/render the view logic may use: header only, no D3D
    file(READ ${CMAKE_SOURCE_DIR}/src/render/Camera.h text)
    if(text MATCHES "#include[ ]*[<\"][^>\"]*(imgui|windows\\.h|d3d11)")
        message(FATAL_ERROR "src/render/Camera.h must stay free of ImGui / Win32 / D3D (the view logic includes it)")
    endif()
endfunction()
av_check_view_core_dependencies()
