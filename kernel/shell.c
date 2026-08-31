#include "../include/shell.h"
#include "../include/vga.h"
#include "../include/keyboard.h"
#include "../include/mm.h"
#include "../include/fs.h"
#include "../include/timer.h"
#include "../include/buddy.h"
#include "../include/kernel.h"
#include "../include/string.h"
#include "../include/serial.h"
#include "../include/rtc.h"
#include "../include/scheduler.h"
#include "../include/ata.h"
#include "../include/elf.h"
#include "../include/apic.h"
#include "../include/pci.h"
#include "../include/fb.h"
#include "../include/klog.h"
#include "../include/vt.h"
#include "../include/mouse.h"
#include "../include/fat32.h"
#include "../include/rtl8139.h"
#include "../include/smp.h"
#include "../include/ac97.h"
#include "../include/tests.h"
#include "../include/net.h"
#include "../include/storage.h"

// Global shell state
shell_t shell;

// ============================================================
// Environment variables
// ============================================================

#define ENV_MAX_VARS  32
#define ENV_NAME_MAX  64
#define ENV_VAL_MAX   256

typedef struct {
    char name[ENV_NAME_MAX];
    char value[ENV_VAL_MAX];
    bool active;
} env_var_t;

static env_var_t env_vars[ENV_MAX_VARS];

static void env_init(void) {
    kmemset(env_vars, 0, sizeof(env_vars));
    // Set defaults
    kstrcpy(env_vars[0].name, "HOME");
    kstrcpy(env_vars[0].value, "/");
    env_vars[0].active = true;

    kstrcpy(env_vars[1].name, "USER");
    kstrcpy(env_vars[1].value, "null");
    env_vars[1].active = true;

    kstrcpy(env_vars[2].name, "SHELL");
    kstrcpy(env_vars[2].value, "/bin/nullsh");
    env_vars[2].active = true;

    kstrcpy(env_vars[3].name, "PATH");
    kstrcpy(env_vars[3].value, "/bin:/usr/bin");
    env_vars[3].active = true;

    kstrcpy(env_vars[4].name, "TERM");
    kstrcpy(env_vars[4].value, "null-vga");
    env_vars[4].active = true;
}

static env_var_t* env_find(const char* name) {
    for (int i = 0; i < ENV_MAX_VARS; i++) {
        if (env_vars[i].active && kstrcmp(env_vars[i].name, name) == 0) {
            return &env_vars[i];
        }
    }
    return NULL;
}

static env_var_t* env_find_free(void) {
    for (int i = 0; i < ENV_MAX_VARS; i++) {
        if (!env_vars[i].active) return &env_vars[i];
    }
    return NULL;
}

// Expand $VAR references in a string into output buffer.
// Returns number of chars written (excluding NUL).
// If a variable is not found, the $NAME is left as-is.
static int env_expand(const char* input, char* output, u32 out_size) {
    u32 oi = 0;
    for (u32 i = 0; input[i] && oi < out_size - 1; i++) {
        if (input[i] == '$' && input[i + 1] != '\0') {
            // Try to read variable name
            u32 ni = i + 1;
            while (input[ni] && (kisalnum(input[ni]) || input[ni] == '_')) ni++;

            if (ni > i + 1) {
                // Extract variable name
                char varname[ENV_NAME_MAX];
                u32 vlen = ni - (i + 1);
                if (vlen >= ENV_NAME_MAX) vlen = ENV_NAME_MAX - 1;
                kmemcpy(varname, input + i + 1, vlen);
                varname[vlen] = '\0';

                env_var_t* ev = env_find(varname);
                if (ev) {
                    u32 vsize = (u32)kstrlen(ev->value);
                    if (oi + vsize >= out_size) vsize = out_size - oi - 1;
                    kmemcpy(output + oi, ev->value, vsize);
                    oi += vsize;
                    i = ni - 1; // skip past variable name
                    continue;
                }
                // Variable not found — leave $NAME as-is
            }
        }
        output[oi++] = input[i];
    }
    output[oi] = '\0';
    return (int)oi;
}

// ============================================================
// Shell core
// ============================================================

// Extended scan code prefix
#define KBD_EXTENDED 0xE0

// Command table
static shell_command_t commands[] = {
    {"help",     cmd_help,     "Show this help message"},
    {"clear",    cmd_clear,    "Clear the screen"},
    {"echo",     cmd_echo,     "Print text to screen (supports $VAR)"},
    {"reboot",   cmd_reboot,   "Reboot the system"},
    {"shutdown", cmd_shutdown, "Shutdown the system"},
    {"meminfo",  cmd_meminfo,  "Show memory information"},
    {"ls",       cmd_ls,       "List directory contents"},
    {"cat",      cmd_cat,      "Display file contents (no args: pipe stdin)"},
    {"grep",     cmd_grep,     "Filter lines: grep [-v] pattern [file]"},
    {"wc",       cmd_wc,       "Count lines/words/bytes"},
    {"head",     cmd_head,     "First N lines: head [-n N] [file]"},
    {"tail",     cmd_tail,     "Last N lines: tail [-n N] [file]"},
    {"more",     cmd_more,     "Page through file contents"},
    {"mkdir",    cmd_mkdir,    "Create directory"},
    {"touch",    cmd_touch,    "Create empty file"},
    {"rm",       cmd_rm,       "Remove file or empty directory"},
    {"cd",       cmd_cd,       "Change directory"},
    {"pwd",      cmd_pwd,      "Print working directory"},
    {"write",    cmd_write,    "Write text to a file"},
    {"uptime",   cmd_uptime,   "Show time since boot"},
    {"sysinfo",  cmd_sysinfo,  "Show system information"},
    {"date",     cmd_date,     "Show current date and time (RTC)"},
    {"serial",   cmd_serial,   "Send text over COM1 serial port"},
    {"tasks",    cmd_tasks,    "List running tasks"},
    {"spawn",    cmd_spawn,    "Spawn a background task"},
    {"multi",    cmd_multi,    "Run N busy tasks (interleave if preempt on)"},
    {"preempt",  cmd_preempt,  "Toggle IRQ preemption: preempt on|off"},
    {"history",  cmd_history,  "Show command history"},
    {"ata",      cmd_ata,     "ATA/IDE disk info and read/write"},
    {"elf",      cmd_elf,     "Parse ELF64 executable info"},
    {"apic",     cmd_apic,    "Show APIC information"},
    {"pci",      cmd_pci,     "List PCI devices with driver info"},
    {"fb",       cmd_fb,      "Framebuffer test"},
    {"scrollback", cmd_scrollback, "VGA scrollback info"},
    {"slab",     cmd_slab,    "Show slab allocator info"},
    {"neofetch",  cmd_neofetch, "System info with ASCII eye"},
    {"ping",      cmd_ping,      "Send ICMP echo request"},
    {"sync",      cmd_sync,      "Save filesystem to disk"},
    {"mount",     cmd_mount,     "Restore filesystem from disk"},
    {"ext4mount", cmd_ext4mount, "Mount ext4 volume (ATA disk)"},
    {"https",     cmd_https,     "HTTPS GET (TLS1.2; verify w CA bundle)"},
    {"tlssanity", cmd_tlssanity, "TLS bignum+heap self-test (#tls-ske-stall)"},
    {"ext4sync",  cmd_ext4sync,  "Copy new ramfs files into ext4"},
    {"rawmode",  cmd_rawmode, "Toggle keyboard raw mode (for games)"},
    {"dmesg",    cmd_dmesg,   "Show kernel log buffer"},
    {"hexdump",  cmd_hexdump, "Hex dump of file or memory address"},
    {"xxd",      cmd_hexdump, "Alias for hexdump"},
    {"export",   cmd_export,  "Set environment variable"},
    {"env",      cmd_env,     "List environment variables"},
    {"drivers",  cmd_drivers,  "List/init PCI device drivers"},
    {"mouse",    cmd_mouse,   "Show PS/2 mouse state"},
    {"fat32",    cmd_fat32,   "Mount/list FAT32 filesystem"},
    {"rtl8139",  cmd_rtl8139, "RTL8139 network driver status"},
    {"smp",      cmd_smp,      "Show SMP CPU info / start AP cores"},
    {"ac97",     cmd_ac97,     "AC97 audio driver info / test tone"},
    {"test",     cmd_test,     "Run kernel self-test suite"},
    {NULL, NULL, NULL}
};

