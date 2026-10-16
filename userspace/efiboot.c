// MiLin efiboot (M22): EFI boot manager на ESP — BOOTX64.EFI это не ядро,
// а наш лоадер. Прошивка грузит его по fallback-имени, лоадер читает с ESP
// \EFI\BOOT\CMDLINE (ascii, пишет minstall при установке) и стартует
// \EFI\BOOT\VMLINUZ.EFI с LoadOptions — per-install root=PARTUUID без
// перекомпиляции ядра (у EFI stub LoadOptions перекрывают CONFIG_CMDLINE).
//
// PE32+ без .reloc: образ линкуется на фиксированный ImageBase 0x100000,
// геометрия зашита в efiboot.ld (заголовки 512 + тело 0xFE00 = образ 16К).
// Одна секция .flat: прошивка копирует её целиком на ImageBase, entry —
// первый байт (efi_call-туннели из efiboot.S: прошивка зовёт в Win64).
// CMDLINE/VMLINUZ.EFI читаются через SimpleFileSystem, ядро грузится
// LoadImage по device path (DeviceHandle ESP + FILE_PATH-нода) — у ядра
// будет честный DeviceHandle, как при загрузке boot-менеджером прошивки.

typedef unsigned long u64;
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;
typedef u64 efi_status;

#define EFI_SUCCESS           0
#define EFI_LOAD_ERROR        ((efi_status)0x8000000000000001ull)
#define EFI_INVALID_PARAMETER ((efi_status)0x8000000000000002ull)
#define EFI_NOT_FOUND         ((efi_status)0x800000000000000Eull)

#define FILE_MODE_READ 1

// --- PE32+ заголовок, секция .pe (ld ставит её в LMA 0) ---
// фиксированная геометрия: entry 0x200, образ 16К, ImageBase 0x100000,
// одна секция .flat VA 0x200 size 0xFE00, data directory нет.
struct pe_image {
    u8  dos[0x3C];
    u32 lfanew;         // 0x3C -> 0x80
    u8  pad1[0x40];
    u32 sig;            // "PE\0\0"
    u16 machine, nsections;
    u32 stamp, symtab, nsyms;
    u16 optsize, coff_char;
    u16 magic; u8 lmajor, lminor;
    u32 code_size, init_size, uninit_size, entry, base_code;
    u64 image_base;
    u32 sec_align, file_align, os_ver, img_ver, ss_ver, win32_ver;
    u32 image_size, hdr_size, checksum;
    u16 subsystem, dll_char;
    u64 stack_res, stack_commit, heap_res, heap_commit;
    u32 loader_flags, nrva;
    u8  sname[8];
    u32 vsize, vaddr, rawsize, rawptr;
    u32 rel_ptr, ln_ptr;
    u16 nrel, nln;
    u32 sec_char;
    u8  tail[0x200 - 0x130];
} __attribute__((packed));

__attribute__((section(".pe"), used))
static const struct pe_image PE = {
    .dos = { 'M', 'Z' },
    .lfanew = 0x80,
    .sig = 0x00004550,
    .machine = 0x8664,
    .nsections = 1,
    .optsize = 112,
    .coff_char = 0x0022,
    .magic = 0x20B,
    .code_size = 0xFE00,
    .entry = 0x200,
    .base_code = 0x200,
    .image_base = 0x100000,
    .sec_align = 0x20,
    .file_align = 0x20,
    .image_size = 0x10000,
    .hdr_size = 0x200,
    .subsystem = 10, // EFI_APPLICATION
    .stack_res = 0x100000,
    .stack_commit = 0x1000,
    .heap_res = 0x10000,
    .nrva = 0,
    .sname = { '.', 'f', 'l', 'a', 't' },
    .vsize = 0xFE00,
    .vaddr = 0x200,
    .rawsize = 0xFE00,
    .rawptr = 0x200,
    .sec_char = 0x60000020,
};

// --- EFI: минимум структур, вызовы только через туннели efiboot.S ---
struct efi_tbl_hdr {
    u64 signature;
    u32 revision;
    u32 hdr_size;
    u32 crc;
    u32 reserved;
};

