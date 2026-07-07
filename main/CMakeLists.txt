set(MAIN_SRCS main.cpp lvgl_adapter_init.c)
set(MAIN_REQUIRES
    apps
    bsp_extra
    dht11
    esp-brookesia
    esp32_p4_function_ev_board_noglib
    esp_hw_support
    esp_lvgl_adapter
    heap
    lvgl__lvgl
    mq2
    nvs_flash
    parent_call_alert)

if(CONFIG_EXAMPLE_ENABLE_AS608_VALIDATION)
    list(APPEND MAIN_SRCS as608_validation.c)
    list(APPEND MAIN_REQUIRES as608)
endif()

idf_component_register(
    SRCS ${MAIN_SRCS}
    INCLUDE_DIRS .
    REQUIRES ${MAIN_REQUIRES})

idf_build_get_property(build_components BUILD_COMPONENTS)
foreach(COMPONENT ${build_components})
    if(COMPONENT MATCHES "esp-brookesia" OR COMPONENT MATCHES "espressif__esp-brookesia")
        set(ESP_BROOKESIA_COMPONENT ${COMPONENT})
        break()
    endif()
endforeach()

if(ESP_BROOKESIA_COMPONENT)
    idf_component_get_property(ESP_BROOKESIA_LIB ${ESP_BROOKESIA_COMPONENT} COMPONENT_LIB)
    get_target_property(ESP_BROOKESIA_SRCS ${ESP_BROOKESIA_LIB} SOURCES)
    if(ESP_BROOKESIA_SRCS)
        # Keep the Brookesia core SquareLine helpers, but drop its unused demo app.
        set(ESP_BROOKESIA_FILTERED_SRCS "")
        foreach(SOURCE ${ESP_BROOKESIA_SRCS})
            file(TO_CMAKE_PATH "${SOURCE}" SOURCE_PATH)
            if(NOT SOURCE_PATH MATCHES "/src/app_examples/phone/squareline/")
                list(APPEND ESP_BROOKESIA_FILTERED_SRCS "${SOURCE}")
            endif()
        endforeach()
        set_property(TARGET ${ESP_BROOKESIA_LIB} PROPERTY SOURCES ${ESP_BROOKESIA_FILTERED_SRCS})
    endif()
endif()

idf_component_get_property(LVGL_LIB lvgl__lvgl COMPONENT_LIB)
target_compile_options(
    ${LVGL_LIB} 
    PRIVATE 
        -DLV_LVGL_H_INCLUDE_SIMPLE
        -DLV_USE_DEMO_MUSIC
)

if(CONFIG_ESP_BROOKESIA_MEMORY_USE_CUSTOM)
    if(NOT ESP_BROOKESIA_COMPONENT)
        message(FATAL_ERROR "Component 'esp-brookesia' not found.")
    endif()

    target_compile_options(
        ${ESP_BROOKESIA_LIB}
        PUBLIC
            "-DESP_BROOKESIA_MEMORY_CUSTOM_MALLOC(x)=heap_caps_aligned_alloc(1, x, MALLOC_CAP_SPIRAM)"  "-DESP_BROOKESIA_MEMORY_CUSTOM_FREE=free"
    )
endif()

if(CONFIG_LV_MEM_CUSTOM)
    target_compile_options(
        ${LVGL_LIB} 
        PRIVATE 
            "-DLV_MEM_CUSTOM_ALLOC(x)=heap_caps_malloc(x, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)"
    )
endif()

spiffs_create_partition_image(storage ../spiffs FLASH_IN_PROJECT)
