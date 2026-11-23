set(COMMON_DIR "${CMAKE_CURRENT_LIST_DIR}/../../common")

idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "." "${COMMON_DIR}"
)
