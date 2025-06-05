#include <stdio.h>
#include <stdlib.h>
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

#define CSI "\033["
#define NL CSI"K\r\n"

#define RED     CSI"31m"
#define GREEN   CSI"32m"
#define YELLOW  CSI"33m"
#define BLUE    CSI"34m"
#define MAGENTA CSI"35m"
#define CYAN    CSI"36m"
#define WHITE   CSI"37m"
#define RESET   CSI"0m"

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
    // Main loop
    for (char c;;) {
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n < 0) {
            fprintf(stderr, "[ERROR] %s\r\n", strerror(errno));
            break;
        }
        ioctl(STDOUT_FILENO, TIOCGWINSZ, &w_size);

        printf(CSI";H"); // Move Cursor to (1, 1);

        printf(RED"[INFO] Entering raw mode... (CTRL+C to exit)"NL);
        printf("[INFO] Column count: %d"NL,       w_size.ws_col);
        printf("[INFO] Row count:    %d"RESET NL, w_size.ws_row);

        DIR* proc_dir = opendir("/proc/"); // readdir consumes proc_dir so we have to open it once more
        // readdir(3) is in the POSIX standard but not in the C standard
        for (struct dirent* dir = readdir(proc_dir); dir; dir = readdir(proc_dir)) {
            // /proc/./ also has a cmdline file but it is irrelevant to us so we explicitly ignore it
            if (dir->d_type == DT_DIR && dir->d_name[0] != '.') {
                sprintf(tmp_str, "/proc/%s/cmdline", dir->d_name);
                FILE* cmdline = fopen(tmp_str, "r");
                if (cmdline) {
                    int pid_len = printf(CYAN"%4s"RESET": ", dir->d_name);
                    size_t n = fread(tmp_str, sizeof *tmp_str, TMP_STR_SIZE - 1, cmdline);

                    char* tmp_str2 = tmp_str + n;
                    char* tmp_cur = tmp_str2;
                    // the cmdline file contains the program name and the list of arguements as a null-seperated list, so we iterate through it like this
                    for (char* i = tmp_str; i < tmp_str + n;) {
                        int n = sprintf(tmp_cur, "%s ", i);
                        tmp_cur += n;
                        i += n + 1;
                    }
                    fclose(cmdline);

                    // Center the command line string
                    int cmdline_len = tmp_cur - tmp_str2;
                    printf("%*s", (w_size.ws_col - cmdline_len)/2 - pid_len, "");
                    printf(GREEN"%s"RESET NL, tmp_str2);
                }

            }
        }
        closedir(proc_dir);

        if (n > 0) {
            printf(BLUE"Code: %d"RESET NL, c);
            if (c == 3) { // CTRL+C
                printf("^C"NL);
                fflush(stdout);
                break;
            }
        } else {
            printf(RED"Press a key to see its ASCII code."RESET NL);
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
