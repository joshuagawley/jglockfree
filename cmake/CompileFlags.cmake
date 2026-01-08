function(
        set_project_compile_flags
        common_target_name
        WARNING_AS_ERRORS
)
    set(COMPILE_FLAGS
            -Wall
            -Wextra # reasonable and standard
            -Wshadow # warn the user if a variable declaration shadows one from a parent context
            -Wnon-virtual-dtor # warn the user if a class with virtual functions has a non-virtual destructor. This helps
            # catch hard to track down memory errors
            -Wold-style-cast # warn for c-style casts
            -Wcast-align # warn for potential performance problem casts
            -Wunused # warn on anything being unused
            -Woverloaded-virtual # warn if you overload (not override) a virtual function
            -Wpedantic # warn if non-standard C++ is used
            -Wconversion # warn on type conversions that may lose data
            -Wsign-conversion # warn on sign conversions
            -Wnull-dereference # warn if a null dereference is detected
            -Wdouble-promotion # warn if float is implicit promoted to double
            -Wformat=2 # warn on security issues around functions that format output (ie printf)
            -Wimplicit-fallthrough # warn on statements that fallthrough without an explicit annotation
    )

    if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        list(APPEND
                COMPILE_FLAGS
                -Wmisleading-indentation # warn if indentation implies blocks where blocks do not exist
                -Wduplicated-cond # warn if if / else chain has duplicated conditions
                -Wduplicated-branches # warn if if / else branches have duplicated code
                -Wlogical-op # warn about logical operations being used where bitwise were probably wanted
                -Wuseless-cast # warn if you perform a cast to the same type
                -Wsuggest-override # warn if an overridden member function is not marked 'override' or 'final'
        )
    endif ()

    if (WARNINGS_AS_ERRORS)
        message(TRACE "Warnings are treated as errors")
        list(APPEND COMPILE_FLAGS -Werror)
    endif ()

    if (ENABLE_TSAN)
        message(TRACE "ThreadSanitizer enabled")
        list(APPEND COMPILE_FLAGS -fsanitize=thread)
        list(APPEND LINK_FLAGS -fsanitize=thread)
    endif ()

    if (CMAKE_CXX_COMPILER_ID MATCHES ".*Clang")
        set(PROJECT_WARNINGS_CXX ${CLANG_WARNINGS})
    elseif (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        set(PROJECT_WARNINGS_CXX ${GCC_WARNINGS})
    endif ()

    # use the same warning flags for C
    set(PROJECT_WARNINGS_C "${PROJECT_WARNINGS_CXX}")

    target_compile_options(
            ${common_target_name}
            INTERFACE
            $<$<COMPILE_LANGUAGE:CXX>:${PROJECT_WARNINGS_CXX}>
            $<$<COMPILE_LANGUAGE:C>:${PROJECT_WARNINGS_C}>
            $<$<COMPILE_LANGUAGE:CUDA>:${PROJECT_WARNINGS_CUDA}>)
    target_link_options(${common_target_name} INTERFACE ${LINK_FLAGS})

endfunction(set_project_compile_flags)