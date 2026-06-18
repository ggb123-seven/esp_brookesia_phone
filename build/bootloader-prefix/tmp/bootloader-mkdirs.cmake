# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "F:/ESP32/ESP-IDF-v5..5.4/v5.5.4/esp-idf/components/bootloader/subproject")
  file(MAKE_DIRECTORY "F:/ESP32/ESP-IDF-v5..5.4/v5.5.4/esp-idf/components/bootloader/subproject")
endif()
file(MAKE_DIRECTORY
  "F:/ESP32/esp32p4-demo/esp-dev-kits-a88faa6/examples/esp32-p4-function-ev-board/examples/esp_brookesia_phone/build/bootloader"
  "F:/ESP32/esp32p4-demo/esp-dev-kits-a88faa6/examples/esp32-p4-function-ev-board/examples/esp_brookesia_phone/build/bootloader-prefix"
  "F:/ESP32/esp32p4-demo/esp-dev-kits-a88faa6/examples/esp32-p4-function-ev-board/examples/esp_brookesia_phone/build/bootloader-prefix/tmp"
  "F:/ESP32/esp32p4-demo/esp-dev-kits-a88faa6/examples/esp32-p4-function-ev-board/examples/esp_brookesia_phone/build/bootloader-prefix/src/bootloader-stamp"
  "F:/ESP32/esp32p4-demo/esp-dev-kits-a88faa6/examples/esp32-p4-function-ev-board/examples/esp_brookesia_phone/build/bootloader-prefix/src"
  "F:/ESP32/esp32p4-demo/esp-dev-kits-a88faa6/examples/esp32-p4-function-ev-board/examples/esp_brookesia_phone/build/bootloader-prefix/src/bootloader-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "F:/ESP32/esp32p4-demo/esp-dev-kits-a88faa6/examples/esp32-p4-function-ev-board/examples/esp_brookesia_phone/build/bootloader-prefix/src/bootloader-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "F:/ESP32/esp32p4-demo/esp-dev-kits-a88faa6/examples/esp32-p4-function-ev-board/examples/esp_brookesia_phone/build/bootloader-prefix/src/bootloader-stamp${cfgdir}") # cfgdir has leading slash
endif()
