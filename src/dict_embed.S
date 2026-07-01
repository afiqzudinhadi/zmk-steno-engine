    .section .rodata.steno_dict, "a", %progbits
    .global _steno_dict_start
    .global _steno_dict_end

    .balign 4
_steno_dict_start:
#ifdef STENO_DICT_BIN_PATH
    .incbin STENO_DICT_BIN_PATH
#else
    .byte 0x00, 0x00, 0x00, 0x00
#endif
_steno_dict_end:
