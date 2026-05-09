###############################################################################
# example_plugin — rAthena example plugin
###############################################################################

# Plugin output goes next to the source (plugins/example/) so that
# conf/plugins.conf can reference it as "plugins/example/example_plugin"
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

add_library(example_plugin SHARED
    example_plugin.cpp
)

# No PREFIX on Windows (.dll), keep default on Linux (.so)
set_target_properties(example_plugin PROPERTIES PREFIX "")

target_include_directories(example_plugin PRIVATE
    "${RATHENA_SRC}"
)

target_compile_features(example_plugin PRIVATE cxx_std_17)

# Plugins must not link against map-server — all server calls go through
# the plugin_api_t function pointer struct passed to plugin_init().
# The only permitted link target is the C++ runtime.
if(WIN32)
    # Suppress the LNK4217 warning about importing symbols that are also exported
    if(MSVC)
        target_link_options(example_plugin PRIVATE /ignore:4217)
    endif()
endif()