struct efi_sto {
    u64 reset;
    u64 output_string;   // (this, CHAR16*) -> status
    u64 test_string;
};

struct efi_bs {
    struct efi_tbl_hdr hdr;
    u64 raise_tpl, restore_tpl;
    u64 allocate_pages, free_pages, get_memory_map, allocate_pool, free_pool;
    u64 create_event, set_timer, wait_for_event, signal_event;
    u64 close_event, check_event;
    u64 install_pi, reinstall_pi, uninstall_pi;
    u64 handle_protocol; // (handle, GUID*, void**) -> status
    u64 reserved;
    u64 register_pn, locate_handle, locate_dp, install_cfg;
    u64 load_image;      // (boot_policy, parent, dp, buf, size, *handle)
    u64 start_image;     // (handle, *exit_size, **exit_data)
    u64 exit, unload_image, exit_boot_services;
    u64 get_next_mono, stall, set_watchdog, connect_controller, disconnect;
    u64 open_protocol, close_protocol, open_pi_info, protocols_per_handle;
    u64 locate_handle_buffer, locate_protocol; // LocateProtocol
};

struct efi_st {
    struct efi_tbl_hdr hdr;
    u16 *fw_vendor;
    u32 fw_rev;
    u32 pad;
    u64 con_in_handle, con_in, con_out_handle;
    struct efi_sto *con_out;
    u64 err_handle, err_out;
    u64 runtime;
    struct efi_bs *bs;
    u64 ntables, config;
};

struct efi_li {
    u32 revision;
    u32 pad;
    u64 parent, sys_table, device_handle, file_path, reserved;
    u32 load_options_size;
    u32 pad2;
    u64 load_options;
};

struct efi_sfs {
    u64 revision;
    u64 open_volume;     // (this, **root) -> status
};

struct efi_file {
    u64 revision;
    u64 open;            // (this, **new, CHAR16*, mode, attrs)
    u64 close;           // (this)
    u64 del, read, write;
    u64 get_pos, set_pos;
    u64 get_info, set_info, flush;
};

struct efi_dp_node {
    u8 type, sub;
    u16 len;
};

#define GUID(a, b, c, d0, d1, d2, d3, d4, d5, d6, d7) \
    { (a) & 0xFF, ((a) >> 8) & 0xFF, ((a) >> 16) & 0xFF, ((a) >> 24) & 0xFF, \
      (b) & 0xFF, ((b) >> 8) & 0xFF, (c) & 0xFF, ((c) >> 8) & 0xFF, \
      d0, d1, d2, d3, d4, d5, d6, d7 }

static const u8 g_li[16] =
    GUID(0x5B1B31A1, 0x9562, 0x11d2, 0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B);
static const u8 g_sfs[16] =
    GUID(0x964E5B22, 0x6459, 0x11D2, 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B);
static const u8 g_dp[16] =
    GUID(0x09576E91, 0x6D3F, 0x11D2, 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B);

static const u16 p_cmdline[] = { '\\', 'E', 'F', 'I', '\\', 'B', 'O', 'O', 'T',
                                 '\\', 'C', 'M', 'D', 'L', 'I', 'N', 'E', 0 };
static const u16 p_kernel[] = { '\\', 'E', 'F', 'I', '\\', 'B', 'O', 'O', 'T',
                                '\\', 'V', 'M', 'L', 'I', 'N', 'U', 'Z', '.', 'E', 'F', 'I', 0 };

extern u64 efi_call1(u64 fn, u64 a);
extern u64 efi_call2(u64 fn, u64 a, u64 b);
extern u64 efi_call3(u64 fn, u64 a, u64 b, u64 c);
extern u64 efi_call4(u64 fn, u64 a, u64 b, u64 c, u64 d);
extern u64 efi_call5(u64 fn, u64 a, u64 b, u64 c, u64 d, u64 e);
extern u64 efi_call6(u64 fn, u64 a, u64 b, u64 c, u64 d, u64 e, u64 f);
u64 efi_main(u64 image_handle, u64 stp);

