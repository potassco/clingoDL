# [[[source: .
set(ide_source_group "Source Files")
set(source-group
    "${CMAKE_CURRENT_SOURCE_DIR}/catch.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/main.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/solve.cc")
source_group("${ide_source_group}" FILES ${source-group})
set(source
    ${source-group})
# ]]]

add_executable(test_clingo-dl ${source})
target_link_libraries(test_clingo-dl PRIVATE libclingo libclingo-dl)
target_include_directories(test_clingo-dl PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)
set_target_properties(test_clingo-dl PROPERTIES FOLDER test)
if(MSVC)
    target_compile_definitions(test_clingo-dl PRIVATE _SCL_SECURE_NO_WARNINGS)
endif()

add_test(NAME test_clingo-dl COMMAND test_clingo-dl)
