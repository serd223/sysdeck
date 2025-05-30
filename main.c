#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <termios.h>
#include <unistd.h>
#include <dirent.h>

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
If scanning the '/proc/' directory manually becomes too cumbersome, perhaps parse the output of something like `ps -eLo pid,tid,user,%cpu,%mem,args` instead
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

#define CMD_BUF_SIZE 4096
char cmd_buf[CMD_BUF_SIZE] = {0};
int main(void) {
    // Init 
    if (!isatty(STDIN_FILENO)) return 1;

    struct termios saved = {0};
    if (tcgetattr(STDIN_FILENO, &saved) != 0) {
        fprintf(stderr, "[ERROR] %s\n", strerror(errno));
        return 1;
    }

    system("tput smcup"); // Enter alternate screen
    printf(CSI";H"); // Move Cursor to (1, 1)
    struct termios raw = saved;
    make_raw(&raw);
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    printf(CSI"?25l"); // Escape code to hide cursor
    fflush(stdout);

    // Main loop
    for (char c;;) {
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n < 0) {
            fprintf(stderr, "[ERROR] %s\r\n", strerror(errno));
            break;
        }

        printf(CSI";H"); // Move Cursor to (1, 1)

        printf(RED"[INFO] Entering raw mode... (CTRL+C to exit)"RESET NL);
        DIR* proc_dir = opendir("/proc/"); // readdir consumes proc_dir so we have to open it once more
        // readdir(3) is in the POSIX standard but not in the C standard
        for (struct dirent* dir = readdir(proc_dir); dir; dir = readdir(proc_dir)) {
            // /proc/./ also has a cmdline file but it is irrelevant to us so we explicitly ignore it
            if (dir->d_type == DT_DIR && dir->d_name[0] != '.') {
                sprintf(cmd_buf, "/proc/%s/cmdline", dir->d_name);
                FILE* cmdline = fopen(cmd_buf, "r");
                if (cmdline) {
                    printf(CYAN"%4s"RESET": ", dir->d_name);
                    size_t n = fread(cmd_buf, sizeof *cmd_buf, CMD_BUF_SIZE - 1, cmdline);
                    // the cmdline file contains the program name and the list of arguements as a null-seperated list, so we iterate through it like this
                    printf(GREEN);
                    for (char* i = cmd_buf; i < cmd_buf + n; i += strlen(i) + 1) printf("%s ", i);
                    printf(RESET NL);
                    fclose(cmdline);
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
    return 0;
}
