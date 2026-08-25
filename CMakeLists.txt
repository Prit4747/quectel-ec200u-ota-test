cmake_minimum_required(VERSION 3.16)

# Bump for each release when generating delta OTA patches (tools/make_patch.py
# diffs against the .bin matching the PROJECT_VER currently on the device).
set(PROJECT_VER "v1.0.3-quectel-ec200u-uart-priority-fix")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(quectel_ec200u_ota)
