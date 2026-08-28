cmake_minimum_required(VERSION 3.16)

# Bump for each release when generating delta OTA patches (tools/make_patch.py
# diffs against the .bin matching the PROJECT_VER currently on the device).
set(PROJECT_VER "v4.0.0-base")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(quectel_ec200u_ota)
