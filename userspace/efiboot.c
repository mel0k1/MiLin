// MiLin efiboot (M22): EFI boot manager на ESP — BOOTX64.EFI это не ядро,
// а наш лоадер. Прошивка грузит его по fallback-имени, лоадер читает с ESP
// \EFI\BOOT\CMDLINE (ascii, пишет minstall при установке) и стартует
// \EFI\BOOT\VMLINUZ.EFI с LoadOptions — per-install root=PARTUUID без
// перекомпиляции ядра (у EFI stub LoadOptions перекрывают CONFIG_CMDLINE).
//
// PE32+ без .reloc: образ линкуется на фиксированный ImageBase 0x100000,
// геометрия зашита в efiboot.ld (заголовки 512 + тело 0xFE00 = 64К).
// Две секции: .flat (код) и .sbat (SBAT-метаданные для revocation —
// shim/revocation-инфраструктура читает её по таблице секций); таблица
// data directories на месте (Certificate Table заполнит sbsign) — образ
// подписывается sbsign'ом без правки геометрии. Подпись — tools/sb-keys.sh
// (тестовые PK/KEK/db), гейт SecureBoot — бут D в tools/test-install.sh.
// CMDLINE/VMLINUZ.EFI читаются через SimpleFileSystem, ядро грузится
// LoadImage по device path (DeviceHandle ESP + FILE_PATH-нода) — у ядра
// будет честный DeviceHandle, как при загрузке boot-менеджером прошивки.
//
// NVRAM (M22): через Runtime Services лоадер сам ведёт boot-entry
// Boot####/BootOrder в efibootmgr-совместимом формате — имя "MiLin",
// target \EFI\MILIN\BOOTX64.EFI (второй экземпляр лоадера, minstall
// пишет его же цепочкой кластеров). Первый бут — fallback, лоадер создаёт
// запись и проверяет её чтением назад; следующие буты прошивка делает
// через BootOrder — лоадер узнаёт это по собственному device path.

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
// фиксированная геометрия: entry 0x200, ImageBase 0x100000, .flat VA 0x200
// size 0xFE00 + .sbat VA 0x10000 (см. efiboot.ld), 5 data directories.
struct pe_sec {
    u8  sname[8];
    u32 vsize, vaddr, rawsize, rawptr;
    u32 rel_ptr, ln_ptr;
    u16 nrel, nln;
    u32 sec_char;
} __attribute__((packed));

// секция .sbat: revocation-база shim-инфраструктуры (CSV, версия 1)
#define SBAT_CSV \
    "sbat,1,SBAT Version,sbat,1,https://github.com/rhboot/shim/blob/main/SBAT.md\n" \
    "milin-loader,1,MiLin,milin-loader,1,https://github.com/mel0k1/MiLin\n"
__attribute__((section(".sbat"), used))
static const char SBAT[] = SBAT_CSV;
#define SBAT_LEN (sizeof(SBAT) - 1)
#define SBAT_RAW ((SBAT_LEN + 0x1F) & ~0x1Fu)

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
    u32 dirs[6][2];     // до Certificate Table (sbsign заполнит индекс 4)
    struct pe_sec sects[2];
    u8  tail[0x200 - 0x188];
} __attribute__((packed));

__attribute__((section(".pe"), used))
static const struct pe_image PE = {
    .dos = { 'M', 'Z' },
    .lfanew = 0x80,
    .sig = 0x00004550,
    .machine = 0x8664,
    .nsections = 2,
    .optsize = 160,
    .coff_char = 0x0022,
    .magic = 0x20B,
    .code_size = 0xFE00,
    .entry = 0x200,
    .base_code = 0x200,
    .image_base = 0x100000,
    .sec_align = 0x20,
    .file_align = 0x20,
    .image_size = 0x10000 + SBAT_RAW,
    .hdr_size = 0x200,
    .subsystem = 10, // EFI_APPLICATION
    .stack_res = 0x100000,
    .stack_commit = 0x1000,
    .heap_res = 0x10000,
    .nrva = 6,
    .dirs = { { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 } },
    .sects = {
        { .sname = { '.', 'f', 'l', 'a', 't' },
          .vsize = 0xFE00, .vaddr = 0x200,
          .rawsize = 0xFE00, .rawptr = 0x200,
          .sec_char = 0x60000020 },
        { .sname = { '.', 's', 'b', 'a', 't' },
          .vsize = SBAT_LEN, .vaddr = 0x10000,
          .rawsize = SBAT_RAW, .rawptr = 0x10000,
          .sec_char = 0x40000040 },
    },
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
    struct efi_rt *runtime;
    struct efi_bs *bs;
    u64 ntables, config;
};

