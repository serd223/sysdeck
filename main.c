#include <stdio.h>
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
TODO: The directory '/proc/<pid>/task' contains the <pid> directories of tasks associated with this proc
*/

#define CMD_BUF_SIZE 4096
// Max size of the pathname string here is 270 bytes since dirent.d_name is char[256] but I set the size to 512 for a nice even number
#define CMDLINE_PATHNAME_BUF_SIZE 512

int main(void) {
    DIR* proc_dir = opendir("/proc/");
    char cmdline_pathname_buf[CMDLINE_PATHNAME_BUF_SIZE] = {0};
    char cmd_buf[CMD_BUF_SIZE] = {0};
    // readdir(2) is in the POSIX standard but not in the C standard
    for (struct dirent* dir = readdir(proc_dir); dir; dir = readdir(proc_dir)) {
        // /proc/./ also has a cmdline file but it is irrelevant to us so we explicitly ignore it
        if (dir->d_type == DT_DIR && dir->d_name[0] != '.') {
            sprintf(cmdline_pathname_buf, "/proc/%s/cmdline", dir->d_name);
            FILE* cmdline = fopen(cmdline_pathname_buf, "r");
            if (cmdline) {
                printf("Command line of pid %s: ", dir->d_name);
                size_t n = fread(cmd_buf, sizeof *cmd_buf, CMD_BUF_SIZE - 1, cmdline);
                // the cmdline file contains the program name and the list of arguements as a null-seperated list, so we iterate through it like this
                for (char* i = cmd_buf; i < cmd_buf + n; i += strlen(i) + 1) {
                    printf("%s ", i);
                }
                printf("\n");
                fclose(cmdline);
            }

        }
    }
    closedir(proc_dir);
   
    if (!isatty(STDIN_FILENO)) return 1;

    struct termios saved = {0};
    if (tcgetattr(STDIN_FILENO, &saved) != 0) {
        fprintf(stderr, "[ERROR] %s\n", strerror(errno));
        return 1;
    } else {
        struct termios raw = saved;
        make_raw(&raw);
        printf("[INFO] Entering raw mode... (CTRL+C to exit)\n");
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        printf("\033[?25l"); // Escape code to hide cursor
        fflush(stdout);
    }

    int printed_n = 0;    
    for (char c;;) {
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n < 0) {
            fprintf(stderr, "[ERROR] %s\r\n", strerror(errno));
            break;
        } else if (n > 0) {
            int current_printed_n = printf("Code: %d", c);
            for (int i = 0; i < printed_n - current_printed_n; ++i) printf(" ");
            printf("\r");
            fflush(stdout);
            printed_n = current_printed_n;
            if (c == 3) { // CTRL+C
                current_printed_n = printf("^C");
                for (int i = 0; i < printed_n - current_printed_n; ++i) printf(" ");
                printf("\r\n");
                fflush(stdout);
                break;
            }
        } else {
            int current_printed_n = printf("Press a key to see its ASCII code.");
            for (int i = 0; i < printed_n - current_printed_n; ++i) printf(" ");
            printf("\r");
            fflush(stdout);
            printed_n = current_printed_n;
        }
    }

    tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    printf("\033[?25h"); // Escape code to show cursor
    printf("[INFO] Restored terminal.\n");
    return 0;
}
