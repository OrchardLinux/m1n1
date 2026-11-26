ENTRY(_start)

/*
 * 32-bit Mach-O for the iBoot of Apple's ARMv7 chips (A5-A6X), which starts
 * it as the kernel: boot arguments in r0, MMU and caches off.
 */
/* Where XNU's own text sits on these chips; iBoot maps it onto the start of RAM. */
_va_base = 0x80001000;

_stack_size = 0x20000;
_exc_stack_size = 0x4000;

_max_payload_size = 64*1024*1024;

/* We are actually relocatable */
. = 0;

PHDRS
{
  hdr PT_LOAD;
  text PT_LOAD;
  rodata PT_LOAD;
  data PT_LOAD;
}

SECTIONS {
    _base = .;

    .header : {
        _mach_header = .;
        /* mach-o header */
        LONG(0xfeedface); /* magic */
        LONG(12); /* cputype = ARM */
        LONG(9); /* cpusubtype = ARM_V7 */
        LONG(0x02); /* filetype */
        LONG(7); /* ncmds */
        LONG(_cmd_end - _cmd_start); /* sizeofcmds */
        LONG(1); /* flags = NOUNDEFS */

        _cmd_start = .;

        /* unix_thread (entrypoint) */
        LONG(0x5); /* type = UNIX_THREAD */
        LONG(0x54); /* cmdsize */
        LONG(1); /* ARM_THREAD_STATE */
        LONG(17); /* length */
        . += 15 * 4; /* r0-r12, sp, lr */
        LONG(_start + _va_off) /* pc */
        LONG(0xd3); /* cpsr: SVC, IRQ and FIQ masked */

        ASSERT(. - _cmd_start == 0x54, "Bad unix_thread structure");

        /* segment: mach-o structures */
        LONG(0x1); /* type = SEGMENT */
        LONG(0x38); /* cmdsize */
        LONG(0x5244485f); /* segname = "_HDR" */
        . += 12;
        LONG(ADDR(.header) + _va_off); /* vmaddr */
        LONG(SIZEOF(.header)); /* vmsize */
        LONG(ADDR(.header) - _base); /* fileoff */
        LONG(SIZEOF(.header)); /* filesize */
        LONG(PROT_READ); /* maxprot */
        LONG(PROT_READ); /* initprot */
        LONG(0); /* nsects */
        LONG(0); /* flags */

        /* The 64-bit iBoot insists on __PRELINK_TEXT; kept here as well */
        LONG(0x1); /* type = SEGMENT */
        LONG(0x38); /* cmdsize */
        LONG(0x52505f5f); /* segname = "__PR" */
        LONG(0x4e494c45); /* segname = "ELIN" */
        LONG(0x45545f4b); /* segname = "K_TE" */
        LONG(0x00005458); /* segname = "XT.." */
        LONG(ADDR(.init) + _va_off); /* vmaddr */
        LONG(0); /* vmsize */
        LONG(ADDR(.init) - _base); /* fileoff */
        LONG(0); /* filesize */
        LONG(PROT_READ | PROT_EXECUTE); /* maxprot */
        LONG(PROT_READ | PROT_EXECUTE); /* initprot */
        LONG(0); /* nsects */
        LONG(0); /* flags */

        /* segment: text */
        LONG(0x1); /* type = SEGMENT */
        LONG(0x38); /* cmdsize */
        LONG(0x45545f5f); /* segname = "__TE  */
        LONG(0x00005458); /*            XT.." */
        . += 8;
        LONG(ADDR(.init) + _va_off); /* vmaddr */
        LONG(_text_size); /* vmsize */
        LONG(ADDR(.init) - _base); /* fileoff */
        LONG(_text_size); /* filesize */
        LONG(PROT_READ | PROT_EXECUTE); /* maxprot */
        LONG(PROT_READ | PROT_EXECUTE); /* initprot */
        LONG(0); /* nsects */
        LONG(0); /* flags */

        /* segment: rodata */
        LONG(0x1); /* type = SEGMENT */
        LONG(0x38); /* cmdsize */
        LONG(0x41444F52); /* segname = "RODA" */
        . += 12;
        LONG(ADDR(.rodata) + _va_off); /* vmaddr */
        LONG(_rodata_end - ADDR(.rodata)); /* vmsize */
        LONG(ADDR(.rodata) - _base); /* fileoff */
        LONG(_rodata_end - ADDR(.rodata)); /* filesize */
        LONG(PROT_READ); /* maxprot */
        LONG(PROT_READ); /* initprot */
        LONG(0); /* nsects */
        LONG(0); /* flags */

        /* segment: data */
        LONG(0x1); /* type = SEGMENT */
        LONG(0x38); /* cmdsize */
        LONG(0x41544144); /* segmname = "DATA" */
        . += 12;
        LONG(ADDR(.data) + _va_off); /* vmaddr */
        LONG(_data_size); /* vmsize */
        LONG(ADDR(.data) - _base); /* fileoff */
        LONG(SIZEOF(.data)); /* filesize */
        LONG(PROT_READ | PROT_WRITE); /* maxprot */
        LONG(PROT_READ | PROT_WRITE); /* initprot */
        LONG(0); /* nsects */
        LONG(0); /* flags */

        /*
         * segment: payload. Empty as built: whoever appends a payload to the
         * file sets vmsize and filesize to its length.
         */
        _payload_segment = .;
        LONG(0x1); /* type = SEGMENT */
        LONG(0x38); /* cmdsize */
        LONG(0x414c5f5f); /* segmname = "__LA  */
        LONG(0x00005453); /*            "ST.." */
        . += 8;
        LONG(_end + _va_off); /* vmaddr */
        LONG(0); /* vmsize */
        LONG(_file_end - _base); /* fileoff */
        LONG(0); /* filesize */
        LONG(PROT_READ | PROT_WRITE); /* maxprot */
        LONG(PROT_READ | PROT_WRITE); /* initprot */
        LONG(0); /* nsects */
        LONG(0); /* flags */

        _cmd_end = .;

        . = ALIGN(0x1000);
        _hdr_end = .;
    } :hdr
    _text_start = .;
    .init : ALIGN(0x1000) {
        *(.init)
        *(.init.*)
    } :text
    .text : ALIGN(0x1000) {
        *(.text)
        *(.text.*)
        . = ALIGN(8);
        *(.got.plt)
        . = ALIGN(0x1000);
    } :text
    _text_size = . - _text_start;
    .rodata : ALIGN(0x1000) {
        *(.rodata)
        *(.rodata.*)
        . = ALIGN(8);
    } :rodata
    .rel.dyn : {
        _rel_start = .;
        *(.rel)
        *(.rel.text)
        *(.rel.got)
        *(.rel.plt)
        *(.rel.bss)
        *(.rel.ifunc)
        *(.rel.text.*)
        *(.rel.data)
        *(.rel.data.*)
        *(.rel.rodata)
        *(.rel.rodata*)
        *(.rel.dyn)
        _rel_end = .;
        . = ALIGN(0x1000);
    } :rodata
    _rodata_end = .;
    _data_start = .;
    .data : ALIGN(0x1000) {
        *(.data)
        *(.data.*)
        . = ALIGN(8);
        _got_start = .;
        *(.got)
        _got_end = .;
        . = ALIGN(0x1000);
        _file_end = .;
    } :data
    .bss : ALIGN(0x1000) {
        _bss_start = .;
        *(.bss)
        *(.bss.*)
        *(.dynbss)
        *(COMMON)
        . = ALIGN(0x1000);
        _bss_end = .;
        PROVIDE(_exc_stack_top = .);
        . += _exc_stack_size;
        PROVIDE(_exc_stack_bot = .);
        . = ALIGN(0x1000);
        PROVIDE(_stack_top = .);
        . += _stack_size;
        PROVIDE(_stack_bot = .);
        . = ALIGN(0x1000);
    } :data
    _data_size = . - _data_start;
    _end = .;
    _payload_start = .;
    _payload_end = . + _max_payload_size;

    .symtab 0 : { *(.symtab) }
    .strtab 0 : { *(.strtab) }
    .shstrtab 0 : { *(.shstrtab) }

    /DISCARD/ : {
        *(.discard)
        *(.discard.*)
        *(.interp .dynamic)
        *(.dynsym .dynstr .hash .gnu.hash)
        *(.eh_frame)
        *(.gnu.version*)
        *(.note*)
        *(.comment*)
        *(.ARM.attributes)
        *(.ARM.exidx*)
        *(.ARM.extab*)
    }

    .empty (NOLOAD) : {
        *(.plt) *(.plt.*) *(.iplt) *(.igot)
        *(.data.rel.ro)
    }
    ASSERT(SIZEOF(.empty) == 0, "Unexpected sections detected!")

    .got.plt (NOLOAD) : {
        *(.got.plt)
    }
    ASSERT(SIZEOF(.got.plt) == 0 || SIZEOF(.got.plt) == 0xc, "Unexpected GOT PLT detected!")

    .stab          0 : { *(.stab) }
    .stabstr       0 : { *(.stabstr) }
    .stab.excl     0 : { *(.stab.excl) }
    .stab.exclstr  0 : { *(.stab.exclstr) }
    .stab.index    0 : { *(.stab.index) }
    .stab.indexstr 0 : { *(.stab.indexstr) }
    .comment       0 : { *(.comment) }
    .debug          0 : { *(.debug) }
    .line           0 : { *(.line) }
    .debug_srcinfo  0 : { *(.debug_srcinfo) }
    .debug_sfnames  0 : { *(.debug_sfnames) }
    .debug_aranges  0 : { *(.debug_aranges) }
    .debug_pubnames 0 : { *(.debug_pubnames) }
    .debug_info     0 : { *(.debug_info .gnu.linkonce.wi.*) }
    .debug_abbrev   0 : { *(.debug_abbrev) }
    .debug_line     0 : { *(.debug_line) }
    .debug_frame    0 : { *(.debug_frame) }
    .debug_str      0 : { *(.debug_str) }
    .debug_loc      0 : { *(.debug_loc) }
    .debug_macinfo  0 : { *(.debug_macinfo) }
    .debug_weaknames 0 : { *(.debug_weaknames) }
    .debug_funcnames 0 : { *(.debug_funcnames) }
    .debug_typenames 0 : { *(.debug_typenames) }
    .debug_varnames  0 : { *(.debug_varnames) }
    .debug_pubtypes 0 : { *(.debug_pubtypes) }
    .debug_ranges   0 : { *(.debug_ranges) }
    .debug_types    0 : { *(.debug_types) }
    .debug_addr     0 : { *(.debug_addr) }
    .debug_line_str 0 : { *(.debug_line_str) }
    .debug_loclists 0 : { *(.debug_loclists) }
    .debug_macro    0 : { *(.debug_macro) }
    .debug_names    0 : { *(.debug_names) }
    .debug_rnglists 0 : { *(.debug_rnglists) }
    .debug_str_offsets 0 : { *(.debug_str_offsets) }
    .debug_sup      0 : { *(.debug_sup) }
}

PROT_READ = 0x01;
PROT_WRITE = 0x02;
PROT_EXECUTE = 0x04;

_va_off = _va_base - _base;
