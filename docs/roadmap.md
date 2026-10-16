# MiLin — дорожная карта

Цель проекта в прямом смысле: взять настоящее ядро Linux и, разрезая его,
довести до понятия «микроядро». Мы не пишем ядро с нуля — мы оперируем
живым монолитом и откусываем от него по куску, каждый раз сохраняя
работоспособность системы.

## Принцип: резать в трёх плоскостях

1. **Конфигурация.** Почти всё в Linux можно выключить через Kconfig, не
   трогая ни строчки кода. Это безопасная тренировка: мы учимся понимать,
   что действительно нужно ядру, а что — балласт, и получаем метрики.
2. **Исходники.** Затем мы физически удаляем подсистемы из дерева: `drivers/`,
   `sound/`, `media/`, лишние `fs/`, чужие `arch/`. Ядро собирается из
   обрезанного дерева, а дерево и скрипты резни живут в репозитории.
3. **Архитектура.** Финальная плоскость: то, что нельзя просто удалить,
   перемещается в userspace (драйверы → UIO-серверы, файловые системы →
   FUSE-серверы), а внутри ядра появляется IPC в микроядерном смысле —
   сообщениями между процессами. В идеале ядро сводится к связке
   «планировщик + память + IPC».

## Этапы

### M1 — Базовый бут ✅
Собрать настоящий Linux минимальной конфигурацией, загрузить в QEMU и
убедиться, что наш userspace `/init` получает управление и пишет приветствие.
Это фундамент: дальше мы режем уже работающую систему и после каждой операции
проверяем, что она всё ещё грузится.

- источник: стабильное ядро, версия зафиксирована в `tools/kernel-version`;
- конфигурация: tinyconfig + минимальный набор (сейчас живёт в `configs/milin.config`);
- критерий готовности: QEMU-лог содержит `Hello from MiLin`, CI зелёный.

### M2 — Конфиг-диета ✅
Конфигурация превращена из «tinyconfig + добавки» в явную конституцию
`configs/milin.config`: каждый включённый пункт обязан доказать право на
жизнь. Вырезаны: input-подсистема, VT-консоли, HID, procfs/sysfs/tmpfs,
sysctl, пять из шести декомпрессоров initramfs, perf/debug/objtool,
микрокод CPU, vsyscall, IBT, pkeys, DMA-зоны, core dumps.

- конфигурация: `configs/milin.config` — фрагмент-конституция поверх tinyconfig;
- результат: bzImage −22.9% (1.00 → 0.77 МиБ), text −12.8%, опций =y −13% (529 → 460);
- инструменты: `tools/size-report.sh`, метрики в CI — job summary на каждый коммит;
- открытие: ядро собирается и работает без UID/GID (tinyconfig выключил
  MULTIUSER ещё в M1) — miлин пока даже не многопользовательская система,
  и это честно для микроядра, вернём многопользовательность в M4, когда
  появятся userspace-серверы.

### M3 — Резня в исходниках ✅
Дерево исходников переехало в **форк [mel0k1/linux](https://github.com/mel0k1/linux)**
(ветка `milin`, база — тег v7.2), репозиторий MiLin остался проектом:
инструменты, конфиг, CI, документация. Скрипт резни — `tools/carve.sh`
в репозитории MiLin: физически удаляет подсистемы и чинит Kbuild/Kconfig,
чтобы сборка не замечала пропажу (фиксер — `tools/fix-dead-refs.py`).

Вырезано: все архитектуры кроме x86, Documentation, samples, rust, certs,
usr (built-in initramfs), io_uring, sound, net, block, crypto, ipc, kvm;
drivers ужаты до base/tty/char/virtio/firmware/uio; fs — до ядра ВФС +
ramfs/fuse; kernel/ — без bpf/trace/cgroup/livepatch/power/kexec/gcov;
tools/ — до машинерии objtool; security/ — до двух файлов-клея.

- результат: дерево 1.8G → 139 МиБ (−92%), файлов 94,856 → 12,212 (−87%),
  строк .c/.h 37.2M → 3.5M (−91%); ядро собирается и грузится
  (docs/boot-m3.log), метрики — docs/metrics/m3.json;
- швы (честные заплатки резни): io_wq-стабы планировщика, `__initramfs_size = 0`,
  меню RD_* переехало из usr/Kconfig в init/Kconfig, ядро само создаёт
  `/dev/console` (initramfs собирается без root и без device-нод);
- открытие №1: x86-ядро безусловно линкуется с drivers/clocksource (i8253 PIT),
  drivers/rtc (CMOS) и security/commoncap — это клей платформы, а не подсистемы;
- открытие №2: в v7.2 PERF_EVENTS потерял Kconfig-промпт (always-on),
  значит perf-ядро (kernel/events) — часть современного скелета Linux;
- открытие №3: база переезжает с 7.2.9 (stable-тарбол) на v7.2 (тег форка):
  stable-теги живут в stable-дереве, которое на GitHub не форкнётся.

### M4 — Userspace-изация ✅
Всё, что осталось за пределами «судьбоносного треугольника»
(планировщик/память/системные вызовы), переезжает в userspace:

- драйверы → серверы на базе UIO (Userspace I/O);
- файловые системы → серверы на базе FUSE;
- `/init` превращается в супервизор серверов.

Сделано. Userspace-триада без libc (raw syscalls): супервизор
`userspace/supervisor.c` → `/init`, серверы `fused` и `uiod` → `/sbin`.
Супервизор порождает серверы через fork+execve, привязывает железо к
userspace-драйверу и следит за жизненным циклом: graceful shutdown через
umount → сервер видит EOF и выходит сам.

Демо-сценарий (docs/boot-m4.log):
1. супервизор монтирует devtmpfs и sysfs;
2. привязывает pci-testdev (1b36:0005) к uio_pci_generic записью в
   `/sys/bus/pci/drivers/uio_pci_generic/new_id` — супервизор сам решает,
   какое железо обслуживает какой сервер (драйвер без таблицы device-id —
   умышленно: назначение железа — политика, а не ядро);
3. `uiod` мапит BAR0 через /dev/uio0 и читает регистр устройства —
   драйвер в userspace, шина в ядре;
4. `fused` монтирует /mnt через /dev/fuse и отвечает ядру
   FUSE_INIT/LOOKUP/GETATTR/ACCESS;
5. клиент создаётся с setuid(1000) и читает /mnt/README через FUSE:
   непривилегированный пользователь получает данные от userspace-сервера.

Возвращено в ядро — «контракт серверов», не функциональность:
MULTIUSER (uid-изоляция серверов), PCI-шина (перечисление устройств),
UIO+UIO_PCI_GENERIC, FUSE_FS, devtmpfs (ноды для динамических major),
sysfs+kernfs (discovery-интерфейс, восстановлены из v7.2). В дереве
восстановлены fs/iomap (select у FUSE), drivers/video (screen_info_pci
для vgaarb) и fs/sysfs/fs/kernfs — ровно то, на что ссылаются включённые
подсистемы; по этой же причине выключен SERIAL_8250_EXAR (тянул
eeprom_93cx6 из вырезанного drivers/misc).

- результат: демо-цикл проходит за 1.3 с (docs/boot-m4.log, маркер M4 OK);
  bzImage 812K → 988K (+21.6%) — цена контракта, документирована честно;
- открытие №1: в ядре без sysfs uio_pci_generic бесполезен — привязка
  device-id выполняется только через /sys/bus/.../new_id, поэтому sysfs
  вернулся не как «фс дайвинда», а как контракт discovery;
- открытие №2: nostdlib-процессы получают управление с rsp, выровненным
  на 16 и БЕЗ return address — gcc генерирует movaps, рассчитанный на
  rsp%16==8; без trampлина в _start любой 16-байтный struct ловит GPF;
- открытие №3: wait4 без явного NULL-rusage возвращает EFAULT — мусорный
  r10 ядро трактует как указатель на rusage;
- открытие №4: wait4/mount — syscalls с 4–5 аргументами: короткие обёртки
  оставляют мусор в r10/r8/r9, и ядро читает их как настоящие указатели.
  Все вызовы с более чем 3 аргументами должны обнулять регистры явно.

### M5 — Микроядерный IPC ✅
Серверы перестают быть сиротами: у них есть шина. Формат сообщений и
семантика реестра подсмотрены у Managarm (bragi + mbus), транспорт —
самый простой из возможных: AF_UNIX SOCK_SEQPACKET (одно сообщение =
один атомарный пакет), хэндлы = файловые дескрипторы, передача хэндлов
= SCM_RIGHTS.

- реестр `/sbin/mlbus` (идея mbus): `REGISTER(name)` от сервера,
  `LOOKUP(name)` от клиента; шина создаёт socketpair и раздаёт концы —
  дальше клиент и сервер общаются напрямую, шина данных не видит
  (аналог GetRemoteLane);
- формат сообщения (идея bragi): заголовок {proto, msg_id, err,
  tail_len} + inline-поля head[24] + переменный tail + hooks (хэндлы);
- log-сервер `/sbin/logsrv`: принимает сообщения клиентов, печатает,
  отвечает ack и ведёт счётчик; остановка — SIGTERM супервизора;
- критерий M5 выполнен: непривилегированный клиент получает lane-хэндл
  через реестр и обменивается сообщениями с log-сервером
  (WRITE×2 + STATS served=2), FUSE-регрессия M4 проходит.

Цена контракта v7.2: NET тянет select BPF, а net/core/filter.o
ссылается на хелперы BPF безусловно — значит, в ядре снова net/core +
net/unix + netlink + sched + loopback + kernel/bpf (JIT выключен).
net/Kconfig у MiLin авторский, минимальный: протокольные семейства
(inet и прочее) остаются вырезанными. bzImage: 988 160 → 1 393 664
(+41%).

### M5.1 — Асинхронные очереди HelQueue ✅
Транспорт M5 честный, но медленный: каждое сообщение — два syscall'а
(sendmsg/recvmsg), ядро копирует данные в skb и обратно. У Managarm
queue'ы живут в разделяемой памяти (HelQueue): ядро вообще не участвует
в data path. Идея пересажена с kernel↔user на user↔user и смешана с
простым IPC:

- пара очередей в shm-регионе (ramfs-файл → MAP_SHARED → fd по
  SCM_RIGHTS → unlink: объект живёт, пока живы маппинги — без TMPFS и
  MEMFD_CREATE);
- чанки с progress-словом (24 бита длины + флаги DONE/FULL, как
  HelChunk) и length-prefixed элементами: потребитель читает payload
  ПРЯМО из общей памяти (zero copy);
- будильник — futex-слово с битами DATA/SUPPLY (аналог
  userNotify/kernelNotify): producer ставит бит атомарным OR и будит
  futex'ом только если бит не стоял; consumer снимает бит AND'ом,
  перепроверяет состояние и паркуется настоящим futex (аналог
  HelFutex). Протухший бит снимается при наблюдении пустого состояния —
  иначе пропущенный wake усыпил бы навсегда;
- flow control — свободный список чанков: consumer возвращает обработанные
  чанки producer'у (обмен цепочками через atomic exchange/CAS);
- creds: SO_PASSCRED/SCM_CREDENTIALS — ядро прикладывает struct ucred к
  каждому сообщению lane'а; шина (единственная точка, где доверенный
  uid известен) пересылает creds серверу в LaneRequest. Лог-записи
  подписываются uid=1000;
