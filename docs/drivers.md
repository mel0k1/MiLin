# Драйверный контракт MiLin: от UIO до amdgpu

Вопрос M9: выдержит ли ядро MiLin «настоящие» драйверы — уровня amdgpu?
Ответ проверен и состоит из трёх частей: механизм размещения (уже в ядре),
зависимостный контракт (проверен Kconfig-пробой на v7.2) и план
восстановления вырезанных подсистем.

## 1. Механизм: kdriver-домен (уже работает)

UIO-путь (M4) остаётся fallback'ом, но основной путь для тяжёлых драйверов —
kdriver-домен (`drivers/mlin`, CONFIG_MILIN_KDRIVER):

- драйвер = kthread домена: ring 0, прямой доступ к MMIO/IRQ/DMA, свой
  pid и стек, обработка запросов НЕ в syscall-контексте чужого процесса;
- userspace говорит с ним через `/dev/<name>`: write = запрос, read =
  ответ (до 4КБ в каждую сторону) — тот же request/response контракт, что
  у lane'ов IPC;
- размещение — политика, а не архитектура: `auto` ставит драйвер в домен
  (быстрый путь), `'user'` через
  `/sys/class/mlkd/<name>/placement` отдаёт его userspace-серверу
  (UIO-путь), `'kernel'` возвращает в домен — на живой системе, без
  перезагрузки;
- жизненный цикл как у процесса: probe в собственном kthread'е, при
  пробое backoff и перезапуск, счётчики requests/restarts в sysfs.

Для условного amdgpu это означает: CAP/DRM-драйвер регистрируется в
домене и по умолчанию живёт в ядре (быстродействие), а при желании его
можно отодвинуть в userspace одной записью в sysfs (безопасность/
изоляция) — ровно то «между ядром и юзерспейсом», что и требовалось.

## 2. Контракт зависимостей (проверен на v7.2)

Проба: pristine v7.2, `tinyconfig + configs/milin.config +
configs/milin-kdrv.config (CONFIG_DRM=y, CONFIG_DRM_AMDGPU=y,
CONFIG_FW_LOADER=y)` → olddefconfig. Kconfig разворачивает замыкание из
**47 опций** сверх milin.config; ключевое:

```
DRM, DRM_AMDGPU, DRM_AMD_DC (+DC_FP), DRM_KMS_HELPER, DRM_CLIENT,
DRM_TTM(+TTM_HELPER), DRM_EXEC, DRM_BUDDY(+GPU_BUDDY), DRM_SUBALLOC_HELPER,
DRM_SCHED, DRM_DISPLAY_HELPER (DP/DSC/HDCP/HDMI/CEC), DRM_BRIDGE/PANEL,
DMA_SHARED_BUFFER, SYNC_FILE, INTERVAL_TREE, KCMP, GENERIC_ALLOCATOR,
FW_LOADER, POWER_SUPPLY(+HWMON), HWMON, I2C (+ALGOBIT/BOARDINFO), HDMI,
CEC_CORE(+NOTIFIER), VIDEO, BACKLIGHT_CLASS_DEVICE, CRC16, SHMEM, NET_DEVMEM
```

Вывод: **Kconfig-замыкание полное** — ни одна зависимость не ведёт в
вырезанное неустранимо (SHMEM, INTERVAL_TREE, KCMP, FW_LOADER уже в
дереве или восстанавливаются из сохранившихся каталогов).

## 3. Что вырезано и как вернуть

Проба выявила каталоги, которых нет в форке — **восстановлены в M10**
из базы v7.2 целиком, как это уже делалось для sysfs/kernfs/iomap в M4.
Оговорка: top-level `drivers/cec` в v7.2 НЕ существует — CEC живёт в
`drivers/media/cec` и вернулся вместе с media:

| Каталог | Для чего | Статус |
|---------|----------|--------|
| `drivers/gpu` | DRM core + amdgpu (+ttm, scheduler, display) | ✅ M10 |
| `drivers/dma-buf` | dma-buf/sync_file: обмен буферами с userspace | ✅ M10 |
| `drivers/i2c` | DDC/aux-каналы мониторов, DP AUX | ✅ M10 |
| `drivers/power` | POWER_SUPPLY (батарея/адаптер для DC) | ✅ M10 |
| `drivers/hwmon` | температурные сенсоры GPU | ✅ M10 |
| `drivers/media` | CEC-нотификаторы HDMI (+media/cec) | ✅ M10 |

Уже на месте: `include/drm`, `include/uapi/drm`, `include/uapi/linux/dma-buf.h`
(возвращены в M4), `drivers/video` (+backlight), `drivers/base/firmware_loader`
(прошивки грузятся из userspace `/lib/firmware` — сама точка загрузки
остаётся userspace-контрактом).

