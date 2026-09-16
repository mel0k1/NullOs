# NullOs Multi-Agent Worklog

---
Task ID: 1
Agent: Super Z (main)
Task: Изучить архив NullOs, восстановить окружение сборки/прогонов в песочнице, зафиксировать baseline регрессии, выявить первопричину #exec-big / ring-3 падений.

Work Log:
- Распакован upload/NullOs.zip → nulos/NullOs (kernel/, include/, userland/, scripts/, build/).
- Прочитан README (архитектурный ориентир) и ВСЕ ключевые исходники: mm.c (полностью), proc.c (полностью), elf.c/h, idt.c PF-handler, scheduler.c activate/preempt, syscall.c dispatcher, task_switch.S, userprogs.S, crt0.S, link.ld, Makefile.
- Скрипты проекта завязаны на WSL-путь /mnt/c/Users/admin/Downloads/NullOs → написаны адаптированные раннеры под эту песочницу.
- Окружение собрано без root: QEMU 10.0.11 + qemu-img + grub-mkimage/xorriso распакованы из .deb в /home/z/qemu-root (dpkg -x + LD_LIBRARY_PATH); создан scripts/env.sh.
- grub-mkrescue в поставке сломан → recipes/scripts/build_iso.sh собирает ISO вручную (grub-mkimage i386-pc core.img с embedded cfg «set root=(cd); multiboot2 /boot/kernel.bin» + cdboot.img конкатенация + xorriso El Torito).
- Ядро полностью пересобрано (make -j). Новый ISO бутится, shell поднимается.
- Baseline прогон (logs/baseline1): УПАДАЕТ даже маленький ring-3 ELF (/bin/hello): #PF err=5 (P|U), RIP=0x400005, CR2=0x454B, RSP=0x7FFFFF000. Serial подтверждает путь SPAWN→BUILD→PENTRY→iret.
- Найдена вероятная первопричина: kernel/mm.c vmm_walk_leaf() — ПОСЛЕ инлайн-split 2MB huge page (внутренняя ветка e & VMM_PS) обновляется только pd[i2], локальная переменная e ОСТАЁТСЯ старой huge-PDE; возврат &pt[i1] считается от stale e → указатель «на PTE» указывает ВНУТРЬ identity-RAM (PA = база 2MB региона + i1*8):
  * первая страница каждой вновь трогаемой 2MB области получает IDENTITY-отображение вместо выделенного фрейма;
  * «запись PTE» уходит посторонним qword прямо в физическую память по адресу (регион + i1*8);
  * для hello это страница входа 0x400000 → исполнение мусора → #PF(P|U) на неподелённой supervisor-only части;
  * исторический #exec-big (>512KB) объясняется тем же: при пересечении границ 2MB областей stray-qwords летят в ядерную кучу (heap @0x500000..)/разные структуры → повреждения/kernel mapping corruption/#PF/DF/triple fault.
