# DEC15 (GR15) -- docs/roadmap/GRADE-RESOLVE-2026-10.md. The zstd decoder
# the engine reads DaVinci Resolve's grades with (ResolveGrade.cc): Resolve
# stores each grade version as a zstd frame in its project database.
#
# DEC44 -- docs/roadmap/DONNEES-2026-10.md. And the single-threaded encoder
# of the same pinned archive: the project archive stores each chunk of a
# capture once, as a zstd frame (ArchiveManifest.cc). The multithreaded
# encoder (zstdmt_compress.c, pool.c, threading.c) is left out: a chunk is a
# few hundred kilobytes, and ZSTD_MULTITHREAD stays undefined.
#
# Without the x86-64 assembly (one portable path on every host) and without
# the weak tracing hooks, which a static link on macOS cannot leave
# undefined.
#
# Included by CMakeLists.txt and by headless/CMakeLists.txt once the pinned
# archive is available, so both builds compile the same files.
if(NOT zstd_SOURCE_DIR)
    message(FATAL_ERROR "zstd must be made available before CutmachineZstd.cmake")
endif()
add_library(cutmachine_zstd STATIC
    "${zstd_SOURCE_DIR}/lib/common/debug.c"
    "${zstd_SOURCE_DIR}/lib/common/entropy_common.c"
    "${zstd_SOURCE_DIR}/lib/common/error_private.c"
    "${zstd_SOURCE_DIR}/lib/common/fse_decompress.c"
    "${zstd_SOURCE_DIR}/lib/common/xxhash.c"
    "${zstd_SOURCE_DIR}/lib/common/zstd_common.c"
    "${zstd_SOURCE_DIR}/lib/decompress/huf_decompress.c"
    "${zstd_SOURCE_DIR}/lib/decompress/zstd_ddict.c"
    "${zstd_SOURCE_DIR}/lib/decompress/zstd_decompress.c"
    "${zstd_SOURCE_DIR}/lib/decompress/zstd_decompress_block.c"
    "${zstd_SOURCE_DIR}/lib/compress/fse_compress.c"
    "${zstd_SOURCE_DIR}/lib/compress/hist.c"
    "${zstd_SOURCE_DIR}/lib/compress/huf_compress.c"
    "${zstd_SOURCE_DIR}/lib/compress/zstd_compress.c"
    "${zstd_SOURCE_DIR}/lib/compress/zstd_compress_literals.c"
    "${zstd_SOURCE_DIR}/lib/compress/zstd_compress_sequences.c"
    "${zstd_SOURCE_DIR}/lib/compress/zstd_compress_superblock.c"
    "${zstd_SOURCE_DIR}/lib/compress/zstd_double_fast.c"
    "${zstd_SOURCE_DIR}/lib/compress/zstd_fast.c"
    "${zstd_SOURCE_DIR}/lib/compress/zstd_lazy.c"
    "${zstd_SOURCE_DIR}/lib/compress/zstd_ldm.c"
    "${zstd_SOURCE_DIR}/lib/compress/zstd_opt.c"
    "${zstd_SOURCE_DIR}/lib/compress/zstd_preSplit.c"
)
target_include_directories(cutmachine_zstd SYSTEM PUBLIC "${zstd_SOURCE_DIR}/lib")
target_compile_definitions(cutmachine_zstd PRIVATE
    ZSTD_DISABLE_ASM ZSTD_NO_TRACE ZSTD_LEGACY_SUPPORT=0)
set_target_properties(cutmachine_zstd PROPERTIES POSITION_INDEPENDENT_CODE ON)
