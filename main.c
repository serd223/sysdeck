#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <termios.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/ioctl.h>

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
*/

#define PRINT(fmt) (line_count + 1 < w_size.ws_row) ? printf(fmt) : 0
#define PRINTF(fmt, ...) (line_count + 1 < w_size.ws_row) ? printf(fmt, __VA_ARGS__) : 0

#define CSI "\033["
#define NLC CSI"K\r\n"
#define NL PRINT(NLC); line_count++
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

#define TMP_STR_SIZE 4096
char tmp_str[TMP_STR_SIZE] = {0};
int main(void) {
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
    size_t proc_count = 0, shown_procs = 0;
    int current_pid = 0, current_thread = 0;
    bool show_help = true;
    bool sending_signal = false;

    // TODO: Unused
    (void)signals_scroll; (void)current_signal; (void) current_thread;

    // Main loop
    for (char c;;) {
        size_t line_count = 0;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n < 0) {
            fprintf(stderr, "[ERROR] %s\r\n", strerror(errno));
            break;
        }
        ioctl(STDOUT_FILENO, TIOCGWINSZ, &w_size);

        printf(CSI";H"RESET); // Move Cursor to (1, 1);

        int badge_len = sprintf(tmp_str, "Currently running processes: %lu | Processes shown: %lu", proc_count, shown_procs);
        bool fits = badge_len < w_size.ws_col;
        PRINT(WHITE CSI"48;5;1m");
        if (fits) PRINTF("%*s", (w_size.ws_col - badge_len)/2 - PIDTEXT_LEN, "");
        PRINTF("%.*s", fits ? badge_len : w_size.ws_col, tmp_str); NL;

        PRINT(RESET WHITE CSI"48;5;4mPID |");
        PRINTF("%*s", (w_size.ws_col - 8)/2 - 5, "");
        PRINT("CMDLINE "); NL;
        PRINT(RESET BLUE);
        for (int i = 0; i < w_size.ws_col; ++i) printf("-");
        PRINT(RESET); NL;

        proc_count = 0; shown_procs = 0;
        DIR* proc_dir = opendir("/proc/"); // readdir consumes proc_dir so we have to open it once more
        // readdir(3) is in the POSIX standard but not in the C standard
        for (struct dirent* dir = readdir(proc_dir); dir; dir = readdir(proc_dir)) {
            // /proc/./ also has a cmdline file but it is irrelevant to us so we explicitly ignore it
            if (dir->d_type == DT_DIR && dir->d_name[0] != '.') {
                sprintf(tmp_str, "/proc/%s/cmdline", dir->d_name);
                FILE* cmdline = fopen(tmp_str, "r");
                if (cmdline) {
                    if (shown_procs < MAX_SHOWN_PROCS && proc_count >= procs_scroll) {
                        PRINTF(CYAN"%4s"RESET": ", dir->d_name);
                        size_t n = fread(tmp_str, sizeof *tmp_str, TMP_STR_SIZE - 1, cmdline);

                        char* tmp_str2 = tmp_str + n;
                        char* tmp_cur = tmp_str2;
                        // the cmdline file contains the program name and the list of arguements as a null-seperated list, so we iterate through it like this
                        for (char* i = tmp_str; i < tmp_str + n;) {
                            int n = sprintf(tmp_cur, "%s ", i);
                            tmp_cur += n;
                            i += n;
                        }
                        fclose(cmdline);

                        // Center the command line string
                        int cmdline_len = tmp_cur - tmp_str2;
                        bool fits = cmdline_len + PIDTEXT_LEN < w_size.ws_col;
                        if (fits) PRINTF("%*s", (w_size.ws_col - cmdline_len)/2 - PIDTEXT_LEN, "");
                        if (current_proc == shown_procs) {
                            current_pid = atoi(dir->d_name);
                            printf(CSI"7m");
                        }
                        if (PRINTF(GREEN"%.*s"RESET, fits ? cmdline_len : w_size.ws_col - PIDTEXT_LEN, tmp_str2) > 0) shown_procs++;
                        NL;
                    }
                    proc_count++;
                }

            }
        }
        closedir(proc_dir);
        if (current_proc >= shown_procs) {
            current_proc = shown_procs - 1;
        }

        if (n > 0) {
            if (c == 3) { // CTRL+C
                PRINT("^C"); NL;
                fflush(stdout);
                break;
            } else if (c == 'q' || c == 'Q') {
                break;  
            } else if (!sending_signal && (c == 'h' || c == 'H')) {
                show_help = !show_help;
            } else if (c == 'k' || c == 'K') {
                if (!sending_signal) {
                    if (current_proc <= 0) {
                        if (procs_scroll > 0) procs_scroll--;
                    } else {
                        current_proc--;
                    }
                } else {
                    
                }
            } else if (c == 'j' || c == 'J') {
                if (!sending_signal) {
                    if (current_proc >= shown_procs - 1) {
                        if (shown_procs + procs_scroll < proc_count) procs_scroll++;
                    } else {
                        current_proc++;
                    }
                } else {
                    
                }
            } else if (!sending_signal && (c == 't' || c == 'T')) {
                sprintf(tmp_str, "kill -s TERM %d", current_pid);
                system(tmp_str);
            } else if (!sending_signal && (c == 's' || c == 'S')) {
                sending_signal = true;
                current_signal = 0;
            } else if (sending_signal && c == 27) { // ESC
                sending_signal = false;
            }
        }
        PRINT(BLUE);
        if (line_count + 1 < w_size.ws_row) for (int i = 0; i < w_size.ws_col; ++i) printf("-");
        PRINT(RESET); NL;
        if (show_help && !sending_signal) {
            PRINT(WHITE CSI"48;5;4mK/J      -> Select Up/Down"); NL;
            PRINT("H        -> Toggle this help text"); NL;
            PRINT("Q/CTRL+C -> Quit"); NL;
            PRINT("T        -> Send SIGTERM to selected proc"); NL;
            PRINT("S        -> Send signal to selected proc (opens signal selection menu)"); NL;
            PRINT("ESC      -> Cancel send signal"); NL; printf(RESET);
        }
        printf(CSI"J"); // Clear the rest of the screen
        fflush(stdout);
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