void shell_init() {
    for (u32 i = 0; i < SHELL_HISTORY_SIZE; i++) {
        shell.history[i][0] = '\0';
    }
    shell.history_index = 0;
    shell.history_size  = 0;
    shell.arg_count     = 0;
    shell.running       = true;
    shell.history_browse_index = 0;
    shell.input_pos     = 0;
    shell.current_dir[0] = '/';
    shell.current_dir[1] = '\0';

    env_init();
    vga_print("[Shell] NullOs Shell initialized\n");
}

static void shell_erase_line(int* len) {
    for (int j = 0; j < *len; j++) {
        vga_putchar('\b');
        vga_putchar(' ');
        vga_putchar('\b');
    }
    *len = 0;
}

static void shell_show_history_entry(u32 idx, int* len) {
    shell_erase_line(len);
    const char* entry = shell.history[idx];
    for (int j = 0; entry[j] && *len < SHELL_MAX_CMD_LEN - 1; j++) {
        shell.cmd_buffer[*len] = entry[j];
        vga_putchar(entry[j]);
        (*len)++;
    }
    shell.cmd_buffer[*len] = '\0';
}

void shell_run() {
    while (shell.running) {
        // If input_pos == 0, we need a fresh prompt.
        // If input_pos > 0, we are resuming after a VT switch —
        // the prompt and partial input are already in the VGA buffer.
        if (shell.input_pos == 0) {
            fs_get_path(fs_get_cwd(), shell.current_dir, MAX_PATH);
            shell_print_prompt();
        }

        int i = shell.input_pos;
        shell.history_browse_index = shell.history_size;

        while (i < SHELL_MAX_CMD_LEN - 1) {
            if (!keyboard_haschar()) {
                hlt();
                continue;
            }

            u8 raw = keyboard_buffer_pop();

            if (raw == KBD_EXTENDED) {
                while (!keyboard_haschar()) hlt();
                u8 next = keyboard_buffer_pop() & 0x7F;

                switch (next) {
                    case 0x48:
                        if (shell.history_browse_index > 0) {
                            shell.history_browse_index--;
                            shell_show_history_entry(shell.history_browse_index, &i);
                        }
                        break;
                    case 0x50:
                        if (shell.history_browse_index < shell.history_size) {
                            shell.history_browse_index++;
                            if (shell.history_browse_index == shell.history_size) {
                                shell_erase_line(&i);
                                shell.cmd_buffer[0] = '\0';
                            } else {
                                shell_show_history_entry(shell.history_browse_index, &i);
                            }
                        }
                        break;
                    default:
                        break;
                }
                continue;
            }

            bool released = (raw & 0x80) != 0;
            u8 scancode = raw & 0x7F;

            if (released) {
                switch (scancode) {
                    case 0x1D: keyboard_clear_modifier(KBD_CTRL_PRESSED);  break;
                    case 0x2A:
                    case 0x36: keyboard_clear_modifier(KBD_SHIFT_PRESSED); break;
                    case 0x38: keyboard_clear_modifier(KBD_ALT_PRESSED);   break;
                }
                continue;
            }

            if (scancode == 0x1C) {
                vga_putchar('\n');
                shell.cmd_buffer[i] = '\0';
                shell.input_pos = 0;
                break;
            }

            if (scancode == 0x0E) {
                if (i > 0) {
                    i--;
                    vga_putchar('\b');
                    vga_putchar(' ');
                    vga_putchar('\b');
                }
                continue;
            }

            switch (scancode) {
                case 0x1D: keyboard_set_modifier(KBD_CTRL_PRESSED);  continue;
                case 0x2A:
                case 0x36: keyboard_set_modifier(KBD_SHIFT_PRESSED); continue;
                case 0x38: keyboard_set_modifier(KBD_ALT_PRESSED);   continue;
                case 0x3A: keyboard_toggle_caps_lock();              continue;
            }

            // ---- Virtual console switch: Alt+F1..F4 ----
            u8 mods = keyboard_get_modifiers();
            if ((mods & KBD_ALT_PRESSED) && scancode >= 0x3B && scancode <= 0x3E) {
                shell.input_pos = i;   // save partial input
                vt_switch(scancode - 0x3B);
                return;                // exit to mainloop
            }

            char c = keyboard_scancode_to_char(scancode, mods);

            if (c >= 32 && c < 127 && i < SHELL_MAX_CMD_LEN - 1) {
                shell.cmd_buffer[i] = c;
                vga_putchar(c);
                i++;
            }
        }

        shell.cmd_buffer[i] = '\0';

        if (shell.cmd_buffer[0] != '\0') {
            shell_add_to_history(shell.cmd_buffer);
        }

        shell_process_command();
    }
}

void shell_stop() {
    shell.running = false;
}

// ============================================================
// Pipes: `cmd1 | cmd2` — output of cmd1 (captured through the VGA
// layer) becomes stdin for cmd2. Consumers: grep/wc/head/tail and
// cat with no arguments read from the pipe buffer.
// ============================================================

static char* pipe_in_buf = NULL;   /* owned here */
static u32   pipe_in_len = 0;
static u32   pipe_in_pos = 0;

static void pipe_set_input(char* buf, u32 len) {
    if (pipe_in_buf) kfree(pipe_in_buf);
    pipe_in_buf = buf;                /* ownership transferred */
    pipe_in_len = len;
    pipe_in_pos = 0;
}

static void pipe_clear(void) {
    if (pipe_in_buf) kfree(pipe_in_buf);
    pipe_in_buf = NULL;
    pipe_in_len = 0;
    pipe_in_pos = 0;
}

/* Read up to max bytes from the current pipe position */
s32 shell_pipe_read(char* dst, u32 max) {
    if (!pipe_in_buf || pipe_in_pos >= pipe_in_len) return 0;
    u32 avail = pipe_in_len - pipe_in_pos;
    u32 n = (avail < max) ? avail : max;
    kmemcpy(dst, pipe_in_buf + pipe_in_pos, n);
    pipe_in_pos += n;
    return (s32)n;
}

bool shell_pipe_active(void) { return pipe_in_buf != NULL; }

/* ---- One pipeline stage ------------------------------------------*/
/* Runs a single command segment. When capture_to_pipe is true the
 * segment's console output is captured and installed as the next
 * stage's stdin. Output redirection ("> file", ">> file") is handled
 * here as well and takes precedence over piping.                    */
#define PIPE_MAX_SEGMENTS 8

static void run_command_segment(char** argv, int argc, bool capture_to_pipe) {
    if (argc == 0) return;

    /* ---- Output redirection inside this segment -------------------*/
    char* redir_buf = NULL;
    s32   redir_ffd = -1;
    bool  to_file  = false;
    bool  truncating = false;

    for (int i = 1; i < argc - 1; i++) {
        bool append   = (kstrcmp(argv[i], ">>") == 0);
        bool truncate = (kstrcmp(argv[i], ">")  == 0);
        if (!append && !truncate) continue;
        truncating = truncate;

        const char* outfile = argv[i + 1];
        if (truncate) {
            redir_ffd = fs_open(outfile, FS_WRITE | FS_CREATE | FS_TRUNC);
        } else {
            redir_ffd = fs_open(outfile, FS_WRITE | FS_CREATE | FS_APPEND);
        }

        to_file = (redir_ffd >= 0);
        if (to_file && truncate) {
            /* Start writing from 0; tail is cut after flush (fs_resize_fd) */
            fs_seek((u32)redir_ffd, 0, 0);
        }
        if (to_file) {
            redir_buf = (char*)kmalloc(SHELL_REDIRECT_MAX);
        }
        if (redir_buf) {
            vga_capture_start(redir_buf, SHELL_REDIRECT_MAX);
        } else {
            vga_print("shell: redirection failed\n");
            if (redir_ffd >= 0) { fs_close((u32)redir_ffd); redir_ffd = -1; }
        }
        argc = i;                             /* strip '> file' tokens */
        break;
    }

    /* ---- Capture for the next pipe stage --------------------------*/
    char* pipe_buf = NULL;
    if (capture_to_pipe && !to_file) {
        pipe_buf = (char*)kmalloc(SHELL_REDIRECT_MAX);
        if (pipe_buf) {
            vga_capture_start(pipe_buf, SHELL_REDIRECT_MAX);
        } else {
            vga_print("shell: out of memory for pipe\n");
        }
    }

    /* ---- Dispatch --------------------------------------------------*/
    bool matched = false;
    for (int i = 0; commands[i].name != NULL; i++) {
        if (kstrcmp(argv[0], commands[i].name) == 0) {
            commands[i].handler(argc, argv);
            matched = true;
            break;
        }
    }
    if (!matched) {
        vga_print("Unknown command: ");
        vga_print(argv[0]);
        vga_print("\nType 'help' for available commands.\n");
    }

    /* ---- Flush captured output -------------------------------------*/
    if (redir_buf) {
        u32 n = vga_capture_stop();
        if (to_file) {
            fs_write((u32)redir_ffd, redir_buf, n);
            if (truncating) fs_resize_fd((u32)redir_ffd, n);
            fs_close((u32)redir_ffd);
        }
        kfree(redir_buf);
    } else if (pipe_buf) {
        u32 n = vga_capture_stop();
        pipe_set_input(pipe_buf, n);          /* ownership transferred */
    }
}

void shell_process_command() {
    shell.arg_count = 0;

    char* str = shell.cmd_buffer;
    char* token_start = NULL;
    bool in_token = false;

    while (*str && shell.arg_count < SHELL_MAX_ARGS) {
        if (*str == ' ' || *str == '\t') {
            if (in_token) {
                *str = '\0';
                shell.args[shell.arg_count++] = token_start;
                in_token = false;
            }
        } else {
            if (!in_token) {
                token_start = str;
                in_token = true;
            }
        }
        str++;
    }

    if (in_token) {
        shell.args[shell.arg_count++] = token_start;
    }

    if (shell.arg_count == 0) return;

    /* ---- Split into pipeline stages on "|" -------------------------*/
    char*  seg_argv[PIPE_MAX_SEGMENTS][SHELL_MAX_ARGS];
    int    seg_argc[PIPE_MAX_SEGMENTS];
    int    nseg = 0;

    seg_argc[0] = 0;
    for (int i = 0; i < shell.arg_count; i++) {
        if (kstrcmp(shell.args[i], "|") == 0) {
            if (nseg + 1 >= PIPE_MAX_SEGMENTS) {
                vga_print("shell: too many pipe stages\n");
                return;
            }
            if (seg_argc[nseg] == 0) {
                vga_print("shell: syntax error near '|'\n");
                return;
            }
            nseg++;
            seg_argc[nseg] = 0;
            continue;
        }
        if (seg_argc[nseg] < SHELL_MAX_ARGS) {
            seg_argv[nseg][seg_argc[nseg]++] = shell.args[i];
        }
    }
    if (seg_argc[nseg] == 0 && nseg > 0) {
        vga_print("shell: syntax error near '|'\n");
        return;
    }

    /* ---- Run the pipeline -------------------------------------------*/
    if (nseg == 0) {
        run_command_segment(seg_argv[0], seg_argc[0], false);
    } else {
        for (int s = 0; s <= nseg; s++) {
            bool more = (s < nseg);
            run_command_segment(seg_argv[s], seg_argc[s], more);
            if (more && !pipe_in_buf) break;   /* producer gave nothing */
        }
        pipe_clear();
    }
}

void shell_add_to_history(const char* cmd) {
    if (shell.history_size > 0) {
        if (kstrcmp(shell.history[shell.history_size - 1], cmd) == 0) {
            return;
        }
    }

    if (shell.history_size >= SHELL_HISTORY_SIZE) {
        for (u32 i = 0; i < shell.history_size - 1; i++) {
            for (int j = 0; j < SHELL_MAX_CMD_LEN; j++) {
                shell.history[i][j] = shell.history[i + 1][j];
            }
        }
        shell.history_size--;
    }

    for (int i = 0; i < SHELL_MAX_CMD_LEN && cmd[i] != '\0'; i++) {
        shell.history[shell.history_size][i] = cmd[i];
    }
    shell.history[shell.history_size][SHELL_MAX_CMD_LEN - 1] = '\0';
    shell.history_size++;
    shell.history_index = shell.history_size;
}

void shell_print_prompt() {
    env_var_t* ev_user = env_find("USER");
    const char* user = ev_user ? ev_user->value : "null";
    char tty_str[2] = { '0' + (char)(vt_get_active() + 1), '\0' };
    vga_print_color("[", VGA_LIGHT_GREEN, VGA_BLACK);
    vga_print_color(tty_str, VGA_LIGHT_CYAN, VGA_BLACK);
    vga_print_color(" ", VGA_LIGHT_GREEN, VGA_BLACK);
    vga_print(user);
    vga_print_color("@", VGA_WHITE, VGA_BLACK);
    vga_print(shell.current_dir);
    vga_print_color("]$ ", VGA_LIGHT_GREEN, VGA_BLACK);
}

char* shell_trim(char* str) {
    while (*str == ' ' || *str == '\t') str++;
    if (*str == '\0') return str;
    char* end = str;
    while (*end) end++;
    end--;
    while (end > str && (*end == ' ' || *end == '\t')) end--;
    *(end + 1) = '\0';
    return str;
}

// ============================================================
// Command implementations
// ============================================================

void cmd_help(int argc, char** argv) {
    (void)argc; (void)argv;
    vga_print("\nAvailable commands:\n");
    vga_print("-------------------\n");
    for (int i = 0; commands[i].name != NULL; i++) {
        vga_printf("  %-10s - %s\n", commands[i].name, commands[i].description);
    }
    vga_print("\n");
}

void cmd_clear(int argc, char** argv) {
    (void)argc; (void)argv;
    vga_clear(VGA_WHITE, VGA_BLACK);
}

void cmd_echo(int argc, char** argv) {
    // Expand $VAR in all arguments
    char expanded[SHELL_MAX_CMD_LEN];
    expanded[0] = '\0';
    u32 elen = 0;
    for (int i = 1; i < argc; i++) {
        char buf[SHELL_MAX_CMD_LEN];
        env_expand(argv[i], buf, sizeof(buf));
        // Bounded append: never write past the end of `expanded`
        const char* parts[2] = { (i > 1) ? " " : NULL, buf };
        for (int p = 0; p < 2; p++) {
            if (!parts[p]) continue;
            for (const char* s = parts[p]; *s && elen < SHELL_MAX_CMD_LEN - 1; s++) {
                expanded[elen++] = *s;
            }
        }
    }
    expanded[elen] = '\0';
    vga_print(expanded);
    vga_print("\n");
}

void cmd_reboot(int argc, char** argv) {
    (void)argc; (void)argv;
    vga_print("\n[Shell] Rebooting system...\n");
    while (inb(0x64) & 0x02);
    outb(0x64, 0xFE);
    cli();
    u64 temp = 0;
    __asm__ volatile ("lidt (%0)" :: "r"(&temp));
    __asm__ volatile ("int $3");
    while(1) { hlt(); }
}

void cmd_shutdown(int argc, char** argv) {
    (void)argc; (void)argv;
    vga_print("\n[Shell] Shutting down system...\n");
    kernel_shutdown();
    while(1) { hlt(); }
}

void cmd_meminfo(int argc, char** argv) {
    (void)argc; (void)argv;
    vga_print("\nMemory Information:\n");
    vga_print("--------------------\n");
    u64 total = pmm_get_total_memory();
    u64 free  = pmm_get_available_memory();
    u64 used  = total - free;
    vga_printf("  Total RAM:  %u MB (%u KB)\n",
               (u32)(total / 1024 / 1024), (u32)(total / 1024));
    vga_printf("  Used:       %u KB\n", (u32)(used / 1024));
    vga_printf("  Free:       %u KB\n", (u32)(free / 1024));
    vga_printf("  Heap used:  %u KB / %u KB free\n",
               (u32)(heap_get_used() / 1024), (u32)(heap_get_free() / 1024));
    vga_printf("  Buddy free: %u pages\n\n", buddy_get_free_pages());
}

void cmd_ls(int argc, char** argv) {
    const char* path = ".";
    if (argc >= 2) path = argv[1];
    fs_node_t* nodes[FS_MAX_CHILDREN];
    int count = fs_list_dir(path, nodes, FS_MAX_CHILDREN);
    if (count < 0) {
        vga_print("ls: ");
        vga_print(fs_get_error(count));
        vga_print("\n");
        return;
    }
    if (count == 0) {
        vga_print("(empty directory)\n");
        return;
    }
    for (int i = 0; i < count; i++) {
        if (nodes[i]->type == FS_FILE_TYPE_DIR) {
            vga_print_color(nodes[i]->name, VGA_LIGHT_BLUE, VGA_BLACK);
            vga_print("/  ");
        } else {
            vga_printf("%-12s %uB  ", nodes[i]->name, nodes[i]->size);
        }
    }
    vga_print("\n");
}

// ============================================================
// Preemption demo: N CPU-bound tasks WITHOUT cooperative yields.
// The only thing that can switch them is the timer IRQ — so any
// interleaving in the output proves IRQ-driven preemption works.
// ============================================================

static void mtask_body(void* arg) {
    u32 id = (u32)(u64)arg;
    for (u32 i = 1; i <= 6; i++) {
        for (volatile u64 d = 0; d < 30000000ULL; d++) { }   /* busy */
        vga_printf("  [mtask%u] step %u/6\n", id, i);
    }
}

// Toggle IRQ preemption (experimental)
void cmd_preempt(int argc, char** argv) {
    if (argc >= 2 && kstrcmp(argv[1], "on") == 0) {
        sched_set_preempt(true);
        vga_print("[SCHED] IRQ preemption ENABLED (experimental)\n");
    } else if (argc >= 2 && kstrcmp(argv[1], "off") == 0) {
        sched_set_preempt(false);
        vga_print("[SCHED] IRQ preemption disabled (cooperative)\n");
    } else {
        vga_printf("[SCHED] preemption: %s (usage: preempt on|off)\n",
                   sched_get_preempt() ? "ON" : "off");
    }
}

void cmd_multi(int argc, char** argv) {
    int n = (argc > 1) ? katoi(argv[1]) : 3;
    if (n < 2) n = 2;
    if (n > 4) n = 4;

    vga_printf("[multi] %d busy tasks (IRQ preemption %s)\n",
               n, sched_get_preempt() ? "ON" : "off");

    s32 ids[4];
    for (int i = 0; i < n; i++) {
        ids[i] = task_create("mtask", mtask_body, (void*)(u64)(i + 1), 0);
    }

    /* Keep sync-running unfinished tasks until all are done. With IRQ
     * preemption the tasks rotate among themselves inside each call. */
    u64 t0 = timer_get_ticks();
    for (int round = 0; round < 500; round++) {
        bool busy = false;
        for (int i = 0; i < n; i++) {
            s32 slot = task_slot_of(ids[i]);
            if (slot >= 0 && !task_is_finished(slot)) {
                task_run_to_completion(slot);
                busy = true;
            }
        }
        if (!busy) break;
    }
    vga_printf("[multi] done: ticks=%lu preempts=%lu\n",
               (unsigned long)(timer_get_ticks() - t0),
               (unsigned long)scheduler_preempt_count());
    vga_print("[multi] all tasks finished\n");
}

void cmd_cat(int argc, char** argv) {
    /* No file argument: dump pipe stdin if one is active */
    if (argc < 2) {
        if (shell_pipe_active()) {
            char chunk[256];
            s32 n;
            while ((n = shell_pipe_read(chunk, sizeof(chunk))) > 0) {
                for (s32 j = 0; j < n; j++) vga_putchar(chunk[j]);
            }
            return;
        }
        vga_print("Usage: cat <filename>\n");
        return;
    }
    s32 fd = fs_open(argv[1], FS_READ);
    if (fd < 0) {
        vga_print("cat: ");
        vga_print(argv[1]);
        vga_print(": ");
        vga_print(fs_get_error(fd));
        vga_print("\n");
        return;
    }
    char buf[256];
    s64 bytes_read;
    u64 total = 0;
    /* Byte budget: character devices like /dev/zero never hit EOF */
    while (total < FS_MAX_FILE_SIZE &&
           (bytes_read = fs_read(fd, buf, sizeof(buf))) > 0) {
        total += (u64)bytes_read;
        for (s64 j = 0; j < bytes_read; j++) {
            vga_putchar(buf[j]);
        }
    }
    fs_close(fd);
    vga_print("\n");
}

// ============================================================
// Text filter commands (grep/wc/head/tail)
// Shared input: file argument, or the active pipe as stdin.
// ============================================================

typedef struct {
    char* data;
    u32   len;
    bool  owned;      /* kmalloc'd here → must free */
} input_src_t;

static bool input_open_file(input_src_t* src, const char* path) {
    s32 fd = fs_open(path, FS_READ);
    if (fd < 0) return false;
    u32 cap = FS_MAX_FILE_SIZE;
    src->data = (char*)kmalloc(cap + 1);
    src->owned = true;
    src->len = 0;
    if (!src->data) { fs_close(fd); return false; }
    s64 r;
    while (src->len < cap &&
           (r = fs_read(fd, src->data + src->len, cap - src->len)) > 0) {
        src->len += (u32)r;
    }
    src->data[src->len] = '\0';
    fs_close(fd);
    return true;
}

/* File arg at path_idx wins; otherwise consume the pipe stdin */
static bool input_get(input_src_t* src, int argc, char** argv, int path_idx,
                      const char* cmdname) {
    if (path_idx < argc) {
        if (input_open_file(src, argv[path_idx])) return true;
        vga_print(cmdname); vga_print(": ");
        vga_print(argv[path_idx]);
        vga_print(": no such file\n");
        return false;
    }
    if (pipe_in_buf && pipe_in_pos <= pipe_in_len) {
        src->data  = pipe_in_buf + pipe_in_pos;
        src->len   = pipe_in_len - pipe_in_pos;
        src->owned = false;
        return true;
    }
    vga_print(cmdname);
    vga_print(": no input (pipe something in or name a file)\n");
    return false;
}

static void input_release(input_src_t* src) {
    if (src->owned && src->data) kfree(src->data);
    src->data = NULL;
}

/* Substring search with explicit haystack length (lines are not NUL-terminated) */
static bool mem_contains(const char* hay, u32 hay_len, const char* needle) {
    u32 nl = kstrlen(needle);
    if (nl == 0) return true;
    if (nl > hay_len) return false;
    for (u32 i = 0; i + nl <= hay_len; i++) {
        u32 j = 0;
        while (j < nl && hay[i + j] == needle[j]) j++;
        if (j == nl) return true;
    }
    return false;
}

void cmd_grep(int argc, char** argv) {
    int pat_idx = 1;
    bool invert = false;
    if (argc >= 2 && kstrcmp(argv[1], "-v") == 0) {
        invert = true;
        pat_idx = 2;
    }
    if (pat_idx >= argc) {
        vga_print("Usage: grep [-v] <pattern> [file]\n");
        return;
    }
    const char* pat = argv[pat_idx];

    input_src_t src;
    if (!input_get(&src, argc, argv, pat_idx + 1, "grep")) return;

    u32 i = 0;
    while (i < src.len) {
        u32 e = i;
        while (e < src.len && src.data[e] != '\n') e++;
        bool m = mem_contains(src.data + i, e - i, pat);
        if (m != invert) {
            for (u32 k = i; k < e; k++) vga_putchar(src.data[k]);
            vga_print("\n");
        }
        i = e + 1;
    }
    input_release(&src);
}

void cmd_wc(int argc, char** argv) {
    input_src_t src;
    if (!input_get(&src, argc, argv, 1, "wc")) return;

    u64 lines = 0, words = 0, bytes = src.len;
    bool in_word = false;
    for (u32 i = 0; i < src.len; i++) {
        char c = src.data[i];
        if (c == '\n') lines++;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            in_word = false;
        } else if (!in_word) {
            in_word = true;
            words++;
        }
    }
    if (src.len > 0 && src.data[src.len - 1] != '\n') lines++;
    vga_printf("%8lu %8lu %8lu\n",
               (unsigned long)lines, (unsigned long)words, (unsigned long)bytes);
    input_release(&src);
}

static void parse_count_arg(int argc, char** argv, const char* cmdname,
                            s32* n, int* path_idx) {
    *n = 10;
    *path_idx = 1;
    if (argc >= 2 && kstrcmp(argv[1], "-n") == 0 && argc >= 3) {
        *n = katoi(argv[2]);
        *path_idx = 3;
    } else if (argc >= 2 && argv[1][0] >= '0' && argv[1][0] <= '9') {
        *n = katoi(argv[1]);
        *path_idx = 2;
    } else if (argc >= 2 && argv[1][0] == '-') {
        vga_printf("%s: unknown option %s\n", cmdname, argv[1]);
        *n = -1;
    }
}

void cmd_head(int argc, char** argv) {
    s32 n, path_idx;
    parse_count_arg(argc, argv, "head", &n, &path_idx);
    if (n < 0) return;

    input_src_t src;
    if (!input_get(&src, argc, argv, path_idx, "head")) return;

    u32 i = 0;
    while (i < src.len && n > 0) {
        u32 e = i;
        while (e < src.len && src.data[e] != '\n') e++;
        for (u32 k = i; k < e; k++) vga_putchar(src.data[k]);
        vga_print("\n");
        n--;
        i = e + 1;
    }
    input_release(&src);
}

void cmd_tail(int argc, char** argv) {
    s32 n, path_idx;
    parse_count_arg(argc, argv, "tail", &n, &path_idx);
    if (n < 0 || n > 4096) n = 4096;

    input_src_t src;
    if (!input_get(&src, argc, argv, path_idx, "tail")) return;

    /* Collect line starts, keep the last N of them */
    static u32 line_start[4096];
    u32 count = 0;
    u32 i = 0;
    while (i < src.len) {
        if (count < 4096) {
            line_start[count++] = i;
        } else {
            for (u32 k = 0; k < 4095; k++) line_start[k] = line_start[k + 1];
            line_start[4095] = i;
        }
        u32 e = i;
        while (e < src.len && src.data[e] != '\n') e++;
        i = e + 1;
    }

    u32 first = (count > (u32)n) ? count - (u32)n : 0;
    for (u32 li = first; li < count; li++) {
        u32 end = (li + 1 < count) ? line_start[li + 1] : src.len;
        if (end > line_start[li] && src.data[end - 1] == '\n') end--;
        for (u32 k = line_start[li]; k < end; k++) vga_putchar(src.data[k]);
        vga_print("\n");
    }
    input_release(&src);
}

void cmd_mkdir(int argc, char** argv) {
    if (argc < 2) {
        vga_print("Usage: mkdir <dirname>\n");
        return;
    }
    s32 err = fs_mkdir(argv[1]);
    if (err != FS_OK) {
        vga_print("mkdir: ");
        vga_print(fs_get_error(err));
        vga_print("\n");
    }
}

void cmd_touch(int argc, char** argv) {
    if (argc < 2) {
        vga_print("Usage: touch <filename>\n");
        return;
    }
    s32 err = fs_create_file(argv[1]);
    if (err != FS_OK) {
        vga_print("touch: ");
        vga_print(fs_get_error(err));
        vga_print("\n");
    }
}

void cmd_rm(int argc, char** argv) {
    if (argc < 2) {
        vga_print("Usage: rm <path>\n");
        return;
    }
    s32 err = fs_delete_file(argv[1]);
    if (err != FS_OK) {
        vga_print("rm: ");
        vga_print(fs_get_error(err));
        vga_print("\n");
    }
}

void cmd_cd(int argc, char** argv) {
    const char* target = "/";
    if (argc >= 2) target = argv[1];
    s32 err = fs_set_cwd(target);
    if (err != FS_OK) {
        vga_print("cd: ");
        vga_print(fs_get_error(err));
        vga_print("\n");
    }
}

void cmd_pwd(int argc, char** argv) {
    (void)argc; (void)argv;
    char path[MAX_PATH];
    fs_get_path(fs_get_cwd(), path, MAX_PATH);
    vga_print(path);
    vga_print("\n");
}

void cmd_write(int argc, char** argv) {
    if (argc < 3) {
        vga_print("Usage: write <filename> <text...>\n");
        return;
    }
    char buf[1024];
    buf[0] = '\0';
    for (int i = 2; i < argc; i++) {
        if (i > 2) kstrcat(buf, " ");
        kstrcat(buf, argv[i]);
    }
    s32 err = fs_write_file(argv[1], buf, kstrlen(buf));
    if (err != FS_OK) {
        vga_print("write: ");
        vga_print(fs_get_error(err));
        vga_print("\n");
    } else {
        vga_printf("Written %u bytes to %s\n", (u32)kstrlen(buf), argv[1]);
    }
}

void cmd_uptime(int argc, char** argv) {
    (void)argc; (void)argv;
    u64 ms = timer_get_uptime_ms();
    u64 secs = ms / 1000;
    u64 mins = secs / 60;
    u64 hours = mins / 60;
    vga_print("Uptime: ");
    if (hours > 0) {
        vga_print_unsigned(hours);
        vga_print("h ");
    }
    vga_printf("%um %us (%u ms)\n",
               (u32)(mins % 60), (u32)(secs % 60), (u32)ms);
}

void cmd_sysinfo(int argc, char** argv) {
    (void)argc; (void)argv;
    vga_print("\n=== NullOs System Information ===\n");
    vga_printf("Kernel:     NullOs v%s\n", NULLOS_VERSION);
    vga_print("Arch:       x86_64 (Long Mode)\n");
    vga_printf("RAM:        %u MB\n", (u32)(pmm_get_total_memory() / 1024 / 1024));
    vga_printf("Uptime:     %us\n", (u32)(timer_get_uptime_ms() / 1000));
    char timebuf[24];
    rtc_format_time(timebuf, sizeof(timebuf));
    vga_printf("RTC Time:   %s\n", timebuf);
    vga_printf("Tasks:      %u active\n", task_get_count());
    vga_printf("Log entries: %u\n", klog_get_count());
    vga_print("\nSubsystems:\n");
    vga_print("  - IDT (256 entries) + Exception handling\n");
    vga_print("  - PIC 8259A (IRQ 32-47)\n");
    vga_printf("  - PIT Timer (%u Hz)\n", TIMER_FREQ);
    vga_print("  - RTC (CMOS, IRQ8, 2 Hz)\n");
    vga_print("  - GDT/TSS (kernel + user segments, IST1)\n");
    vga_print("  - syscall/IRETQ (MSR LSTAR, user return)\n");
    vga_print("  - FPU/SSE (FXSAVE/FXRSTOR)\n");
    vga_print("  - ACPI shutdown (RSDP/XSDT/FADT)\n");
    vga_print("  - VMM demand paging + 2MB->4KB split\n");
    vga_print("  - Buddy allocator (coalescing)\n");
    vga_print("  - Kernel log buffer (dmesg)\n");
    vga_print("  - Environment variables (export/env)\n");
    vga_print("  - PCI driver binding\n");
    vga_printf("  - Interactive Shell (%u commands)\n", 33);
    vga_print("=================================\n\n");
}

// ============================================================
// New commands
// ============================================================

void cmd_date(int argc, char** argv) {
    (void)argc; (void)argv;
    rtc_time_t t;
    rtc_read_time(&t);
    vga_printf("%s, %s %u %u %02u:%02u:%02u\n",
               rtc_weekday_name(t.weekday),
               rtc_month_name(t.month),
               (u32)t.day, (u32)t.year,
               (u32)t.hour, (u32)t.minute, (u32)t.second);
}

void cmd_serial(int argc, char** argv) {
    if (argc < 2) {
        vga_print("Usage: serial <text...>\n");
        vga_print("  Send text over COM1 serial port.\n");
        vga_print("  Output visible with: qemu -serial stdio\n");
        return;
    }
    char buf[512];
    buf[0] = '\0';
    for (int i = 1; i < argc; i++) {
        if (i > 1) kstrcat(buf, " ");
        kstrcat(buf, argv[i]);
    }
    serial_print(buf);
    serial_putchar('\r');
    serial_putchar('\n');
    vga_printf("[SERIAL] Sent %u bytes to COM1\n", (u32)(kstrlen(buf) + 2));
}

static void demo_task(void* arg) {
    u32 count = (u32)(u64)arg;
    vga_printf("  [task] Starting, will count to %u\n", count);
    for (u32 i = 1; i <= count; i++) {
        vga_printf("  [task] %u/%u\n", i, count);
        task_yield();
    }
    vga_printf("  [task] Finished!\n");
}

void cmd_spawn(int argc, char** argv) {
    u32 count = 10;
    if (argc >= 2) {
        count = (u32)katoi(argv[1]);
        if (count == 0) count = 10;
        if (count > 1000) count = 1000;
    }
    s32 id = task_create("demo", demo_task, (void*)(u64)count, 0);
    if (id >= 0) {
        vga_printf("[SHELL] Spawned task id=%u (count=%u), running...\n", (u32)id, count);
        s32 slot = task_slot_of(id);
        if (slot >= 0) task_run_to_completion(slot);
        vga_print("[SHELL] Task finished.\n");
    } else {
        vga_print("[SHELL] Failed to spawn task (max tasks reached?)\n");
    }
}

void cmd_tasks(int argc, char** argv) {
    (void)argc; (void)argv;
    task_list();
}

void cmd_history(int argc, char** argv) {
    (void)argc; (void)argv;
    if (shell.history_size == 0) {
        vga_print("(no history)\n");
        return;
    }
    for (u32 i = 0; i < shell.history_size; i++) {
        vga_printf("  %3u  %s\n", (u32)(i + 1), shell.history[i]);
    }
}

void cmd_apic(int argc, char** argv) {
    (void)argc; (void)argv;
    vga_print("\nAPIC Information:\n");
    vga_print("------------------\n");
    if (apic_is_supported()) {
        vga_print("APIC:     Supported (CPUID.01H:EDX[bit 9])\n");
        vga_printf("LAPIC ID: %u (read via MSR)\n", lapic_get_id());
        vga_print("Status:   Available, not activated\n");
        vga_print("Note:     Call apic_init() to activate and replace PIC.\n");
    } else {
        vga_print("APIC:     Not supported by CPU.\n");
    }
    vga_print("\n");
}

void cmd_scrollback(int argc, char** argv) {
    (void)argc; (void)argv;
    extern bool vga_scrollback_active(void);
    vga_printf("Scrollback: %s\n", vga_scrollback_active() ? "active" : "inactive");
}

// ============================================================
// neofetch — block-art eye + system summary
// ============================================================

#define NEOFETCH_ART_W 25

static void neofetch_row(const char* art, const char* label, const char* value)
{
    // Art column: pad or truncate to NEOFETCH_ART_W cells, cyan
    char padded[NEOFETCH_ART_W + 1];
    u32 i = 0;
    for (; i < NEOFETCH_ART_W && art[i]; i++) padded[i] = art[i];
    for (; i < NEOFETCH_ART_W; i++) padded[i] = ' ';
    padded[i] = '\0';
    vga_print_color(padded, VGA_LIGHT_CYAN, VGA_BLACK);

    if (label && label[0]) {
        vga_print_color(label, VGA_WHITE, VGA_BLACK);
        if (value) {
            vga_print_color(value, VGA_LIGHT_CYAN, VGA_BLACK);
        }
    }
    vga_print("\n");
}

static void neofetch_kv(char* out, u32 size, const char* label, const char* value)
{
    ksnprintf(out, size, " %s %s", label, value);
}

