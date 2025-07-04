#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <termios.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <signal.h>

// see termios(3) 'Raw mode' for details
void make_raw(struct termios *t) {
    t->c_iflag &= ~(IGNBRK | BRKINT | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    t->c_oflag &= ~OPOST;
    t->c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    t->c_cflag &= ~CSIZE; // clean character size mask
    t->c_cflag |= CS8; // set character size to 8 bits

    // read a minimum of 0 characters for each `read` call
    t->c_cc[VMIN] = 0; // set to 1 for a guaranteed read
    t->c_cc[VTIME] = 1; // set read timeout to 100ms 
}

/*
NOTES:
The symlink '/proc/<pid>/exe' points to the executable (requires sudo permissions and the use of readlink(2))
The file '/proc/<pid>/cmdline' contains the command line string that invoked the program (easily readable)
TODO: The directory '/proc/<pid>/task' contains the <tid> directories of threads associated with this proc
Although it kinda contradicts the whole idea of the project, if scanning the '/proc/' directory manually becomes too cumbersome, perhaps parse the output of something like `ps -eLo pid,tid,user,%cpu,%mem,args` instead
'/proc/meminfo/' for system memory info
'/proc/<pid>/stat/' for process memory info
Use the time informaton from '/proc/cpuinfo' and the stat file to calculate %CPU and cpu core utilization
Divide the time data from the files by clock_tick to get the time in seconds
*/

#define PRINTF(...) ((line_count + 1 < w_size.ws_row) ? printf(__VA_ARGS__) : 0)

#define ESC "\033"
#define CSI ESC"["
#define NLC CSI"K\r\n"
#define NL PRINTF(NLC); line_count++
#define BLACK   CSI"30m"
#define RED     CSI"31m"
#define GREEN   CSI"32m"
#define YELLOW  CSI"33m"
#define BLUE    CSI"34m"
#define MAGENTA CSI"35m"
#define CYAN    CSI"36m"
#define WHITE   CSI"37m"
#define RESET   CSI"0m"

// "0000: "
#define PIDTEXT_LEN 6

#define MAX_SHOWN_PROCS 20
#define MAX_SHOWN_SIGNALS 10

typedef struct {
    pid_t pid;
    char cmdline[128];
    size_t time; // utime + stime
} Proc;

typedef struct {
    Proc* items;
    size_t len, __cap;
} Procs;

void grow_procs(Procs* procs, size_t size) {
    if (procs->__cap == 0) procs->items = malloc((procs->__cap = 4) * sizeof *procs->items);
    if (size > procs->__cap) procs->items = realloc(procs->items, (procs->__cap *= (size % procs->__cap + 1)) * sizeof *procs->items);
}

void push_proc(Procs* procs, Proc proc) {
    grow_procs(procs, procs->len + 1);
    procs->items[procs->len++] = proc;
}

void copy_procs(Procs* src, Procs* dest) {
    grow_procs(dest, src->len);
    dest->items = memcpy(dest->items, src->items, src->len);
    dest->len = src->len;
}

Proc* search_pid(Procs* procs, int pid) {
    for (size_t i = 0; i < procs->len; ++i) {
        if (procs->items[i].pid == pid) {
            return procs->items + i;
        }
    }
    return NULL;
}

typedef struct {
    const char* str_repr;
    int code; 
} Signal;

Signal signals[] = {
    { .str_repr = "SIGABRT", .code = SIGABRT },
    { .str_repr = "SIGALRM", .code = SIGALRM },
    { .str_repr = "SIGBUS", .code = SIGBUS },
    { .str_repr = "SIGCHLD", .code = SIGCHLD },
    { .str_repr = "SIGCONT", .code = SIGCONT },
    { .str_repr = "SIGFPE", .code = SIGFPE },
    { .str_repr = "SIGHUP", .code = SIGHUP },
    { .str_repr = "SIGILL", .code = SIGILL },
    { .str_repr = "SIGINT", .code = SIGINT },
    { .str_repr = "SIGKILL", .code = SIGKILL },
    { .str_repr = "SIGPIPE", .code = SIGPIPE },
    { .str_repr = "SIGPOLL", .code = SIGPOLL },
    { .str_repr = "SIGPROF", .code = SIGPROF },
    { .str_repr = "SIGQUIT", .code = SIGQUIT },
    { .str_repr = "SIGSEGV", .code = SIGSEGV },
    { .str_repr = "SIGSTOP", .code = SIGSTOP },
    { .str_repr = "SIGTSTP", .code = SIGTSTP },
    { .str_repr = "SIGSYS", .code = SIGSYS },
    { .str_repr = "SIGTERM", .code = SIGTERM },
    { .str_repr = "SIGTRAP", .code = SIGTRAP },
    { .str_repr = "SIGTTIN", .code = SIGTTIN },
    { .str_repr = "SIGTTOU", .code = SIGTTOU },
    { .str_repr = "SIGURG", .code = SIGURG },
    { .str_repr = "SIGUSR1", .code = SIGUSR1 },
    { .str_repr = "SIGUSR2", .code = SIGUSR2 },
    { .str_repr = "SIGVTALRM", .code = SIGVTALRM },
    { .str_repr = "SIGXCPU", .code = SIGXCPU },
    { .str_repr = "SIGXFSZ", .code = SIGXFSZ },
};

typedef enum {
    FOCUS_PROCS,
    FOCUS_SIGNALS,
    FOCUS_SEARCH, // TODO(SEARCHING)
    FOCUS_SORT    // TODO(SORTING)
} InputFocus;

#define TMP_STR_SIZE 4096
char tmp_str[TMP_STR_SIZE] = {0};
int main(void) {
    long clock_tick = sysconf(_SC_CLK_TCK);
    // Init 
    if (!isatty(STDIN_FILENO)) return 1;

    struct termios saved = {0};
    if (tcgetattr(STDIN_FILENO, &saved) != 0) {
        fprintf(stderr, "[ERROR] %s\n", strerror(errno));
        return 1;
    }

    // `tput` retrieves the value of `smcup` and `rmcup` via `infocmp` which parses the correct `terminfo` file inside the appropriate `terminfo` directory according to the TERM environment variable
    // The `terminfo` files could be inside: (searched in this order)
    //  - $HOME/.terminfo/
    //  - /etc/terminfo/
    //  - /lib/terminfo/
    //  - /usr/share/terminfo/
    // So if your TERM variable is set to `xterm-256color`, your correct `terminfo` file could be /lib/terminfo/x/xterm-256color (which is the case in my Debian install)
    // We _could_ just do all that ourselves in code but for now I will stick with `tput` for easy portability
    system("tput smcup"); // Enter alternate screen
    // printf(CSI"?1049h"CSI"22;0;0t"); // Enter alternate screen (smcup) for xterm-256color
    // fflush(stdout);

    printf(CSI";H"); // Move Cursor to (1, 1)

    struct termios raw = saved;
    make_raw(&raw);
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);

    printf(CSI"?25l"); // Escape code to hide cursor
    fflush(stdout);

    struct winsize w_size;

    // State
    size_t signals_scroll = 0, current_signal = 0;
    size_t procs_scroll = 0, current_proc = 0;
    size_t shown_procs = 0;
    pid_t current_pid = 0, current_thread = 0;
    bool show_help = true;
    InputFocus focus = FOCUS_PROCS;
    Procs procs = {0}; // Leaks
    Procs prev_procs = {0}; // Leaks

    // Unused: TODO(THREADS)
    (void)current_thread;

    // Unsued: TODO(%CPU)
    (void)prev_procs; (void)clock_tick;

    // Main loop
    for (char c;;) {
        size_t line_count = 0; // NOTE: Assumes that all lines fit the width of the terminal
        ssize_t in_count = read(STDIN_FILENO, &c, 1);
        if (in_count < 0) {
            fprintf(stderr, "[ERROR] %s\r\n", strerror(errno));
            break;
        }
        ioctl(STDOUT_FILENO, TIOCGWINSZ, &w_size);

        printf(CSI";H"RESET); // Move Cursor to (1, 1);

        int badge_len = sprintf(tmp_str, "Currently running processes: %lu | Processes shown: %lu", procs.len, shown_procs);
        bool fits = badge_len < w_size.ws_col;
        PRINTF(WHITE CSI"48;5;1m");
        if (fits) PRINTF("%*s", (w_size.ws_col - badge_len)/2 - PIDTEXT_LEN, "");
        PRINTF("%.*s", fits ? badge_len : w_size.ws_col, tmp_str); NL;

        PRINTF(RESET WHITE CSI"48;5;4mPID |");
        PRINTF("%*s", (w_size.ws_col - 8)/2 - 5, "");
        PRINTF("CMDLINE "); NL;
        PRINTF(RESET BLUE);
        for (int i = 0; i < w_size.ws_col; ++i) printf("-");
        PRINTF(RESET); NL;

        procs.len = 0; // Reset procs array
        // Collect procs
        DIR* proc_dir = opendir("/proc/"); // readdir consumes proc_dir so we have to open it once more
        // readdir(3) is in the POSIX standard but not in the C standard
        for (struct dirent* dir = readdir(proc_dir); dir; dir = readdir(proc_dir)) {
            // /proc/./ also has a cmdline file but it is irrelevant to us so we explicitly ignore it
            if (dir->d_type == DT_DIR && dir->d_name[0] != '.') {
                sprintf(tmp_str, "/proc/%s/cmdline", dir->d_name);
                FILE* cmdline = fopen(tmp_str, "rb");
                if (cmdline) {
                    Proc p = {0};
                    p.pid = atoi(dir->d_name);
                    size_t cmd_n = fread(tmp_str, sizeof *tmp_str, TMP_STR_SIZE - 1, cmdline);
                    fclose(cmdline);

                    // The memory starting from tmp_str + cmd_n is safe to use for our other temporary operations
                    char* tmp_str2 = tmp_str + cmd_n;
                    char* tmp_cur = tmp_str2;
                    // Iterate over the null-seperated list inside the cmdline file
                    for (char* i = tmp_str; i < tmp_str + cmd_n;) {
                        int n = sprintf(tmp_cur, "%s ", i);
                        tmp_cur += n;
                        i += n;
                    }
                    int cmdline_len = tmp_cur - tmp_str2;

                    // -1 just in case we somehow mess up the string buffers and are left without a null terminator
                    bool fits = (size_t)cmdline_len < sizeof (p.cmdline) - 1;
                    memcpy(p.cmdline, tmp_str2, fits ? cmdline_len : sizeof (p.cmdline) - 1);
                    push_proc(&procs, p);
                }

            }
        }
        closedir(proc_dir);

        // Display procs
        size_t pi;
        for (pi = procs_scroll; pi < procs.len; ++pi) {
            if ((pi - procs_scroll) >= MAX_SHOWN_PROCS) break;

            Proc* p = procs.items + pi;
            bool is_current = current_proc == pi - procs_scroll;
            if (is_current) {
                current_pid = p->pid;
                PRINTF(CSI"48;5;2m"BLACK);
            } else {
                PRINTF(CYAN);
            }
            // PRINTF macro returns 0 if we ran out of lines to print on (checked via line_count which is set by PRINTF and PRINT)
            if (PRINTF("%4d: ", p->pid) <= 0) break;

            int cmdline_len = (int)strlen(p->cmdline);
            bool fits = cmdline_len + PIDTEXT_LEN < w_size.ws_col;
            int padding_size = 0;
            // Padding to center the cmdline text
            if (fits) padding_size = PRINTF("%*s", (w_size.ws_col - cmdline_len)/2 - PIDTEXT_LEN, "");
            fits = cmdline_len + PIDTEXT_LEN + padding_size < w_size.ws_col;

            // Cut off the cmdline text if it doesn't fit on the screen
            if (is_current) {
                PRINTF(BLACK"%.*s", fits ? cmdline_len : (w_size.ws_col - PIDTEXT_LEN - padding_size), p->cmdline);
            } else {
                PRINTF(GREEN"%.*s", fits ? cmdline_len : (w_size.ws_col - PIDTEXT_LEN - padding_size), p->cmdline);
            }
            NL; PRINTF(RESET);
        }
        shown_procs = pi - procs_scroll;

        PRINTF(BLUE);
        if (line_count + 1 < w_size.ws_row) for (int i = 0; i < w_size.ws_col; ++i) printf("-");
        PRINTF(RESET); NL;

        size_t shown_signals = 0;
        if (focus == FOCUS_SIGNALS) {
            size_t i;
            for (i = signals_scroll; i < sizeof signals / sizeof signals[0]; ++i) {
                int len;
                bool is_current;
                if ((is_current = ((i - signals_scroll) == current_signal))) {
                    PRINTF(CSI"48;5;1m"BLACK);
                }
                len = sprintf(tmp_str, "%s", signals[i].str_repr);
                if (!PRINTF("%*s", (w_size.ws_col - len)/2, "")) break;
                if (!is_current) PRINTF(RED);
                PRINTF("%s", tmp_str);
                NL; PRINTF(RESET);
            }
            printf(RESET);
            shown_signals = i - signals_scroll;
        } else if (show_help) {
            PRINTF(WHITE CSI"48;5;4mK/J      -> Select Up/Down"); NL;
            PRINTF("H        -> Toggle this help text"); NL;
            PRINTF("Q/CTRL+C -> Quit"); NL;
            PRINTF("T        -> Send SIGTERM to selected proc"); NL;
            PRINTF("S        -> Send signal to selected proc"); NL;
            PRINTF("            |-> Opens signal selection menu: RETURN to select, ESC to cancel"); NL;
            printf(RESET);
        }

        printf(CSI"J"); // Clear the rest of the screen
        fflush(stdout);

        if (in_count > 0) {
            if (c == 3) { // CTRL+C
                PRINTF("^C"); NL;
                fflush(stdout);
                break;
            } else if (c == 'q' || c == 'Q') {
                break;
            } else {
                switch (focus) {
                case FOCUS_PROCS: {
                    if (c == 'h' || c == 'H') {
                        show_help = !show_help;
                    } else if (c == 'k' || c == 'K') {
                        if (current_proc <= 0) {
                            if (procs_scroll > 0) procs_scroll--;
                        } else {
                            current_proc--;
                        }
                    } else if (c == 'j' || c == 'J') {
                        if (current_proc >= shown_procs - 1) {
                            if (shown_procs + procs_scroll < procs.len) procs_scroll++;
                        } else {
                            current_proc++;
                        }
                    } else if (c == 't' || c == 'T') {
                        kill(current_pid, SIGTERM);
                    } else if (c == 's' || c == 'S') {
                        focus = FOCUS_SIGNALS;
                        current_signal = 0;
                    }
                } break;
                case FOCUS_SIGNALS: {
                    if (c == 27) { // ESC
                        focus = FOCUS_PROCS;
                    } else if (c == 'k' || c == 'K') {
                        if (current_signal <= 0) {
                            if (signals_scroll > 0) signals_scroll--;
                        } else {
                            current_signal--;
                        }
                    } else if (c == 'j' || c == 'J') {
                        if (current_signal >= shown_signals - 1) {
                            if (shown_signals + signals_scroll < sizeof signals / sizeof signals[0]) signals_scroll++;
                        } else {
                            current_signal++;
                        }
                    } else if (c == '\r') {
                        focus = FOCUS_PROCS;
                        kill(current_pid, signals[current_signal + signals_scroll].code);
                    }
                } break;
                case FOCUS_SEARCH: {
                        
                } break;
                case FOCUS_SORT: {
                        
                } break;
                }
            }
       }
    }

    // Cleanup
    tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    printf(CSI"?25h"); // Escape code to show cursor
    printf("[INFO] Restored terminal.\n");
    system("tput rmcup"); // Leave alternate screen
    // printf(CSI"?1049l"CSI"23;0;0t"); // Leave alternate screen (rmcup) for xterm-256color
    // fflush(stdout);
    return 0;
}