// --- Runtime Services: NVRAM-переменные (M22) ---
struct efi_rt {
    struct efi_tbl_hdr hdr;
    u64 get_time, set_time, get_wakeup, set_wakeup;
    u64 set_vam, convert_ptr;
    u64 get_variable, get_next_var, set_variable;
    u64 next_hi_mono, reset_system;
    u64 update_capsule, query_capsule, query_var_info;
};

#define EFI_VARIABLE_NV 1u
#define EFI_VARIABLE_BS 2u
#define EFI_VARIABLE_RT 4u

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
static const u8 g_gv[16] = // EFI_GLOBAL_VARIABLE
    GUID(0x8BE4DF61, 0x93CA, 0x11D2, 0xAA, 0x0D, 0x00, 0xE0, 0x98, 0x03, 0x2B, 0x8C);

static const u16 p_cmdline[] = { '\\', 'E', 'F', 'I', '\\', 'B', 'O', 'O', 'T',
                                 '\\', 'C', 'M', 'D', 'L', 'I', 'N', 'E', 0 };
static const u16 p_kernel[] = { '\\', 'E', 'F', 'I', '\\', 'B', 'O', 'O', 'T',
                                '\\', 'V', 'M', 'L', 'I', 'N', 'U', 'Z', '.', 'E', 'F', 'I', 0 };
static const u16 p_milin[] = { '\\', 'E', 'F', 'I', '\\', 'M', 'I', 'L', 'I', 'N',
                               '\\', 'B', 'O', 'O', 'T', 'X', '6', '4', '.', 'E', 'F', 'I', 0 };

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

// --- M22: boot-entry в NVRAM ---

// сравнение CHAR16-строк
static int eq16(const u16 *a, const u16 *b) {
    for (;;) {
        if (*a != *b) return 0;
        if (!*a) return 1;
        a++;
        b++;
    }
}

// Boot0000..BootFFFF: имя переменной в буфере
static void boot_var_name(u16 *b, u32 n) {
    static const u16 hex[] = { '0','1','2','3','4','5','6','7',
                               '8','9','A','B','C','D','E','F' };
    b[0] = 'B'; b[1] = 'o'; b[2] = 'o'; b[3] = 't';
    b[4] = hex[(n >> 12) & 15];
    b[5] = hex[(n >> 8) & 15];
    b[6] = hex[(n >> 4) & 15];
    b[7] = hex[n & 15];
    b[8] = 0;
}

// GetVariable: 1 = есть (данные в buf, длина в *len), 0 = нет/ошибка
static int get_var(struct efi_rt *rt, const u16 *name, void *buf, u64 *len) {
    u32 attr = 0;
    efi_status s = efi_call5(rt->get_variable, (u64)name, (u64)g_gv,
                             (u64)&attr, (u64)len, (u64)buf);
    return s == EFI_SUCCESS;
}

// последний FILE_PATH-нода device path — имя, которым нас загрузили
static const u16 *self_name(const struct efi_dp_node *dp) {
    const u16 *name = 0;
    u64 off = 0;
    for (;;) {
        const struct efi_dp_node *n = (const struct efi_dp_node *)
            ((const u8 *)dp + off);
        if (n->len < 4) break;
        if (n->type == 4 && n->sub == 4)
            name = (const u16 *)((const u8 *)n + 4);
        off += n->len;
        if (n->type == 0x7F && n->sub == 0xFF) break;
    }
    return name;
}

// SecureBoot-переменная: 1 = прошивка проверяет подписи (PK записан)
static void sb_check(struct efi_st *st) {
    struct efi_rt *rt = st->runtime;
    static const u16 p_sb[] = { 'S','e','c','u','r','e','B','o','o','t', 0 };
    u8 v = 0;
    u64 len = 1;
    if (!rt) return;
    if (get_var(rt, p_sb, &v, &len) && v == 1) {
        con_out(st, "MILEFI: secure boot on");
        con_out(st, "M22 SECUREBOOT OK");
    }
}

