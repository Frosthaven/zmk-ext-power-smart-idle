cmake_minimum_required(VERSION 3.13.1)

target_sources_ifdef(CONFIG_ZMK_EXT_POWER_SMART_IDLE app PRIVATE src/ext_power_smart_idle.c)