void cmd_neofetch(int argc, char** argv) {
    (void)argc; (void)argv;

    char line[96];

    // The Null Gaze — block-art eye (CP437: █ ▄ ▀ ░ ▒ ▓)
    static const char* art[] = {
        "   \xdb\xdb\xdb\xdb\xdb\xdb\xdb\xdb\xdb\xdb\xdb\xdb\xdb\xdb\xdb\xdb   ",   /* 0  */
        "  \xdb\xc4\xc4\xc4\xc4\xc4\xc4\xc4\xc4\xc4\xc4\xc4\xc4\xc4\xc4\xc4\xdb  ",   /* 1  */
        " \xdb\xdb\xb0\xb0\xb0\xb0\xb0\xb0\xb0\xb0\xb0\xb0\xb0\xb0\xb0\xb0\xdb\xdb  ",   /* 2  */
        "\xdb\xdb\xb0  \xdc\xdc\xdc\xdc\xdc\xdc\xdc  \xb0\xdb\xdb ",               /* 3  */
        "\xdb    \xdb\xdb\xdb\xdb\xdb\xdb\xdb    \xdb  ",                           /* 4  */
        "\xdb\xdb\xb0  \xdf\xdf\xdf\xdf\xdf\xdf\xdf  \xb0\xdb\xdb ",                /* 5  */
        " \xdb\xdb\xb1\xb1\xb1\xb1\xb1\xb1\xb1\xb1\xb1\xb1\xb1\xb1\xb1\xb1\xdb\xdb  ",/* 6 */
        "  \xdf\xdf\xdf\xdf\xdf\xdf\xdf\xdf\xdf\xdf\xdf\xdf\xdf\xdf\xdf\xdf\xdf  ", /* 7 */
        "   \xdf\xdf\xdf\xdf\xdf\xdf\xdf\xdf\xdf\xdf\xdf\xdf\xdf\xdf\xdf\xdf   ",   /* 8  */
    };

    env_var_t* user_ev = env_find("USER");
    const char* user = user_ev ? user_ev->value : "null";
    env_var_t* sh_ev = env_find("SHELL");

    char userline[64];
    ksnprintf(userline, sizeof(userline), "%s@nullos", user);
    char sep[40];
    u32 ul = kstrlen(userline);
    for (u32 k = 0; k < ul && k < sizeof(sep) - 1; k++) sep[k] = '-';
    sep[ul] = '\0';

    char upt[32];
    ksnprintf(upt, sizeof(upt), "%lu min %lu s",
              (unsigned long)(timer_get_uptime_ms() / 60000),
              (unsigned long)((timer_get_uptime_ms() / 1000) % 60));

    char memline[80];
    u64 total = pmm_get_total_memory();
    u64 freem = pmm_get_available_memory();
    ksnprintf(memline, sizeof(memline), "%lu MB / %lu MB",
              (unsigned long)((total - freem) / (1024 * 1024)),
              (unsigned long)(total / (1024 * 1024)));

    char taskline[32];
    ksnprintf(taskline, sizeof(taskline), "%lu", (unsigned long)task_get_count());

    neofetch_row("", "", "");
    neofetch_row(art[0], "", "");
    neofetch_row(art[1], "  ", userline);
    neofetch_row(art[2], "  ", sep);
    neofetch_kv(line, sizeof(line), "OS:",     "NullOs " NULLOS_VERSION " x86_64");
    neofetch_row(art[3], "", line);
    neofetch_kv(line, sizeof(line), "Kernel:", NULLOS_VERSION);
    neofetch_row(art[4], "", line);
    neofetch_kv(line, sizeof(line), "Uptime:", upt);
    neofetch_row(art[5], "", line);
    neofetch_kv(line, sizeof(line), "Shell:",  sh_ev ? sh_ev->value : "nullsh");
    neofetch_row(art[6], "", line);
    neofetch_kv(line, sizeof(line), "WM:",     "None");
    neofetch_row(art[7], "", line);
    neofetch_kv(line, sizeof(line), "Memory:", memline);
    neofetch_row(art[8], "", line);
    neofetch_kv(line, sizeof(line), "Tasks:",  taskline);
    neofetch_row("", "", line);
    neofetch_kv(line, sizeof(line), "Disk:",   "nullos.iso (live)");
    neofetch_row("", "", "");
}

void cmd_slab(int argc, char** argv) {
    (void)argc; (void)argv;
    vga_print("[SLAB] Page-tracked slab allocator active\n");
}

void cmd_rawmode(int argc, char** argv) {
    (void)argc; (void)argv;
    bool current = keyboard_get_raw_mode();
    keyboard_set_raw_mode(!current);
    vga_printf("[KBD] Raw mode: %s\n", !current ? "ON (no echo/translation)" : "OFF (normal)");
}

// ============================================================
// Feature 1: dmesg — print kernel log buffer
// ============================================================

void cmd_dmesg(int argc, char** argv) {
    u8 level = 0;
    if (argc >= 2) {
        // Parse optional level filter: -l <level>
        if (kstrcmp(argv[1], "-l") == 0 && argc >= 3) {
            const char* lstr = argv[2];
            if (kstrcmp(lstr, "emerg") == 0 || kstrcmp(lstr, "0") == 0) level = 0;
            else if (kstrcmp(lstr, "alert") == 0 || kstrcmp(lstr, "1") == 0) level = 1;
            else if (kstrcmp(lstr, "crit") == 0 || kstrcmp(lstr, "2") == 0) level = 2;
            else if (kstrcmp(lstr, "err") == 0 || kstrcmp(lstr, "3") == 0) level = 3;
            else if (kstrcmp(lstr, "warn") == 0 || kstrcmp(lstr, "4") == 0) level = 4;
            else if (kstrcmp(lstr, "notice") == 0 || kstrcmp(lstr, "5") == 0) level = 5;
            else if (kstrcmp(lstr, "info") == 0 || kstrcmp(lstr, "6") == 0) level = 6;
            else if (kstrcmp(lstr, "debug") == 0 || kstrcmp(lstr, "7") == 0) level = 7;
        }
    }
    klog_print_buffer(level);
}

// ============================================================
// Feature 3: hexdump / xxd — hex dump of file or memory
// ============================================================

// Hex dump helper: dump `size` bytes from `data` with `base_addr` as virtual address
static void hex_dump(const void* data, u32 size, u64 base_addr) {
    const u8* ptr = (const u8*)data;
    u32 offset = 0;

    while (offset < size) {
        // Print address
        vga_printf("%08lx  ", (unsigned long)(base_addr + offset));

        // Hex bytes (16 per line)
        u32 i;
        for (i = 0; i < 16; i++) {
            if (offset + i < size) {
                vga_printf("%02x ", ptr[offset + i]);
            } else {
                vga_print("   ");
            }
            if (i == 7) vga_putchar(' ');
        }

        // ASCII representation
        vga_print(" |");
        for (i = 0; i < 16; i++) {
            if (offset + i < size) {
                u8 c = ptr[offset + i];
                vga_putchar(c >= 32 && c < 127 ? c : '.');
            }
        }
        vga_print("|\n");

        offset += 16;

        // Pause every 24 lines (like more)
        if (offset % (24 * 16) == 0 && offset < size) {
            vga_print("-- More -- (press any key)\n");
            while (!keyboard_haschar()) hlt();
            keyboard_buffer_pop();
        }
    }

    vga_printf("\n%u bytes dumped\n", size);
}

