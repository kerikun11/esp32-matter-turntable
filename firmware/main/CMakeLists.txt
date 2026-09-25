# CMakeLists.txt for the esp-idf main component
idf_component_register(SRC_DIRS "." INCLUDE_DIRS "."
  PRIV_REQUIRES
    driver
    esp_app_format
    esp_http_server
    esp_netif
    esp_system
    esp_timer
    esp_wifi
    app_update
    freertos
    led_strip
    log
    mdns
    nvs_flash
    json
)

target_compile_options(${COMPONENT_LIB} PRIVATE
  -fmacro-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}/=
  -ffile-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}/=
)

# Generate compressed assets only when their source or the builder changes.
set(WEB_ASSET_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/web_assets.h")
add_custom_command(OUTPUT "${WEB_ASSET_HEADER}"
  COMMAND ${PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/../tools/web/build_web.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html" "${WEB_ASSET_HEADER}"
  DEPENDS "web/index.html" "../tools/web/build_web.py" "../tools/web/requirements.txt"
  VERBATIM)
add_custom_target(turntable_web_assets DEPENDS "${WEB_ASSET_HEADER}")
add_dependencies(${COMPONENT_LIB} turntable_web_assets)
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