// Boot####: [ u32 attr ][ u16 FilePathListLength ][ CHAR16 desc ][ dp ][ opt ]
// ищем запись "MiLin"; нет — создаём на первом свободном номере (target
// \EFI\MILIN\BOOTX64.EFI на ESP загрузки) и ставим первым в BootOrder;
// запись проверяем чтением назад (M22 NVRAM ENTRY OK)
static void nvram_entry(struct efi_st *st, struct efi_li *li) {
    struct efi_rt *rt = st->runtime;
    static u8 ent[1024], rb[1024], ord[256], nord[256];
    static const u16 desc[] = { 'M', 'i', 'L', 'i', 'n', 0 };
    static const u16 p_order[] = { 'B','o','o','t','O','r','d','e','r', 0 };
    u16 nm[9];

    if (!rt || !li->device_handle) return;

    struct efi_dp_node *esp = 0;
    if (efi_call3(st->bs->handle_protocol, li->device_handle,
                  (u64)g_dp, (u64)&esp) != EFI_SUCCESS || !esp)
        return;
    u64 esplen = dp_len(esp);   // включая END-ноду ESP-пути
    u64 fplen = 4 + 2 * (sizeof(p_milin) / sizeof(p_milin[0])); // node + NUL
    u64 eplen = esplen + fplen; // esp без END + FILE-нода + END
    if (eplen > sizeof(ent) - 32) return;

    // свой номер: свободный или уже с описанием "MiLin"
    u32 n, have = 0xFFFFFFFF;
    for (n = 0; n < 256; n++) {
        boot_var_name(nm, n);
        u64 len = sizeof(rb);
        if (!get_var(rt, nm, rb, &len)) break;
        if (len > 6 && eq16((const u16 *)(rb + 6), desc)) { have = n; break; }
    }
    if (n >= 256) return;
    boot_var_name(nm, have != 0xFFFFFFFF ? have : n);

    if (have == 0xFFFFFFFF) {
        u32 attr = 1; // LOAD_OPTION_ACTIVE
        u32 o = 0;
        ent[o++] = (u8)attr;
        ent[o++] = (u8)(attr >> 8);
        ent[o++] = (u8)(attr >> 16);
        ent[o++] = (u8)(attr >> 24);
        u16 fpl = (u16)eplen;
        ent[o++] = (u8)fpl;
        ent[o++] = (u8)(fpl >> 8);
        for (u32 i = 0; i < sizeof(desc) / sizeof(desc[0]); i++) {
            ent[o++] = (u8)desc[i];
            ent[o++] = (u8)(desc[i] >> 8);
        }
        for (u32 i = 0; i < esplen - 4; i++) // ESP-путь без своей END-ноды
            ent[o++] = ((const u8 *)esp)[i];
        ent[o++] = 4; ent[o++] = 4; // MEDIA_DEVICE_PATH, MEDIA_FILEPATH_DP
        ent[o++] = (u8)fplen;
        ent[o++] = (u8)(fplen >> 8);
        for (u32 i = 0; i < sizeof(p_milin) / sizeof(p_milin[0]); i++) {
            ent[o++] = (u8)p_milin[i];
            ent[o++] = (u8)(p_milin[i] >> 8);
        }
        ent[o++] = 0x7F; ent[o++] = 0xFF; // END
        ent[o++] = 4; ent[o++] = 0;

        efi_status s = efi_call5(rt->set_variable, (u64)nm, (u64)g_gv,
                                 EFI_VARIABLE_NV | EFI_VARIABLE_BS |
                                 EFI_VARIABLE_RT, (u64)o, (u64)ent);
        if (s != EFI_SUCCESS) {
            con_hex(st, "MILEFI: SetVariable", s);
            return;
        }
        // чтение назад — байт в байт
        u64 rlen = sizeof(rb);
        int same = get_var(rt, nm, rb, &rlen) && rlen == o;
        for (u32 i = 0; same && i < o; i++)
            if (rb[i] != ent[i]) same = 0;
        if (same) con_out(st, "M22 NVRAM ENTRY OK");
        have = n;
        boot_var_name(nm, have);
    }

    // BootOrder: наш номер первым (если шина ещё не ведёт его)
    u64 olen = sizeof(ord);
    int have_order = get_var(rt, p_order, ord, &olen);
    int present = 0;
    for (u64 i = 0; have_order && i + 1 < olen; i += 2)
        if (ord[i] == (have & 0xFF) && ord[i + 1] == ((have >> 8) & 0xFF))
            present = 1;
    if (present) return;
    u32 q = 0;
    nord[q++] = (u8)have;
    nord[q++] = (u8)(have >> 8);
    for (u64 i = 0; have_order && i < olen; i++)
        nord[q++] = ord[i];
    efi_call5(rt->set_variable, (u64)p_order, (u64)g_gv,
              EFI_VARIABLE_NV | EFI_VARIABLE_BS | EFI_VARIABLE_RT,
              (u64)q, (u64)nord);
    con_out(st, "MILEFI: boot entry in BootOrder");
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

    // M22: кем загружены — NVRAM-entry (\EFI\MILIN\...) или fallback
    if (li->file_path) {
        const u16 *self = self_name((const struct efi_dp_node *)li->file_path);
        if (self && eq16(self, p_milin))
            con_out(st, "M22 NVRAM BOOT OK");
    }
    sb_check(st);
    nvram_entry(st, li);

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