- шина переписана с последовательного accept-цикла на poll(): устранена
  живая блокировка M5 (LOOKUP-ретраи клиента не давали принять REGISTER
  сервера — гонка старта, всплывшая на M5.1).

Бенчмарк прямо в системе (QEMU TCG, rdtsc, n=256; числа — для
относительного сравнения): RTT seqpacket 297 678 циклов, очередь с
futex-парковкой 181 730 (×1.64), поллинг+yield 161 177 (×1.85),
постановка в очередь пачкой — 5 080 циклов/сообщение (в 58 раз дешевле
RTT seqpacket: асинхронная отправка не ждёт ответа). Цена контракта:
CONFIG_FUTEX (+RT_MUTEXES/FUTEX_PI/FUTEX_PRIVATE_HASH) = +8 КБ bzImage
(+0.6%).

### M6 — Отчёт ✅
Честное сравнение «до/после»: строки кода ядра, размер образа, поверхность
системных вызовов, время загрузки. Таблицы, графики, выводы — в `docs/`
(report-m6.md + charts/), метрики по каждому этапу — docs/metrics/m*.json.

### M7 — Capabilities и отказоустойчивость ✅
IPC M5/M5.1 честный, но безадресный в смысле прав: lane получал любой,
кто знал имя, и мог вызывать любые операции сервера. В M7 lane'и становятся
полноценными capability'ями:

- lane-fd — непогашаемый хэндл (kernel object): подделать нельзя, передать
  можно только через SCM_RIGHTS — классическая передача права, как в
  seL4/Fuchsia;
- сервер при REGISTER объявляет шине политику (tail: маски world/root);
  шина сверяет kernel-верифицированный uid клиента (SO_PASSCRED) и выдаёт
  lane только при непустой маске — адмиссионный контроль;
- маска прав доставляется ОБЕИМ сторонам: клиенту в LookupResponse (клиент
  знает, что ему разрешено), серверу в tail LaneRequest (сервер знает,
  что разрешить);
- сервер enforced'ит права на каждой операции: WRITE/WRITEQ →
  ML_CAP_WRITE, STATS → ML_CAP_STATS, CLOSE → ML_CAP_CLOSE, RESET
  (админ: сброс счётчика) → ML_CAP_ADMIN; нарушение — ответ ML_E_PERM;
- log-сервер работает с наименьшими привилегиями: setuid(1000) на старте —
  сервису не нужен root ни для чего.

Отказоустойчивость: супервизор больше не ждёт фиксированные pid в
фиксированном порядке —

- reaper-цикл wait4(-1) на ЛЮБОГО ребёнка: нежданная смерть сервиса
  (сигнал или код выхода без stopping-флага) — это сбой: лог, нарастающая
  пауза (backoff), перезапуск; лимит попыток → «система нездорова»;
- сервисы долгоживущие: uiod больше не выходит после демо, а ждёт
  остановки супервизором (SIGTERM), как положено серверу-драйверу;
- гонка «сервер убит, имя ещё в реестре» закрыта честно: LaneRequest
  уходит ПЕРЕД ответом клиенту, EPIPE на мёртвом conn превращается в
  NO_SUCH_ENTITY; MSG_NOSIGNAL обязателен — иначе SIGPIPE убил бы шину.

