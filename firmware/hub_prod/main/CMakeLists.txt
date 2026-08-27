# dashboard.html is NOT copied in here. It is embedded straight from
# firmware/hub/data/, which is where it already lived and where it is edited.
# Two copies of a 1400-line page in a repo with this much documentation drift
# behind it would diverge, and the divergence would be invisible until someone
# noticed the wall display disagreeing with the phone.
idf_component_register(SRCS "app_main.c" "app_rs485.c" "app_sensors.c" "app_cal.c" "app_web.c"
                       INCLUDE_DIRS "."
                       EMBED_FILES "../../hub/data/dashboard.html")
