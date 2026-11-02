cmake_minimum_required(VERSION 3.20)
project(ChunkHarbor VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(linux_default OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(linux_default ON)
endif()
option(CHUNKHARBOR_BUILD_SERVER "Build Linux server" ${linux_default})
option(CHUNKHARBOR_BUILD_CLI "Build Linux CLI" ${linux_default})
option(CHUNKHARBOR_BUILD_GUI "Build Qt connection prototype" OFF)
option(CHUNKHARBOR_DEPLOY_GUI "Copy Windows Qt GUI runtime dependencies next to the executable" OFF)
option(CHUNKHARBOR_BUILD_TESTS "Build tests" ON)
option(CHUNKHARBOR_SANITIZERS "Enable ASan/UBSan" OFF)
option(CHUNKHARBOR_BUILD_HASH "Build OpenSSL-backed portable SHA-256 utilities" ${linux_default})
if(NOT linux_default AND CHUNKHARBOR_BUILD_SERVER)
    message(FATAL_ERROR "Server requires Linux; disable CHUNKHARBOR_BUILD_SERVER")
endif()
if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()
if(CHUNKHARBOR_SANITIZERS)
    if(NOT linux_default)
        message(FATAL_ERROR "Sanitizer preset is currently Linux only")
    endif()
    add_compile_options(-g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie)
    add_link_options(-fsanitize=address,undefined -no-pie)
endif()
add_library(chunkharbor_protocol src/protocol/FrameCodec.cpp src/protocol/Handshake.cpp
    src/protocol/DataChunk.cpp src/protocol/BeginUpload.cpp src/protocol/UploadPlan.cpp
    src/protocol/ChunkAck.cpp src/protocol/UploadControl.cpp src/protocol/SessionStatus.cpp
    src/protocol/ManifestPage.cpp src/protocol/DeltaControl.cpp src/protocol/PlanPage.cpp
    src/protocol/FilesList.cpp src/protocol/Download.cpp)
target_include_directories(chunkharbor_protocol PUBLIC include)
target_include_directories(chunkharbor_protocol SYSTEM PRIVATE third_party)
add_library(chunkharbor_client_core src/client_core/EchoExchange.cpp src/client_core/HandshakeExchange.cpp
    src/client_core/DeltaPlanner.cpp src/client_core/SourceSnapshot.cpp src/client_core/DeltaExchange.cpp
    src/client_core/DirectoryScanner.cpp src/client_core/BackupBatch.cpp)
target_include_directories(chunkharbor_client_core SYSTEM PRIVATE third_party)
target_link_libraries(chunkharbor_client_core PUBLIC chunkharbor_protocol)
if(CHUNKHARBOR_BUILD_HASH)
    find_package(OpenSSL 1.1.1 REQUIRED COMPONENTS Crypto)
    add_library(chunkharbor_hash src/common/Sha256.cpp)
    target_include_directories(chunkharbor_hash PUBLIC include)
    target_link_libraries(chunkharbor_hash PRIVATE OpenSSL::Crypto)
endif()
if(linux_default)
    find_package(Threads REQUIRED)
    find_package(SQLite3 REQUIRED)
    add_library(chunkharbor_linux_net src/common/Logger.cpp src/net/EventLoop.cpp
        src/net/EpollPoller.cpp src/net/Channel.cpp src/net/Buffer.cpp
        src/net/TcpConnection.cpp src/net/TcpServer.cpp src/concurrency/DiskExecutor.cpp)
    target_include_directories(chunkharbor_linux_net PUBLIC include)
    target_link_libraries(chunkharbor_linux_net PUBLIC Threads::Threads)
    add_library(chunkharbor_server_core src/storage/TempFileStore.cpp src/storage/SqliteStore.cpp)
    target_include_directories(chunkharbor_server_core PUBLIC include)
    target_compile_definitions(chunkharbor_server_core PRIVATE
        CHUNKHARBOR_SCHEMA_V1_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/storage/schema_v1.sql")
    target_link_libraries(chunkharbor_server_core PUBLIC chunkharbor_protocol PRIVATE Threads::Threads SQLite::SQLite3)
    if(CHUNKHARBOR_BUILD_HASH)
        target_sources(chunkharbor_server_core PRIVATE src/storage/UploadStorage.cpp src/storage/DeltaStorage.cpp)
        target_link_libraries(chunkharbor_server_core PRIVATE chunkharbor_hash chunkharbor_client_core)
    endif()
endif()
if(CHUNKHARBOR_BUILD_SERVER)
    if(NOT CHUNKHARBOR_BUILD_HASH)
        message(FATAL_ERROR "Server upload requires CHUNKHARBOR_BUILD_HASH=ON")
    endif()
    add_executable(chunkharbor-server apps/server/main.cpp apps/server/EchoHandler.cpp apps/server/HandshakeHandler.cpp
        apps/server/UploadHandler.cpp)
    target_link_libraries(chunkharbor-server PRIVATE chunkharbor_linux_net chunkharbor_protocol chunkharbor_server_core)
endif()
if(CHUNKHARBOR_BUILD_CLI)
    if(linux_default)
        if(NOT CHUNKHARBOR_BUILD_HASH)
            message(FATAL_ERROR "Linux CLI upload requires CHUNKHARBOR_BUILD_HASH=ON")
        endif()
        add_executable(chunkharbor-cli apps/cli/main.cpp apps/cli/EchoClient.cpp apps/cli/UploadClient.cpp
            apps/cli/DeltaCommand.cpp apps/cli/DeltaClient.cpp apps/cli/DownloadClient.cpp)
        target_link_libraries(chunkharbor-cli PRIVATE chunkharbor_client_core chunkharbor_hash)
    else()
        find_package(Qt6 6.5 REQUIRED COMPONENTS Core Network)
        add_executable(chunkharbor-cli apps/cli/QtCliMain.cpp apps/cli/DeltaCommand.cpp apps/cli/QtDeltaClient.cpp)
        target_link_libraries(chunkharbor-cli PRIVATE chunkharbor_client_core Qt6::Core Qt6::Network)
    endif()
    target_include_directories(chunkharbor-cli SYSTEM PRIVATE third_party)
endif()
if(CHUNKHARBOR_BUILD_GUI)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Network)
    add_library(chunkharbor_gui_state STATIC
        apps/gui/TaskModel.cpp apps/gui/TaskModel.h
        apps/gui/WorkspaceStore.cpp apps/gui/WorkspaceStore.h)
    set_target_properties(chunkharbor_gui_state PROPERTIES AUTOMOC ON)
    target_include_directories(chunkharbor_gui_state PUBLIC apps/gui)
    target_link_libraries(chunkharbor_gui_state PUBLIC Qt6::Core Qt6::Gui)
    add_library(chunkharbor_gui_transfer STATIC
        apps/gui/TransferController.cpp apps/gui/TransferController.h
        apps/gui/BackupWorker.cpp apps/gui/TransferTypes.h
        apps/gui/QtTcpTransport.cpp apps/gui/QtTcpTransport.h
        apps/gui/ServerFilesModel.cpp apps/gui/ServerFilesModel.h
        apps/gui/FileListController.cpp apps/gui/FileListController.h)
    set_target_properties(chunkharbor_gui_transfer PROPERTIES AUTOMOC ON)
    target_link_libraries(chunkharbor_gui_transfer PUBLIC chunkharbor_gui_state chunkharbor_client_core Qt6::Network)
    add_library(chunkharbor_gui_pages STATIC apps/gui/MainWindow.cpp apps/gui/MainWindow.h
        apps/gui/widgets/InfoCard.cpp apps/gui/widgets/TableKit.cpp apps/gui/pages/WorkspacePage.cpp
        apps/gui/pages/DashboardPage.cpp apps/gui/pages/BackupPage.cpp
        apps/gui/pages/FilesPage.cpp apps/gui/pages/VersionsPage.cpp
        apps/gui/pages/LogsPage.cpp apps/gui/pages/SettingsPage.cpp)
    set_target_properties(chunkharbor_gui_pages PROPERTIES AUTOMOC ON)
    target_link_libraries(chunkharbor_gui_pages PUBLIC chunkharbor_gui_transfer Qt6::Widgets)
    qt_add_executable(chunkharbor-gui WIN32 apps/gui/main.cpp apps/gui/styles/styles.qrc)
    set_target_properties(chunkharbor-gui PROPERTIES AUTOMOC ON AUTORCC ON)
    target_include_directories(chunkharbor-gui PRIVATE apps/gui)
    target_link_libraries(chunkharbor-gui PRIVATE chunkharbor_gui_pages)
    find_file(CHUNKHARBOR_QT_ZH_CN_TRANSLATION qtbase_zh_CN.qm
        HINTS "${Qt6_DIR}/../../../translations" NO_DEFAULT_PATH)
    if(CHUNKHARBOR_QT_ZH_CN_TRANSLATION)
        get_filename_component(qt_translation_dir "${CHUNKHARBOR_QT_ZH_CN_TRANSLATION}" DIRECTORY)
        qt_add_resources(chunkharbor-gui qt_chinese_translations
            PREFIX "/i18n" BASE "${qt_translation_dir}"
            FILES "${CHUNKHARBOR_QT_ZH_CN_TRANSLATION}")
    else()
        message(WARNING "Qt Simplified Chinese translation not found; standard dialogs use system defaults")
    endif()
    if(MSVC)
        target_compile_options(chunkharbor-gui PRIVATE /utf-8)
        target_compile_options(chunkharbor_gui_state PRIVATE /utf-8)
        target_compile_options(chunkharbor_gui_transfer PRIVATE /utf-8)
        target_compile_options(chunkharbor_gui_pages PRIVATE /utf-8)
    endif()
    if(WIN32 AND CHUNKHARBOR_DEPLOY_GUI)
        find_program(CHUNKHARBOR_WINDEPLOYQT_EXECUTABLE NAMES windeployqt
            HINTS "${Qt6_DIR}/../../../bin" NO_DEFAULT_PATH REQUIRED)
        get_filename_component(qt_runtime_bin "${CHUNKHARBOR_WINDEPLOYQT_EXECUTABLE}" DIRECTORY)
        get_filename_component(compiler_runtime_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
        add_custom_command(TARGET chunkharbor-gui POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E env
                "PATH=${qt_runtime_bin};${compiler_runtime_bin};$ENV{PATH}"
                "${CHUNKHARBOR_WINDEPLOYQT_EXECUTABLE}"
                --no-translations --no-opengl-sw --compiler-runtime
                "$<TARGET_FILE:chunkharbor-gui>"
            COMMENT "Deploying Qt GUI runtime libraries and platform plugins"
            VERBATIM)
        # Existing headless GUI probes use this test-only platform plugin.
        if(CHUNKHARBOR_BUILD_TESTS AND EXISTS "${qt_runtime_bin}/../plugins/platforms/qoffscreen.dll")
            add_custom_command(TARGET chunkharbor-gui POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${qt_runtime_bin}/../plugins/platforms/qoffscreen.dll"
                    "$<TARGET_FILE_DIR:chunkharbor-gui>/platforms/qoffscreen.dll"
                VERBATIM)
        endif()
    endif()
endif()
if(CHUNKHARBOR_BUILD_TESTS)
    enable_testing()
    foreach(test delta_control plan_page delta_planner client_delta directory_scanner files_list download)
        add_executable(chunkharbor-${test}-test tests/unit/${test}_test.cpp)
        target_link_libraries(chunkharbor-${test}-test PRIVATE chunkharbor_client_core)
        if(MSVC)
            target_compile_options(chunkharbor-${test}-test PRIVATE /UNDEBUG)
        else()
            target_compile_options(chunkharbor-${test}-test PRIVATE -UNDEBUG)
        endif()
        add_test(NAME ${test}_test COMMAND chunkharbor-${test}-test)
        set_tests_properties(${test}_test PROPERTIES TIMEOUT 30)
    endforeach()
    add_executable(chunkharbor-manifest-page-test tests/unit/manifest_page_test.cpp)
    target_link_libraries(chunkharbor-manifest-page-test PRIVATE chunkharbor_protocol)
    add_test(NAME manifest_page_test COMMAND chunkharbor-manifest-page-test)
    if(CHUNKHARBOR_BUILD_GUI)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        add_executable(chunkharbor-gui-state-test tests/unit/gui_state_test.cpp)
        target_link_libraries(chunkharbor-gui-state-test PRIVATE chunkharbor_gui_state)
        add_test(NAME gui_state_test COMMAND chunkharbor-gui-state-test)
        set_tests_properties(gui_state_test PROPERTIES TIMEOUT 20)
        add_executable(chunkharbor-gui-transfer-probe tests/integration/gui_transfer_probe.cpp)
        target_link_libraries(chunkharbor-gui-transfer-probe PRIVATE chunkharbor_gui_pages)
        add_executable(chunkharbor-gui-files-probe tests/integration/gui_files_probe.cpp apps/gui/styles/styles.qrc)
        set_target_properties(chunkharbor-gui-files-probe PROPERTIES AUTORCC ON)
        target_link_libraries(chunkharbor-gui-files-probe PRIVATE chunkharbor_gui_pages)
        add_test(NAME gui_files_test COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/gui_files_test.py
            $<TARGET_FILE:chunkharbor-gui-files-probe>)
        set_tests_properties(gui_files_test PROPERTIES TIMEOUT 90)
        add_executable(chunkharbor-gui-worker-test tests/unit/gui_worker_test.cpp)
        add_executable(chunkharbor-backup-layout-test tests/unit/backup_layout_test.cpp apps/gui/styles/styles.qrc)
        set_target_properties(chunkharbor-backup-layout-test PROPERTIES AUTORCC ON)
        target_link_libraries(chunkharbor-backup-layout-test PRIVATE chunkharbor_gui_pages)
        add_test(NAME backup_layout_test COMMAND chunkharbor-backup-layout-test)
        set_tests_properties(backup_layout_test PROPERTIES TIMEOUT 25 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        target_link_libraries(chunkharbor-gui-worker-test PRIVATE chunkharbor_gui_transfer)
        add_test(NAME gui_worker_test COMMAND chunkharbor-gui-worker-test)
        set_tests_properties(gui_worker_test PROPERTIES TIMEOUT 60)
        add_executable(chunkharbor-gui-pages-test tests/unit/gui_pages_test.cpp)
        target_include_directories(chunkharbor-gui-pages-test PRIVATE apps/gui)
        target_link_libraries(chunkharbor-gui-pages-test PRIVATE chunkharbor_gui_pages)
        if(MSVC)
            target_compile_options(chunkharbor-gui-pages-test PRIVATE /utf-8)
            target_compile_options(chunkharbor-gui-state-test PRIVATE /utf-8)
            target_compile_options(chunkharbor-gui-worker-test PRIVATE /utf-8)
            target_compile_options(chunkharbor-backup-layout-test PRIVATE /utf-8)
            target_compile_options(chunkharbor-gui-transfer-probe PRIVATE /utf-8)
            target_compile_options(chunkharbor-gui-files-probe PRIVATE /utf-8)
        endif()
        add_test(NAME gui_pages_test COMMAND chunkharbor-gui-pages-test)
        set_tests_properties(gui_pages_test PROPERTIES TIMEOUT 20 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        add_test(NAME gui_probe_test COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/gui_probe_test.py
            $<TARGET_FILE:chunkharbor-gui>)
        set_tests_properties(gui_probe_test PROPERTIES TIMEOUT 40 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()
    add_executable(chunkharbor-upload-control-test tests/unit/upload_control_test.cpp)
    target_link_libraries(chunkharbor-upload-control-test PRIVATE chunkharbor_protocol)
    add_test(NAME upload_control_test COMMAND chunkharbor-upload-control-test)
    add_executable(chunkharbor-chunk-ack-test tests/unit/chunk_ack_test.cpp)
    target_link_libraries(chunkharbor-chunk-ack-test PRIVATE chunkharbor_protocol)
    add_test(NAME chunk_ack_test COMMAND chunkharbor-chunk-ack-test)
    set_tests_properties(chunk_ack_test PROPERTIES TIMEOUT 15)
    add_executable(chunkharbor-upload-plan-test tests/unit/upload_plan_test.cpp)
    target_link_libraries(chunkharbor-upload-plan-test PRIVATE chunkharbor_protocol)
    add_test(NAME upload_plan_test COMMAND chunkharbor-upload-plan-test)
    set_tests_properties(upload_plan_test PROPERTIES TIMEOUT 15)
    add_executable(chunkharbor-session-status-test tests/unit/session_status_test.cpp)
    target_link_libraries(chunkharbor-session-status-test PRIVATE chunkharbor_protocol)
    add_test(NAME session_status_test COMMAND chunkharbor-session-status-test)
    add_executable(chunkharbor-begin-upload-test tests/unit/begin_upload_test.cpp)
    target_link_libraries(chunkharbor-begin-upload-test PRIVATE chunkharbor_protocol)
    add_test(NAME begin_upload_test COMMAND chunkharbor-begin-upload-test)
    set_tests_properties(begin_upload_test PROPERTIES TIMEOUT 15)
    add_executable(chunkharbor-data-chunk-test tests/unit/data_chunk_test.cpp)
    target_link_libraries(chunkharbor-data-chunk-test PRIVATE chunkharbor_protocol)
    add_test(NAME data_chunk_test COMMAND chunkharbor-data-chunk-test)
    set_tests_properties(data_chunk_test PROPERTIES TIMEOUT 15)
    if(CHUNKHARBOR_BUILD_HASH)
        add_executable(chunkharbor-sha256-test tests/unit/sha256_test.cpp)
        target_link_libraries(chunkharbor-sha256-test PRIVATE chunkharbor_hash)
        add_test(NAME sha256_test COMMAND chunkharbor-sha256-test)
        set_tests_properties(sha256_test PROPERTIES TIMEOUT 30)
    endif()
    add_executable(chunkharbor-framecodec-test tests/unit/framecodec_test.cpp)
    target_link_libraries(chunkharbor-framecodec-test PRIVATE chunkharbor_protocol chunkharbor_client_core)
    add_test(NAME framecodec_test COMMAND chunkharbor-framecodec-test)
    add_executable(chunkharbor-handshake-test tests/unit/handshake_test.cpp)
    target_link_libraries(chunkharbor-handshake-test PRIVATE chunkharbor_protocol chunkharbor_client_core)
    add_test(NAME handshake_test COMMAND chunkharbor-handshake-test)
    if(linux_default)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        add_test(NAME schema_v1_test COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/schema_v1_test.py
            ${CMAKE_CURRENT_SOURCE_DIR}/src/storage/schema_v1.sql)
        add_test(NAME schema_v2_test COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/schema_v2_test.py
            ${CMAKE_CURRENT_SOURCE_DIR}/src/storage/schema_v1.sql
            ${CMAKE_CURRENT_SOURCE_DIR}/src/storage/schema_v2.sql)
        add_executable(chunkharbor-sqlite-store-test tests/unit/sqlite_store_test.cpp)
        target_link_libraries(chunkharbor-sqlite-store-test PRIVATE chunkharbor_server_core SQLite::SQLite3)
        add_test(NAME sqlite_store_test COMMAND chunkharbor-sqlite-store-test
            ${CMAKE_CURRENT_SOURCE_DIR}/src/storage/schema_v1.sql)
        if(CHUNKHARBOR_BUILD_HASH)
            add_executable(chunkharbor-delta-storage-test tests/unit/delta_storage_test.cpp)
            target_link_libraries(chunkharbor-delta-storage-test PRIVATE chunkharbor_server_core)
            target_compile_options(chunkharbor-delta-storage-test PRIVATE -UNDEBUG)
            target_link_options(chunkharbor-delta-storage-test PRIVATE -Wl,--wrap=renameat)
            add_test(NAME delta_storage_test COMMAND chunkharbor-delta-storage-test)
            set_tests_properties(delta_storage_test PROPERTIES TIMEOUT 90)
            add_executable(chunkharbor-upload-storage-test tests/unit/upload_storage_test.cpp)
            target_link_libraries(chunkharbor-upload-storage-test PRIVATE chunkharbor_server_core chunkharbor_hash)
            target_link_options(chunkharbor-upload-storage-test PRIVATE -Wl,--wrap=pwrite -Wl,--wrap=fdatasync -Wl,--wrap=write -Wl,--wrap=fsync)
            add_test(NAME upload_storage_test COMMAND chunkharbor-upload-storage-test)
            set_tests_properties(upload_storage_test PROPERTIES TIMEOUT 30)
            add_executable(chunkharbor-upload-handler-test tests/unit/upload_handler_test.cpp apps/server/UploadHandler.cpp)
            target_include_directories(chunkharbor-upload-handler-test PRIVATE apps/server)
            target_link_libraries(chunkharbor-upload-handler-test PRIVATE chunkharbor_server_core chunkharbor_linux_net)
            add_test(NAME upload_handler_test COMMAND chunkharbor-upload-handler-test)
            set_tests_properties(upload_handler_test PROPERTIES TIMEOUT 15)
        endif()
        add_executable(chunkharbor-temp-file-store-test tests/unit/temp_file_store_test.cpp)
        target_link_libraries(chunkharbor-temp-file-store-test PRIVATE chunkharbor_server_core Threads::Threads)
        target_link_options(chunkharbor-temp-file-store-test PRIVATE -Wl,--wrap=syscall -Wl,--wrap=fchmod -Wl,--wrap=fstat)
        add_test(NAME temp_file_store_test COMMAND chunkharbor-temp-file-store-test)
        set_tests_properties(temp_file_store_test PROPERTIES TIMEOUT 20)
        foreach(test common eventloop buffer channel_lifetime tcp_connection wakeup server_stop disk_executor application_pause)
            add_executable(chunkharbor-${test}-test tests/unit/${test}_test.cpp)
            target_link_libraries(chunkharbor-${test}-test PRIVATE chunkharbor_linux_net)
            add_test(NAME ${test}_test COMMAND chunkharbor-${test}-test)
            set_tests_properties(${test}_test PROPERTIES TIMEOUT 15)
        endforeach()
        target_link_options(chunkharbor-tcp_connection-test PRIVATE -Wl,--wrap=send -Wl,--wrap=recv)
        target_link_options(chunkharbor-wakeup-test PRIVATE -Wl,--wrap=write -Wl,--wrap=read -Wl,--wrap=epoll_wait)
    endif()
    if(CHUNKHARBOR_BUILD_SERVER)
        add_test(NAME server_help COMMAND chunkharbor-server --help)
    endif()
    if(CHUNKHARBOR_BUILD_CLI)
        add_test(NAME cli_help COMMAND chunkharbor-cli --help)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        add_test(NAME delta_cli_peer_test COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/delta_cli_peer_test.py $<TARGET_FILE:chunkharbor-cli>)
        set_tests_properties(delta_cli_peer_test PROPERTIES TIMEOUT 60)
        add_test(NAME delta_cli_recovery_peer_test COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/delta_cli_recovery_peer_test.py $<TARGET_FILE:chunkharbor-cli>)
        set_tests_properties(delta_cli_recovery_peer_test PROPERTIES TIMEOUT 60)
    endif()
    if(CHUNKHARBOR_BUILD_SERVER AND CHUNKHARBOR_BUILD_CLI)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        add_test(NAME files_list_integration COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/files_list_test.py
            $<TARGET_FILE:chunkharbor-server> $<TARGET_FILE:chunkharbor-cli>)
        set_tests_properties(files_list_integration PROPERTIES TIMEOUT 90)
        add_test(NAME download_integration COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/download_test.py
            $<TARGET_FILE:chunkharbor-server> $<TARGET_FILE:chunkharbor-cli>)
        set_tests_properties(download_integration PROPERTIES TIMEOUT 90)
        add_test(NAME download_peer_test COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/download_peer_test.py
            $<TARGET_FILE:chunkharbor-cli>)
        set_tests_properties(download_peer_test PROPERTIES TIMEOUT 90)
        add_test(NAME network_integration COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/network_test.py
            $<TARGET_FILE:chunkharbor-server> $<TARGET_FILE:chunkharbor-cli>)
        set_tests_properties(network_integration PROPERTIES TIMEOUT 90)
        add_test(NAME handshake_integration COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/handshake_test.py
            $<TARGET_FILE:chunkharbor-server> $<TARGET_FILE:chunkharbor-cli>)
        set_tests_properties(handshake_integration PROPERTIES TIMEOUT 45)
        add_test(NAME upload_integration COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/upload_test.py
            $<TARGET_FILE:chunkharbor-server> $<TARGET_FILE:chunkharbor-cli>)
        set_tests_properties(upload_integration PROPERTIES TIMEOUT 120)
        add_test(NAME resume_integration COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/resume_test.py
            $<TARGET_FILE:chunkharbor-server>)
        set_tests_properties(resume_integration PROPERTIES TIMEOUT 60)
        add_test(NAME delta_protocol_integration COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/delta_protocol_test.py
            $<TARGET_FILE:chunkharbor-server>)
        set_tests_properties(delta_protocol_integration PROPERTIES TIMEOUT 180)
        add_test(NAME delta_cli_integration COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/delta_cli_test.py
            $<TARGET_FILE:chunkharbor-server> $<TARGET_FILE:chunkharbor-cli>)
        set_tests_properties(delta_cli_integration PROPERTIES TIMEOUT 300)
        if(NOT CHUNKHARBOR_SANITIZERS)
            add_library(chunkharbor-download-fault-hook SHARED tests/helpers/download_fault_hook.cpp)
            target_link_libraries(chunkharbor-download-fault-hook PRIVATE ${CMAKE_DL_LIBS})
            add_test(NAME download_fault_test COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/download_peer_test.py
                $<TARGET_FILE:chunkharbor-cli> $<TARGET_FILE:chunkharbor-download-fault-hook>)
            set_tests_properties(download_fault_test PROPERTIES TIMEOUT 60)
            add_library(chunkharbor-delta-crash-hook SHARED tests/helpers/delta_crash_hook.cpp)
            target_link_libraries(chunkharbor-delta-crash-hook PRIVATE ${CMAKE_DL_LIBS})
            add_test(NAME delta_recovery_integration COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/delta_recovery_test.py
                $<TARGET_FILE:chunkharbor-server> $<TARGET_FILE:chunkharbor-delta-crash-hook>)
            set_tests_properties(delta_recovery_integration PROPERTIES TIMEOUT 120)
            add_test(NAME delta_fault_integration COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/delta_fault_test.py
                $<TARGET_FILE:chunkharbor-server> $<TARGET_FILE:chunkharbor-delta-crash-hook>)
            set_tests_properties(delta_fault_integration PROPERTIES TIMEOUT 120)
        endif()
        add_test(NAME cli_upload_peer_test COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/cli_upload_peer_test.py
            $<TARGET_FILE:chunkharbor-cli>)
        set_tests_properties(cli_upload_peer_test PROPERTIES TIMEOUT 30)
    endif()
endif()