Демо-цикл (docs/boot-m7.log): клиент узнаёт pid log-сервера через
WHEREIS, убивает его SIGKILL'ом (разрешено — одинаковый uid), супервизор
перезапускает сервер («перезапуск #1»), клиент ретраями LOOKUP дожидается
нового экземпляра и продолжает работу на новом lane'е; RESET без
ML_CAP_ADMIN отклонён (ML_E_PERM, lane 0x7). Цена контракта: ноль байт
ядра — M7 целиком в userspace, bzImage не изменился (1 401 856).

### M7.1 — Отзыв capabilities при смерти клиента ✅
Модель M7 выдавала права, но не забирала их: мёртвый клиент с helq-lane
оставался владельцем действующего capability навсегда — в shm нет EOF,
сервер парился на futex мёртвой очереди, а права числились живыми.

- шина — владелец прав: журнал выданных capabilities (клиент-conn, pid,
  сервер-conn); EOF на конне клиента — шина шлёт серверу RevokeRequest;
- сервер отзывает права и на своей стороне: helq-очередь ждёт с
  сторожем 50мс (futex-таймаут), тишина — проверка sig0 клиента;
  ESRCH — capability REVOKED, lane закрыт (зомби жив для sig0, поэтому
  родня обязана реапнуть труп — иначе сторож не сработает);
- seqpacket-транспорт отзывает права сам: EOF на lane'е — сессия и
  права умирают вместе с ней, теперь это видно в логе;
- демо (docs/boot-m71.log): клиент-жертва берёт helq-lane и паркуется,
  родня убивает его SIGKILL'ом и реапает; шина ревокает, сервер
  отпускает мёртвый lane и обслуживает следующий — без ревока система
  бы заклинила, и следующий LOOKUP не прошёл бы.

### M7.2 — Каскадный перезапуск при смерти шины ✅
Шина — TCB: через неё идут все lane-рандеву и права. Но её смерть — не
конец системы: это микроядро, каждый зависимый — отдельный процесс со
своим state, и каждый сам встаёт в строй.

- супервизор перезапускает шину (reaper из M7: «перезапуск #1»);
- log-сервер видит EOF на конне к шине и сам перерегистрируется в
  новой (регистрация умирает вместе с шиной, а его state — его
  собственный: счётчик пережил и смерть сервера, и смерть шины);
- шина работает с наименьшими привилегиями: setuid(1000), /tmp отдан
  ей chown'ом (mkdir режется umask'ом) — и теперь клиент того же uid
  может убить её в демо, без root'а никто не нужен;
- клиент узнаёт pid шины из kernel-creds (SO_PASSCRED на своём конце:
  ядро прикладывает к ответам шины ЕЁ pid — подделать нельзя), убивает,
  ждёт ESRCH (SIGKILL не мгновенен: пока ядро не добьёт процесс, его
  слушатель ещё принимает конны), переподключается, дожидается
  перерегистрации сервера и продолжает работу;
- без CLOSE на helq-сессии сервер сидел бы в serve_queue_lane живого
  родителя и не увидел бы смерть шины — каскадный WRITE повис бы;
  чистое закрытие сессии (LOG_CLOSE) обязательно перед убиением шины.

Демо-цикл (docs/boot-m72.log): клиент убивает шину → супервизор
перезапускает mlbus → log-сервер перерегистрируется (счётчик пережил:
3) → клиент переподключается и пишет на новый lane → STATS served=4:
state пережил смерть и сервера, и шины.

### M8 — Раздельные пути IPC + glibc в userspace ✅
Две вещи, без которых микроядро нельзя называть работоспособным:
большие данные между процессами и обычные программы в userspace.

- раздельные пути в helq-очередях: мелкие сообщения (≤ 4КБ) едут
  inline в чанках (как раньше — быстрый путь), крупные (до 32КБ) —
  через отдельные blob-слоты региона: producer один раз копирует
  payload в слот, элемент в очереди несёт только ссылку, consumer
  читает прямо из общей памяти (zero copy) и возвращает слот через
  mlq_consume; mlq_put сам выбирает путь по размеру;
- blob-элемент публикуется немедленно (payload уже в слоте, батчить
  нечего) — иначе пачка крупных сообщений взаимоблокируется: слоты
  кончаются, а потребитель не видит ни одного элемента;
- контракт потоковости: недренированных ответов не больше 4 (столько
  же blob-слотов и чанков на направление) — конвейерный бенчмарк
  держит 32КБ-сообщения с 4 в полёте;
- hardening helq: мусорные len/blob от недобросовестного producer'а
  пропускаются, а не читаются (потребитель не доверяет данным);
- багфиксы: утечка fd при MSG_TRUNC/MSG_CTRUNC в ml_recv (доставленные
  права закрываются), утечка fd при переполнении poll-таблицы шины,
  дубликат REGISTER (ML_E_BUSY), непроверенные имена (ML_MAXNAME = 16
  с гарантией NUL), честные коды ошибок (ML_E_IO), детерминированный
  абстрактный адрес (ml_addr обнуляет sockaddr — раньше хвост был
  стековым мусором, совпадавшим по чистой случайности);
- glibc в userspace: протокол вынесен в mlproto.h (общий для nostdlib
  и glibc миров), /sbin/gclient — динамический бинарник (ld.so +
  libc.so.6 в initramfs): ELF-интерпретатор ядра, stdio, malloc,
  полный набор syscall'ов — и тот же IPC через шину (LOOKUP → WRITE →
  STATS). Ловушка: при SO_PASSCRED ядро прикладывает ДВА cmsg
  (creds + права) — буфер под контрольные данные обязан вмещать оба;
- бенчмарки M8 (QEMU TCG): helq-blob 32K rtt ~1.5M cycles/msg,
  конвейер 4-в-полёте ~1.8M; мелкие: helq-futex ~370K, seqpacket
  ~560K, enqueue 8.4K — большой путь дороже мелкого в 4 раза на
  сообщение, но несёт в 800 раз больше данных.

Демо-цикл (docs/boot-m8.log): WRITE 20480 байт через blob-слот (в
очереди — только ссылка), контрольная сумма STATS сошлась (933),
динамический gclient собрался, подключился к шине и подтвердил WRITE
(printf + malloc + SCM_RIGHTS под glibc) — ядро тянет обычный
userspace.

### M9 — kdriver-домен: драйверы «между ядром и userspace» ✅

UIO-путь (M4) честный, но медленный там, где драйверу нужен каждый регистр:
каждое обращение — syscall + переключение контекста. Полностью в ядро —
небезопасно, в userspace — медленно. Домен — третий вариант, по Tuduce &
Fetzer («driver isolation»): драйвер живёт в ядре (ring 0, прямой доступ к
MMIO/IRQ/DMA), но как ОТДЕЛЬНЫЙ процесс домена — kthread со своим pid,
стеком и циклом запросов, а не код в syscall-контексте чужого процесса.

- каждый драйвер домена — kthread с очередью запросов: userspace пишет
  запрос в /dev/<name> (write = вызов, read = ответ, до 4КБ в каждую
  сторону), домен обрабатывает в своём контексте;
