# dashboard.html is NOT copied in here. It is compressed and embedded straight
# from firmware/hub/data/, which is where it already lived and where it is edited.
# Two copies of a 1400-line page in a repo with this much documentation drift
# behind it would diverge, and the divergence would show up as a wall display
# disagreeing with a phone.
idf_component_register(SRCS "app_main.c" "app_rs485.c" "app_sensors.c" "app_cal.c" "app_web.c"
                       INCLUDE_DIRS ".")

# ---------------------------------------------------------------------------
# Gzip the dashboard at build time and embed the compressed bytes.
#
# The page is ~65 KB of HTML, CSS and JS, and it is held TWICE in flash because
# of the two OTA slots. gzip takes it to roughly a fifth of that for one response
# header, which is the cheapest flash in the project. Compressing at build time
# rather than per request also means the CPU never does it while the poll loop is
# trying to run.
#
# mtime=0 keeps the output byte-identical across builds. Without it the gzip
# header carries a timestamp, every rebuild produces a different binary, and an
# OTA diff looks like a change when nothing changed.
# ---------------------------------------------------------------------------
set(DASH_SRC "${COMPONENT_DIR}/../../hub/data/dashboard.html")
set(DASH_GZ  "${CMAKE_CURRENT_BINARY_DIR}/dashboard.html.gz")

find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_custom_command(
    OUTPUT  "${DASH_GZ}"
    COMMAND "${Python3_EXECUTABLE}" -c
            "import gzip,sys;open(sys.argv[2],'wb').write(gzip.compress(open(sys.argv[1],'rb').read(),compresslevel=9,mtime=0))"
            "${DASH_SRC}" "${DASH_GZ}"
    DEPENDS "${DASH_SRC}"
    COMMENT "Compressing dashboard.html for embedding"
    VERBATIM)

add_custom_target(dashboard_gz DEPENDS "${DASH_GZ}")
add_dependencies(${COMPONENT_LIB} dashboard_gz)

# BINARY, not TEXT: no trailing NUL is appended, so the served length is exactly
# end - start. app_web.c relies on that.
target_add_binary_data(${COMPONENT_LIB} "${DASH_GZ}" BINARY)
