cmake_minimum_required(VERSION 3.20.0)

target_include_directories(app PRIVATE include)
target_sources_ifdef(CONFIG_MACROZ_DYNAMIC app PRIVATE src/macroz_dynamic.c)
target_sources_ifdef(CONFIG_MACROZ_USB app PRIVATE src/macroz_usb.c)
