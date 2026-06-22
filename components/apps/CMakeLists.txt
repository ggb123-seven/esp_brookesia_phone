set(APPS_DIR ./)
# ESP-IDF resolves component requirements before CONFIG-gated source filtering is reliable.
set(APPS_REQUIRES lvgl__lvgl esp_lvgl_adapter esp_event esp_wifi nvs_flash esp_driver_jpeg esp_mm esp-brookesia bsp_extra esp32_p4_function_ev_board_noglib esp_video pedestrian_detect human_face_detect espressif__esp_lcd_touch_gt911 as608 student_store dht11 mq2)
file(GLOB_RECURSE APPS_C_SRCS ${APPS_DIR}/*.c)
file(GLOB_RECURSE APPS_CPP_SRCS ${APPS_DIR}/*.cpp)

function(exclude_app_sources source_list app_dir)
    set(filtered_sources "")
    foreach(source ${${source_list}})
        file(TO_CMAKE_PATH "${source}" source_path)
        if(NOT source_path MATCHES "/${app_dir}/")
            list(APPEND filtered_sources "${source}")
        endif()
    endforeach()
    set(${source_list} "${filtered_sources}" PARENT_SCOPE)
endfunction()

if(NOT CONFIG_EXAMPLE_ENABLE_APP_CALCULATOR)
    exclude_app_sources(APPS_C_SRCS "calculator")
    exclude_app_sources(APPS_CPP_SRCS "calculator")
endif()

if(NOT CONFIG_EXAMPLE_ENABLE_APP_GAME_2048)
    exclude_app_sources(APPS_C_SRCS "game_2048")
    exclude_app_sources(APPS_CPP_SRCS "game_2048")
endif()

if(NOT CONFIG_EXAMPLE_ENABLE_APP_FINGERPRINT)
    exclude_app_sources(APPS_C_SRCS "fingerprint")
    exclude_app_sources(APPS_CPP_SRCS "fingerprint")
endif()

if(NOT CONFIG_EXAMPLE_ENABLE_APP_ENVIRONMENT_MONITOR)
    exclude_app_sources(APPS_C_SRCS "environment_monitor")
    exclude_app_sources(APPS_CPP_SRCS "environment_monitor")
endif()

idf_component_register(
    SRCS ${APPS_C_SRCS} ${APPS_CPP_SRCS}
    INCLUDE_DIRS ${APPS_DIR}
    REQUIRES ${APPS_REQUIRES})

target_compile_options(
    ${COMPONENT_LIB}
    PRIVATE
    $<$<COMPILE_LANGUAGE:C>:
        -Wno-format
        -Wno-incompatible-pointer-types
        -Wno-int-conversion
    >
    $<$<COMPILE_LANGUAGE:CXX>:
        -Wno-format
        -Wno-conversion-null
        -Wno-attributes
        -Wno-write-strings
    >
    -DLV_LVGL_H_INCLUDE_SIMPLE
)
