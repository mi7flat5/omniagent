cmake_minimum_required(VERSION 3.21)
project(omniagent_engine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
    add_compile_options(/W4 /permissive-)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(nlohmann_json CONFIG REQUIRED)
find_package(httplib CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)

include(CTest)

add_library(omni-engine STATIC
    src/commands/command_registry.cpp
    src/core/message.cpp
    src/core/stream_assembler.cpp
    src/core/microcompact.cpp
    src/core/auto_compact.cpp
    src/core/query_engine.cpp
    src/core/slash_commands.cpp
    src/core/query.cpp
    src/providers/http_provider.cpp
    src/permissions/permission_checker.cpp
    src/services/cost_tracker.cpp
    src/services/hooks.cpp
    src/services/memory_loader.cpp
    src/services/run_persistence.cpp
    src/services/session_persistence.cpp
    src/tools/tool_registry.cpp
    src/tools/tool_executor.cpp
    src/tools/workspace_tools.cpp
    src/tools/web_tools.cpp
    src/tools/planner_tools.cpp
    src/agents/agent_manager.cpp
    src/agents/agent_tool.cpp
    src/mcp/mcp_client.cpp
    src/mcp/mcp_tool_wrapper.cpp
    src/plugins/plugin_manager.cpp
    src/engine_impl.cpp
    src/host_impl.cpp
    src/project_session_impl.cpp
    src/project_run_impl.cpp
    src/session_impl.cpp
)

add_executable(omni-engine-cli
    src/cli/main.cpp
    src/cli/repl.cpp
)

target_include_directories(omni-engine
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(omni-engine PUBLIC cxx_std_20)

target_include_directories(omni-engine-cli
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(omni-engine-cli PRIVATE omni-engine)

target_link_libraries(omni-engine
    PUBLIC
        nlohmann_json::nlohmann_json
    PRIVATE
        httplib::httplib
        OpenSSL::SSL
        OpenSSL::Crypto
)

# dlopen requires -ldl on Linux/glibc systems.
if(UNIX AND NOT APPLE)
    target_link_libraries(omni-engine PRIVATE dl)
endif()

if(BUILD_TESTING)
    find_package(GTest CONFIG REQUIRED)
    add_executable(omni-engine-tests
        tests/test_types.cpp
        tests/test_workspace_tools.cpp
        tests/test_stream_assembler.cpp
        tests/test_microcompact.cpp
        tests/test_auto_compact.cpp
        tests/test_tool_registry.cpp
        tests/test_tool_executor.cpp
        tests/test_query_engine.cpp
        tests/test_http_provider.cpp
        tests/test_session_submit.cpp
        tests/test_web_tools.cpp
        tests/test_planner_tools.cpp
        tests/test_cost_tracker.cpp
        tests/test_permission_checker.cpp
        tests/test_project_host.cpp
        tests/test_runtime_hardening.cpp
        tests/test_session_persistence.cpp
        tests/test_hooks.cpp
        tests/test_memory_loader.cpp
        tests/test_command_registry.cpp
        tests/test_cli_repl.cpp
        tests/test_agent_manager.cpp
        tests/test_plugin_manager.cpp
        tests/test_mcp_client.cpp
    )

    target_link_libraries(omni-engine-tests PRIVATE
        omni-engine
        GTest::gtest_main
    )

    target_include_directories(omni-engine-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    include(GoogleTest)
    gtest_discover_tests(omni-engine-tests)
endif()