// ascii -> CHAR16 + вывод на ConOut (серийная консоль прошивки)
static void con_out(struct efi_st *st, const char *s) {
    u16 b[64];
    u32 i = 0;
    for (; s[i] && i < sizeof(b) / sizeof(b[0]) - 2; i++)
        b[i] = (u16)s[i];
    b[i++] = '\r';
    b[i++] = '\n';
    b[i] = 0;
    efi_call2(st->con_out->output_string, (u64)st->con_out, (u64)b);
}

static void con_hex(struct efi_st *st, const char *who, efi_status s) {
    char b[32];
    int i = 0;
    while (who[i] && i < 16) { b[i] = who[i]; i++; }
    b[i++] = ' ';
    b[i++] = 'e';
    b[i++] = 'r';
    b[i++] = 'r';
    b[i++] = '=';
    for (int k = 15; k >= 0; k--) {
        int v = ((unsigned long)s >> (k * 4)) & 0xF;
        b[i++] = v < 10 ? '0' + v : 'A' + v - 10;
    }
    b[i] = 0;
    con_out(st, b);
}

// длина device path до END (включая END-ноду)
static u64 dp_len(const struct efi_dp_node *dp) {
    u64 off = 0;
    for (;;) {
        const struct efi_dp_node *n = (const struct efi_dp_node *)
            ((const u8 *)dp + off);
        if (n->len < 4) break;
        off += n->len;
        if (n->type == 0x7F && n->sub == 0xFF) break;
    }
    return off;
}

// файл целиком в buf; size: до вызова — ёмкость, после — фактическая длина
// (не нужен: ядро грузится LoadImage по device path, firmware читает сама)

