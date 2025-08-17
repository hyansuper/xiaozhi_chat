#pragma once
#include <stdlib.h>

#ifdef CONFIG_XZ_BOARD_TYPE_CUSTOM
#define XZ_BOARD_TYPE CONFIG_XZ_BOARD_TYPE_CUSTOM_NAME
#else
#define XZ_BOARD_TYPE APP_NAME
#endif
#define XZ_BOARD_NAME XZ_BOARD_TYPE

// FAKE
#define XZ_VER "1.7.6"
// #define XZ_COMPILE_DATE "May  4 2025"
// #define XZ_COMPILE_TIME "16:50:31"
// #define XZ_IDF_VER "v5.4-dev-5041-gd4aa25a38e-dirty"
// #define XZ_ELF_SHA256 "90e74f2becc2342b93026b6c8c85f487b3355911857d3e788a0521ca3421b7a3"

void xz_board_info_init();

const char* xz_board_info_mac();
const char* xz_board_info_uuid();

size_t xz_board_info_printf(char* buf, size_t len, const char* lang);