# Create a header containing a hash that describes this library.  For a
# released version of Qt, we'll use the .tag file that is updated by git
# archive with the tree hash. For unreleased versions, we'll ask git
# rev-parse. If none of this works, we use CMake to hash all the files
# in the src/qml/ directory.
function(qt_declarative_write_tag_header target_name)
    set(tag_file "${CMAKE_CURRENT_SOURCE_DIR}/../../.tag")
    set(tag_contents "")
    if(EXISTS "${tag_file}")
        file(READ "${tag_file}" tag_contents)
        string(STRIP "${tag_contents}" tag_contents)
    endif()
    find_program(git_path git)

    if(tag_contents AND NOT tag_contents STREQUAL "$Format:%T$")
        set(QML_COMPILE_HASH "${tag_contents}")
    elseif(git_path AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../.git")
        execute_process(
            COMMAND ${git_path} rev-parse HEAD
            OUTPUT_VARIABLE QML_COMPILE_HASH
            OUTPUT_STRIP_TRAILING_WHITESPACE
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    endif()
    string(LENGTH "${QML_COMPILE_HASH}" QML_COMPILE_HASH_LENGTH)
    if(QML_COMPILE_HASH_LENGTH EQUAL 0)
        set(sources_hash "")
        file(GLOB_RECURSE qtqml_source_files "${CMAKE_CURRENT_SOURCE_DIR}/*")
        foreach(file IN LISTS qtqml_source_files)
            file(SHA1 ${file} file_hash)
            string(APPEND sources_hash ${file_hash})
        endforeach()
        string(SHA1 QML_COMPILE_HASH "${sources_hash}")
    endif()

    string(LENGTH "${QML_COMPILE_HASH}" QML_COMPILE_HASH_LENGTH)
    if(QML_COMPILE_HASH_LENGTH GREATER 0)
        configure_file("qml_compile_hash_p.h.in" "${CMAKE_CURRENT_BINARY_DIR}/qml_compile_hash_p.h")
    else()
        message(FATAL_ERROR "QML compile hash is empty! "
                            "You need either a valid git repository or a non-empty .tag file.")
    endif()
endfunction()

# Generate a header file containing a regular expression jit table.
function(qt_declarative_generate_reg_exp_jit_tables consuming_target)
    set(generate_dir "${CMAKE_CURRENT_BINARY_DIR}/.generated")
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        string(APPEND generate_dir "/debug")
    elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
        string(APPEND generate_dir "/release")
    endif()

    set(output_file "${generate_dir}/RegExpJitTables.h")
    set(retgen_script_file "${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/masm/yarr/create_regex_tables")

    add_custom_command(
        OUTPUT "${output_file}"
        COMMAND "${QT_INTERNAL_DECLARATIVE_PYTHON}" ${retgen_script_file} ${output_file}
        MAIN_DEPENDENCY ${retgen_script_file}
    )
    target_sources(${consuming_target} PRIVATE ${output_file})
    target_include_directories(${consuming_target} PRIVATE $<BUILD_INTERFACE:${generate_dir}>)
endfunction()
