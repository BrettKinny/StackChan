# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "1.3.5")
add_definitions(-DFIRMWARE_VERSION=\"${PROJECT_VER}\")

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(stack-chan)

# The server address is compiled in, so a build still carrying the
# sdkconfig.defaults placeholder would ship firmware that reaches no server.
if(CONFIG_OTA_URL MATCHES "<XIAOZHI_HOST>")
    message(FATAL_ERROR
        "CONFIG_OTA_URL is still the placeholder (${CONFIG_OTA_URL}). Set it to "
        "http://<your xiaozhi-server host>:8003/xiaozhi/ota/ in sdkconfig.defaults "
        "(before the first build) or with idf.py menuconfig -> Xiaozhi Assistant -> "
        "Default OTA URL.")
endif()

# IDF resolves managed dependencies during project(); patch only a build copy.
include(cmake/patch_esp_ml307.cmake)