Оценка цены: ~1000 .c-файлов amdgpu (+DRM core) против 12 212 файлов всего
дерева; включение — только фрагментом `configs/milin-kdrv.config`
поверх `milin.config`, не в дефолтной диете.

## 4. Компиляция (проверка сборки)

Проба сборки: `make O=... drivers/gpu/drm/amd/amdgpu/` на pristine v7.2
с замыканием из п.2 — **amdgpu собрался полностью**: 893 объекта, 0 ошибок,
финальный `AR drivers/gpu/drm/amd/amdgpu/built-in.a` (заголовки, ABI и все
зависимости amdgpu работают в этом окружении).

**M10: проверка повторена в ОБРЕЗАННОМ дереве.** Полная сборка bzImage с
фрагментом `configs/milin-kdrv.config` (build.sh KDRV=1): 989 объектов под
drivers/gpu, bzImage 3 339 264 байта (+138% к diet-сборке — вне бюджета
дефолтной диеты, потому и фрагмент), ядро грузится, весь демо-цикл M5–M9
проходит (docs/boot-m10.log, маркер M10 OK через /sys/module/amdgpu).

Оговорка о железе: QEMU не эмулирует AMD GPU, поэтому runtime-смоук
amdgpu возможен только на реальном железе (bzImage + CONFIG_DRM_AMDGPU).
В QEMU amdgpu-конфигурация просто грузится без нахождения устройства —
как и любой другой зарегистрированный, но не привязанный драйвер.

## 5. Политика размещения GPU-драйвера (итог)

```
регистрация драйвера:  mlkd_register(&amdgpu, .placement = MLKD_PL_AUTO)
резолв политики:       AUTO -> ядро (домен, быстрый путь)
откат на изоляцию:     echo user > /sys/class/mlkd/amdgpu/placement
                       (домен: -ENODEV, драйвер обслуживает UIO-сервер)
возврат:               echo kernel > /sys/class/mlkd/amdgpu/placement
```

Домен даёт и изоляцию без перехода в userspace: у драйвера свой kthread,
свой стек, счётчики и перезапуск при пробое — сбой amdgpu не тянет ядро,
он перезапускается как сервис супервизора (по аналогии с logsrv).

## 6. M11: mlkd-фасад amdgpu — запросная плоскость в домене

Фасад — драйвер домена (`drivers/mlin/amdgpu-fac.c`, /dev/amdgpu), который
принимает от клиентов микроядра протокол **mlgpu** (зеркала: форк
`drivers/mlin/mlgpu.h` и MiLin `userspace/mlgpu.h`):

| op | запрос | ответ | смысл |
|----|--------|-------|-------|
| I | — | `mlgpu_info_out` | имя/api/режим/VRAM/заборы |
| A | `mlgpu_alloc_in` | `mlgpu_alloc_out` | буфер из пула VRAM (8 слотов по 1 МиБ) |
| F | `mlgpu_free_in` | — | возврат слота |
| S | `mlgpu_submit_in` + cmd[] | `mlgpu_submit_out` | коммит командного буфера -> забор |
| W | `mlgpu_wait_in` | `mlgpu_wait_out` | состояние забора |

Режимы фасада (лениво на первом INFO — amdgpu регистрирует pci-драйвер
позже device_initcall): **facade** (diet-сборка, эмуляция), **amdgpu**
(kdrv-сборка: драйвер builtin, устройства нет — запросная плоскость перед
DRM), **hw** (AMD-устройство найдено — точка форвардинга в DRM/TTM).

**Одна политика размещения — обе инкарнации.** placement=kernel: цикл
запросов живёт в kthread домена (`/dev/amdgpu`, write=запрос, read=ответ).
placement=user: домен отклоняет (-ENODEV, M9), а протокол обслуживает
userspace-демон **gpud** через lane шины `gpu.amdgpu` (ML_PROTO_GPU, root-only
политика) — тот же wire-формат, отдельный state, уступание дороге домену
(ML_E_PERM) при возврате в kernel. Демо супервизора (docs/boot-m11.log):
kernel-цикл -> flip user -> lane-цикл -> flip kernel -> gpud уступил.
gpud переживает смерть шины каскадным перезапуском (state сохранён).

kdrv-валидация: amdgpu-fac.o собран с конфигом PCI=y/DRM_AMDGPU=y
(driver_find/pci_get_device в объекте); полный линк kdrv — как в M10.
Бут-гейт diet-сборки: маркер **M11 OK**.
