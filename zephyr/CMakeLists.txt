cmake_minimum_required(VERSION 3.13.1)
include($ENV{ZEPHYR_BASE}/cmake/app/boilerplate.cmake NO_POLICY_SCOPE)
project(controle-de-led-uart)

FILE(GLOB app_sources ../src/*.c*)
target_sources(app PRIVATE ${app_sources})

# Bônus: descomente para usar CRC-8 (poly 0x07) no lugar do XOR
# target_compile_definitions(app PRIVATE USE_CRC8=1)