u64 efi_main(u64 image_handle, u64 stp) {
    struct efi_st *st = (struct efi_st *)stp;
    struct efi_bs *bs = st->bs;

    con_out(st, "MILEFI: boot manager");

    struct efi_li *li = 0;
    efi_status s = efi_call3(bs->handle_protocol, image_handle,
                             (u64)g_li, (u64)&li);
    if (s != EFI_SUCCESS || !li) {
        con_hex(st, "MILEFI: LoadedImage", s);
        return EFI_LOAD_ERROR;
    }

    struct efi_sfs *sfs = 0;
    s = efi_call3(bs->handle_protocol, li->device_handle,
                  (u64)g_sfs, (u64)&sfs);
    if (s != EFI_SUCCESS || !sfs) {
        con_hex(st, "MILEFI: SimpleFileSystem", s);
        return EFI_LOAD_ERROR;
    }

    u64 root = 0;
    s = efi_call2(sfs->open_volume, (u64)sfs, (u64)&root);
    if (s != EFI_SUCCESS || !root) {
        con_hex(st, "MILEFI: OpenVolume", s);
        return EFI_LOAD_ERROR;
    }

    // CMDLINE: ascii на ESP -> pool (UTF-16 под LoadOptions)
    u64 cf = 0;
    s = efi_call5(((struct efi_file *)root)->open, root, (u64)&cf,
                  (u64)p_cmdline, FILE_MODE_READ, 0);
    if (s != EFI_SUCCESS || !cf) {
        con_hex(st, "MILEFI: open CMDLINE", s);
        return EFI_LOAD_ERROR;
    }
    struct efi_file *cmd = (struct efi_file *)cf;
    u64 clen = 0;
    s = efi_call2(cmd->set_pos, cf, 0xFFFFFFFFFFFFFFFFull);
    if (s != EFI_SUCCESS) {
        con_hex(st, "MILEFI: seek CMDLINE", s);
        return EFI_LOAD_ERROR;
    }
    s = efi_call2(cmd->get_pos, cf, (u64)&clen);
    if (s != EFI_SUCCESS || !clen || clen > 2048) {
        con_hex(st, "MILEFI: size CMDLINE", s);
        return EFI_LOAD_ERROR;
    }
    u8 *ascii = 0;
    s = efi_call4(bs->allocate_pool, 2 /*LoaderData*/, clen, (u64)&ascii, 0);
    if (s != EFI_SUCCESS || !ascii) {
        con_hex(st, "MILEFI: pool", s);
        return EFI_LOAD_ERROR;
    }
    s = efi_call2(cmd->set_pos, cf, 0);
    s = efi_call3(cmd->read, cf, (u64)&clen, (u64)ascii);
    efi_call1(cmd->close, cf);
    if (s != EFI_SUCCESS) {
        con_hex(st, "MILEFI: read CMDLINE", s);
        return EFI_LOAD_ERROR;
    }

    u16 *opts = 0;
    s = efi_call4(bs->allocate_pool, 2, clen * 2, (u64)&opts, 0);
    if (s != EFI_SUCCESS || !opts) {
        con_hex(st, "MILEFI: pool opts", s);
        return EFI_LOAD_ERROR;
    }
    for (u64 i = 0; i < clen; i++)
        opts[i] = (u16)ascii[i];

    // ядро читаем сами (firmware LoadImage по device path на этом образе
    // виснет) и грузим SourceBuffer'ом: DevicePath=NULL, прошивка делает
    // внутренний memory-device path. initrd= в cmdline нет — DeviceHandle
    // у child будет пустой, EFI-stub это переживает (efi_load_initrd
    // возвращает SUCCESS при отсутствии initrd-источников)
    u64 kf = 0;
    s = efi_call5(((struct efi_file *)root)->open, root, (u64)&kf,
                  (u64)p_kernel, FILE_MODE_READ, 0);
    if (s != EFI_SUCCESS || !kf) {
        con_hex(st, "MILEFI: open VMLINUZ", s);
        return EFI_LOAD_ERROR;
    }
    struct efi_file *kfile = (struct efi_file *)kf;
    u64 ksz = 0;
    s = efi_call2(kfile->set_pos, kf, 0xFFFFFFFFFFFFFFFFull);
    s = efi_call2(kfile->get_pos, kf, (u64)&ksz);
    if (s != EFI_SUCCESS || !ksz || ksz > 64u << 20) {
        con_hex(st, "MILEFI: size VMLINUZ", s);
        return EFI_LOAD_ERROR;
    }
    u64 npages = (ksz + 0xFFF) >> 12;
    u64 kbuf = 0;
    s = efi_call4(bs->allocate_pages, 0 /*AnyPages*/, 2 /*LoaderData*/,
                  npages, (u64)&kbuf);
    if (s != EFI_SUCCESS || !kbuf) {
        con_hex(st, "MILEFI: pages", s);
        return EFI_LOAD_ERROR;
    }
    s = efi_call2(kfile->set_pos, kf, 0);
    s = efi_call3(kfile->read, kf, (u64)&ksz, kbuf);
    efi_call1(kfile->close, kf);
    if (s != EFI_SUCCESS) {
        con_hex(st, "MILEFI: read VMLINUZ", s);
        return EFI_LOAD_ERROR;
    }

    // LoadImage + LoadOptions + StartImage — как boot-менеджер прошивки
    u64 child = 0;
    s = efi_call6(bs->load_image, 0, image_handle, 0, kbuf, ksz, (u64)&child);
    if (s != EFI_SUCCESS || !child) {
        con_hex(st, "MILEFI: LoadImage", s);
        return EFI_LOAD_ERROR;
    }
    struct efi_li *cli = 0;
    s = efi_call3(bs->handle_protocol, child, (u64)g_li, (u64)&cli);
    if (s != EFI_SUCCESS || !cli) {
        con_hex(st, "MILEFI: child LoadedImage", s);
        return EFI_LOAD_ERROR;
    }
    cli->load_options = (u64)opts;
    cli->load_options_size = (u32)(clen * 2);

    con_out(st, "MILEFI: kernel loaded, options set");
    s = efi_call3(bs->start_image, child, 0, 0);
    if (s != EFI_SUCCESS)
        con_hex(st, "MILEFI: kernel exited", s);
    return s;
}
