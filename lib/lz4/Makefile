ccflags-y += -O3 \
    -DLZ4_FREESTANDING=1 \
    -DLZ4_FAST_DEC_LOOP=1

# lz4.c carries both the compress and decompress code, and lib/Makefile only
# descends here when one of LZ4_COMPRESS/LZ4HC_COMPRESS/LZ4_DECOMPRESS is set,
# so lz4.o is always wanted. lz4hc.o is gated on its own symbol: building it
# unconditionally links a ~64 KB-stack LZ4HC_compress_optimal() into vmlinux
# even when nothing uses LZ4HC (EXPORT_SYMBOL keeps LTO from dropping it).
obj-y += lz4.o
obj-$(CONFIG_LZ4HC_COMPRESS) += lz4hc.o