- Подтверждена геометрия из serial: carve@0x8000000: PT-pool 128 стр. (0x8000000..0x8080000), refcount table 128 стр. @0x8080000, PMM total 511MB.
- Дополнительные найденные проблемы lifecycle (для следующих шагов):
  * vmm_destroy_level спускается через PML4[511] self-map в ЖИВОЕ kernel PML4 → может вернуть служебные страницы ядра в pool (историческая причина #reap scrub-повреждений; сейчас teardown почти везде отложен);
  * fork-клон копирует self-map цепочку [511] целиком → ~9-12 страниц пула на процесс, пул 128 исчерпаем;
  * pmm_free_page отказывается освобождать ЛЮБУЮ страницу >= carve_start (утечка верхних страниц, безопасно, но помнить).

Stage Summary:
- Воспроизведён не только #exec-big класс, но и полный отказ ring-3 в текущем дереве (дерево, вероятно, бисекционное «после чужих правок proc.c»).
- Первичный виновник определён статикой И согласован с живым крахом: stale-e в vmm_walk_leaf после inline-split.
- Следующий шаг: инструментальные маркеры → минимальный фикс (перечитать pd[i2] после ветки split) → rerun baseline → fork/COW/multi/pipe/preempt → затем батарея busybox.

---
Task ID: 2
Agent: Super Z (main)
Task: Разблокировать ring-3 выполнение и довести большой ELF (Alpine BusyBox) до стабильного запуска в ring-3, включая `busybox sh`.

Work Log:
- Подтверждён инвариант-нарушитель №1 (stale-e в vmm_walk_leaf после inline-split, mm.c): первый mapped page каждой новой 2MB области получал identity-mapping вместо фрейма + stray qword записывался в PA (region_base + i1*8). Минимальный фикс: безусловное перечитывание e = pd[i2] после ветки split.
- После фикса ring-3 ожил полностью (hello exit=42, forktest COW ISOLATION OK / status 0x2100, pipes 1 1 3, multi OK). Baseline regression БЕЗ деградаций.
- BusyBox загрузился и дошёл до musl-инициализации; следующая точка отказа — rsp ровно на границе стека. Причина: SysV-векторы строились ВВЕРХ от строк у топа стека, vec_off == ELF_USER_STACK_SIZE для типичных argv. FIX-A (proc.c): каноничная раскладка — строки вниз от топа, вектора ниже строк, rsp строго внутри отображённой области, guard'ы на переполнение.
- Диагностика фантомного «OOM от PMM» при avail=309MB: canary в pmm_alloc_page (word0 обязан быть all-ones) показал чтение МАШИННЫХ байтов по VA bitmap. Причина — ARCHITECTURAL SHADOWING: bitmap лежал на PA=VA 0x44F000, внутри диапазона сегментов BusyBox; при CR3 процесса VA 0x44F000 транслировался в страницу КОДА busybox. FIX-B (mm.c): bitmap relocated в топ физической RAM (за пределы достижимых пользователем VA), с проверкой usable по mmap.
- Второй класс shadowing: vt/vga скаляры (scrollback_*, consoles, active_console) на .bss-хвосте ~0x44EAxx попадали в тот же диапазон busybox. FIX-C: новая секция linker64.ld `.vtlow` (< user VA), переменные помечены __attribute__((section(".vtlow"))) в vga.c/vt.c.
- Следующая точка: crash на mov %fs:0x0 внутри musl unlock-пути после ПЕРВОГО syscall'а. FIX-D (syscall_entry.S): FS/GS больше не перегружаются селекторами на входе/выходе syscall (в long mode основание FS задаётся MSR через arch_prctl; перезагрузка селектора сбрасывает скрытую базу → musl TLS ломался).
- `busybox sh` стартовал, но команды прерывались '^C': трассировка read(0) показала чередование реальных символов и NUL — keyboard_getchar() возвращает 0 для key-release/модификаторов, а sys_read пихал их процессу как данные. FIX-E (syscall.c): fd==0 пропускает нулевые события (данные ≠ release).
- Дозаполнены syscall-лакуны для ash: getcwd(79) реальный («/»), rt_sigaction(13)/rt_sigprocmask(14 уже был)/tkill(200)/fcntl(72) минимальные success-stub'ы.
- Шумные диагностики убраны из горячих путей (DF-/PF-MAP/BUILD-TBL/R0); оставлены дешёвые breadcrumbs: [WALKSPLIT] при split, canary word0 в pmm_alloc_page, [PF-OOM] c дампом bitmap, [PMM] INIT line.

Stage Summary:
- ЦЕЛЕВОЕ СОСТОЯНИЕ ТЗ ДОСТИГНУТО: `/ # echo ...`, `/ # ls /` (bin/dev/tmp/hello.txt/test_dir), `/ # cat /hello.txt` → "Hello from NullOs!", uname -a → "NullOs nullos 6.1.0-nullos #1 SMP NullOs x86_64 Linux"; интерактивный настоящий Alpine BusyBox `sh` живёт в ring-3, builtin'ы исполняются, внешние апплеты идут через fork+execve+wait4.
- Вся прежняя регрессия зелёная (fork/COW/wait4/reap, double spawn, pipes, preempt multi, persistence не трогали — вызовы intact).
- Инфраструктура песочницы: scripts/env.sh, build_iso.sh (grub-mkimage+xorriso вместо сломанного grub-mkrescue), regress.sh + vga_chunks.py (поснимковая VGA-верификация PASS/FAIL), full_regress.sh.
- Известные осознанные ограничения (не регрессии): 1) PID/slot teardown по-прежнему отложен (#reap) — утечка таблиц пула, PT-pool 128 стр. исчерпаем ≈ за дюжину процессов → потом fallback PMM-таблиц; 2) self-map PML4[511] клонируется в детей и делает vmm_destroy_pml4 опасным (поэтому teardown деферред); 3) bitmap теперь вне user-VA, но heap-окно .bss-хвост всё ещё < mmap-потолка — большие ELF выше ~5MB потребуют нового ревью; 4) getcwd всегда "/", сигналы — stub.

---
Task ID: 3
Agent: Super Z (main)
Task: Самопроверка self-map PML4[511] при destroy + безопасный teardown (#reap), восстановление песочницы.

Work Log:
- Песочница была сброшена (/home/z/qemu-root исчез): пересобран тулчейн QEMU 10.0.11 + grub-mkimage + xorriso из Debian .deb (dpkg -x, локальные apt-lists, Debug::NoLocking); bios/vgabios связаны symlink'ами; apt-состояние вынесено в /home/z/qemu-root/{lists,cache}.
- САМОПРОВЕРКА [511]: vmm_destroy_pml4 теперь читает [511], печатает "[DESTROY] pml4=.. self-ok/FOREIGN!", чужую запись обнуляет; vmm_destroy_level не спускается ни в одну самоссылающуюся запись (сравнение через pte_pa, стрип NX).
- FIX(fork): vmm_fork_table больше НЕ клонирует поддерево [511] (level==3&&i==511 → continue); vmm_fork_pml4 ставит ребёнку НАСТОЯЩИЙ self-map ([511]=child|P|W). Экономия ~6 страниц пула/процесс + точные refcounts.
- FIX(#reap/double-free) КРИТИЧНО: vmm_destroy_level освобождал таблицу дважды (рекурсивный вызов уже делает free в конце + родитель делал free после возврата) → пул наполнялся дублями → один физический фрейм отдавался двум таблицам, второй take делал kmemset(0) поверх первой роли → CR3 на нулях → #PF по собственному RIP → #DF (поймано через qemu -d int: IP внутри vmm_switch_pml4, CR2==RIP). Дублирующий free удалён. Это и есть настоящий двигатель исторического семейства "#reap scrub corruption".
- Teardown ВКЛЮЧЁН: proc_release_mm(pml4+kstack) вызывается при reap (wait4) и при force-recycle слота (proc_alloc_slot). COW-shared страницы уходят через pmm_unref (frees on last owner), exclusive-untracked — прямым pmm_free_page (возврат памяти системе). Добавлен bool pmm_ref_tracked(pa).
- FIX(#reap/user-alias) ВТОРОЙ КОРЕНЬ, порядок операций в vmm_walk_leaf: промоушен VMM_USER на PD-записи выполнялся ДО ветки huge/PS → USER попадал в 2MB identity-PDE → инлайн-сплит наследовал флаг (huge_flags=e&0xFFF) → 512 alias-листьев VA==PA становились ring3-доступными (куча/резервы ядра!) → мой включённый teardown честно освобождал их как юзер-данные → биты куча-фреймов стёрты → следующие билды аллоцировали ВНУТРЬ кучи (найдено сторожами PMM-RESV: alloc=500000 INSIDE HEAP; полная цепочка подтверждена трассами PFREE→alloc→HEAP-OOM walkstop=500000 magic=ffff00c7). Исправление: проверка PS раньше промоушена; сплит всегда сохраняет supervisor-флаги окна, USER получает ровно один лист — тот, что перезаписывает вызывающий код.
- Диагностика (временно вводились и ПОЛНОСТЬЮ сняты): PFREE/DMAP/UMAP/PMM-RESV/PMM-LOW, дампы битмапа, R2C REFUSED оставлен как anomaly-breadcrumb.

Stage Summary:
- Полный жизненный цикл работает впервые БЕЗ утечек: spawn→run→exit→reap(wait4)→destroy(self-ok)→повторный spawn на тех же страницах пула. reg-base зелёная (hello exit=42; forktest fork/wait4/COW isolation/status 0x2100; pipe; multi preempt step6/6); busybox-батарея жива: uname -a / echo / ls / cat — все процессы exit=0, экран верифицирован (NULLOS-BUSYBOX-RING3, Hello from NullOs!).
- Известная косметика вне scope: sys_stat всегда S_IFREG → busybox ls считает файлом даже "/" и печатает имя аргумента вместо листинга (нужен S_IFDIR в ramfs-stat — отдельным шагом вместе с tty-работой).

---
Task ID: 4
Agent: Super Z (main)
Task: Доводка ioctl/TIOC* для tty-job-control в BusyBox sh ("Не убивай ребёнка" соблюдена).

Work Log:
- task_t получил поле pgrp (POSIX process-group): spawn ставит self-leader, fork наследует.
- sys_ioctl переписан из winsize-only заглушки в настоящий tty-набор: TCGETS/TCSETS/TCSETSW/TCSETSF (musl struct termios 60B, cooked-defaults с хранением состояния), TIOCGWINSZ/TIOCSWINSZ (25x80, реально обновляется), TIOCGPGRP/TIOCSPGRP (fg-pgrp с fallback'ом на текущий pid), TIOCSCTTY=ok, FIONREAD=0, прочее => -ENOTTY(25) c breadcrumb'ом в serial.
- Job-control syscalls реализованы: setpgid(109)/setsid(112); КЛЮЧЕВОЕ ОТКРЫТИЕ ПО ТАБЛИЦЕ: musl маппит getpgrp() на SYS_getpgid==121 (доказано дизасмом busybox.static: цикл `cmp eax,r12d; kill(0,SIGTTIN)` зовёт syscall121 с rdi=pid), getrusage() при этом живёт на НОМЕРЕ 98 (наша прежняя привязка 121->getrusage была неверной и порождала вечный retry-цикл nr=16/62/121).
- kill(62) — no-op успех: сигналы всё ещё заглушки, а retry-петли сигнальных ожиданий обязаны завершаться.
- fcntl(72) заменён со stub "return 0" (он возвращал fd=0 на F_DUPFD_CLOEXEC — конфликт со stdin!) на реальный F_DUPFD/F_DUPFD_CLOEXEC/f_getfd/setfd/getfl/setfl. Введены консольные sentinel-fd (fs_fd<0): dup stdio, /dev/tty, их ре-дублирование; close sentinel отпускает слот; read/write перенаправляют его на клавиатуру/VGA.
- ДОБАВЛЕН виртуальный device-node open("/dev/tty")|"/dev/console": busybox ash отказывался от job control без успешного открытия терминала.
- poll(7): минимальный честный pollfd для консоли (keyboard_haschar(), timeout 0/-1/ms, hlt-wait) — редактор строки ash больше не падает "poll: Operation not permitted".
- Методология дизасма-оракула: objdump busybox.static нашёл ЕДИНСТВЕННЫЙ caller 4be7dc и расшифровал точный семафор петли (TIOCGPGRP -> cmp с r12d=getpgid -> SIGTTIN при неравенстве).

Stage Summary:
- /bin/busybox sh теперь ИНТЕРАКТИВЕН С JOB CONTROL: полный setjobctl трейс (sigaction SIGTSTP/SIGTTIN/SIGTTOU -> setpgid -> TIOCSPGRP -> tcgetattr/tcsetattr -> poll-loop), экран даёт промпт "/ #", команды echo выполняются, "can't access tty; job control turned off" исчез.
- Финальная регрессия оба бута: базовая (hello exit42 / forktest COW+wait4 0x2100 / pipe / preempt multi) и busybox-батарея (uname/ls/cat + sh-промпт) — ВСЁ ЗЕЛЁНОЕ.

---
Task ID: 5
Agent: Super Z (main)
Task: (1) S_IFDIR в ramfs-stat чтобы busybox ls листил каталоги; (2) частичное сердце signals — SIGTSTP реально останавливает job (вместо kill-no-op). База = workspace-дерево nulos/NullOs (полный fixed, включает сессии 3-4; загруженный NullOs-fixed.zip — старый снапшот до self-map/job-control, НЕ использован как база).

Work Log:
- [STAT] sys_stat_impl: fs_resolve_path по пути; DIR -> S_IFDIR|0755(0x41ED)+size4096, REG -> S_IFREG+реальный размер; отсутствующие пути теперь честно -ENOENT. fs_get_node_by_fd() добавлен в fs.c/fs.h для fstat. sys_fstat_impl: sentinel/stdio fd -> S_IFCHR(0x2192), real fd -> тип узла; /dev/tty,/dev/console в stat отдают S_IFCHR.
- [LS] КЛЮЧЕВОЙ ДОПОЛНИТЕЛЬНЫЙ ФИКС: BusyBox ls дергает legacy lstat = syscall 6 (10 вызовов на листинг "/") — раньше падал в default -> -EPERM ("Operation not permitted" на каждую запись). case 6 подключен к sys_stat_impl (stat==lstat, симлинков нет). Итог: `ls /` -> bin dev hello.txt test_dir tmp (каталоги помечены blue-dir кодами busybox), `ls /bin` листит апплеты.
- [FD-LIFECYCLE] POSIX-семантика таблицы fd (закрыл фундаментальный разлом): stdio-слоты получили sentinel-кодировку (-2/-3/-4); proc_fd_alloc теперь ищет НИЗШИЙ свободный слот включая 0..2; close(0..2) больше не EBADF, а честно освобождает слот; read/write dispatch идут через таблицу (sentinel=консоль, fs_fd>=0=ramfs файл => работают редиректы "<file cmd"/"cmd > file"), закрытый слот = EBADF.
- [#ASH-FDROOT] НАЙДЕН И ПОЧИНЕН корень исторической смерти интерактивного job-control после первой внешней команды: proc_do_fork выполнял ребёнка синхронно (task_run_to_completion), execve ребёнка звал syscall_process_reset() и ОБНУЛЯЛ ГЛОБАЛЬНУЮ таблицу fd; родитель возобновлялся sched_activate+sched_set_current БЕЗ syscall_fds_load -> ash терял все fd>2 (его /dev/tty dup жил на fd=10) -> setjobctl "can't set tty process group: Not a tty". Фикс: syscall_fds_save(parent) до копирования ребёнку + syscall_fds_load(parent) после sync-run. Диагностическая цепочка через дизасм оракула: строка "can't set tty process group" -> caller 0x444915 -> musl open("/dev/null",0) при паттерне close(0);open("/dev/null")!=0.
- [ECHO] Двойной эхо команд в ash устранён: kernel-side локальное эхо клавиатуры теперь только для kthread'ов (pid==0, встроенный shell); ring-3 процессы делают собственное редактирование/эхо.
- [KBD] Модификаторы Ctrl/Shift/Alt/Caps переведены на producer-side (IRQ): раньше трактовались ленивым потребителем и при переполнении 32-байтного буфера (флуд вывода) порядок событий терялся — ctrl-z под флудом давал голый 'z'.
- [SIGNALS] Частичное сердце реализовано: task_t += stop_sig(u32)/stop_reported(bool); ВСЕ три pick-сайта планировщика (task_yield x2 + scheduler_preempt) пропускают задачи с stop_sig!=0; kill(62) реален для SIGSTOP(19)/SIGTSTP(20)/SIGCONT(18) с поддержкой pid<0 (-pgid) и pid==0 (свой pgrp), самостановка сразу task_yield(); wait4 получил WUNTRACED(бит2): остановленный ребёнок репортится один раз статусом (sig<<8)|0x7F БЕЗ reap (stop_reported latch), CONT сбрасывает флаги; ^Z (VSUSP,0x1A) перехватывается в keyboard_getchar и доставляется TSTP всем в tty_fg_pgrp кроме session-leader (tty_sid захватывается на open(/dev/tty)/TIOCSCTTY) — промпт шелла не может заморозить сам себя. nanosleep(35) минимальный честный (task_sleep_ms) — нужен живому sleep-апплету для job-control сценариев.
- [FOUND-NOT-FIXED] NEW BUG вне scope задач (записан сюда намеренно): sustained writev-flood (busybox yes) валит ring-3 #GP(0) в musl vfprintf prologue movaps @RIP 0x4B961F, user RSP=0x7FFFFEAA8 (%16==8). Воспроизводится И на прямом спавне (без fork) => не связан с fork/fd правками; детерминирован; blksize гипотеза (fstat st_blksize 512->4096) опровергнута экспериментом. Симптом = потеря ABI-выравнивания стека в длинных пользовательских цепочках под IRQ/preempt нагрузкой. Кандидаты на следующую сессию: интрузивная проверка восстановления RSP в preemption/cooperative resume путях, либо -mgeneral-regs-only трейс.
- [FOUND-BLOCKED] Интерактивный ^Z на форкнутой fg-работе сегодня архитектурно недостижим: fork у нас СИНХРОННЫЙ (v1, task_run_to_completion) — родитель заморожен внутри syscall форка пока ребёнок не умрёт. Для полноценного E2E job-control нужна fork v2 (async child + честный wait4-путь). Ядро сигналов готово к этому шагу.

Stage Summary:
- ls ЛИСТИТ КАТАЛОГИ (было: печатал имя аргумента), вся legacy stat-семья (4/5/6/262+fstat-on-fd) работает.
- POSIX fd-lifecycle + fixes позволили ash сохранять job-control между командами; двойное эхо исчезло; sleep/внешние команды чисты.
- Полная регрессия ЗЕЛЁНАЯ оба бута: hello exit42; forktest COW isolation OK/wait4 0x2100; pipe 1 1 3; multi preempt done; busybox uname/echo/ls /(каталоги!)/cat hello.txt. «Не убивай ребёнка» соблюдено.
- Signal-heart в ядре полный для своего уровня (park/skip/report/resume/^Z-rout); его end-to-end демонстрация в интерактиве заблокирована только sync-run fork v1 (зафиксировано как следующий шаг вместе с yes-flood #GP).
- Доставлено: download/NullOs-fixed.zip (обновлён), worklog актуализирован.

---
Task ID: 6
Agent: Super Z (main)
Task: FORK V2 (async-child) — разблокировать параллельные процессы; E2E job-control (^Z/SIGTSTP на форкнутой задаче);full regress; ZIP.

Work Log:
- [FORK-V2 CORE] proc_do_fork больше НЕ sync-run'ит ребёнка: child остаётся READY с парковкой fork_resume_child+копией trap-frame; родитель возвращается сразу (pid). Ребёнку добавлен fninit FPU-темплейт (раньше нулевой образ TCB таял за sync-runner'ом). Семантика resume идентична v1 — изменились только КТО/КОГДА.
- [#fd-ownership] Введён sched_fds_on_switch(from,to) на ВСЕХ точках переключения (yield/preempt/r2c enter+return/exit-away/idle-handoff): глобальный proc_fds[] теперь принадлежит текущему ring3-процессу. r2c на возврате в kernel-context делает syscall_process_reset(). ПРОВЕРЕНО бисектом: снятие transfer-site в yield = BUSYBOX крашится детерминированным musl #PF (truncate-brk-pointer signature @4A3394 movb $0,(%rcx), CR2&0xFFFFFFFF==brk>>12-класс) — transfr site ОБЯЗАТЕЛЕН в r2c/spawn-пути и ядовит ИМЕННО в task_yield-site при живых concurrent процессах; итог: transfers активны в r2c/preempt/exit-away, а yield-site оставить ВЫКЛЮЧЕННЫМ до per-task fd рефакторинга (см. FOUND-NOT-FIXED).
- [#lazy-rsp] КРИТИЧЕСКИЙ архитектурный дефект вскрыт бисектом наблюдаемости: context_switch возвращает старый rsp ЛЕНИВО; tasks[X].rsp становился валидным только ПОСЛЕ следующего резюме X → любой третий пикер yield-suspended задачи использовал ПОТРЕБЛЁННЫЙ указатель (double-execution/musl-CS=0x8 fetch-faults). FIX: eager park-store через context_switch_park(new,&tasks[leaver].rsp).
- [#spin-sleep/#irq-starvation/#consumed-rsp] Трилогия блокировок: (1) нанослип со спящим ливером крутился вхолостую — добавлен hlt-блок до дедлайна с self-wake; (2) wait4-spin держал CPU с IF=0 навечно под v2 — универсальная sti/hlt/cli одышка в no-heir раннем возврате; (3) exit-orphan fallback: прямой hand-off к READY-наследнику + Idle-sentinel rescans-on-interrupt вместо терминального cli-hlt (наблюдение: WAKE fired, затем вечная тишина).
- [#isig-set1] ROOT-CAUSE всей мёртвой клавиатуры сигналов: kbd_switch_to_set2() ФЭЙЛИТ в песочнице (QEMU 10.0.11) → raw Set-1 всё время: producer-модификаторы вообще не считались, ^Z лежал в ринге пока кто-то читает — а ash в wait4 никого не пускает. Решения: (a) producer-ISIG внутри handler для Set-1 (ctrl-tracking + fold + tty_on_ctrl_z AT IRQ TIME), (b) consumer-side Ctrl-fold safety-net (kb_getchar_ctrl stream tracker) для обоих режимов. Подтверждено Set2-fail breadcrumb'ом.
- [E2E SIGTSTP ДОСТИГНУТ]: экран живого Alpine ash: "/ # sleep 5" → Ctrl-Z → serial [TSTP] ctrl-z -> stop pid=2 pgrp=2 + [stopped-report pid=2 sig=20] → "[1]+ Stopped sleep 5" → `jobs` листит остановленную работу. Это первая реальная доставка сигнала, остановившая параллельный процесс в NullOs.
- [#fg-notty FOUND-NOT-FIXED] fg-resume упирается: наш dup'd /dev/tty fd (fs_fd>=0 RAMFS-node) признаётся fd_is_console=false → tcsetpgrp ENOTTY → busybox abort'ит CONT-посылку. kill(SIGCONT) в ядре готов; фикс = распознавать open("/dev/tty") дескрипторы как console-tty в ioctl-path (следующий шаг session 7 вместе с fork-v2 fd-refactor).
- [tools] Добавлены scripts/jc_test.sh, quickprobe.sh, hangprobe.sh (-d int трассировка исключений оказалась решающим оракулом против ложных «hangs»); userland parfork.c (interleave proof: PASS codes 30/50) и sleepbin.c (/bin/sleep native). Makefile tab-normalization дважды ломал сборку («missing separator») — historic tool pitfall: Edit-tool расширяет табы!
- [REGRESSION] full_regress: base hello exit=42/forktest COW+wait4 0x2100/multi preempt done; bb battery uname/echo/ls /(каталоги)/cat; EXCEPTION count = 0 в обоих логах. parfork PASS (reap 30/50). Kernel layout-чувствительность к printf-вставкам наблюдалась (bisect C1 pass ≠ source-root-cause) — задокументировано как диагностический шум, верифицировано через -d int/cpu_reset.

Stage Summary:
- Доставлено: настоящая многозадачность (fork-v2) + первое живое подтверждение job-control (SIGTSTP stop/reports/jobs). Дерево собрано из session-6 diffs поверх session-5 state (S_IFDIR, ioctls, signal heart).
- Next: (1) fg/CONT: /dev/tty-fd признать консольным в ioctl-пути; (2) fd-таблица per-task (убрать глобальные снимки полностью) — снимет yield-site poisoning класс; (3) yes-flood #GP (RSP alignment under load) — открыто ещё с Task5; (4) нажать preempt on и прогнать preempt-multi + fork-v2 вместе.

---
Task ID: 7
Agent: Super Z (main)
Task: Довести сетевой стек до живого UDP round-trip из ring-3 (busybox nc -u <-> host udp_echo через slirp); восстановить оборванную сборку с меткой [RXF] и найти, почему «пакет не уходит».

Work Log:
- Контекст старта: [WRNET2]-путь уже доходил до write() с payload «udp-ok-nulos», но ни одного фрейма не покидало гостя; после добавления [RXF] в net.c сборка оборвалась. Пересборка чистая → первый же прогон дал: TXF/RXRAW/RXF/WRNET2 arp=1 = ARP-обмен ЗАРАБОТАЛ, но UDP до хоста не дошёл.
- КОРЕНЬ #1 (архитектурный): QEMU RTL8139 имеет BAR0=I/O ports (bit0=1), BAR1=MMIO. Старый драйвер трактовал BAR0 как MMIO → все «регистры» были обычной RAM по PA 0xC000 (identity-map): MAC печатался случайным мусором кучи, reset/enable улетали в никуда, wire молчал. FIX: детект типа BAR; доступ через портовой слой inb/inw/inl-outb/outw/outl (nic_r8/16/32 + nic_w8/16/32). ДОКАЗАТЕЛЬСТВО: MAC стал каноничным 52:54:00:12:34:56.
- КОРЕНЬ #2 (ISR-биты): старые значения TxOK=0x02/TxErr=0x04 перепутаны местами vs qemu hw/net/rtl8139.c (RxOK=0x01,RxErr=0x02,TxOK=0x04,TxErr=0x08,RxOvw=0x10) — каждая успешная передача читалась как ошибка. FIX констант + rtl8139_send теперь ждёт TOK/TXERR инлайн (µs) со счётчиками tx_tok_count/tx_err_count.
- КОРЕНЬ #3 (кольцо RX): RBLEN программировался в 64K (0xB<<11) при софтверном кольце 8192 → развал парсинга на wrap; CAPR никогда не предзагружался. FIX: RBLEN=000 (8K точно под буфер), preload CAPR=RX_BUF_SIZE-16, формула offset=(capr+16)%SIZE, advance new=(offset+len+4+3)&~3, CAPR=new-16, self-clearing заголовков (затирание 4 байт статусного поля ⇒ нет replay-детекта через ISR вообще).
- КОРЕНЬ #4 — ГЛАВНЫЙ блокер доставки (найден только через pcap): internet-checksum это ЧИСЛО, а код БЕЗ байтового свапа писал его в LE-поле структуры → на проводе байты перевёрнуты (пример: посчитано 0x61B4, на проводе b4 61) → slirp МОЛЧА дропал КАЖДЫЙ IP-фрейм гостя (и ICMP-ping тоже!). Методология: -object filter-dump (QEMU10: vlan= легаси удалён!) + scripts/pcap_sum.py + вставленный в send() forensic [IPHDR stored/want] и [CSUM raw/folded/ret] → неоспоримое расхождение stored=b461 want=61b4 при равных входах. FIX во ВСЕХ трёх строителях пакетов (net_udp_send / icmp_echo_reply / cmd_ping): ip->checksum = net_htons(checksum(...)).
- КОРЕНЬ #5 (семантика write()): однострочный UDP-write не имел retry: если ARP ещё не резолвен, net_udp_send дропал датаграмму навсегда («caller retries later» никем не выполнялся). FIX sys_write_impl(net): предварительный net_poll, hlt-wait до ~300 итераций с ре-kick'ом ARP каждые 32, отправка ТОЛЬКО после arp=ready, иначе явный [WRDROP]. Экспортирован net_arp_kick().
- ИНСТРУМЕНТЫ (остаются в дереве): rtl8139 d/t расширенный dump (BAR/CR/RCR/ISR/IMR/CAPR/TSD/rx-ring hex/last-TXBUF hex); breadcrumbs [TXF]/[RXRAW]/[RXF]/[UDPRX#]/[UDPDROP#]/[RDNET]; native /bin/nettest (socket→bind(40001)→connect→write→read echo, exit42) в Makefile+userprogs.S+kernel.cpp; харнессы net_probe.sh (ICMP-верdict), net_pcap.sh+pcap_sum.py (wire-bytes), net_native.sh, обновлён net_test.sh (ctrl-c + rtl8139 d перед финальным xp).
- cmd_ping false-positive fix: baseline rx_pkts теперь снимается ПОСЛЕ отправки echo (раньше статический latch засчитывал ARP-reply как «Reply received!» мгновенно).
- №38 rename остаётся ENOSYS — nc его переживает, некритично.

Stage Summary:
- ЦЕЛЬ СЕССИИ ДОСТИГНУТА: полный UDP round-trip slirp через настоящий Alpine busybox nc -u: guest «udp-ok-nulos» → host udp_echo → эхо обратно → nc прочитал (RDNET2 result=13) и напечатал на VGA; официальный net_test.sh вердикт NET ROUNDTRIP PASS (both directions). Аналогично NATIVE /bin/nettest ROUNDTRIP OK exit-42 и честный ICMP ping 10.0.2.2 PASS.
- Полный регресс ЗЕЛЁНЫЙ (reg-base: hello exit42/forktest COW+wait4 0x2100/multi done; reg-bb: uname/echo/ls /(каталоги цветом!)/cat hello.txt) — сетевые правки ничего не сломали.
- Стек готов для apk/wget-тематической следующей ступени (TCP это следующий большой шаг; UDP-only сейчас), сетевые апплеты busybox пробуются так же, как nc.

---
Task ID: 8
Agent: Super Z (main)
Task: Минимальный жизнеспособный TCP в ядре (клиентский путь), busybox nc по TCP, первая реальная HTTP-загрузка файла из сети; полный регресс; ZIP.

Work Log:
- [NET.H] tcp_hdr_t, TCPF_-флаги, TS_-состояния, net_socket_t += sock_type/tcp_state/tcp_eof/snd_seq/rcv_seq; API: connect_tcp/stream_send/stream_recv/close_tcp/has_eof/set_type/type, IPPROTO_TCP=6.
- [NET.C] tcp_checksum (pseudo-header proto6), tcp_send_segment (eth+ip+tcp, checksum BE — урок Task7 учтён сразу), net_tcp_receive (dispatcher proto==6): SYN_SENT→ESTABLISHED→FIN_SENT→CLOSED_WAIT, RST→EOF; in-order приём payload с per-segment ACK, FIN ест один seq; window = SOCK_RX_DEPTH*512 = 4096.
- [HANDSHAKE BUG] SYN занимает один seq-number: peer-ACK приходит на ISN+1, сравнение было с snd_seq==ISN → вечные retransmit-SYN (pcap показал серии одинаково csum-ных SYN). FIX: ackn == snd_seq+1 → snd_seq++ при установлении. После фикса [TCPOK] мгновенно.
- [SYSCALL] socket() принимает SOCK_STREAM(1)+proto 0/6 и запоминает тип сокета ([SOCK] type=1); connect(): stream → ПОЛНЫЙ БЛОКИРУЮЩИЙ handshake (ARP-warm внутри, ETIMEDOUT -110); write() ветвление: stream → stream_send ≤1400B PSH|ACK сегменты; read(): stream → stream_recv, честный 0 на EOF (musl-релэй и http-body полагаются на это); close(): graceful FIN (+bounded drain ждёт финальный ACK).
- [STREAM RECV] семантика -1(EAGAIN)/0(EOF)/n>0; кольцо UDP-сокетов переиспользовано для сегментов (чанкинг ≤512B); net_socket_has_data для STREAM выдаёт readable также при tcp_eof → poll будит relay-loop, read отдаёт 0, nc корректно завершается.
- [RELIABILITY MODEL] slirp loss-free ⇒ ретрансмится только начальный SYN в цикле connect (каждые 32 hlt-итерации); TX блокируется до TOK инлайн (Task7), дескрипторы не переполняются.
- [TCP TEST] scripts/tcp_echo.py + net_tcp_test.sh(+pcap): busybox `nc 10.0.2.2 9100` (STREAM!) — типированная строка ушла на хост, эхо вернулось, [RDSTREAM] result=13, nc напечатал ответ → TCP ROUNDTRIP PASS both directions.
- [HTTP MILESTONE] QEMU sendkey не держит Shift в этом пайпе (: -> ';' даже через отдельный "sendkey shift") → вместо хрупкого набора wget сделан нативный /bin/httpget (userland/httpget.c: socket/connect/GET/read-all/open/write file/preview, exit42; sc3-close arg-count fix) — «первая скачанная веб-страница»: python3 -m http.server :8080, GET f.txt (39B запрос), тело 40B в /tmp/got.txt через ramfs, busybox cat прочёл. Трейс образцовый: TCPOK→TCPTX 39→TCPRX 187(заголовки)+40(тело)→TCPFIN(TCPBYE,TCPFINACK) — ПОЛНЫЙ жизненный цикл соединения.
- Инструменты: net_http_test.sh, net_wget_test.sh остался как отчёт о HARNESS-ограничении typing (не OS-limitation; busybox wget как applet попробуем позже через paste-механизм или ash-script файлом).

Stage Summary:
- Доставлено: TCP-ядро клиентского пути с полным lifecycle (handshake/data/FIN/RST/EOF), работающее с РЕАЛЬНЫМИ потребителями ring-3: Alpine busybox nc (STREAM) и нативный httpget закачал первый файл из сети в ramfs. ICMP/UDP/regression не пострадали (все VERDICT PASS, reg-base+reg-bb зелёные).
- Следующие ступени к apk: LISTEN/accept (серверная сторона), rtx-таймер данных (для реальных потерь), DNS-resolver или hosts-file, THEN busybox wget E2E и как вершина — apk-tools static против Alpine-репо (HTTP GET уже есть!).
- ZIP обновлён: NullOs-fixed.zip (дерево+scripts+worklog+ключевые логи net/tcp/http прогонов).

---
Task ID: 9
Agent: Super Z (main)
Task: Обновить NullOs_README.md — привести документацию в соответствие с фактическим состоянием проекта после сессий 2–8; обновить ZIP.

Work Log:
- Прочитан старый README (685 строк, от 26.08) и весь worklog (Tasks 1–8); сверены с кодом: список syscall-cases в kernel/syscall.c, цели Makefile, состав kernel/userland/scripts каталогов, наличие харнессов в /home/z/my-project/scripts.
- README обновлён по секциям (734 строки): интро (параллельный fork v2, сигнальный каркас, живой ICMP/UDP/TCP, интерактивный busybox sh + nc); «What is» список (+signals/job-control, teardown, async fork); статус-чеклисты: memory (self-map-aware destroy + lifecycle), processes (v2-semantics, WUNTRACED, pgrp/sessions, nanosleep/poll), НОВАЯ секция Signals and job control (^Z IRQ-time routing, [1]+ Stopped E2E), filesystems (S_IFDIR/S_IFCHR typing, POSIX fd lifecycle, редиректы, /dev/tty|console), networking переписан честно (BAR/ISR/RBLEN/CAPR/checksum-BE уроки, клиентский TCP).
- Userspace: добавлены /bin/sleep, /bin/nettest, /bin/httpget + параграф про network-aware natives (первый файл из сети).
- Linux ABI fence заменён на группированный актуальный (fcntl real, TIOC*, poll, getcwd, setpgid/getpgid/setsid/kill-subset, nanosleep, SOCK_STREAM clients, LISTEN честно «not wired up yet»).
- musl/BusyBox: compat-список расширен, добавлен реальный пример сессии (uname -a / sleep ^Z / jobs), подтверждено что цель «busybox sh в ring-3» ДОСТИГНУТА; перечислены отработанные апплеты вкл. nc UDP+TCP.
- Process model: fork-v2 абзац (parking/trap-frame, parfork 30/50, WUNTRACED status). Filesystem model: S_IFDIR и virtual char devices.
- Networking: полная замена раздела — таблица проверенных путей (ICMP/UDP/TCP/TCP+HTTP), археология фиксов дна стека, «honest current bounds».
- Regression: in-tree скрипты с аннотациями + отдельный блок про workspace-harnesses (setup_env/build_iso/full_regress/jc/net_*/tcp_echo/udp_echo/pcap_sum/vga_chunks/hangprobe) + два оракула (-d int, pcap).
- Known limitations заменены: закрытое (large-ELF roots, AS-lifecycle) отделено от открытого (yield-site fd snapshot debt, custom signal handlers stub, dup'd tty ENOTTY fg-resume, yes-flood #GP RSP alignment, сеть без LISTEN/DNS/rtx-timer, rename ENOSYS).
- Roadmap переписан: 2 пункта отмечены [x] (busybox sh path, TCP on wire), остальные актуальны к apk.
- Project structure: реальные имена файлов (mm.c/buddy.c, tests.c, nettest.c/httpget.c/parfork.c/sleepbin.c/ttyprobe.c), README → NullOs_README.md.

Stage Summary:
- README теперь отражает действительность (проверено сверкой с исходниками, не из головы): job control, fork v2, S_IFDIR, POSIX fd, сеть ICMP/UDP/TCP+HTTP, актуальные ограничения и roadmap к apk-tools.
- ZIP: download/NullOs-fixed.zip пересобран (236 файлов): дерево NullOs/ (без isodir/iso/img/core.img/*.o), харнессы scripts/*.{sh,py}, worklog.md, logs/{net-run7,tcp-run2,http-run2,udp-final,reg-base,reg-bb} БЕЗ гигантских int.log (~22MB сэкономлено; net-run7 содержит экранный udp-ok-nulos).

---
Task ID: 10
Agent: Super Z (main)
Task: Сетевой сервер-контур + резолвинг + fg-resume: LISTEN/accept, DNS/hosts, ретрансмиссия, TTYHEAL для dup'd tty fd; регрессия; ZIP.

Work Log:
- Восстановлено окружение: /home/z/qemu-root был снесён между сессиями → setup_env.sh скачал deb'ы заново; BIOS-симлинки seabios/vgabios→usr/share/qemu ЗАФИКСИРОВАНЫ прямо в setup_env.sh (долг прошлой сессии закрыт).
- Предзапуск net_test.sh (srv-smoke): NET ROUNDTRIP PASS; сериал дал [RXF] first frame type=0806, [WRNET2] dst=10.0.2.2:9000 count=13 arp=1 → прошлая «оборванная сборка» подтверждена целой.
- include/net.h: TS_LISTEN=5/TS_SYN_RCVD=6; в net_socket_t добавлены txq (SK_TXQ_DEPTH=4 ретрансмиссионных слота), inflight, snd_una, peer_window, backlog, acc_q/acc_head/acc_tail.
- kernel/net.c:
  * tcp_emit(sk, wire_seq,...) с явным seq; tcp_send_segment стал обёрткой.
  * TXQ: tcp_txq_record/tcp_txq_ack/pop по кумулятивному ACK (modular compare seq_diff), ent_cover=len+(FIN?1:0); FIN пишется как запись len=0.
  * net_tcp_rtx_walk(): RTO base 300ms, ×2 до 2400ms, 5 попыток → [TCPTXFAIL] + conn abort (EOF читателю); вызывается из каждого net_poll().
  * stream_send: pacing по peer_window (+cap 8192) и наличию свободного TXQ-слота; частичная запись = честный short-write return.
  * close_tcp: drain незакэченных данных (bounded 200×hlt) перед FIN; FIN записывается в TXQ.
  * Сервер: tcp_listener_syn() спавнит ребёнка (ISN из общего g_isn_seed), SYN_RCVD отвечает SYN|ACK (и переигрывает при повторном SYN), финальный ACK → ESTABLISHED → push в accept-очередь родителя; admission control по backlog (считая SYN_RCVD детей).
  * connect_tcp: snd_una/peer_window init, общий ISN-генератор.
- kernel/syscall.c:
  * listen(50)/accept(43)/accept4(288): sys_listen_impl/sys_accept_impl; accept блокируется (900 hlt-итераций) и выдаёт НОВЫЙ fd (FD_NET_BASE-child) + sockaddr_in пира.
  * sendto: авто-bind эфемерного порта для UDP (Linux-семантика; критично для musl-DNS с никогда не бинженных сокетов).
  * [#fg-notty FIX] tty_ctl_fd запоминается при успешном TIOCSPGRP; fd_is_console() теперь (а) отличает консольные сентинелы (-2..-9) от сетевых fs_fd<=-10 (старый баг-люк!), (б) признаёт ранее зафиксированный ctl-fd даже если его слот закрыли/переработали ([TTYHEAL] breadcrumb).
  * sockaddr_fill_out хелпер.
- kernel/kernel.cpp: регистрация /bin/srvtest; сиды /etc/hosts (localhost + host/gateway→10.0.2.2) и /etc/resolv.conf (nameserver 10.0.2.3) ПОСЛЕ storage_load.
- userland/tcp_srv.c → /bin/srvtest: socket/bind/listen(4)/accept/read-until-FIN/echo/close, exit 42.
- Makefile/userprogs.S: srvtest встроен в образ (патчер scripts/patch_makefile_srvtest.py, т.к. Edit-инструмент разворачивал табы в Makefile — учтено).

Тесты (все зелёные, без [EXC]):
1. srv-run3 (tcp_srv_test.sh, busybox ash + '/bin/srvtest 12345 &' + hostfwd tcp::23145-:12345):
   [TCPLISTEN]→[TCPLSN st=6]→[TCPSRVOK st=2]→[TCPACCQ child=1]→[ACCEPT nfd=4 peer=10.0.2.2:41308]→rx_total=18→peer fin EOF→echoed=18/18→[TCPFINACK]. HOST python: CLIENT-ECHO-PASS. = TCP-SERVER E2E PASS.
   Уроки: 'elf' — билтин родного шелла (в ash путь /bin/... напрямую); апплеты Alpine busybox без симлинков зовутся только '/bin/busybox <applet>'.
2. dns-run5 (dns_hosts_test.sh): 'PING host (10.0.2.2)' = hosts-файл через musl ✓; nslookup kernel.org 10.0.2.3 → реальные A+AAAA ответы ✓ (на экране 172.105.4.254 / v6); сериал: [UDPSND]->10.0.2.3:53 + серия [UDPRX] по эфемерным портам = REAL DNS THROUGH OUR STACK WORKS. = DNS/HOSTS TEST PASS.
   Открытые пункты: busybox ping падает на SOCK_RAW (Protocol not supported) — raw-socket не реализован; internet-side ICMP-ping ждёт его же. QEMU-sendkey теряет Shift в этом пайпе (colon не набрать → wget-by-hostname отложен до paste-harness'а).
3. tstp2 (tstp2_test.sh, sleep-flavor против known yes-flood #GP): FG-RESUME E2E PASS —
   [TSTP] ctrl-z → "[1]+ Stopped" → jobs → fg → [TTYHEAL] ctl_fd=10 survives slot churn (used=0 fs_fd=-1) → SIGCONT доставлен ([KILL] sig=18) → команда возобновлена (fg печатает имя job'а) → второе ^Z работает (repeatability=2). Тот самый jc-run11 сценарий больше НЕ роняет tcsetpgrp.
4. full_regress.sh: build ok; reg-base (hello/forktest/multi3) + reg-bb (uname 'NullOs nullos 6.1.0-nullos x86_64', echo ring3, ls /, cat hello.txt) — всё без исключений (exc=0 во всех пяти логах: reg-base/reg-bb/srv-run3/dns-run5/tstp2).
   [TCPRTX]/[TCPTXFAIL] на живом прогоне не видны — slirp loss-free; код верифицирован сборкой и структурно (известное ограничение проверки, не OS).

Stage Summary:
- Доставлены ЧЕТЫРЕ запросенные вещи: LISTEN/accept (сервер, E2E с внешним клиентом через slirp hostfwd), DNS/hosts (musl-резолвинг работает: hosts-file + реальные DNS-транзакции по нашим UDP-сокетам), ретрансмиссия (per-socket unacked ring + RTO/backoff + окно пира + pacing), fg-resume через dup'd tty fd (TTYHEAL — ctl-fd переживает пересборку слотов).
- Открыто/наметилось: SOCK_RAW для ping; wget-name тест требует paste-harness (shift-loss); [TCPRTX] в живых условиях; далее — apk-tools static против Alpine-репо (HTTP GET есть, имена теперь резолвятся!).
- ZIP обновлён (см. Task 11).

---
Task ID: 12
Agent: Super Z (main)
Task: SOCK_RAW для ping → internet-ping; paste-harness для wget-by-name; проверка всего; ZIP (запрос пользователя).

Work Log:
- Восстановлен сэндбокс (setup_env.sh, BIOS-симлинки внутри скрипта ✓).
- РЕГРЕССИЯ ОБНАРУЖЕНА И ПОБЕЖДЕНА: `net_socket_connect_tcp` стирал `sk->connected` при SYN_SENT -> демуксер терял собственные SYN|ACK ([TCPSHIP] no stream socket), ВСЕ TCP-коннекты пали ETIMEDOUT. Флаг сохранён + комментарий.
- Paste-harness `scripts/paste_run.sh`: HTTP-сервер с payload -> /bin/httpget внутрь гостя -> `ash /tmp/c.sh`; колонки/'&&'/$ работают, тк читаются из файла. STRICT-верификация маркеров (отличать эхо текста от исполнения) через вычисляемые маркеры.
- WGET-BY-NAME E2E PASS: busybox wget http://host:8087/f.txt (hosts->10.0.2.2) -O /tmp/w.bin; тело проверено busybox grep токена в госте.
- SOCK_RAW/ICMP ядро: socket(RAW|DGRAM+IPPROTO_ICMP), net_icmp_send (verbatim ICMP под IP), доставка ответов в rx-ring icmp-сокета ([ICMPRX]), пейсер #ping-no-sigalrm: net_poll() пересылает последний echo req раз в 1с c инкрементом seq+пересчётом checksum (busybox не заметил отсутствия SIGALRM), giveup cap 30.
- PING-GW PASS: 3 пакета приняты busybox, статистика напечатана, чистый выход.
- КЛАСТЕР УБИЙЦ: (1) fcntl(F_DUPFD)/dup без рефкаунта описания файла -> ash умирал на F_DUPFD>=10 скрипта (fs_dup/fs_close refs). (2) dup2 сетевого fd освобождал сокет при алиасе (net_socket_dup/sk_refs). (3) FORK копия таблицы без инкремента ссылок -> close() ребёнка убивал файл родителя (#ash-script-death). Все три починены POSIX-семантикой «alive till last alias».
- #a6-off-by-one (syscall_entry.S): исторически arg7(user arg6) клался ДО паддинга -> каллиб читал мусор из pad-слота (recvfrom/addrlen=17!). Верным оказался третий вариант: subq$16 раньше, arg7 прямо в [rsp], симметричный addq$16. Первый «правильный» реворк с якорем ломал park/wait4-resume потоки (#jc-regress, TTYHEAL), откат bisect-ом подтверждён.
- sendmsg(46)/recvmsg(47): минимальные имплементации (первый iovec, sockaddr out, ctl=0). recvmsg FIX(#rmsg-wrong-slot): proc_fd_to_net обязателен — сырой fd индексировал чужой слот таблицы сокетов.
- sin_family эндрианнесс (#gai-drop): recvfrom/recvmsg писали AF_INET как BE {00,02}; musl-резолвер отбрасывал настоящие DNS-ответы. Теперь native LE {02,00}. Реальный DNS-ответ интернета словлен в ядро целиком ([DNSRX] id/fl=8180/an=1!).
- ARP-warm для blocking-sendto расширен до 260 итераций (#ping-first-shot).
- Чистая пересборка после КАЖДОГО заголовочного изменения (в Makefile НЕТ -MMD; stale .o ловушка реальна).
- Internet-ping по имени: инфраструктура доказана полностью (DNS-транзакции через наши UDP + RAW ICMP работает на шлюзе), но getaddrinfo-level мультизапросы musl пока отвергаются (#gai-aaaa OPEN).

Stage Summary:
- Доставлены обе просьбы: SOCK_RAW+ping (gateway E2E PASS) и paste-harness+wget-by-name (strict PASS). Побочно закрыты четыре глубинных ABI-бага (connect-flag, dup/fork refcounts x3, a6-off-by-one, family-endian), job-control снова зелёный, полный регресс exc=0.
- NEW TESTS: scripts/paste_run.sh, net_wget_name_test.sh, net_ping_test.sh, net_ping_pcap.sh, verify_all/b1/b2.sh.
- OPEN: internet-ping by public name (#gai-aaaa: recvmsg-metadata/мультизапросы musl), pipes в ash (can't create pipe EBADF), user-signal handlers (SIGALRM/setitimer) — next sessions.

---
Task ID: 13
Agent: Super Z (main)
Task: #gai-aaaa (internet-ping/wget по имени), pipe2 для ash, apk-tools против Alpine-репо; проверка; ZIP.

Work Log:
- #gai-aaaa: прочитан musl-res_msend (1.2.3 с Debian-sources + 1.2.5 с git.musl-libc.org): lookup_name требует ответа на ОБА запроса (A+AAAA), иначе EAI_AGAIN. Диагностика по кругу: [SOCKCALL] показал musl 1.2.5 socket(SOCK_DGRAM|CLOEXEC|NONBLOCK=0x80802) -> наш отказ -> musl-фолбэк на голый type=2+fcntl; [DNSQ/DNSA/pcap] — запросы/ответы id-в-id. КОРНИ: (1) #gai-arp-kick — переписанный ARP-warm-цикл потерял периодический kick: ждали ответ на незаданный запрос, ПЕРВЫЙ из пары sendto терял кадр (net_udp_send молчаливый дроп), musl получал ответ только на один запрос; (2) #gai-socket-flags — тип с флагами отвергался (замаскирован type&0xFF, флаги приняты); (3) #route-fix — ОТСУТСТВИЕ ДЕФОЛТНОГО МАРШРУТА: arp_resolve для интернет-IP никогда не отвечал, tcp_emit/udp/icmp/resend теперь идут через net_next_hop() (офсеть -> шлюз 10.0.2.2), + net_arp_ready_route/net_arp_kick_route в syscall-путях.
- ИТОГ #gai: getaddrinfo по публичному имени работает; СТРАНИЦА CERN (info.cern.ch, 646B) скачана busybox wget ИЗ ИНТЕРНЕТА по имени через наш стек (INET-PASS-42 live). Internet-ICMP roundtrip НЕВОЗМОЖЕН физически: у хост-песочницы нет cap_net_raw (ping: Operation not permitted) — slirp не может форвардить ICMP; задокументировано.
- pipe2 (22/293): кернел-пайпы (16 x 8KB ring, refcount концов read/writers), sys_pipe2, read(блок до данных/EOF)/write(EPIPE без читателей)/close/dup/dup2/fcntl/fork-copy ref-инкременты/poll-классы. POSIX-семантика "alive till last alias". Достижение: `echo hello-pipe | busybox cat` ДАЛ ВЫВОД (данные через пайп идут). #pipe-eof-leak: exit процесса теперь РЕЛИЗИТ все его fd (syscall_release_all_fds в proc_exit_current) — читатели видят EOF.
- ОТКРЫТО #pipe-fd-switch: полная ash-пайплайна (cat|wc с EOF) упирается в ПРЕДСУЩЕСТВУЮЩИЙ порядок загрузки fd-таблиц у форк-детей: sched_fds_on_switch НЕ вызывается при первом переключении в fork-ребёнка (fork_resume_child-путь) — ребёнок стартует не со своим снимком; трассы [FDSW]/[PIPEI/PIPED]/[FDTBL]/[CLOSE]/[DUP2] в logs/pipe-test* фиксируют картину. Нужен отдельный заход в scheduler-switch пути.
- apk-tools: отложен осознанно (нужен стабильный pipe/rename/mkdir фундамент); хост-репо схема описана в README-addendum.
- Полная верификация финального дерева: udp/httpget/wgetname/ping-gw/wget-inet/dns/tcp-server/jobcontrol = 8/8 PASS; full_regress reg-base+reg-bb [EXC]=0.
- ВРЕМЕННАЯ диагностика снята из кода (сохр. в логах pipe-test*/gai-probe*).

Stage Summary:
- #gai-aaaa ЗАКРЫТ (интернет-wget по имени — новый E2E), маршрут по умолчанию появился (#route-fix), socket-флаги POSIX (#gai-socket-flags). pipe2 ядро добавлено и частично работает (#pipe-fd-switch открыт с полной картой улик). 8/8 тестов + регресс зелёные. ZIP обновлён.

---
Task ID: 14
Agent: Super Z (main)
Task: Охота на порчу (canary в pt_pool + гипотеза DMA-адресов NIC) → mbedtls/zlib/lwext4 по плану; востановление после сброса песочницы.

Work Log:
- СЕССИЯ НАЧАЛАСЬ С НУЛЯ: qemu-root снесён (setup_env.sh восстановил), логи прошлой APKREG-сессии (probe-apk3/gdb-regs1) утеряны, git-дерево на Task 13. Кит needit.zip распакован в nulos/kit/needit/ (dlmalloc, libzlib, lwext4, lwip, mbedtls 4.2.0+tf-psa-crypto).
- **ZERO-WRITER НАЙДЕН СТАТИКОМ ДО ВСЯКИХ КАНАРЕЕК (#heap-overlap)**: KERNEL_HEAP_START=0x500000 < __kernel_end≈0x6FA000 при встраивании apk.static (4 365 560 байт) в ядро. heap_init() пишет заголовок блока ПРЯМО в 0x500000 = внутрь .rodata → kmalloc/kzalloc-нули = zero-writer на 0x5b2ff0. Второй конфликт: user VA apk (0x401000..0x6FA0A3) затеняет identity-маппинг ядра >4MB в user CR3. Вывод: ядро НЕЛЬЗЯ раздувать >4MB — встраивание apk в образ отменено.
- FIX #heap-overlap: KERNEL_HEAP_START → 0x1000000 (16MB); heap_init() фатальный guard (VGA красным + serial + hlt) при __kernel_end >= KERNEL_HEAP_START.
- Canary (шаг 1 плана): pt_pool расширен 128→130 страниц, крайние страницы — ПОСТОЯННЫЕ sentinel'ы с магией PTCANARY (32 qwords на страницу), проверка на КАЖДОМ take/give → [PTCANARY] alert. [PTPOOL] лог диапазона. NIC: [DMA] audit в rtl8139_init — точные PA-окна rx_ring/tx_buf (DMA write-exclusivity), для диффа при порче.
- apk.static доставлен ДВУХСТАДИЙНО (вместо встраивания): STAGE-A ядро (make APK_DEFS="-DAPK_STAGE_A") — блоб в .rodata, kernel_main копирует в ramfs /bin/apk (CRC32 3d55d03e = хостовому!), storage_sync на диск, poweroff (ACPI-hang — timeout в скрипте). STAGE-B: blobless ядро 1.67MB, storage_load восстанавливает /bin/apk. scripts/apk_import_stageA.sh (сохраняет disk-apk.img!), apk_stageB_test.sh.
- **E2E PASS: elf /bin/apk --version → "apk-tools 2.14.4, compiled for x86_64." ; busybox md5sum /bin/apk = bb83d4c6... = хостовому md5 — байт-в-байт через blob→ramfs→storage→disk→exec.** apk --help печатает полный хелп. Zero-writer мёртв.
- FIX #elf-bigfile: elf_load_from_file читал фикс. 4MB → apk.static TRUNCATED ("Segment 3 extends beyond file") и выполнялся с нулевым хвостом .data! Теперь: fs_get_size_by_fd probe + рост буфера (cap 8MB) + отказ на oversized.
- Регрессия после переноса кучи: reg-base + reg-bb exc=0, busybox uname/echo/ls/cat живы.
- ОХОТА ПРОДОЛЖЕНА: детерминированный краш при exec apk через ash (CR2=0x4C2770, RIP=0x46EC40, err=7 P|W|U — user пишет в СВОЙ present-RO (RX) фрейм). Инструменты добавлены: PFDUMP в page_fault_handler (расширенные регистры r8-r15, PTE-walk активного CR3, дамп стека, frame-walk, дамп 32 байтов страницы по PA); [BUILD] probes (pas[k] после копии и pre-phase2); kernel сеет /tmp/netcmd.sh (#netcmd-seed — замена хрупкого paste-harness через хост-сервер); httpget retry connect ×4.
- Диагноз #exec-frame-dup (ТОЧНО ЛОКАЛИЗОВАН, корень не исправлен): apk seg1 pas-массив ЛИНЕЕН (pas[0]=0x5C2000, pas[193]=0x683000, pas[642]=0x844000), НО контент PA 0x683000 = apk-код для ДРУГОГО VA (file 0x283000 = VA 0x683000), т.е. на фрейм pas[193] легли данные k≈642 — ДВОЙНАЯ ВЫДАЧА ФРЕЙМА PMM в цепочке exec/busybox-ash → exec/apk → destroy старого CR3. Основной подозреваемый: vmm_destroy_level() "exclusive-untracked → pmm_free_page" освобождает фрейм, на который ещё есть живая ссылка (порядок build→destroy в proc_do_execve; identity-листья с USER-битом; pmm_ref_tracked на untracked). PFDUMP уже стоит в ядре — следующая итерация: лог [PMMFREE] при destroy + сверка с pas[].
- Попутно: zombie python http.server от paste-прогонов портил bootstrap (порт 8081/8082) — pkill-гард в paste_run/paste_gdb; gdb 16.3 установлен в /home/z/gdb-root (loopback-TCP песочницы иногда фильтруется — поэтому PFDUMP в ядре надёжнее gdb-stub).
- НЕ НАЧАТО (переносится): dlmalloc-vs-kmalloc, mbedtls-HTTPS, lwext4 (шаг 3), mremap/umask заглушки для musl.

Stage Summary:
- Задача 1 (охота на порчу) ЗАВЕРШЕНА УСПЕХОМ: zero-writer найден и УБИТ (#heap-overlap), canary+guard+DMA-аудит вшиты, apk.static РАБОТАЕТ E2E (версия+md5), полный регресс exc=0.
- Вскрыт и локализован ВТОРОЙ слой порчи: #exec-frame-dup (двойная выдача PMM-фрейма при exec-цепочке с destroy) — все датчики уже в ядре, корень — в vmm_destroy_level/proc_do_execve порядке; это блокер для apk add против dl-cdn.
- Новые артефакты: scripts/apk_import_stageA.sh, apk_stageB_test.sh, apk_net_test.sh, apk_run.sh, paste_gdb.sh, disk-apk.img (с /bin/apk).

---
Task ID: 15
Agent: Super Z (main)
Task: #exec-frame-dup → apk add против dl-cdn (цель №1); dlmalloc/mbedtls/zlib — по надобности; lwext4; охота на «освободителя фрейма».

Work Log:
- **#exec-frame-dup: ЗАКРЫТ (два корня).** (1) #ident-alias — kmemset/demand-pager/COW/build_user_image писали через identity-VA под АКТИВНЫМ user CR3, чьи низкие окна сплиты; после execve-destroy PMM отдавал низкие дырки (0x5b2000...) → kmemset обнулял ТЕКСТ apk + malloc-арена оставалась мусорной → doapr-стайл write-to-text (CR2=0x4C2770, err=7). Фикс: vmm_pa_read_begin/end (kernel-CR3 окно) вокруг ВСЕХ identity-записей (brk/mmap/demand-pager/COW-copy/build_user_image) + argv-строки копируются в kernel-буфер ДО окна (#argv-probe). (2) #fs-base-no-switch — MSR_FS_BASE персистентен между процессами; после exec-ребёнка с другим образом ash умирал на musl stack-canary (%fs:0x28, CR2=0x82F260 = bss apk). Фикс: task_t.fs_base, запись в sys_arch_prctl(SET_FS), восстановление в sched_activate на КАЖДОМ switch-in, наследование в fork. — «Двойная выдача фрейма» из Task 14 оказалась артефактом PFDUMP: page-dump читал identity-VA через user CR3 (split-окна алиасят user-VA). PFDUMP v2: все физические чтения под kernel-CR3 + дамп кода по RIP + стек-скан .text-указателей + deref-якоря + last-syscall. PFDUMP теперь стреляет на ЛЮБОМ user-фолте (не только write).
- Охота на «освободителя фрейма» ЗАВЕРШЕНА ВЕРДИКТОМ: живые фреймы НЕ освобождались. Введены [PMMFREE-IMG]/[PMMALLOC-IMG] (registry последнего образа в proc.c, proc_img_owns_frame), [XV spawn/pre-swap/post-destroy] — полная сверка PTE==pas[k] и содержимого фреймов с файлом: ВСЕ OK во всех прогонах; PMM-алармов ноль. «Освободителей» заменили alias-записи (см. выше) — все закрыты.
- Верификатор образа (g_exec_verify) оставлен в exec-пути: pre-swap и post-destroy сверяют каждый сегмент с файлом через kernel-CR3 окно.
- **Цель №1 (apk add против dl-cdn) — сеть доведена до подписи/парсинга; остался один TCP-блокер.** Введены syscall'ы: mkdir(83)/mkdirat(258), mknod(133)/mknodat(259), unlink(87)/unlinkat(263), rename(82)/renameat(264) + fs_rename() в ramfs (замена цели с отсоединением), flock(73), statfs(137), setfsuid/gid(138/139), честный gettimeofday(96) (был фейк «return 1»), umask(95)→022. openat/mkdirat/newfstatat получили dirfd→path-таблицу (#apk-dirfd: apk открывает «/» и делает ВСЁ через openat(root_fd,...)). open-fail теперь -ENOENT (был -1 → musl errno=EPERM → ложный «Operation not permitted»). MAX_FILENAME 32→64 (#apk-cache-name: .apknew.APKINDEX.<hash>.tar.gz = 32 символа не влезал). fstat: убрана ложь «sz<4096 → 4096» (#apk-empty-read: apk видел CL=4096 у ПУСТОГО world → short read → EIO → abort(hlt) в ring3 → #GP 0x6E372C — musl-ловушкаapk-fatal: apk abort компилится как hlt!). read/write: count==0 → успех ДО валидации буфера. select(23) реализован ПОВЕРХ poll-логики с блокировкой (#select-no-block: односweep-0 libfetch читал как таймаут и рвал загрузку после первого 128KB-блока). sys_sendto/recvmsg: STREAM-ветки (был UDP-fallthrough — GET уходил как UDP-мусор → «operation timed out») + маска типа (& 0xFF) во ВСЕХ сравнениях (musl передаёт SOCK_STREAM|CLOEXEC|NONBLOCK=0x80801). Засеяны /etc/apk/{keys,skeleton} + 5 публичных ключей Alpine (scripts/gen_keys_blob.py → kernel/keys_blob.h).
- Подтверждённые успехи сети на полном конвейере: DNS ✓, TCP-handshake ✓, HTTP GET на проводе (pcap: «GET /alpine/v3.19/main/x86_64/APKINDEX.tar.gz»), 200 OK, заголовок Content-Length=468221 распарсен, gzip-поток принят (局部 195B-тест: apk прошёл fetch→gunzip→tar→adb→план установки «The following NEW packages will be installed: busybox-static. Do you want to continue [Y/n]?» — ВЕСЬ конвейер жив!).
- **Оставшийся блокер цели №1 (#tcp-bigfetch-stall):** на файлах >~131КБ приём замирает: принимающий стек теряет сегмент (кольцо полное → win=0), затем не открывает окно; wget-big тесты: остановка на ровно 65536/131269/126773/59597 байтов, сервер в вечной ретрансмиссии, наши ACK исчезают с провода. Устранено по пути: #tcp-rx-drop-silent-ack (tcp_enqueue возвращает took; rcv_seq/ACK только на принятое), #tcp-zero-window-deadlock (ACK безусловно на in-order, даже took=0 — persist-пробы получают текущее окно), #tcp-window-update-on-drain (window-update ACK после дренажа половины кольца), RX-кольцо 8→16 слотов × 1024B, окно=свободное место кольца, #rtl-rx-header-wrap (RX-заголовок кадра читался за границей 8КБ-кольца → вечный «пустой слот» → CAPR-джем → смерть сети на 60-130КБ; теперь каждый байт заголовка обёрнут), rtl RX_BUF_SIZE 8КБ→32КБ (RBLEN_32K), #tx-busy-drop (rtl8139_send ждёт свободный дескриптор вместо мгновенного дропа ACK-шторма), #tx-hlt-noif (hlt в send только под sti/cli). Финальный рассчёт-кандидат: рекурсия net_poll→dispatcher→send из IRQ-контекста перезаписывает горячий TX-дескриптор (нужен TX-лок/вынос из IRQ) — зафиксировано как следующий шаг.
- dlmalloc/mbedtls/zlib из кита: НЕ ПОТРЕБОВАЛИСЬ (apk.static несёт собственный zlib, качаем по http; mbedtls — для https-фазы later). lwext4 — не начат (переносится, порядок сохранён).
- Полный регресс ЗЕЛЁНЫЙ после всех фиксов: reg-base (hello 42, forktest 33/7, multi) + reg-bb (4×exit=0, uname/echo/ls/cat) — EXC=0 везде. ZIP обновлён (download/NullOs-fixed.zip) с логами cdn/loc/wget/reg.

Stage Summary:
- #exec-frame-dup ЗАКРЫТ (два настоящих корня: identity-alias запись + FS-base без переключения); «освободитель фрейма» не существует — доказано регистром фреймов + верификатором образа; PFDUMP v2 больше не врёт.
- apk add: syscall/файловой слой для apk доведён до конца; сеть проходит DNS→TCP→HTTP→gzip→tar→adb→план установки; единственный остаток — TCP-приём больших потоков (TX-надёжность rtl8139 под ACK-штормом + окно-обновления) с известным планом фикса.
- Новые регрессионные якоря: wget-big*/apk-loc*/apk-cdn* логи + pcap-оракул как основной инструмент сетевой отладки.

---
Task ID: 16
Agent: Super Z (main)
Task: #tcp-bigfetch-stall (TX-лок/вынос send из IRQ — план Task 15) → apk add E2E против dl-cdn → lwext4.

Work Log:
- **Аудит send-путей**: IMR=0 → NIC IRQ мёртв; реальная гонка — PIT-преемпция (IRQ0 → scheduler_preempt) при IF=1 в net.c: 7 отправителей строят кадры в ОДИН статический tx_frame, дескриптор tx_current тоже общий. Починено: (1) локальные кадровые буферы на стеке каждого отправителя (#tcp-bigfetch-stall), (2) TX-лок в rtl8139_send (IF=0 на reserve→copy→kick→TOK-wait, busy-pause вместо sti/hlt/cli), (3) net_poll guard (`__sync_lock_test_and_set`) от параллельного дренажа + локальный poll_buf, (4) rtl8139_irq_handler — net_poll вынесен из IRQ (#poll-deport-irq).
- **256KB-тест всё ещё падал → pcap (filter-dump) + трасса CAPR-обхода**: сервер шлёт, гость ACK-ит, потом гость оглох. RXDIAG (CAPR/ISR/консоль NIC) показал: walker смотрит на ЗАСТАРЕВШЕЕ ТЕЛО старого кадра как на заголовок (st=0x4647="GF", len=0x5445="ET" — байты "BIGFETCH"!), CBA (порт 0x3A = RxBufAddr QEMU) стоит на месте. **#rx-stale-body-cba**: мы чистим только 4 байта заголовка потреблённого кадра; при паузе в передаче poll читает stale-body с ROK=1 и «правдоподобной» длиной → CAPR улетает на 21KB → тотальный десинк. Фикс: принимать кадр только если [header+data+crc] лежит в непрочитанном диапазоне [CAPR+16, CBA) — кадры атомарны (один vCPU). РЕЗУЛЬТАТ: **256KB wget, md5 совпал с хостовым байт-в-байт**.
- **Попутно найдены и починены 2 старых корня** (объясняют все исторические клиффы 60-130KB): (a) **#rtl-rblen-64k** — RBLEN в RCR биты 12:11 (00=8K,01=16K,10=32K,11=64K); старый 0xB<<11 ставил RBLEN=11=64KB NIC-кольцо при 32KB софтверном → NIC DMA-ил за буфер в tx_buf/.bss; (b) **#rtl-rxwrap-pastbuf** — при WRAP=1 QEMU пишет кадр линейно ЗА RxBufferSize (split-путь выключен при `<65536 && RxWrap`) — хвост кадра DMA-ился в чужую память; WRAP снят, наш побайтовый wrap-читатель переваривает швы split-кадров.
- **apk add E2E (ЦЕЛЬ №1)**: 4 syscall-слоя добавлено: **symlinkat(266)+readlinkat(267)+readlink(89)** — полная поддержка symlink в ramfs (FS_FILE_TYPE_SYMLINK, fs_walk_path с follow/no-follow, splice target+rest, бюджет 8 хопов, d_type=DT_LNK в getdents64), **fchownat(260)/fchmodat(268)/utimensat(280)** no-op-заглушки с честными комментариями. Попутно: **#walk-separator-skip** (мой первый walker не потреблял '/' → вечная петля на многокомпонентных путях — пойман по RIP из QEMU-монитора!), **#brk-skip-page** (`cur_page=(cur+0xFFF)&~0xFFF` пропускал страницу на выровненном brk — mallocng растёт ровно так; meta-страницы обслуживались demand-pager'ом без учёта и затирались следующим ростом brk), MAP_FIXED теперь освобождает замещаемую страницу. **РЕЗУЛЬТАТ: apk add --initdb zlib против dl-cdn.alpinelinux.org: APKINDEX.tar.gz 468221B скачан, подпись проверена, musl+zlib (451 KiB) скачаны и установлены, "OK: 0 MiB in 2 packages"** — пакетный менеджер Alpine работает поверх собственного стека.
- **Открыт #apk-exit-malcheck** (следующая сессия): после "OK" apk падает musl-a_crash (hlt в ring3 → #GP 0x6E372C). GPDUMP добавлен в #GP-user путь: meta-область 0x10001000 цела, walk в DATA-области (RDX=0x20001A3C) ушёл под базу группы (RSI=0xC < 0x1000) — порча/двойное освобождение chunk-заголовков группы. Ведёт: d_type=DT_LNK изменил путь apk'овского readdir-сканера, ЛИБО mmap-bump/munmap семантика, ЛИБО двойной free в teardown apk. Данные для расследования в logs/apk-e2e10.

Stage Summary:
- Сеть: большие загрузки работают (256KB byte-perfect), TX-сериализация + poll-guard + RX-CBA-guard + 2 NIC-конфиг-бага починены.
- ФС: symlinks, честный nofollow/follow, readlink, d_type; brk корректен для mallocng.
- apk: ПОЛНЫЙ ЦИКЛ против реального dl-cdn — index/verify/resolve/download/install = OK. Остался краш на ВЫХОДЕ apk (#apk-exit-malcheck) — не мешает установке, но валит сессию.
- lwext4 — НЕ начат (переносится за #apk-exit-malcheck).
- Изменённые файлы: kernel/net.c, kernel/rtl8139.c, kernel/syscall.c, kernel/fs.c, include/fs.h, include/syscall.h, kernel/idt.c, kernel/kernel.cpp (netcmd/repositories), syscall RDSTALL/RDSTREAM-диагностика.

- **Финальная верификация (ядро 12:39)**: wget-inet PASS; wget-big 256KB — 100%/saved/md5 f51003c5... = хост (верdict-маркер BIGFETCH-SIZE не доехал до VGA-дампа — косметика харнесса, контент подтверждён md5). Все фиксы стабильны.

---
Task ID: 18
Agent: Super Z (main)
Task: Завершить E2E mbedTLS (dev/verify/negative), CA-bundle VERIFY_REQUIRED; регресс всей системы после 17a-c (dlmalloc/lwext4/TLS) и охота на всплывшие регрессии.

Work Log:
- E2E TLS стартовал с готовым tls_test.sh (3 фазы). ФАЗА-1 (dev) прошла сразу (HTTP 200 от dl-cdn, 3028B), НО клавиатура умирала после первой https — фазы 2/3 не доходили: prompt печатался, эхо клавиш ноль.
- #tls-if-leak (НАЙДЕН+ПОБЕЖДЁН): bio_recv/dns_resolve_a (tlsglue) и 5 мест net.c использовали паттерн "sti\nhlt\ncli" — cli-хвост оставлял IF=0 на выходе из цикла. Для syscall-контекста (SYSCALL очищает IF) это корректно, но cmd_https — ПЕРВАЯ ring-0 сетевая команда: IF=0 доехал до шелла, чей wait — голый hlt() → вечный сон, клавиатура/таймер мертвы. ФИКС: hlt_irq_restore() (include/types.h) — pushfq/sti/hlt/popfq, восстанавливает IF вызывающего; заменены все 6 мест.
- Попутно #tls-if-leak2: путь успеха https_get_impl возвращал body_len МИМО done:-cleanup (утечка ssl/conf/сокета, FIN-зависание) → общий cleanup на обоих путях; goto done перепрыгивал инициализацию ret → мусорное "DONE (3112493)" → ret объявлен до всех goto.
- #tls-verify-cn (КОРЕНЬ verify-fail): fail-flags=0x04 (CN_MISMATCH); VRFY-hook (mbedtls_ssl_conf_verify per-cert) показал лист CN=j.sni-644-default.ssl.fastly.net — GENERIC FALLBACK-СЕРТ FASTLY! mbedtls_ssl_set_hostname работал только для верификации — в ClientHello НЕТ server_name: в kernel-config отсутствовал MBEDTLS_SSL_SERVER_NAME_INDICATION. С fastly SNI-фиксом лист стал CN=dl-cdn.alpinelinux.org (SAN match) → цепочка leaf→LE YR2→ISRG Root Y→ISRG Root X1 (из bundle, 150 корней) верифицирована: verify result=0x00000000 (REQUIRED). self-signed.badssl.com отвергнут (0x08 NOT_TRUSTED). Вердикт: dev PASS / verify-pos PASS / verify-neg REJECTED. (0x80 в dev-режиме = SKIP_VERIFY — норма; часы RTC корректны.)
- ПОСЛЕ TLS — ПОЛНЫЙ СВИП УПАЛ (все spawn-тесты). #kernel-bss-shadow (КОРЕНЬ, regression от 17a-c+TLS): ядро (mbedtls+ext4) выросло до VA 0x5c0e48 > 0x400000, а user-образы прибиты к VA 0x400000: в дочернем CR3 .bss ядра (g_tasks VA 0x405440!) затенялся байтами busybox → [R2C] go с мусорным pid/rsp/pml4, мгновенная смерть. Диагностика микропробами: TCB цел на [R2C] enter/p1/p2, мусор после sched_activate (CR3 ребёнка). ОФЛАЙН-проверка (openssl verify против извлечённого bundle) доказала: bundle корректен, дело не в сертификатах.
- #kernel-halffix (СТРУКТУРНЫЙ ФИКС, "не костыль"): ядро переехало в VA=PA+1GB. linker64.ld: .boot identity@1MB (multiboot+32-битный стаб, paging off) + единая секция .high с AT() (VMA=LMA+1GB ТОЧНО; раздельные секции давали скос 0xE от выравнивания input-секций — .rodata читался бы мимо фреймов!). boot64.S: .boot.text для стаба, boot_pd_win 512×2MB (.rept — частичное окно из 32 записей ломало PMM bitmap на PA 536MB), GDT/CR3 арифметика в PA. vmm_init: PDPT[1] = окно (VA 1GB+i*2MB → PA i*2MB), identity 0-1GB/2-4GB сохранены для устройств (VGA/MBI/трамплин, PCI/LAPIC MMIO). mm.h: HH_OFFSET=0x40000000. mm.c: heap на VA 0x41000000, guard через PHYS_TO_VIRT, PMM __kernel_phys_end (= __bss_end - 1GB — ВКЛЮЧАЯ .bss: первичный вариант по LMA .high выдавал kernel-.bss/стек-фреймы юзеру — seg0 pas[0]=0x314000=boot-stack frame, kmemset образа стирал стек ядра!), PMM bitmap/refcounts через PHYS_TO_VIRT. syscall.c: user_range_ok — потолок 32GB сохранён (стек юзера на 0x7FFFF0000!) + ДЫРА [1GB,2GB): flat-потолок 1GB ломал ВСЕ syscall со стековыми указателями (musl poll struct → EFAULT → nc умирал мгновенно — найдено бисекцией: ядро Task 16 в worktree воспроизводило тот же nc-фейл, отсекло 17a-c; реальный триггер мой ceiling + перенесённый SNI-диагноз).
- РЕЗУЛЬТАТ HALFFIX: spawn/exec/exit полный цикл (R2C go pid=1 → mmap → write → exit=0 → R2C back), первый же busybox echo байт-в-байт.
- ФИНАЛЬНАЯ ВЕРИФИКАЦИЯ (итоговый бинарник): udp round-trip PASS (host получил 'udp-ok-nulos', nc релеит), httpget PASS, wgetname strict PASS, ping-gw PASS (raw-ICMP), dns-hosts PASS, tcp-server PASS, jobcontrol FG-RESUME PASS, wget-inet INET-PASS-42 PASS, reg-base+reg-bb EXC=0, TLS E2E 3/3 PASS. ext4 reboot-persistence — уже закрыт в Task 17c (git: boot1 write → boot2 cat → boot3 live + debugfs), dlmalloc — Task 17a (self-tests 17/17).

Stage Summary:
- mbedTLS 4.2 HTTPS ДОВЕДЁН ДО ПОЛНОГО E2E: dev (VERIFY_NONE) PASS, VERIFY_REQUIRED с вкомпилированным CA-bundle ПРОТИВ реального dl-cdn PASS (0x00000000), негатив self-signed REJECTED. Заголовок фичи: https <host> <path> [verify] [outfile].
- ТРИ новых корня закрыты: #tls-if-leak (IF=0-утечка sti/hlt/cli), #tls-verify-cn (отсутствие SNI → Fastly fallback), #kernel-bss-shadow → #kernel-halffix (ядро в VA=PA+1GB — структурная защита от затенения ядра юзером навсегда).
- Сеть: 8/8 E2E зелёные; базовый регресс EXC=0; spawn-путь здоров.
- NEW TOOLS: nc_probe.sh (контролируемый UDP-проба), extract_ca_pem.py (офлайн-верификация bundle), tls_test.sh verdict по serial.
- OPEN: #pipe-fd-switch, #exec-sh-c-stack (охоты), [VRFY]/[TLS]-forensics в tlsglue оставлены (serial-only, дёшево).

---
Task ID: 19
Agent: Super Z (main)
Task: dwl-поворот: склонировать dwl, оценить #pipe-fd-switch/#exec-sh-c-stack, подготовить Wayland-субстрат, ZIP + README.

Work Log:
- Склонирован dwl 0.8-dev (codeberg, a2d03cf) в nulos/dwl как reference. Зависимости: wlroots-0.19 → libinput, wayland, xkbcommon (+libseat/pixman/DRM/EGL).
- Обнаружена потерянная сессия (авто-коммит c1c08af): epoll (create1/ctl/wait), AF_UNIX-каналы с SCM_RIGHTS/backlog/ref-transfer, /bin/unixtest (4 стадии) — всё уже в дереве и работало E2E, но epoll был на «сырых» номерах 290/289, недоступных musl (реальный x86_64 ABI: epoll_ctl=233, epoll_wait=232, epoll_pwait=281), worklog-записи не было.
- ОЦЕНКА БАГОВ (директива пользователя «сначала чинить кардинальные»):
  * #exec-sh-c-stack — НЕ кардинальный: `sh -c` давал rc=127 из-за отсутствия /bin/sh symlink (POSIX not-found); `/bin/busybox sh -c` работал идеально (включая пайпы внутри). Часть 1 фикса (argv cap 8→32) уже лежала в дереве. Закрыт seeding'ом applet-symlinks.
  * #pipe-fd-switch — КАРДИАЛЬНЫЙ: любой ash-пайплайн ($(cat|wc)) вешал скрипт → ломал make/tar/gzip/configure = весь dwl-путь. Чинить первым.
- #pipe-fd-switch: ТРИ КОРНЯ (не один):
  1) Блокирующие wait-циклы ядра (pipe read/write, TCP read, epoll_wait, poll, select) делали голый sti/hlt/cli БЕЗ task_yield — а preempt выключен по умолчанию → fork-дети никогда не получали первый слайс. Заменено на task_yield() (hlt-дыхание при отсутствии READY сохранено внутри).
  2) #dup2-oldslot-leak: sys_dup2_impl освобождал старый слот ТОЛЬКО для fs-дескрипторов; pipe/net/unix энкодинг молча терял ссылку. ash-овский dup2(pipeW,1) внутри $() навсегда держал writer замещающего пайпа → EOF никогда не наступал. Найдено счётным следом [PINC]/[PDEC]: финал w=1 при всех мёртвых процессах. Фикс: fdref_dec_enc(proc_fds[newfd].fs_fd) для ВСЕХ классов.
  3) #stdio-redirect-fd: proc_fd_get() отбрасывал fd<3 (исторический guard эпохи «stdio = только консоль») → read(0) на файле после `< file` давал EBADF. Фикс: fd валиден, если несёт fs_fd>=0; консольные сентинелы отсекаются fs_fd<0-проверками. Попутно sys_lseek/sys_fstat перестали форсить консольный путь для редиректнутых файлов.
- #busybox-applet-links: 54 applet-symlinks в /bin (sh, wc, grep, tr, tar, gzip, find, sed, awk, ...) через fs_symlink; cat/ls/echo/uname остались за нативным nsh (он мультиколл). Для этого FS_MAX_CHILDREN 32→96, FS_MAX_NODES 256→512.
- #wl-substrate (новый слой): добавлены РЕАЛЬНЫЕ номера epoll (232/233/281; 289/290 теперь честные signalfd4/eventfd2), eventfd2 (счётчики, EFD_NONBLOCK), timerfd_create/settime/gettime (one-shot+периодика, tfd_pump на uptime-ms), signalfd4 (честный no-signal-yet), memfd_create+ftruncate+fd-backed MAP_SHARED mmap (страницы пула — реальные PA, шарятся между fork-процессами = wl_shm модель), readv/writev (уже были в дереве от потерянной сессии — подключены в таблицу LINUX_NR_*), getrandom (реальные байты xorshift+spin-counter; musl циклится на 0), futex (однопоточные честные семантики: WAIT с рассинхроном = -EAGAIN, спуриос-вейк), ppoll, dup3. Все классы встроены в fdref_dec/release_all_fds/fork-copy/close/poll/epoll-readiness. Сентинелы -95..-109 (epoll остался на -110..-113).
- /bin/wltest (новый, 7 стадий): eventfd+ppoll-probe, timerfd (poll-loop до срабатывания 120ms), readv/writev через пайп, memfd+ftruncate+mmap-shared-across-fork (ребёнок пишет паттерн — родитель читает через СВОЁ отображение), getrandom (32 байта ×2, >=8 различий), futex mismatch=-EAGAIN, epoll НА РЕАЛЬНЫХ номерах над eventfd+timerfd с двумя вейкапами. ИТОГ: WLTEST ALL-PASS. unixtest (с исправленными номерами) тоже ALL-PASS.
- Полная верификация: verify_all 8/9 (udp/httpget/wgetname/ping-gw/dns-hosts/tcp-server/jobcontrol/fullregress PASS; ping-name FAIL = известное физическое ограничение песочницы: нет cap_net_raw → slirp не форвардит интернет-ICMP); reg-base+reg-bb EXC=0; smoke: `cat|wc -l`=3, `$(echo|tr)`=ONE-TWO-THREE, sh -c, sort|uniq>file|wc -l, ls|head — всё работает.
- README (NullOs_README.md, 785→858 строк): Wayland-субстрат в intro, расширенный ABI-список (#wl-substrate), НОВЫЙ раздел "dwl / Wayland compositor readiness" (слои 0/1/2: субстрат готов; xkbcommon/pixman/libseat близки к бесплатным; дальше evdev-синтез, DRM-шим на fb.c, pixman-рендерер вместо EGL), обновлены ограничения (сеть теперь client+server TCP/DNS/retransmission; добавлены границы субстрата).
- Артефакт: download/NullOs-dwl-ready.zip (исходники NullOs + dwl-reference + логи верификации + worklog).

Stage Summary:
- #pipe-fd-switch ЗАКРЫТ (три корня: yield-циклы, dup2-oldslot-leak, stdio-redirect-fd) — командные подстановки/пайплайны/редиректы работают E2E; #exec-sh-c-stack оказался артефактом отсутствия /bin/sh (закрыт symlinks).
- #wl-substrate ДОСТАВЛЕН: полный libwayland-уровень поверх AF_UNIX+SCM_RIGHTS+epoll-real-ABI+eventfd/timerfd/memfd-shared-mmap; доказано двумя ALL-PASS проберами в ring 3.
- Платформа готова к Layer 2 dwl-порта: xkbcommon → pixman → libseat(seatd) → libinput(evdev-синтез) → DRM-шим (fb.c) → pixman-рендерер → dwl.
- Регрессия: verify_all 8/9 (ping-name — среда), reg EXC=0, оба пробера ALL-PASS, smoke зелёный.
- OPEN: DRM/KMS-шим, evdev-узлы, /proc, mprotect-реализация, W^X, EFD_SEMAPHORE, fs-truncate для shm_open-путей.

---
Task ID: 20
Agent: Super Z (main)
Task: Layer-2 Wayland substrate: mprotect/W^X, /proc, /dev/shm, EFD_SEMAPHORE; verify evdev+DRM (lost-session artifacts); regression; commit + README + ZIP.

Work Log:
- Ревизия обнаружила arteфакты ещё одной потерянной сессии: kernel/evdev.c + kernel/drm.c (DRM-lite на Bochs DISPI) + userland/{evtest,drmtest}.c — всё интегрировано (fd-классы в syscall.c, IRQ-зеркала в keyboard.c/mouse.c, /bin-регистрация), собиралось и НЕ было записано в worklog. Мнимая "синтаксическая ошибка" в evdev.c:45 оказалась артефактом рендеринга ([h съедался выводом).
- ОКРУЖЕНИЕ QEMU (полностью пересобрано, старые рецепты мертвы): qemu-system-x86_64 10.0.11 из /home/z/qemu-root (LD_LIBRARY_PATH=/home/z/qemu-root/root/usr/lib/x86_64-linux-gnu), grub-mkrescue требует libefivar/libefiboot (извлечены из deb bpo13 в /tmp/efx). Рецепты, которые НЕ работают: QEMU -kernel (нет PVH ELF note), grub-mkimage-core без модульного дерева (exception-6 спам после "Booting from DVD/CD"). РАБОЧИЙ рецепт: scripts/build_iso.sh — swap kernel.bin ВНУТРЬ последнего известного рабочего ISO (git blob c9b70f81) через xorriso -indev/-outdev -boot_image any replay. Драйвер прогонов: scripts/run_probe.py — QEMU с -monitor unix-сокетом (stdio-pipe монитор ЛОМАЕТ boot: stdin-EOF убивает QEMU, pipe-монитор вносит check_exception-спам), sendkey-печать, screendump, serial-лог.
- #kbd-set1-shift (найден через регресс: '|' и '>' печатались как '\', '.'): QEMU 10 ОТКЛОНЯЕТ переключение PS/2 в Set2 ("[KBD] Set2 SWITCH FAILED"), raw Set-1 продюсер глотал модификаторы НЕ обновляя kbd_state.modifiers → getchar мапил всё с пустой маской. ФИКС: продюсер пушит shift-события в ринг (и обновляет маску для set-2-совместимости), consumer трекает shift В ПОРЯДКЕ КОЛЬЦА (kb_getchar_shift — тот же паттерн, что уже был для Ctrl), Caps — тоггл на make, evdev-зеркало теперь видит make/break шифта. Эмпирика после фикса: A-B>C, D-E;F, G-H?I — всё печатается точно.
- mprotect(10): реальная правка PTE (W/NX) вместо return 0. #mprotect-cow-mask: fork конвертирует все user-страницы в RO+COW → запись ребёнка COW-резолвилась и ТИХО ОБНУЛЯЛА pin. Новый бит VMM_STICKY_RO (PTE bit 10): fork копирует pinverbatim (mark_shared сохранён — refcounts консистентны), vmm_handle_cow ОТКАЗЫВАЕТСЯ резолвить pinned-страницы, PROT_WRITE жадно unshare'ит COW-фреймы (с W бит CPU не фолит → оба процесса писали бы общий фрейм).
- #pf-kill-task: pf_halt останавливал ВСЮ машину даже для ring-3 фолтов. Теперь user-фолт, который не резолвится demand-paging/COW → [PF-KILL] + proc_exit_current(11) (SIGSEGV-семантика, wait4 видит 11<<8); kernel-фолты по-прежнему halt. Это фундамент честного W^X.
- #procfs: материализация на open (генератор → fs_write_file → обычный файловый путь): meminfo (PMM-статы), cpuinfo (cpuid brand), uptime, version, loadavg, osrelease, self/cmdline (t->name), self/maps (текст/ммп-бамп/стек). Хуки: open_full, stat_full_ex (busybox cat/ls делают stat ДО open!), shell-билтины (procfs_touch — cmd_cat/input_open_file зовут fs_open МИМО syscall-слоя). #procfs-cmdline: имя задачи = exec-путь (process_spawn), fork-дети наследуют.
- #shm-open-path: /dev/shm пулы по ключу-ПУТИ (musl shm_open = open("/dev/shm/name")): shmpool_t {path,size,npages,pages[]}, ftruncate ресайзит пул, mmap мапит фреймы пула (MAP_FIXED-замещение по правилам device-backed, devmap_register → fork-шаринг), O_TRUNC обнуляет, unlink уничтожает, fstat отдаёт размер пула. wl_shm-форма: двое мапят один путь независимо после форка.
- EFD_SEMAPHORE: чтение выдаёт 1 за раз с декрементом (флаг 0x1 в eventfd2).
- #mmap-fd-oob: proc_fds[fd_arg].fs_fd без bounds-check (fd=-1 от анонимных mmap = OOB-чтение ядра) → mfd_enc с проверкой до диспетчеризации.
- /bin/layertest (ring-3, 4 стадии): (1) mprotect W^X — fork-ребёнок пишет RO-страницу → умирает (status 11<<8), родитель читает/пишет соседние, ENOMEM на немапленном 0x2F000000 (ВНИМАНИЕ: 0x20000000 = PROC_MMAP_BASE — там живёт сам mmap!), EINVAL на prot=8; (2) /proc 5 файлов по маркерам; (3) /dev/shm — open(O_CREAT|O_TRUNC)+ftruncate+mmap, второй fd = ТЕ ЖЕ фреймы, fstat=8192, fork-ребёнок читает паттерн и пишет свой, родитель видит через своё отображение, unlink; (4) EFD_SEMAPHORE 3→1,1,1,-EAGAIN. ИТОГ: ALL-PASS.
- Регресс: wltest ALL-PASS, unixtest ALL-PASS, evtest ABI-PASS (name=event0 OK), drmtest exit=0 (PCI 1234:1111, DISPI id=b0c5, DUMBCREATE 1024x768x32 ×2, SETCRTC, pageflip, text mode restored), busybox smoke: echo|tr, cat /proc/meminfo|head -n 2 (MemTotal 524160), cat /proc/cpuinfo|grep model (QEMU Virtual CPU), echo>f1.txt;cat|wc -c.

Stage Summary:
- Layer 2 Wayland-субстрата ЗАВЕРШЁН и верифицирован E2E: evdev + DRM-lite (потерянная сессия) подтверждены проберами, mprotect/W^X + pf-kill, /proc, /dev/shm-пулы, EFD_SEMAPHORE — добавлены и покрыты /bin/layertest ALL-PASS.
- Инфраструктура прогонов восстановлена с нуля: build_iso.sh (swap-в-рабочий-ISO) + run_probe.py (unix-HMP + sendkey + screendump) — воспроизводимые E2E-прогоны без участия человека.
- Шесть новых корней: #kbd-set1-shift, #mprotect-cow-mask, #pf-kill-task, #procfs (+#procfs-cmdline), #shm-open-path, #mmap-fd-oob.
- README: Layer 2 переведён в "done", Layer 3 = портирование библиотек (xkbcommon → pixman → libseat → libinput → DRM-glue → pixman-renderer → dwl).
- OPEN: PROT_NONE деградирует в RO (demand-pager пересоздал бы страницу), без-VMA список (ENOMEM per-page), /proc ls показывает только однажды материализованное, munmap не unref'ит COW-фреймы (гигиена refcounts, до-задачная), signal-доставка нет.

---
Task ID: 21
Agent: Super Z (main)
Task: Маршрут libseat → libinput с приоритетной охотой на #bb-spawn-race (мешает ash-скриптам); рекогносцировка механизма ELF-проб.

Work Log:
- РЕКОГНОСЦИРОВКА: механизм ELF-проб = Makefile-правило (crt0.o + userland/link.ld @0x400000) → embed _elf_start/_len в kernel image → fs_write_file("/bin/...") в kernel.cpp при загрузке; 54 applet-symlinks через fs_symlink. Новые пробы добавляются тремя строками.
- ОКРУЖЕНИЕ ВОССТАНОВЛЕНО С НУЛЯ: /home/z/qemu-root и /tmp погибли между сессиями. Пересобрано scripts/setup_env.sh: qemu-system-x86 10.0.11 (Debian trixie debs → dpkg -x, ~60 пакетов; докачаны libcapstone5, libfuse3-4, libkmod2, libndctl6, libnl-{3,route}-200), xorriso 1.5.6, seabios+vgabios скопированы в share/qemu (symlinks запрещены песочницей), ipxe-qemu ROMs (efi-*.rom/pxe-*.rom), gdb 16.3 в /home/z/gdb-root (+libdw/libelf/libdebuginfod). qemu-img из qemu-utils.
- #bb-spawn-race ВОСПРОИЗВЕДЁН: ash-стресс (последовательные спавны/подстановки/вложенные ash -c/редиректы/фоновые & + wait) молча умирал в СЛУЧАЙНЫХ точках (brk, fcntl, старт ash) — QEMU reset, серийник обрывался без EXC. Две пробы = две разные точки смерти = гонка.
- ИНСТРУМЕНТЫ ОХОТЫ: NULOS_QEMU_D=int,cpu_reset (run_probe.py) дал сигнатуру: timer IRQ (v=20) из ring3 → #SS(v=0c,e=0) при доставке → #DF → triple fault. gdb-watchpoint (tss_watch.py, late attach) на TSS.rsp0 (VA 0x4040A084) доказал: СТРУКТУРА TSS не портилась — порчен ВИД. tss_probe.py (виртуальное vs физическое чтение через HMP) поймал расхождение: PT[10] для VA 0x40A000 в CR3 ash указывал на ТЕКСТОВУЮ страницу busybox (0x6a9025).
- **#tss-identity-overlap (КОРЕНЬ №1)**: дескриптор TSS в gdt.c паковал базу маской 0xFFFFFF, биты 56-63 (base[31:24]) не писались → TR linear = 0x40A080 (identity-окно). ЛЮБОЙ образ ≥4MB (busybox 0x400000..0x4F7450) затеняет TSS через сплит-PT спавна; fake-rsp0 = байты образа → канонично-немапленный = #PF при доставке, ноль = #SS → #DF → triple fault. «Гонка» = рулетка байтов образа (какие фреймы достались). ФИКС: полная упаковка base (байт 7). Проверка HMP: TR = 0x4040a080 ✓ (kernel .high, несбиваемый). Попутная ловня: первая попытка фикса положила байт на биты 52-59 — long-mode TSS-дескриптор держит base[31:24] на 56-63; дамп GDT-кворда через xp это показал.
- **#sigsuspend-spin (КОРЕНЬ №2)**: после фикса TSS стресс дошёл до BG-фазы и встал: бесконечный поток «unimplemented nr=130» = rt_sigsuspend. flat -EINTR тоже спиннил (ash доверяет EINTR как пойманному SIGCHLD и возвращается в sigsuspend БЕЗ wait4). Реализована **#sigchld-mach**: rt_sigaction(13) хранит handler+SA_RESTORER для SIGCHLD=17 (musl act={handler,flags,restorer,mask}); rt_sigsuspend(130) блокируется (task_yield) до события ребёнка (proc_wait_event: зомби/живые/нет) и ДОСТАВЛЯЕТ патчем живого трэп-фрейма (RIP=handler, RDI=17, RSP=sigframe на 512 ниже user rsp, pretcode=musl-restorer, 160B копия фрейма); rt_sigreturn(15) восстанавливает из блоба (формат наш, само-консистентный); SIGCHLD РЕГЕНЕРИРУЕТСЯ пока есть незабранные зомби (Linux-семантика); execve сбрасывает sig-состояние; выход ребёнка лэтчит pending родителю. sigframe_ensure_pages пре-мапит страницы под фрейм (kernel-записи не деманд-фолтят).
- **#zombie-reap-race (КОРЕНЬ №3)**: scheduler_reap_finished деактивировал зомби-ПРОЦЕССЫ (state=FINISHED) тиком после выхода — wait4/proc_wait_event не видели фоновых детей (доставка №1-2 выигрывала гонку, дальше — шторм). ФИКС: зомби с живым родителем принадлежит ТОЛЬКО wait4; сироты добираются proc_reap_orphan (MM-teardown + слот).
- **#frame-ptr-park (КОРЕНЬ №4)**: глобальный syscall_frame_ptr затирался чужими syscall-входами, пока задача стояла park внутри своего syscall — sigchld_deliver патчил мёртвый фрейм ребёнка (доставка №2 не доезжала до handler'а, in_handler залипал). ФИКС: per-task saved_frame save/restore в sched_fds_on_switch (все switch-сайты: R2C enter/exit, preempt, exit-away, yield).
- ЛОВУШКА СБОРКИ (дважды): вставка полей в task_t + неполные header-deps в Makefile = stale .o со старой раскладкой (R2C REFUSED с pml4=0x421686d8 = parked rsp по старому оффсету). ЛЕЧЕНИЕ: make clean && make ПОСЛЕ каждого изменения task_t. qemu-img потерялся → qemu-utils.
- ВЕРИФИКАЦИЯ: полный ash-стресс ДОШЁЛ ДО BB-RACE-END: 12 последовательных spawn, подстановки (echo|tr), cat|wc пайпы, 5× вложенных ash -c, 3× redirect-wc, 4× фоновых echo BG-& + wait, ls|head — ноль тройных фолтов/PF-KILL; ash вернулся в промпт. РЕГРЕСС ЗЕЛЁНЫЙ: reg-base + reg-bb EXC=0; WLTEST ALL-PASS; UNIXTEST ALL-PASS; LAYERTEST ALL-PASS (segv в t1 = ожидаемый W^X-тест); drmtest PASS (modeset+dumb+text-restore); evtest клавиатурный ABI-PASS (мышь PARTIAL = среда: -display none без PS/2 aux, «No PS/2 mouse detected» — не регрессия). Сеть попутно подтверждена (каждая проба начиналась с httpget-феча скрипта).
- README: раздел «ash readiness (closed before Layer 3)» — четыре корня + верификация; «Honest gaps» уточнён (сигналы = только SIGCHLD-доставка, достаточно для job control ash).
- Артефакт: download/NullOs-ash-ready.zip (см. make_zip.sh).

Stage Summary:
- #bb-spawn-race ЗАКРЫТ: четыре корня (#tss-identity-overlap, #sigsuspend-spin+#sigchld-mach, #zombie-reap-race, #frame-ptr-park). Ash-скрипты с фоновыми задачами и пайпами работают E2E.
- Минимальная сигнальная машина SIGCHLD — первая честная доставка сигналов в ring 3 (handler+restorer+sigreturn, self-consistent frame).
- Инфраструктура охоты: gdb-watchpoint на живой VM (late-attach), HMP virt-vs-phys сравнение, -d int,cpu_reset трасса — новый стандарт для «тихих» смертей.
- Механизм ELF-проб документирован; следующим шагам (libseat → libinput) ничего не мешает: субстрат Layer 2 зелёный, ash готов как драйвер сборки/тестов.
- OPEN: libseat-lite, libinput-lite, /bin/seatprobe (Layer 3); ls выносит ANSI-коды (косметика); мышь в evtest = ограничение песочницы.

---
Task ID: 22
Agent: Super Z (main)
Task: Маршрут Layer 3: libseat-lite → libinput-lite → /bin/seatprobe (по подтверждению пользователя; «всё рекогносцировано, ничего не мешает»).

Work Log:
- ОКРУЖЕНИЕ СНОВА ПЕРЕСОБРАНО (погибло между сессиями): scripts/setup_env.sh + докачаны libcapstone5/libfuse3-4/libkmod2/libndctl6/libnl-{3,route}-200; ВАЖНО: qemu-system-data ставит bios-256k.bin/vgabios как СИМЛИНКИ (песочница их не создаёт) → ROM-ы absent → «could not load PC BIOS»; лечится копированием из deb seabios/vgabios/ipxe-qemu (cp -L). qemu-img из qemu-utils.
- Рекогносцировка ABI: evdev-кольца (EV_KEY/EV_REL/EV_SYN + EVIOCGVERSION/ID/NAME), /dev/dri/card0 (dumb/modeset/pageflip), реальные номера epoll (291/233/232)/eventfd2(290), регистрация проб (userprogs.S .incbin + kernel.cpp fs_write_file + Makefile-правила).
- libseat-lite (userland/lib/libseat_lite.{h,c}): builtin-бэкенд с РЕАЛЬНОЙ поверхностью libseat 1.x: open_seat сразу вызывает enable_session (единственная always-active сессия), get_fd = eventfd-wakeup, dispatch = poll(timeout), open_device с флагами libinput (O_RDWR|O_CLOEXEC|O_NONBLOCK, RDONLY-fallback), честный bad-path. seatd-over-socket — задокументированный follow-up (SCM_RIGHTS уже в субстрате).
- libinput-lite (userland/lib/libinput_lite.{h,c}): udev_create_context (udev игнорируется — прямой скан /dev/input/event*), assign_seat открывает ноды ЧЕРЕЗ interface-колбэки = через libseat (точная dwl-цепочка), классификация по EVIOCGBIT (kbd = бит KEY_ESC, pointer = бит REL_X), ВНУТРЕННИЙ epoll по fd устройств (get_fd epoll-корректен для event-loop wlroots), события: DEVICE_ADDED; EV_KEY → KEYBOARD_KEY; дельты REL_X/Y аккумулируются между SYN_REPORT → один POINTER_MOTION на кадр; кнопки → POINTER_BUTTON; REL_WHEEL → POINTER_AXIS(wheel). Значения энумов 300/400/402/403 — апстримные. SPSC-очередь событий.
- ЯДРО: EVIOCGBIT(EV_SYN/KEY/REL/ABS)+EVIOCGKEY в kernel/evdev.c — per-slot маски (kbd = таблица keycodes, mouse = BTN 0x110-0x112 — нужны 96 байт окна EV_KEY, старого 32-байтного не хватало!), zero-extend до запрошенной длины, cap 256. Ловня краёв: EVIOCGBIT(EV_SYN,1) — нечётное окно копирует младший байт.
- ЛОВНЯ СБОРКИ: Edit-инструмент превратил табы Makefile в 8 пробелов («missing separator») → git checkout + python-патчер с явными \t (scripts/patch_makefile_seatprobe.py). libinput_lite.c компилируется БЕЗ -mno-sse (API возвращает double как настоящий libinput; ring-3 FPU/SSE включены — kernel/fpu.c FXSAVE/FXRSTOR), иначе «SSE register return with SSE disabled».
- /bin/seatprobe (userland/seatprobe.c, линк из 3 .o): t1 SEAT (жизненный цикл + card0/event0/event1 + bad-path отклонён), t2 CTX (2× DEVICE_ADDED, event0 caps=KEY, event1 caps=+POINTER), t3 KBD (poll→dispatch→KEYBOARD_KEY: sendkey a=30/b=48 от харнесса), t4 PTR (PARTIAL: без PS/2 aux в -display none события мыши невозможны — прецедент evtest).
- ДРАЙВЕР scripts/seatprobe_e2e.py (unix-HMP, sendkey-инъекция, xp-снимки). ЛОВНИ: HMP-вывод unix-монитора приходит НА СОКЕТ (не в stdout) — xp-дампы надо сохранять из drain; пробел в sendkey = «spc»; poll(nfds=0) в ядре = EINVAL → цикл пробы пролетал мгновенно и ключи уходили в шелл → msleep через nanosleep(35). Бэклог evdev-кольца не портит проверку: refs 0→1 сбрасывает кольцо (первое открытие в t1).
- РЕГРЕСС: seatprobe ALL-PASS; wltest/unixtest/layertest ALL-PASS; drmtest exit=0 (modeset+dumb+text-restore; 0xff в xp-снимках после DISPI-modeset — артефакт декодера, не сбой); evtest exit=0; ash-скриптовый пайплайн E2E (echo → /t.sh; busybox sh /t.sh; cat /proc/meminfo|head -n 1 → MemTotal: 524160 kB). Ловня regress.sh: мапа sendkey не знала «;»/«'» — дополнена; «sh -c 'строка'» kernel-shell не квотирует → ash-скрипты через файл.
- README: раздел «Layer 3» переписан: libseat/libinput = DONE (lite), полный отчёт + 4 стадии пробы.
- Артефакт: download/NullOs-layer3-ready.zip (git-archive + dwl-ref + логи + worklog).

Stage Summary:
- Layer 3substrate ГОТОВ: libseat-lite + libinput-lite с апстримной поверхностью API; dwl/wlroots-цепочка open_restricted→libseat_open_device→evdev доказана E2E пробой /bin/seatprobe (ALL-PASS).
- Ядро получило EVIOCGBIT/EVIOCGKEY — последний отсутствовавший ioctl-класс для libevdev/libinput-классификации.
- Следующее по маршруту: xkbcommon → pixman → DRM-glue/wlroots-DRM-бэкенд (ядро уже умеет) → pixman-рендерер → dwl; ptr-события в песочнице без PS/2 aux остаются best-effort (или QEMU-сторона: usb-tablet не вариант — нет USB-стека).
- OPEN: seatd-lite over socket (медиация fd), EVIOCGABS/MT для тачпадов, libinput_device_get_udev_device (возвращает NULL — wlroots-порт должен брать sysname), ls с ANSI-кодами (косметика).

---
Task ID: 17
Agent: Super Z (main, audit-session)
Task: Полный аудит кодовой базы NullOs (layer3) по запросу владельца: найти баги, исправить, предложить идеи развития.

Work Log:
- Параллельный аудит четырёх зон ядра ( Explore-агенты): mm/аллокаторы, scheduler/proc/syscall, net/драйверы, VFS/FAT32/ext4/userland.
- Личная верификация всех критических находок по исходникам; каждая правка помечена комментарием FIX(#тег).
- Исправлено 15 файлов ядра + mm.h, ~700 строк, ядро пересобрано gcc/ld успешно (build/kernel.bin).
- КРИТИЧЕСКИЕ: #pmm-free-never (guard carve-зоны без верхней границы — все фреймы ≥128MB никогда не освобождались, PMM истощался безвозвратно); #uaccess-supervisor-hole (user_range_ok не отсекал супервайзорные identity-окна [4MB,1GB) и MMIO [2GB,4GB) из пользовательских PML4 — ядро читало/писало свою кучу по указателю из ring 3); #icmp-total-len-smash (total_len из пакета без сверки с длиной кадра — ~64KB запись в 1480-байтный стековый буфер); #udp-send-overflow (кап 1500 вместо 1472 — 28 байт за границу стека); #ioctl-arg-hole (evdev/drm ioctl arg без валидации — запись в произвольную память ядра); #getcwd-raw-ptr; #execve-raw-path/argv; #fd-oob-epollwait/timerfd/ftruncate (proc_fds[fd] без bounds — OOB/halt); #elf-offset-wrap (целочисленный перенос в проверках phdr); #ext4-path-overflow (рекурсивный конкат путей без границ — смэш стека ring 0).
- ВЫСОКИЕ: #getphys-nx (бит NX в возвращённом PA — утечка фрейма при каждом munmap/MAP_FIXED); #conread-no-yield (голый sti/hlt/cli вместо task_yield в консольном read — голодание задач и сломанный ^Z); #dup-net-catchall (dup/dup2 гнали все отрицательные fd ≤ -10 в net-путь); #dup2-ref-before-fail (декремент refcount цели до ошибки — преждевременный teardown pipe); #rx-ring-guard-udp (мёртвый guard после 16 дейтаграмм — перезапись непрочитанных); #rx-error-jam (ROK=0 кадры навсегда заклинивали RX-кольцо RTL8139); #lfb-pages-per-scanline (индексация страниц по строке — NULL-deref при pitch != 4096); #fat-eoc-link (FAT-запись по индексу EOC-сентинела — порча сектора/потеря цепочки); #rename-cycle (fs_rename без проверки циклов — вечный hang fs_get_path).
- СРЕДНИЕ/НИЗКИЕ: #build-img-leak (утечки фреймов на всех путях ошибок build_user_image + утечка pa-массивов на каждый exec); #brk-oom-leak; #wait4-echild; #mouse-midpacket-resync; #fs-sparse-leak (дыры sparse-write отдавали содержимое кучи ядра); #fat-size-truncate; #dns-label-overflow; #https-req-overread; #tcp-demux-no-ip; #evdev-modifier-mirror; #r2c-bounds-order; #ping-octet; #dnsrx-overread; #rxraw-overread; #pmm-carve-unbounded (масштабирование зоны carve на RAM > 1.4GB).
- Новая инфраструктура: vmm_range_is_user() (обход таблиц активного PML4 с проверкой VMM_USER), syscall_user_range_ok() (экспорт для драйверов), ext4_path_join/append (ограниченная сборка путей), общая метка build_fail в build_user_image с перемоткой PTE.

Stage Summary:
- Ядро собирается: gcc/ld, build/kernel.bin 4.4MB, без новых предупреждений.
- Закрыто ~30 багов, из них 11 критических (в т.ч. 5 дистанционно/из ring 3 эксплуатируемых примитивов порчи памяти ядра).
- Отчёт о всех фиксах: NullOs/nulos/NullOs/BUGFIXES-AUDIT.md; исправленный код — в том же дереве.
- НЕ исправлено (осознанно, требует загрузочных тестов на QEMU): rt_sigreturn возвращает номер syscall (130) вместо -EINTR; FPU-контекст ребёнка при fork (fxrstor нет); kill(self, SIGSTOP) в однозадачном случае; сироты-зомби вне task_create; SMP-окно sti до lidt на AP; race refcount при активных AP; сигнал-смерть кодируется как обычный exit-код. Всё задокументировано в BUGFIXES-AUDIT.md.

---
Task ID: 23
Agent: Super Z (main)
Task: Пивот dwl→labwc (директива владельца: «сначала вместо пункта 1 (TCP) заменяем dwl на Labwc — с ним банально легче работать и понимать, что да как»; git clone https://github.com/labwc/labwc.git; «тесты в QEMU можешь проводить сам»).

Work Log:
- ОКРУЖЕНИЕ СНОВА ПЕРЕСОБРАНО (погибло между сессиями, паттерн Task 21-22): scripts/setup_env.sh — QEMU 10.0.11 + xorriso 1.5.6 из trixie-debs (35 пакетов, dpkg -x → /home/z/qemu-root), ROM-ы поверх симлинков (cp -L). НОВОЕ против прошлых сессий: QEMU не находит bios-256k.bin по скомпиленному префиксу → ЛЕЧИТСЯ флагом `-L /home/z/qemu-root/usr/share/qemu/` в каждом запуске.
- ВОССТАНОВЛЕНИЕ АРТЕФАКТОВ: make clean в upload-дереве снёс build/nullos.iso + disk.img → извлечены из upload/NullOs-layer3-ready.zip обратно. Свежая пересборка ядра: kernel.bin воспроизводим (совпадает с лежавшим в дереве до-чистки).
- ISO-рецепт восстановлен: scripts/build_iso.sh — подмена kernel.bin внутрь известно-рабочего ISO (xorriso -indev/-outdev -boot_image any replay -update). Драйвер прогонов: scripts/run_probe.py — unix-HMP сокет (stdio-монитор ломает boot), sendkey-печать, чтение VGA через `xp /4096bx 0xb8000`, serial→файл, вердикты по `[PROC] pid=N exit=C`. ГЛАВНАЯ ЛОВНЯ КРОСС-СЕССИЙ: при SIGKILL QEMU буфер serial-файла ТЕРЯЕТСЯ → kill() теперь сначала HMP `quit` (честный flush), затем SIGKILL. Без этого «мертвеющий» serial вел ложным следам.
- БУТ-ВАЛИДАЦИЯ АУДИТА (Task 17 фиксы НИ РАЗУ не бут-тестировались!): boot OK 7.6s, hello=42 — но wltest t4 FAIL mmap, layertest exit=1, drmtest «FAIL mmap A». ОХОТА: `[MMAP] addr=0 len=2000 prot=3 flags=1` в serial БЕЗ ветки memfd → возврат до vmm_map.
  * КОРЕНЬ #1 (#audit-mmap-regression): фикс аудита #uaccess-supervisor-hole подключил vmm_range_is_user() и к ЦЕЛЯМ мапинга; каждый PML4 наследует supervisor identity-alias [4MB,1GB), а mmap-регион ОС = 0x20000000 — внутри окна → walker режет собственный mmap ОС (-ENOMEM на все fd-backed mmap). ФИКС: mmap_range_ok() (статические правила) в 4 ветках sys_mmap_impl; walker остаётся для буферного I/O.
  * КОРЕНЬ #2 (guard, до-аудиторская дыра заодно): MAP_FIXED замещение отдавало alias-PA в pmm_free_page = живая RAM ядра в PMM (double-alloc; huge-PDE alias даже не unmap'ится). ФИКС: vmm_leaf_is_user() — фрейм освобождается только если старый leaf VMM_USER.
  * КОРЕНЬ #3 (#fs-size-symlink, до-аудиторский): fs_file_size() NO-follow, fs_open() follow → `elf /bin/sh script` падал голым -1 (symptom «Failed to create process», serial до [SPAWN] enter). Прошлые харнессы обходили, вызывая busybox напрямую. ФИКС: follow-резолвер.
- ЛОВНИ ХАРНЕССА (задокументированы): a/b-инъекция seatprobe оставляет клавиши в консольном кольце → следующая команда склеивается («abelf /bin/nettest») → холостой ret перед каждой командой; modeset-пробы затирают консоль без перерисовки промпта → ret-тычок по таймауту; nettest коннектится на 10.0.2.2:9000 (не 40001 — это локальный bind); httpget exit=42=OK; NIC поднимается ТОЛЬКО `drivers init N` (rtl8139 = индекс 5) — сетевые пробы без bring-up молча таймаутятся.
- ПОЛНАЯ РЕГРЕССИЯ (после фиксов): battery (hello=42, wltest/unixtest/layertest exit=0, drmtest exit=0 c `[MMAP] drm-dumb ... ret=20000000`) + scenario2.py (NIC up, seatprobe ALL-PASS t1-t3 с sendkey-инъекцией, nettest UDP=42, httpget HTTP=42, ash-smoke 4/4 МАРКЕРОВ через сам /bin/sh — пайпы/подстановки//proc/фон+wait, spawn 3 OK, multi 3 OK изолированно). ЛОЖНЫЙ FAIL multi в сценарии = артефакт тайминга харнесса (изолированный прогон зелёный).
- LABWC-ПИВОТ: склонирован labwc (github, HEAD dda4ef9 = 0.20.2) → nullos-work/labwc (3MB, .git срезан). РЕКОГНОСЦИРОВКА: wlroots-0.20 (>=0.20.1<0.21), wayland-server>=1.22, wayland-protocols>=1.39, xkbcommon, libdrm, libxml2, glib-2.0, cairo+pangocairo, libpng, libinput>=1.26, pixman; опции выключения: xwayland/svg(icon=libsfdo)/labnag/nls. Карта использования: pango ТОЛЬКО через src/common/font.c (обёртка ~15 вызовов) → pango-lite на встроенном bitmap-шрифте; glib ~8 точек (spawn shell-parse, strcasecmp, dir) → glib-lite ~200 строк; cairo image-surface без freetype/fontconfig (текст не через cairo-API); libxml2 reader-mode + libpng — musl-static. Структура: 19.6 KLOC / 40+ модульных файлов против dwl 3.2 KLOC одним файлом — обоснование владельца «легче работать и понимать» подтверждено инспекцией.
- README: секция «dwl readiness» → «labwc readiness» (пивот-абзац + обоснование), Layer 3 переписан (12-шаговый порядок портирования labwc-цепочки: wayland-server+libffi → xkbcommon → pixman → libdrm-lite → wlroots-0.20 minimal (backends=drm,libinput,headless; renderers=pixman) → glib-lite → xml2/png → cairo-lite → pango-lite → labwc borderless-first), добавлен вопрос wlroots-libinput (нет uevents → path-mode патч или расширение libinput-lite), НОВАЯ секция «Post-audit boot validation (2026-09-16)», роадмап: [ACTIVE] labwc bring-up, TCP-доработки отложены решением владельца.
- BUGFIXES-AUDIT.md: дополнение «бут-валидация аудита» — #audit-mmap-regression (+ownership guard) и #fs-size-symlink с симптомами/корнями/фиксами; верификация; примечания к тестированию (NIC bring-up, порт 9000).
- Артефакт: download/NullOs-labwc-pivot.zip (NullOs-fixes + labwc-ref + dwl-ref + worklog + логи верификации).

Stage Summary:
- Аудит Task 17 ПОЛНОСТЬЮ БУТ-ВАЛИДИРОВАН: найдены и закрыты 2 его регресса (#audit-mmap-regression — walker на целях мапинга; #fs-size-symlink — до-аудиторский) + guard против отдачи alias-фреймов в PMM.
- Регрессия ВСЯ ЗЕЛЁНАЯ: ядро/субстрат (wltest/unixtest/layertest/seatprobe/drmtest), сеть (UDP echo=42, TCP HTTP=42), ash-скрипты через /bin/sh, kernel-таски (spawn/multi). Layer 3 готов к labwc-маршруту.
- Пивот dwl→labwc оформлен: reference-дерево labwc 0.20.2 в проекте, README-роадмап переписан под 12-шаговую labwc-цепочку с lite-шимами (glib-lite, pango-lite на bitmap-шрифте, cairo image-only, libdrm-lite) и wlroots-0.20-minimal конфигурацией.
- Следующая сессия: wayland-server+libffi как musl-static userland-либа (Слой 0 субстрат уже готов), проба /bin/wlprobe (socket+wl_display_connect каркас).
- OPEN: pango-lite (bitmap-рендер), glib-lite, wlroots libinput path-mode патч, XDG_RUNTIME_DIR env на exec, increase_nofile_limit (setrlimit) заглушка.
# NullOs Multi-Agent Worklog (продолжение; полная история — upload/NullOs-labwc-pivot.zip:worklog.md)

> Точка входа сессии: Фаза «labwc bring-up». Пользователь подтвердил маршрут:
> xkeyboard-config → glib-lite → cairo → pango → labwc; drm-бэкенд (seatd/libseat +
> libdisplay-info); wlroots в QEMU на virtio-gpu — по желанию.
> Песочница пересобрана (паттерн Task 21-23): окружение восстановлено заново.

---
Task ID: 24
Agent: Super Z (main)
Task: Фаза A+B — восстановить окружение (QEMU + musl-тулчейн), собрать wayland-стек
(musl-static: libffi, expat, wayland client+server+scanner, wayland-protocols),
провести E2E wlprobe vs fakecomp через реальный unix-сокет (Milestone-1 Layer-0).

Work Log:
- Развёрнуто рабочее дерево: nulos/NullOs (свежие исходники из upload/NullOs-labwc-pivot.zip,
  с фиксами аудита и #audit-mmap-regression), базовый ISO build/nullos.iso (7.4MB, known-good)
  восстановлен из layer3-архива; src/labwc (референс 0.20.2); sandbox-scripts → scripts/
  (setup_env.sh, build_iso.sh, run_probe.py).
- QEMU-окружение восстановлено: scripts/setup_env.sh → QEMU 10.0.11 + xorriso 1.5.6 в
  /home/z/qemu-root, ROM-ы cp -L поверх симлинков (та же ловня, что в Task 22-23).
- musl 1.2.5 пересобран (scripts/02-build-musl.sh): musl-gcc, hello-static = "musl-static-ok".
- libffi 3.4.7 (scripts/03-build-libffi.sh): UAPI-шимы tools/musl/include/linux/{limits,types}.h
  (ЛОВНЯ: tramp.c требует ОБЕ — прошлой сессии хватало limits.h), LDFLAGS=-static,
  ffi_call(19,23)=42 ✓, stage/lib/libffi.a ✓.
- gitlab.freedesktop.org закрыт Anubis-антиботом («Oh noes!») → ЛОВНЯ КРОСС-СЕССИЙ: источники
  freedesktop брать ТОЛЬКО из Debian pool (deb.debian.org/debian/pool/...): wayland_1.23.1.orig.tar.gz,
  wayland-protocols_1.47.orig.tar.xz.
- wayland-стек (scripts/04-build-wayland-stack.sh): expat 2.7.1 (autotools) → wayland 1.23.1
  (meson, -Dtests/-Ddocumentation/-Ddtd_validation=false) → wayland-protocols 1.47 (-Dtests=false;
  ЛОВНЯ: tests-поддир требует C++ и wayland-scanner как .pc-депу). ЛОВНЯ: meson на Debian-хосте
  кладёт либы в lib/x86_64-linux-gnu → ВСЕГДА --libdir=lib. Итог: stage/{lib/libwayland-{client,server,cursor,egl}.a,
  bin/wayland-scanner (statically linked), share/wayland-protocols}.
- wlprobe.c (wl_display_connect каркас: connect→registry→roundtrip→globals) и fakecomp.c
  (wl_shm global + сокет-сервер) в nulos/NullOs/userland/, собраны musl-static.
- ОТЛАДКА E2E (3 гипотезы): клиент получал EOF→EPIPE при живом сервере. Голый unix ping-pong
  musl-static — OK (слой сокетов чист). Серверная сторона WAYLAND_DEBUG показывала идеальную
  обработку (get_registry→global, sync→done) БЕЗ доставки клиенту. КОРЕНЬ: мой ручной цикл
  wl_event_loop_dispatch() без flush — в libwayland 1.23 флаш делает сам wl_display_run()
  (wl_display_flush_clients перед каждым dispatch), а WRITABLE-подписка на fd клиента
  ставится ТОЛЬКО из wl_display_flush_clients при EAGAIN. ФИКС: wl_display_flush_clients()
  в цикле fakecomp. (Урок для портов: ручной цикл = flush вручную.)
- ИТОГ E2E: wlprobe: connect+roundtrip OK globals=1 shm=1, exit=0; fakecomp: served clean.
  [MILESTONE] wayland Layer-0 E2E PASS.

Stage Summary:
- Субстрат Layer-0 musl-static готов: musl → libffi → expat → wayland(client+server+scanner)
  + wayland-protocols в stage/; E2E по реальному unix-сокету зелёный.
- wlprobe/fakecomp станут /bin/-пробами ОС (wlprobe уже в userland/).
- Дальше по роадмапу: xkbcommon+xkeyboard-config → pixman+libdrm+libdisplay-info →
  libseat/libinput-lite .pc-обёртки → wlroots-0.20-minimal.
- OPEN (из рекогносцировки сессии): auxv в elf.c пуст (только AT_NULL) — добавить
  AT_RANDOM/AT_PHDR/AT_PAGESZ; envp exec пустой — XDG_RUNTIME_DIR решается setenv в labwc;
  DRM-шиму ядра надо расширить (GET_CAP/SET_CLIENT_CAP/ADDFB2/RMFB/properties) под wlroots;
  signalfd — честный no-op (graceful exit под вопросом, kill -9 работает).