void cmd_hexdump(int argc, char** argv) {
    if (argc < 2) {
        vga_print("Usage: hexdump <file> [length]\n");
        vga_print("       hexdump 0x<addr> [length]\n");
        vga_print("  Dump file contents or memory in hex+ASCII.\n");
        return;
    }

    // Check if argument looks like a hex address (starts with 0x or 0X)
    bool is_addr = false;
    u64 addr = 0;
    if (argv[1][0] == '0' && (argv[1][1] == 'x' || argv[1][1] == 'X')) {
        // Simple hex parser for address
        for (int i = 2; argv[1][i]; i++) {
            char c = argv[1][i];
            addr <<= 4;
            if (c >= '0' && c <= '9') addr |= (c - '0');
            else if (c >= 'a' && c <= 'f') addr |= (c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') addr |= (c - 'A' + 10);
            else break;
        }
        is_addr = true;
    }

    u32 length = 256; // default dump length
    if (argc >= 3) {
        u32 parsed = (u32)katoi(argv[2]);
        if (parsed > 0) length = parsed;
    }

    if (is_addr) {
        // Direct memory dump. This is a raw physical/virtual read —
        // useful for debugging but dangerous (can fault on unmapped
        // pages, though demand paging will usually satisfy them).
        // Clamp the length to keep the dump bounded.
        u32 clamped = length > 4096 ? 4096 : length;
        vga_printf("Dumping %u bytes from 0x%lx (raw memory access):\n\n",
                   clamped, (unsigned long)addr);
        hex_dump((const void*)(uintptr_t)addr, clamped, addr);
    } else {
        // File dump
        s32 fd = fs_open(argv[1], FS_READ);
        if (fd < 0) {
            vga_print("hexdump: ");
            vga_print(argv[1]);
            vga_print(": ");
            vga_print(fs_get_error(fd));
            vga_print("\n");
            return;
        }

        // Read file into buffer (up to `length` bytes)
        u32 read_len = length;
        if (read_len > 4096) read_len = 4096; // cap at 4KB for RAM FS safety
        u8* buf = (u8*)kmalloc(read_len);
        if (!buf) {
            vga_print("hexdump: out of memory\n");
            fs_close(fd);
            return;
        }

        s64 nread = fs_read(fd, buf, read_len);
        fs_close(fd);

        if (nread <= 0) {
            vga_print("hexdump: empty or unreadable file\n");
            kfree(buf);
            return;
        }

        vga_printf("Dumping %u bytes from %s:\n\n", (u32)nread, argv[1]);
        hex_dump(buf, (u32)nread, 0);
        kfree(buf);
    }
}

// ============================================================
// Feature 5: Environment variables
// ============================================================

void cmd_export(int argc, char** argv) {
    if (argc < 2) {
        vga_print("Usage: export NAME=VALUE\n");
        vga_print("  Set an environment variable.\n");
        vga_print("  Example: export EDITOR=vim\n");
        return;
    }

    // Parse NAME=VALUE
    char* eq = kstrchr(argv[1], '=');
    if (!eq) {
        // Just 'export NAME' — mark as exported (already set or empty)
        env_var_t* ev = env_find(argv[1]);
        if (!ev) {
            ev = env_find_free();
            if (!ev) {
                vga_print("export: too many variables\n");
                return;
            }
            kstrncpy(ev->name, argv[1], ENV_NAME_MAX - 1);
            ev->value[0] = '\0';
            ev->active = true;
        }
        return;
    }

    // Split at '='
    *eq = '\0';
    const char* name = argv[1];
    const char* value = eq + 1;

    if (kstrlen(name) == 0) {
        vga_print("export: empty variable name\n");
        *eq = '=';
        return;
    }

    env_var_t* ev = env_find(name);
    if (!ev) {
        ev = env_find_free();
        if (!ev) {
            vga_print("export: too many variables\n");
            *eq = '=';
            return;
        }
        kstrncpy(ev->name, name, ENV_NAME_MAX - 1);
        ev->active = true;
    }
    kstrncpy(ev->value, value, ENV_VAL_MAX - 1);
    *eq = '='; // restore for history
}

void cmd_env(int argc, char** argv) {
    (void)argc; (void)argv;
    vga_print("\nEnvironment variables:\n");
    for (int i = 0; i < ENV_MAX_VARS; i++) {
        if (env_vars[i].active) {
            vga_printf("  %s=%s\n", env_vars[i].name, env_vars[i].value);
        }
    }
    vga_print("\n");
}

// ============================================================
// Feature 8: more — pager for files
// ============================================================

void cmd_more(int argc, char** argv) {
    if (argc < 2) {
        vga_print("Usage: more <filename>\n");
        vga_print("  Page through a file, 23 lines at a time.\n");
        vga_print("  Press any key for next page, 'q' to quit.\n");
        return;
    }

    s32 fd = fs_open(argv[1], FS_READ);
    if (fd < 0) {
        vga_print("more: ");
        vga_print(argv[1]);
        vga_print(": ");
        vga_print(fs_get_error(fd));
        vga_print("\n");
        return;
    }

    char buf[256];
    s64 bytes_read;
    int lines_shown = 0;
    int total_lines = 0;

    while ((bytes_read = fs_read(fd, buf, sizeof(buf))) > 0) {
        for (s64 j = 0; j < bytes_read; j++) {
            char c = buf[j];
            vga_putchar(c);

            if (c == '\n') {
                lines_shown++;
                total_lines++;
            }

            // Pause every 23 lines
            if (lines_shown >= 23) {
                lines_shown = 0;
                vga_print_color("-- More -- (q=quit, any key=continue)", VGA_LIGHT_GREY, VGA_BLACK);
                // Erase prompt on next key
                while (!keyboard_haschar()) hlt();
                u8 key = keyboard_buffer_pop() & 0x7F;
                // Erase the prompt line
                for (int k = 0; k < 42; k++) vga_putchar('\b');
                for (int k = 0; k < 42; k++) vga_putchar(' ');
                for (int k = 0; k < 42; k++) vga_putchar('\b');

                if (key == 0x10 || key == 'q' || key == 'Q') {
                    vga_printf("\n(quit at line %d)\n", total_lines);
                    fs_close(fd);
                    return;
                }
            }
        }
    }

    fs_close(fd);
    if (total_lines > 0) vga_printf("\n(end, %d lines)\n", total_lines);
}

// ============================================================
// Feature: drivers — list and init PCI device drivers
// ============================================================

void cmd_drivers(int argc, char** argv) {
    vga_print("\nPCI Device Drivers:\n");
    vga_print("-------------------\n");

    // Sub-command: "drivers init <n>"
    if (argc >= 3 && kstrcmp(argv[1], "init") == 0) {
        u32 idx = (u32)katoi(argv[2]);
        const pci_device_t* d = pci_get_device(idx);
        if (!d) {
            vga_printf("  Device %u not found.\n", idx);
            return;
        }

        // Try to match and init driver
        if (d->class_code == 0x02 && (d->vendor_id == 0x10EC)) {
            vga_printf("[DRIVERS] Initializing RTL8139 on device %u...\n", idx);
            int r = rtl8139_init(d);
            if (r == 0) vga_print("[DRIVERS] RTL8139 initialized successfully!\n");
            else vga_printf("[DRIVERS] RTL8139 init failed (err=%d)\n", r);
        } else if (d->class_code == 0x04 && d->subclass == 0x01) {
            vga_printf("[DRIVERS] Initializing AC97 on device %u...\n", idx);
            int r = ac97_init(d);
            if (r == 0) vga_print("[DRIVERS] AC97 initialized successfully!\n");
            else vga_printf("[DRIVERS] AC97 init failed (err=%d)\n", r);
        } else {
            vga_print("  No driver available for this device.\n");
        }
        return;
    }

    // Default: list all devices;
    u32 count = pci_get_device_count();
    if (count == 0) {
        vga_print("  No PCI devices found.\n");
        vga_print("  Run 'pci' to see detected devices.\n");
        return;
    }
    for (u32 i = 0; i < count; i++) {
        const pci_device_t* d = pci_get_device(i);
        const char* vendor = pci_vendor_name(d->vendor_id);
        const char* devname = pci_device_name(d->vendor_id, d->device_id,
                                       d->class_code, d->subclass);
        const char* classname = pci_class_name(d->class_code, d->subclass);
        const char* driver = pci_suggest_driver(d->class_code, d->subclass,
                                              d->prog_if);

        vga_printf("%u. %02X:%02X.%u ", i+1,
                   (u32)d->bus, (u32)d->dev, (u32)d->func);
        if (vendor) vga_printf("%-8s ", vendor);
        if (devname) vga_printf("%-24s ", devname);
        if (!devname && classname) vga_printf("%-24s ", classname);
        vga_putchar('\n');

        // BARs
        for (u8 b = 0; b < 6; b++) {
            u32 bar = d->bar[b];
            if (bar == 0) continue;
            bool io = (bar & 1) != 0;
            if (io) {
                vga_printf("  BAR%u: I/O  0x%04lx\n", b,
                           (unsigned long)(bar & 0xFFFFFFFC));
            } else {
                u64 addr = bar & 0xFFFFFFF0;
                vga_printf("  BAR%u: MEM  0x%08lx\n", b,
                           (unsigned long)addr);
            }
        }

        // Driver status
        if (driver) {
            vga_printf("  Suggested driver: %s", driver);

            // Check if driver is already initialized
            if (d->class_code == 0x02 && d->device_id == 0x8129) {
                if (rtl8139_get()) {
                    vga_print("  Status: ACTIVE\n");
                } else {
                    vga_printf("  Status: not initialized (run 'drivers init %u')\n", i);
                }
            } else if (d->class_code == 0x04 && d->subclass == 0x01) {
                if (ac97_get()) {
                    vga_print("  Status: ACTIVE\n");
                } else {
                    vga_printf("  Status: not initialized (run 'drivers init %u')\n", i);
                }
            }
        }
    }
    vga_printf("\nTotal: %u device(s)\n", count);
}

