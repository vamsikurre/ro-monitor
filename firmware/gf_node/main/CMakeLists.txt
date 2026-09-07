# No PRIV_REQUIRES here. Declaring any component list on main cancels the
# special case that otherwise gives main every component in the build, and
# this file needs more than the obvious Wi-Fi/HTTP/OTA set (esp_event for
# net.c's event loop, esp_app_format for the version banner) - components
# that are not in IDF's always-added common set either. Match hub_prod/main:
# SRCS and INCLUDE_DIRS only, and let main have the whole component graph.
idf_component_register(SRCS "main.c" "net.c" "web.c" "ota.c" "sensors_sump.c" "sensors_util.c"
                       INCLUDE_DIRS ".")