- жизненный цикл как у процесса: probe в собственном kthread'е, при
  пробое — backoff и перезапуск по требованию (счётчик restarts в sysfs),
  недообработанные запросы при смерти домена получают -EIO, а не молча
  зависают;
- размещение — ПОЛИТИКА, а не архитектура: при регистрации AUTO ставит
  драйвер в домен; через /sys/class/mlkd/<name>/placement его можно
  отдать userspace-серверу ('user' — домен отклоняет запросы -ENODEV,
  дальше путь uiod/UIO) или вернуть ('kernel' — стартует новый kthread).
  Условный amdgpu в этой модели: драйвер регистрируется в домене, политика
  решает, где он живёт; UIO остаётся fallback'ом;
- sysfs: placement/state/requests/restarts на каждый драйвер — discovery
  и управление размещением без перезагрузки;
- kdemo: образец драйвера (echo, счётчик с state в kthread'е, инфо с
  pid'ом домена) и бут-тест: супервизор проверяет полный цикл включая
  переключение размещения в обе стороны.

Цена контракта: bzImage 1 401 856 → 1 405 952 (+4096 байт, +0.3%).
Демо-цикл (docs/boot-m9.log): kdemo стартует kthread'ом (pid 17), echo и
счётчик обслужены доменом, инфо показывает pid домена (обработка НЕ в
syscall-контексте), placement=user отклоняет запросы, placement=kernel
возвращает драйвер (новый kthread pid 23).

### M10 — amdgpu в обрезанном дереве ✅

M9 дал механизм размещения, проба (docs/drivers.md) доказала контракт на
pristine-дереве. M10 доводит до конца: GPU-стек восстанавливается в
ОБРЕЗАННОЕ дерево, ядро с amdgpu собирается и грузится.

- восстановлено из v7.2 целиком: drivers/gpu (DRM core + amdgpu + ttm +
  scheduler + display helper + vgaarb), drivers/dma-buf, drivers/i2c,
  drivers/power, drivers/hwmon, drivers/media. drivers/cec в v7.2 НЕ
  существует — CEC живёт в media/cec и вернулся вместе с ней;
- швы: drivers/Makefile (i2c/ media/, power/, hwmon/, gpu/, dma-buf/),
  drivers/Kconfig (5 source), drivers/video/Kconfig — шов на drivers/gpu/*
  вырезался ещё в M3 вместе с video-меню, возвращён;
- 11 980 файлов, 0 битых source-ссылок на 294 Kconfig; дефолтная диета
  не тронута — milin.config не изменился;
- включение — фрагмент configs/milin-kdrv.config {DRM, DRM_AMDGPU,
  FW_LOADER} поверх milin.config: build.sh KDRV=1. Замыкание Kconfig
  (~47 опций: TTM, scheduler, display helper, i2c, hwmon, power, VGA_ARB,
  SYNC_FILE, KCMP, INTERVAL_TREE, SHMEM, CRC16) разворачивается прямо
  в обрезанном дереве;
- результат: kdrv-bzImage 3 339 264 байта (+138% к diet 1 401 856 —
  цена документирована честно и живёт только в kdrv-сборке); 989
  объектов под drivers/gpu;
- бут-тест (docs/boot-m10.log): супервизор открывает /sys/module/amdgpu —
  amdgpu встроен в ядро (маркер M10 OK); в QEMU нет AMD-железа, драйвер
  ни на что не привязался (vgaarb: loaded), весь демо-цикл M5–M9 прошёл
  на kdrv-ядре. Runtime-смоук GPU — на реальном железе.

### M11 — mlkd-фасад amdgpu: один протокол, два размещения ✅

Смещённый запрос реализован: драйвер домена `drivers/mlin/amdgpu-fac.c`
(`/dev/amdgpu`) принимает от клиентов протокол **mlgpu** (INFO/ALLOC/FREE/
SUBMIT/WAIT — зеркала `userspace/mlgpu.h` и форк-заголовок). Режимы — лениво
на первом INFO: **facade** (diet-сборка, эмуляция), **amdgpu** (kdrv-сборка,
запросная плоскость перед DRM), **hw** (AMD-железо найдено — форвардинг).
Placement — та же политика M9: kernel — цикл в kthread'е домена; user —
демон `gpud` через lane `gpu.amdgpu` (ML_PROTO_GPU), тот же wire-формат,
уступание дороги домену (ML_E_PERM) при возврате. gpud переживает смерть
шины каскадным перезапуском. Демо: docs/boot-m11.log, маркер **M11 OK**.

### M12 — userspace-сеть: netd + socket-шима ✅

Сеть без единого сетевого сисколла ядру про TCP: `/sbin/netd` —
userspace-стек (e1000 поверх UIO: BAR0 в mmap, кольца/буферы в
pinned-DMA регионе `/dev/mlpinned` — резерва e820, форк M12) + ARP/IPv4/
ICMP/TCP + сервис `net0` (ML_PROTO_NET) на шине: NET_OPEN/CONNECT/SEND/
CLOSE, данные сервера — push'и NET_DATA/EOF владельцу сокета.

- `userspace/netsh.so` — LD_PRELOAD-шима для динамических glibc-бинарей:
  socket/connect/send/recv/read/write/poll/close уезжают lane'ом в netd,
  дескриптор — eventfd, reader-тред копит rx и дёргает событие;
- `netclient` — гейт: HTTP GET 10.0.2.2:8000 (slirp → host http.server)
  через полный путь IPC → netd → e1000 → slirp (маркер **M12 OK**);
- отладка по пути: sockaddr в host-order на проводе, sid из NET_OPEN_R,
  доверие IP total length (slirp паддит короткие кадры), идемпотентный
  connect (ретраи happy-eyeballs libfetch);
- цена: EVENTFD в ядре; boot-тест добавил e1000 + memmap-резерв + http-root.

### M13 — динамический XBPS: настоящий пакетный менеджер над IPC ✅

Финал директивы «glibc-полировка до динамического xbps с сетью»: в госте
работает настоящий XBPS 0.60.7, слинкованный с glibc, и ставит пакеты из
подписанного репозитория по HTTP через userspace-сеть.

- мир Void в initramfs (`tools/get-void-xbps.sh`): xbps + libxbps +
  libarchive + libcrypto3/libssl3 + zlib/zstd/lzma/lz4/bzip2/acl/attr +
  glibc 2.41 — .xbps-пакеты распаковываются прямо в rootfs, /lib64 →
  usr/lib, ld.so.cache от Void-ldconfig (в госте нет host-путей);
- подписанный репо (`tools/mkrepo.sh`): xbps-create + xbps-rindex
  (--sign репо, --sign-pkg пакет), ключ стабильный между сборками,
  plist доверенного ключа предоврен гостю в /var/db/xbps/keys — e2e
  репо валидируется установкой на хосте перед каждой сборкой;
- `/sbin/xbpstest` (гейт): xbps-install -Syy hello -y → реподата →
  .sig2 (RSA через libcrypto) → пакет → распаковка → /usr/bin/hello
  (маркер **M13 OK**);
- по пути: netd csum_fin не сворачивал перенос (сегменты с удачной
  арифметикой проходили, «несчастливые» отбрасывались slirp — ловилось
  только filter-dump'ом); netsh адресовал push'ы DATA/EOF индексом
  таблицы вместо sid'а netd (где sid==слоту — работало, иначе вечно
  молчаливый select в fetch_read).

### M14 — реальное железо и настоящий Void-репо ✅

Директива «XBPS стучится в реальные библиотеки приложений Void Linux,
система работает на реальном железе»:

- конфиг-контракт (configs/milin.config): ATA/AHCI (ata_piix+ich9),
  SCSI+sd, NVMe, virtio-blk, ext4, VT+VGA-консоль, AT-клавиатура,
  EFI-stub — ядро грузится BIOS-ом и UEFI-ом без serial-первенства;
- гибридный ISO (tools/mkimage.sh): El Torito BIOS + ESP UEFI,
  GRUB-ядра собираются grub-mkimage'ом (offline-тулчейн, без root);
- e2e-сеть: netd получил next-hop-маршрутизацию (шлюз вне /24),
  DNS-over-TCP резолвер в netsh, TLS 1.2-потолок для openssl гостя
  — xbpstest ставит dash из repo-default.voidlinux.org по https
  с их подписью и ca-certificates (маркер **M14 OK**);
- бут-гейты реального железа (tools/test-realhw.sh): virtio-blk /
  AHCI / NVMe — ext4 roundtrip mnttest'ом, VGA-консоль (дисплей без
  serial), маркеры **M14HW OK**.

### M15 — настоящая сеть и постоянный корень ✅

Одобрено: «DHCP в netd (UDP) для настоящей сети, r8169/e1000e,
USB-клавиатуры, switch_root на постоянный ext4».

- DHCP-клиент в netd (RFC 2131) поверх нового UDP-слоя (RFC 768,
  pseudo-header csum): DISCOVER/OFFER/REQUEST/ACK, broadcast-флаг,
  T1-renew unicast'ом, T2/lease-expiry распад, NAK-restart. Адресация
  больше не зашита: my_ip/mask/gw — переменные, ip_tx получил
  src-параметр (0.0.0.0 до бинда), limited broadcast мимо ARP.
  DNS-опция 6 перезаписывает /etc/resolv.conf — netsh/xbps читают
  его как раньше. Нет сервера — fallback slirp-статики через 24с,
  DHCP продолжает пробовать в фоне. Гейт: **DHCP OK**;
- NIC-слой netd: struct nic_ops {txf/txc/poll/init} + probe по PCI
  vendor/device из sysfs. e1000e (82574L) — карта регистров
  совместима, legacy-дескрипторы (DTYP=0), ASPM-эррата гасится через
  PCIe config space (LNKCTL walk). r8169 (RTL8168/8111) — C+ дескрипторы
  (Own=1 -> NIC, RingEnd, RxRES), регистры/биты из ванильного
  r8169_main.c 7.2.9; QEMU rtl8169 не моделирует — валидация на
  реальном железе. Гейт realhw nic-e1000e: DHCP на 82574L;
- switch_root на постоянный ext4: root= из /proc/cmdline, guard «/init
  есть только в initramfs». Пустой корень — seed: рекурсивная copytree
  с lstat (симлинки сохраняются, /lib64 не разворачивается), 17МБ мира
  + /init -> /sbin/init. Переезд: mount --move /dev /sys /proc,
  mount --move /newroot /, chroot, exec /sbin/init — новый PID 1 живёт
  на диске, xbps-установки переживают бут. e2e (tools/test-persist.sh):
  бут A сеет+ставит, бут B читает маркер с диска (**PERSIST OK**);
- USB-клавиатуры — обратная резня (форк mel0k1/linux @ 9f72159c):
  drivers/usb (usbcore + xHCI/EHCI/UHCI) + drivers/hid (usbhid,
  hid-generic) + fs/nls из v7.2. usbhid регистрирует клавиатуру в
  input core, VT-консоль подхватывает. Гейты realhw: qemu-xhci и
  piix3-usb-uhci с usb-kbd — **input: QEMU QEMU USB Keyboard**.

### M16 — современный x86: SMP, MSI, oneshot-таймеры ✅

Одобрено: «M16 «современный x86»: APIC + SMP + PCI_MSI + HPET — четыре
config-флипа и швы, гейты в QEMU (-smp 4 + DHCP на e1000e)».

- аудит аудитом, но факты оказались интереснее: X86_LOCAL_APIC,
  X86_IO_APIC, HPET_TIMER и X86_MPPARSE — def_bool y на x86_64 и
  были включены дефолтом с M1: ядро MiLin всё это время жило на
  LAPIC-таймере и IO-APIC, «PIC-картина» из M2 — иллюзия tinyconfig.
  Реально флипаются три: **CONFIG_SMP=y, CONFIG_NR_CPUS=8** (tinyconfig
  молча держит NR_CPUS=1 даже при SMP=y — olddefconfig его сохраняет),
  **CONFIG_PCI_MSI=y** (GENERIC_MSI_IRQ по select) и
  **CONFIG_HIGH_RES_TIMERS=y** (oneshot-режим clockevent — на PIT-less
  чипсетах это единственный таймер вообще). Швов не потребовалось —
  kernel/smp*, arch/x86/kernel/{smp,apic,hpet}.c резня не трогала;
  bzImage 2.0 -> 2.1 МиБ;
- открытие про перечисление CPU: на современных типах машин QEMU
  (SeaBIOS 1.16.3) кладёт в MPS-таблицу ТОЛЬКО boot-CPU — AP-перечисление
  переехало в ACPI MADT (которого у нас нет до M17), и SMP-гейт на
  pc-10.0 честно поднимает 1 CPU из 4. Гейты переведены на
  совместимые машины (pc-i440fx-2.5 / pc-q35-2.9): там MPS legacy-BIOS
  несёт все процессоры — **smp: Brought up 1 node, 4 CPUs**. Честная
  цена: -cpu max на этих машинах роняет загрузчик SeaBIOS ("requires
  an x86-64 CPU") — убран, энтропия держится на virtio-rng (M14);
- гейты: run-qemu.sh — BOOT OK + **SMP OK (4 CPUs)** (вся цепочка
  M11/M12/M14 + DHCP на 4-процессорной системе); test-realhw.sh — все
  7 гейтов с -smp 4 (virtio/ahci/nvme ext4-roundtrip, e1000e **DHCP
  OK**, usb-xhci/usb-uhci, vga); test-persist.sh — **PERSIST GATES OK**
  (switch_root/seed/установка переживают reboot при SMP). Логи:
  docs/boot-m16-smp.log, docs/boot-m16-realhw-e1000e-smp.log;
- что осталось от «современного x86»: AP-перечисление на настоящих
  машинах = ACPI MADT → обратная резня drivers/acpi — следующий этап
  (M17); x2APIC заблокирован зависимостью IRQ_REMAP (drivers/iommu
  вырезан) — возвращается вместе с ним.

### M17 — ACPI: MADT, _PRT, S5 ✅

Одобрено: «M17 «ACPI»: обратная резня drivers/acpi — MADT (многоядерность
на настоящих машинах), _PRT (маршрутизация прерываний PCIe),
poweroff/reboot».

- обратная резня оказалась почти чистым откатом: drivers/acpi (ACPICA +
  pci_root/_link/_irq + ec + sleep) и drivers/pnp (pnpacpi) вернулись из
  v7.2 как есть (форк 4d7252896), а швов — три строки: source в меню PM
  arch/x86/Kconfig (fix-dead-refs подчистил его в M3 — файла-то не было),
  source pnp в drivers/Kconfig и obj-$(CONFIG_ACPI/PNP) в drivers/
  Makefile. Все select'ы x86 (ARCH_SUPPORTS_ACPI,
  ACPI_SYSTEM_POWER_STATES_SUPPORT и пр.) уцелели с M1 условными
  («if ACPI») и заработали сами. C-швов ноль: сборка прошла с первого
  захода, bzImage 2.1 -> 2.4 МиБ;
- конфиг: CONFIG_ACPI=y + CONFIG_ACPI_MCFG=y (ECAM для q35: его никто
  не select'ит, а mmconfig-shared ссылается на pci_mcfg.o);
  ACPI_PROCESSOR/AC/BATTERY/BUTTON off — idle и P-состояния в
  микроядерном мире живут в userspace-серверах;
- закрытие открытия M16: на q35 (свежий тип машины) **Using ACPI (MADT)
  for SMP configuration information** и **smp: Brought up 1 node,
  4 CPUs** — там, где MPS несёт только boot-CPU. Многоядерность на
  современных системах больше не зависит от legacy-MPS;
- _PRT/PCI: **PCI: Using ACPI for IRQ routing** (в v7.2 строка переехала
  в arch/x86/pci/acpi.c), pnp: **PnP ACPI: found N devices** — проход
  namespace'а ACPICA; в живом буте виден и acpi_pci_link:
  **ACPI: \_SB_.GSIH: Enabled at IRQ 23** — линк выдан UIO-устройству;
- poweroff/reboot: sys_reboot дотянут до pm_power_off/machine_restart —
  гейты S5 (**reboot: Power down**, qemu завершается сам) и рестарта
  (**reboot: Restarting system** при -no-reboot); /sbin/poweroff и
  /sbin/reboot — один исходник, два бинаря (userspace/poweroff.c);
- бонусная находка: ACPI удлинил бут и вскрыл гонки, жившие с M7.2/M12 —
  netclient, gpu_demo и xbps-install не переживали смерть шины в
  каскадном демо. Укреплены ретраями: коннект к шине, полная HTTP-сессия
  с новым lane'ом, poll-потолок 5с на каждый net_rpc (висение oneshot'а
  на потерянном ответе теперь невозможно), xbps-install до 3 попыток;
- гейты: test-acpi.sh — **ACPI GATES OK (q35 -smp 4: MADT 4 CPUs +
  IRQ routing + chain M16)** + **POWEROFF OK (S5)** + **REBOOT OK**;
  регрессии зелёные: run-qemu.sh (BOOT + SMP OK), test-realhw.sh
  (**REALHW GATES OK**, 7 гейтов), test-persist.sh (**PERSIST GATES
  OK**). Логи: docs/boot-m17-acpi-q35-smp4.log,
  docs/boot-m17-poweroff.log, docs/boot-m17-reboot.log;
- осталось из «современного x86»: x2APIC — за IRQ_REMAP (drivers/iommu
  вырезан), IOMMU/DMA-remap — отдельным этапом.

### M19 — MiLini как устанавливаемая ОС ✅

Одобрено: «M19 «MiLini как устанавливаемая ОС»: инсталлятор, getty/login,
обновление realhw.md».

- getty/login: /sbin/getty — подсказка входа на /dev/console (реальное
  железо: tty0/fbcon или VGA text, гейты: ttyS0), setsid + TIOCSCTTY,
  сессия = fork -> /sbin/login; /sbin/login — /etc/passwd + /etc/shadow
  (**sha512-crypt**, userspace/crypt.h: SHA-512 + transpose-base64 без
  libc, верифицирован против openssl passwd -6 на 4 векторах), эхо
  пароля гасится TCSETS, затем setgid/setuid/chdir/umask и exec /bin/sh
  (-sh — login shell); аккаунты root/milin и user/user, /etc/shadow
  генерится mkinitramfs.sh хостовым openssl passwd -6;
- minstall: /sbin/minstall /dev/sdX (или cmdline install=) — mkfs.ext4 на
  диск целиком (syslinux 6.03 не читает 64bit/metadata_csum), копия мира
  (копир сохраняет режимы), /init -> /sbin/init, маркер
  /etc/milin/.installed, /boot с vmlinuz + мини-initramfs (12 КиБ,
  только /init — мир приезжает с диска), extlinux.conf
  (root= + rw + консольные параметры установочной сессии), extlinux
  --install — boot-сектор пишется через ФС (ext4 оставляет 1 КиБ под
  него). Цель — диск без таблицы партиций: живой BLKRRPART-рескан в
  ядре отдаёт устаревший кэш attach-скана (страница сектора 0 не
  сбрасывается) — таблица партиций кандидатом в M20;
- супервизор: установленная система (маркер .installed, /init
  отсутствует) не завершается после oneshot'ов — getty держит вход,
  выход только poweroff/reboot (S5/рестарт); live-сессия ведёт себя
  как раньше (exit после всех oneshot'ов); install= сеет сессию
  установки — сервисы + инсталлятор, без демо-oneshot'ов;
- мир: e2fsprogs + syslinux (Void, usr-merge: /usr/bin/mkfs.ext4,
  /usr/bin/extlinux) + libblkid/libuuid/util-linux-common;
  /sbin/poweroff и /sbin/reboot теперь в живой системе;
- гейт test-install.sh — **INSTALL GATES OK**: бут A (live, q35
  ich9-ahci): MINSTALL OK; бут B (без -kernel): SeaBIOS -> extlinux ->
  ядро + мини-initramfs -> switch_root -> login root (sha512-crypt) ->
  root shell -> login user (setuid 1000) -> user shell -> poweroff
  (**reboot: Power down**). Регрессии зелёные: run-qemu.sh (BOOT +
  SMP OK), test-realhw.sh (**REALHW GATES OK**, 10 гейтов),
  test-acpi.sh (POWEROFF + REBOOT OK), test-persist.sh (**PERSIST
  GATES OK**). Логи: docs/boot-m19-install.log,
  docs/boot-m19-login.log;
- осталось: UEFI-бут установленного диска (extlinux — BIOS-only,
  нужен GRUB EFI/gummiboot или EFISTUB-путь), таблица партиций
  (BLKRRPART-рескан), смена пароля в госте (passwd/shadow-утилиты),
  utmp/wtmp.

### M20 — таблица партиций + UEFI-бут установленного диска + passwd

Одобрено: «M20: таблица партиций (рескан), UEFI-бут установленного диска
(GRUB EFI/EFISTUB), passwd в госте». Закрыто целиком: партиции, UEFI-путь,
passwd.

- таблица партиций: minstall больше не занимает диск целиком — пишет GPT
  (защитный MBR + заголовок + записи + запасные копии, свой CRC32 без
  libc): p1 = ESP 64M (GUID C12A7328), p2 = корень (Linux root x86-64,
  discoverable GUID 4F68BCE3; legacy-bootable бит для gptmbr). Рескан —
  ioctl BLKRRPART: ядро пересканирует таблицу без ребута, ноды /dev/sda1
  и /dev/sda2 появляются в devtmpfs сами (M19-диагноз «устаревший кэш
  скана» не подтвердился — путь bdev_disk_changed sync+invalidate цел;
  реальной причиной был пробник). Чтение сектора 0 после рескана
  возвращает свежую таблицу (защитный MBR 0xEE);
- UEFI-бут: ядро с EFI stub уже с M14 — вместо GRUB на ESP кладётся само
  ядро как /EFI/BOOT/BOOTX64.EFI (fallback-путь, без efibootmgr). ESP —
  FAT16, писатель встроен в minstall (все имена 8.3, LFN и boot-код не
  нужны — прошивка читает FAT своим драйвером): BPB + две FAT + корень +
  EFI/BOOT, ядро пишется подряд линейной цепочкой. mtools отвергнуты:
  Void-glibc не несёт CP850.so (iconv_open «Invalid argument»), а
  дебиановский бинарь на девайсе требует геометрию и кодовые страницы
  хоста;
- EFISTUB без LoadOptions берёт cmdline из ядра: CONFIG_CMDLINE_BOOL
  флип с baked «root=PARTUUID=4d494c49-4d20-4d32-8000-000000000001 rw
  rootwait memmap=4M$0x8000000 console=tty0 console=ttyS0» — канонический
  PARTUUID корня зашит и в GPT (unique guid p2), и в конфиг: единая
  точка правды. rootwait ждёт скана AHCI; memmap — контракт
  /dev/mlpinned (M12). LoadOptions при !CMDLINE_OVERRIDE добавляются
  поверх built-in (x86-stub);
- ядро: прямой mount без initramfs упирался в отсутствие /root в rootfs
  — built-in initramfs (usr/) вырезан в M3, его default-cpio создавал
  /dev и /root; /dev уже чинит шов console_on_rootfs (M3), /root возвращает
  новый шов init/do_mounts.c (mount_block_root, init_mkdir) — коммит в
  форке linux@milin + воспроизводимый stitch в carve.sh;
- супервизор: cmdline_root берёт ПОСЛЕДНИЙ root= из /proc/cmdline —
  семантика ядра (bootloader-приложение перекрывает built-in); первый
  токен после M20 — всегда PARTUUID, который userspace-парсер не
  резолвит;
- passwd в госте: /sbin/passwd (setuid root — bprm_fill_uid жив в форке)
  меняет пароль в /etc/shadow: root — любой аккаунт без текущего пароля,
  остальные — свой, подтверждая текущий (sha512_verify из crypt.h, механика
  login с M19). Соль — 16 символов из /dev/urandom (ноду регистрирует
  mem.c, devtmpfs создаёт сам). Запись атомарная: shadow.tmp -> fsync ->
  fchmod 0600 -> rename; при любом сбое tmp снимается — shadow либо
  старый, либо полный новый. Хеш-поле «$6$salt$hash86» совместимо с
  openssl passwd -6 (вектор проверки — вход по новому паролю в гейте);
- гейт test-install.sh — четыре бута, **INSTALL GATES OK**: A (live):
  MINSTALL OK + BLKRRPART -> 0 + ноды в devtmpfs без ребута + gptmbr +
  ESP OK; B (BIOS): SeaBIOS -> gptmbr -> extlinux (sda2) -> switch_root
  -> login root/user -> poweroff S5; B2 (BIOS): passwd в госте — root
  меняет user без текущего пароля, вход user с новым паролем, self-смена
  через setuid с подтверждением текущего, вход с паролем после self-смены
  -> poweroff S5; C (UEFI): OVMF -> ESP -> BOOTX64.EFI
  -> прямой root-mount по PARTUUID («VFS: Mounted root (ext4 filesystem)
  on device 8:2») -> /sbin/init -> login -> poweroff S5. Регрессии:
  run-qemu (BOOT + SMP OK), test-acpi (POWEROFF + REBOOT OK),
  test-persist (PERSIST GATES OK), test-realhw 9/10 гейтов OK (полный
  повтор — за таймаутом тулчейна; usb-storage не менялся с M18). Логи:
  docs/boot-m20-install.log, docs/boot-m20-bios.log, docs/boot-m20-passwd.log,
  docs/boot-m20-uefi.log;
- осталось: utmp/wtmp, resize2fs на p2 для крупного мира, пользоватские
  PARTUUID (одна каноническая GUID на все установки).

### M21 — utmp/wtmp + resize2fs

Хвосты M20. Одобрено: «да, давай utmp/wtmp и resize2fs».

- utmp/wtmp: формат — glibc struct utmp x86_64 (384 байта, compat32:
  ut_session/ut_tv 32-битные), общий userspace/utmp.h для записи. login
  пишет USER_PROCESS, getty после смерти сессии (wait4) — DEAD_PROCESS;
  /var/run/utmp — слотовая база (запись той же линии заменяется на месте,
  до 16 слотов), /var/log/wtmp — только аппенд. Читалка /sbin/last печатает
  историю и маркер M21 WTMP OK n=<число входов>;
- два бага, найденных гейтом: (1) login писал utmp после setuid — записи
  user падали с EACCES на root-owned файлах; перенос до setgid/setuid —
  utmp пишет root, как и настоящий login(1); (2) poweroff/reboot уходили
  в sys_reboot без sync — хвост page cache терялся при S5 (на диске:
  размер файла закоммичен, данные — нули; записи прошлого бута пропали).
  Фикс — sync в poweroff.c + fsync на каждую запись utmp/wtmp;
- resize2fs: minstall делает mkfs.ext4 128M (мир 71M), копит мир и
  бут-пейлоад, umount — и resize2fs до края партиции: на больших дисках
  быстрый mkfs малого fs + resize выигрывает у полного mkfs; fs
  растягивается на весь p2 (RESIZE OK). Классический паттерн
  инсталляторов;
- гейт: INSTALL GATES OK — бут A получил чек RESIZE OK, бут B2 — чтение
  wtmp через /sbin/last (USER_PROCESS/DEAD_PROCESS обеих сессий: root и
  user). Проверка вскрытием образа: 15/15 записей ненулевые, история
  бутов B -> B2 -> C полная, utmp-слот корректен. Логи:
  docs/boot-m21-install.log, docs/boot-m21-last.log;
### M22 — пользоватские PARTUUID + mlkd-фасад e2e + fast/bulk IPC

Хвосты M21. Одобрено: «да, двигаемся дальше пользоватские PARTUUID,
mlkd-facade e2e и fast/bulk IPC».

- пользоватские PARTUUID: канонического GUID больше нет нигде. minstall
  генерирует disk/ESP/root GUID'ы из /dev/urandom (версия 4, вариант
  RFC 4122), самопроверка писателя — GUID корня читается назад с диска —
  и печат «minstall: PARTUUID <guid>» (строку берёт гейт). BIOS-путь:
  extlinux APPEND root=PARTUUID=<guid>. UEFI-путь: BOOTX64.EFI — наш
  EFI boot manager (userspace/efiboot.{c,S} + efiboot.ld; PE32+ 16 КБ
  без .reloc на фиксированном ImageBase, SysV→Win64 туннели
  efi_call1..6): читает с ESP CMDLINE (ascii-cmdline с per-install
  root=) и стартует VMLINUZ.EFI через LoadImage+LoadOptions+StartImage
  (LoadOptions перекрывают CONFIG_CMDLINE — ядро не пересобирается под
  установку). mini-initramfs: резолвер PARTUUID→узел в супервизоре
  (скан /dev, разбор GPT LBA1, имя партиции sda2/nvme0n1p2) — mount(2)
  имена PARTUUID не понимает. CONFIG_CMDLINE очищен от root=;
  - два бага, найденных гейтом: (1) в EFI_BOOT_SERVICES есть
    SignalEvent между WaitForEvent и CloseEvent — без него все слоты с
    12-го съезжали, HandleProtocol читался как
    UninstallProtocolInterface и отвечал NOT_FOUND; (2) efi_call6: у
    SysV fn — 7-й аргумент (RDI, как у всех), а 8-й (a6) — на стеке —
    перепутанные слоты прыгали по адресу &child, firmware exception
    handler молча зацикливался (LoadImage выглядел «зависшим» и по
    device path, и с SourceBuffer); LoadImage по device path на этом
    firmware так и не завёлся — ядро читается сами через
    SimpleFileSystem и грузится SourceBuffer'ом (DeviceHandle у child
    пустой, efi_load_initrd переживает отсутствие источников);
- mlkd-фасад amdgpu e2e: gpu_demo переехал из /init в /sbin/gputest
  (oneshot, order 92) — у фасада один владелец placement, гонок с демо
  шины нет. Фазы: kernel (/dev/amdgpu INFO→ALLOC→SUBMIT→WAIT→FREE в
  kthread'е домена), deny (placement=user → /dev ALLOC = -ENODEV),
  caps (fork+setuid(1000): LOOKUP "gpu.amdgpu" = ML_E_PERM — world=0),
  user (lane к gpud, mode=FACADE, полный цикл), return (placement=
  kernel → /dev ожил, gpud уступил ML_E_PERM). Вердикт M11/M22 — по
  статусу oneshot'а; шинные фазы переживают смерть шины ретраями
  (демо каскада M7.2);
- fast/bulk split IPC: userspace/bulk.h — арена bulk-слотов в
  разделяемой памяти (4×128 КБ, lock-free freelist на CAS,
  futex-будильник с таймаутом 50 мс — сосед по shm умирает невидимо).
  Транспорт ML_TR_BULK в шине: seqpacket-lane + арена (hooks[1], shm в
  /tmp, unlink после раздачи). LOG_WRITEB у log-сервера: h[0]=слот,
  h[1]=len, payload читается прямо из общей памяти (zero copy на
  receive), слот возвращается ДО ack (flow control, как в helq).
  Клиент: 8×128 КБ через арену, stored_len в ack сходится
  (M22 BULK OK), печат 64 байт паттерна в лог — байты реально прошли;
- ramfs statfs (попутно, ядро): simple_statfs отдавал нули в
  f_blocks/f_bfree — xbps из живого репо читает их как «0B free» и
  срывает транзакцию; ramfs_statfs честно отдаёт totalram/freeram;
- гейт: INSTALL GATES OK — бут A (уникальный GUID + ESP из трёх
  файлов: BOOTX64.EFI/VMLINUZ.EFI/CMDLINE), бут B (BIOS/extlinux
  root=PARTUUID + резолвер + GPU e2e + bulk в установленной системе),
  бут B2 (passwd/last), бут C (OVMF → наш лоадер → LoadOptions →
  прямой root-mount по PARTUUID → login → poweroff). Регрессии: BOOT
  OK + SMP OK, ACPI GATES OK (POWEROFF + REBOOT). Логи:
  docs/boot-m22-live.log, docs/boot-m22-install.log,
  docs/boot-m22-bios.log, docs/boot-m22-passwd.log,
  docs/boot-m22-uefi.log;
- пользовательский override PARTUUID (доводка): minstall принимает
  uuid_disk=/uuid_esp=/uuid_root= — GUID'ы задаёт оператор (переустановка
  с сохранением GUID, клонирование образов): парсер 8-4-4-4-12 строгий
  (тире на 8/13/18/23, ровно 32 hex-цифры, обратная guid_str — первые три
  поля little-endian), дубликаты между партициями запрещены, эхо
  round-trip parse->str. Супервизор выносит uuid_*= из /proc/cmdline и
  передаёт их в argv minstall'а (cmdline-токены не утекают в
  extlinux.conf/CMDLINE — фильтр APPEND). Гейт: бут A2 — переустановка
  с операторскими GUID'ами на тот же диск, случайный PARTUUID вытеснен,
  бут B грузится по операторскому root=PARTUUID (резолвер + APPEND
  проверены на заданных GUID'ах); STAGE-режим гейта для посегментного
  прогона (TCG-песочницы без KVM);
- осталось: подпись ESP-лоадера (SBAT/secure boot), boot-entry в
  NVRAM (efibootmgr-совместимость), bulk-путь для GPU-протокола
  (командные буферы больше ML_MAXTAIL) — закрыто бутами C1/C2/D/D2
  (см. docs/boot-m22-uefi-c*.log, docs/boot-m22-secureboot*.log) и
  SUBMITB-ареной gputest/gpud.

## Статус

| Этап | Название | Статус |
|------|----------|--------|
| M1 | Базовый бут | ✅ сделано |
| M2 | Конфиг-диета | ✅ сделано |
| M3 | Резня в исходниках | ✅ сделано |
| M4 | Userspace-изация | ✅ сделано |
| M5 | Микроядерный IPC | ✅ сделано |
| M5.1 | Асинхронные очереди HelQueue | ✅ сделано |
| M6 | Отчёт | ✅ сделано |
| M7 | Capabilities и отказоустойчивость | ✅ сделано |
| M7.1 | Отзыв capabilities при смерти клиента | ✅ сделано |
| M7.2 | Каскадный перезапуск при смерти шины | ✅ сделано |
| M8 | Раздельные пути IPC + glibc userspace | ✅ сделано |
| M9 | kdriver-домен | ✅ сделано |
| M10 | amdgpu в обрезанном дереве | ✅ сделано |
| M11 | mlkd-фасад amdgpu | ✅ сделано |
| M12 | userspace-сеть (netd + netsh) | ✅ сделано |
| M13 | динамический XBPS над IPC | ✅ сделано |
| M14 | реальное железо + настоящий Void-репо | ✅ сделано |
| M15 | DHCP, r8169/e1000e, USB-клавиатуры, switch_root | ✅ сделано |
| M16 | современный x86: SMP, PCI_MSI, HIGH_RES_TIMERS | ✅ сделано |
| M17 | ACPI: MADT, _PRT, S5 poweroff/reboot | ✅ сделано |
| M18 | видимая система: fbcon + EFI-fb, psmouse, usb-storage | ✅ сделано |
| M19 | MiLini как устанавливаемая ОС: инсталлятор, getty/login | ✅ сделано |
| M20 | таблица партиций (рескан) + UEFI-бут + passwd в госте | ✅ сделано |
| M21 | utmp/wtmp + resize2fs | ✅ сделано |
| M22 | пользоватские PARTUUID + mlkd-фасад e2e + fast/bulk IPC | ✅ сделано |

## Честные оговорки

Мы понимаем, что настоящий «Linux-микроядро» — это годы работы и тема целых
научных программ (L4Linux, K42, MINIX 3). MiLin — это не попытка немедленно
заменить Linux, а методичный эксперимент: превратить монолит в микроядро
поэтапно, с рабочей системой после каждого шага. Даже частичный успех —
уникальный инженерный результат.
