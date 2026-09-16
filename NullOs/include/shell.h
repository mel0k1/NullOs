#ifndef SHELL_H
#define SHELL_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SHELL_MAX_CMD_LEN   256
#define SHELL_MAX_ARGS      16
#define SHELL_HISTORY_SIZE  32

/* Max bytes captured by `cmd > file` output redirection */
#define SHELL_REDIRECT_MAX  (32 * 1024)

typedef struct {
    char* name;
    void (*handler)(int argc, char** argv);
    char* description;
} shell_command_t;

typedef struct {
    char current_dir[MAX_PATH];
    char cmd_buffer[SHELL_MAX_CMD_LEN];
    char* args[SHELL_MAX_ARGS];
    int  arg_count;
    char history[SHELL_HISTORY_SIZE][SHELL_MAX_CMD_LEN];
    u32  history_index;
    u32  history_size;
    u32  history_browse_index;  // For up/down arrow navigation
    bool running;
    int  input_pos;  // Cursor position in cmd_buffer during input (for VT switch)
} shell_t;

extern shell_t shell;

void shell_init(void);
void shell_run(void);
void shell_stop(void);
void shell_process_command(void);
void shell_add_to_history(const char* cmd);
void shell_print_prompt(void);
char* shell_trim(char* str);

// Built-in commands
void cmd_help(int argc, char** argv);
void cmd_clear(int argc, char** argv);
void cmd_echo(int argc, char** argv);
void cmd_reboot(int argc, char** argv);
void cmd_shutdown(int argc, char** argv);
void cmd_meminfo(int argc, char** argv);
void cmd_ls(int argc, char** argv);
void cmd_cat(int argc, char** argv);
void cmd_grep(int argc, char** argv);
void cmd_wc(int argc, char** argv);
void cmd_head(int argc, char** argv);
void cmd_tail(int argc, char** argv);
void cmd_mkdir(int argc, char** argv);
void cmd_touch(int argc, char** argv);
void cmd_rm(int argc, char** argv);
void cmd_pwd(int argc, char** argv);
void cmd_sysinfo(int argc, char** argv);
void cmd_cd(int argc, char** argv);
void cmd_uptime(int argc, char** argv);
void cmd_write(int argc, char** argv);
void cmd_date(int argc, char** argv);
void cmd_serial(int argc, char** argv);
void cmd_tasks(int argc, char** argv);
void cmd_spawn(int argc, char** argv);
void cmd_multi(int argc, char** argv);
void cmd_preempt(int argc, char** argv);
void cmd_history(int argc, char** argv);
void cmd_ata(int argc, char** argv);
void cmd_elf(int argc, char** argv);
void cmd_apic(int argc, char** argv);
void cmd_pci(int argc, char** argv);
void cmd_fb(int argc, char** argv);
void cmd_scrollback(int argc, char** argv);
void cmd_slab(int argc, char** argv);
void cmd_neofetch(int argc, char** argv);
void cmd_ping(int argc, char** argv);
void cmd_sync(int argc, char** argv);
void cmd_mount(int argc, char** argv);
void cmd_ext4mount(int argc, char** argv);
void cmd_ext4sync(int argc, char** argv);
void cmd_https(int argc, char** argv);
void cmd_tlssanity(int argc, char** argv);
void cmd_rawmode(int argc, char** argv);
void cmd_dmesg(int argc, char** argv);
void cmd_hexdump(int argc, char** argv);
void cmd_env(int argc, char** argv);
void cmd_export(int argc, char** argv);
void cmd_more(int argc, char** argv);
void cmd_mouse(int argc, char** argv);
void cmd_fat32(int argc, char** argv);
void cmd_drivers(int argc, char** argv);
void cmd_rtl8139(int argc, char** argv);

#ifdef __cplusplus
}
#endif

#endif // SHELL_H
