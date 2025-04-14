#include <fcntl.h>
#include <getopt.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "game.h"

#define XO_STATUS_FILE "/sys/module/kxo/initstate"
#define XO_DEVICE_FILE "/dev/kxo"
#define XO_DEVICE_ATTR_FILE "/sys/class/kxo/kxo/kxo_state"

struct task {
    jmp_buf env;
    // char *task_name;
};

/*for schedule to save status*/
jmp_buf schedule_buf;

struct task *tasklist[3];

static bool read_attr, end_attr;
fd_set readset;
int device_fd;
int max_fd;

static char table[N_GRIDS];

static int draw_board(char *table)
{
    int k = 0;
    printf("\n");
    printf("\n");

    for (int row = 0; row < BOARD_SIZE; row++) {
        for (int j = 0; j < (BOARD_SIZE << 1) - 1 && k < N_GRIDS; j++) {
            char draw = j & 1 ? '|' : table[k++];
            if (!draw)
                draw = ' ';
            printf("%c", draw);
        }
        printf("\n");
        for (int j = 0; j < (BOARD_SIZE << 1) - 1; j++) {
            printf("-");
        }
        printf("\n");
    }
    return 0;
}

static bool status_check(void)
{
    FILE *fp = fopen(XO_STATUS_FILE, "r");
    if (!fp) {
        printf("kxo status : not loaded\n");
        return false;
    }

    char read_buf[20];
    fgets(read_buf, 20, fp);
    read_buf[strcspn(read_buf, "\n")] = 0;
    if (strcmp("live", read_buf)) {
        printf("kxo status : %s\n", read_buf);
        fclose(fp);
        return false;
    }
    fclose(fp);
    return true;
}

static struct termios orig_termios;

static void raw_mode_disable(void)
{
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

static void raw_mode_enable(void)
{
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(raw_mode_disable);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}


static void listen_keyboard_handler(void)
{
    int attr_fd = open(XO_DEVICE_ATTR_FILE, O_RDWR);
    char input;

    if (read(STDIN_FILENO, &input, 1) == 1) {
        char buf[20];
        switch (input) {
        case 16: /* Ctrl-P */
            read(attr_fd, buf, 6);
            buf[0] = (buf[0] - '0') ? '0' : '1';
            read_attr ^= 1;
            write(attr_fd, buf, 6);
            if (!read_attr)
                printf("Stopping to display the chess board...\n");
            break;
        case 17: /* Ctrl-Q */
            read(attr_fd, buf, 6);
            buf[4] = '1';
            read_attr = false;
            end_attr = true;
            write(attr_fd, buf, 6);
            printf("Stopping the kernel space tic-tac-toe game...\n");
            break;
        }
    }
    close(attr_fd);
}

int current = -1;

/*first game include AI1 and AI2*/
static void task1(void)
{
    if (setjmp(tasklist[0]->env) == 0)
        longjmp(schedule_buf, 1);

    char step[5];

    if (read_attr && FD_ISSET(device_fd, &readset)) {
        step[4] = '\0';
        FD_CLR(device_fd, &readset);

        read(device_fd, step, 4);
        printf("\033[H\033[J"); /* ASCII escape code to clear the screen */
        table[step[1] - '0'] = step[0];
        draw_board(table);

        if (step[2] == 'W') {
            printf("%c win!!!\n", step[0]);
            memset(table, ' ',
                   N_GRIDS); /* Reset the table so the game restart */
            // read(device_fd, step, 4);
            memset(step, ' ', 4); /* Reset the table so the game restart */
        }
    }

    longjmp(schedule_buf, 1);
}
static void task2(void)
{
    if (setjmp(tasklist[1]->env) == 0)
        longjmp(schedule_buf, 1);

    longjmp(schedule_buf, 1);
}
static void keyboard(void)
{
    if (setjmp(tasklist[2]->env) == 0)
        longjmp(schedule_buf, 1);

    if (FD_ISSET(STDIN_FILENO, &readset)) {
        FD_CLR(STDIN_FILENO, &readset);
        listen_keyboard_handler();
    }
    longjmp(schedule_buf, 1);
}

/*initial tasklist and add task1 task2 into it*/
static void initial_tasklist(void)
{
    struct task *task_1 = malloc(sizeof(struct task));
    struct task *task_2 = malloc(sizeof(struct task));
    struct task *key_board = malloc(sizeof(struct task));

    // task1->task_name = strdup("task1");
    // task2->task_name = strdup("task2");
    // keyboard->task_name = strdup("keyboard");

    tasklist[0] = task_1;
    tasklist[1] = task_2;
    tasklist[2] = key_board;
}
/*open file and use select to listen the file*/
static void file_listen(void)
{
    FD_ZERO(&readset);
    FD_SET(STDIN_FILENO, &readset);
    FD_SET(device_fd, &readset);

    int result = select(max_fd + 1, &readset, NULL, NULL, NULL);
    if (result < 0) {
        printf("Error with select system call\n");
        exit(1);
    }
}

/*the function to switch task*/
static void switch_task()
{
    current %= 3;
    longjmp(tasklist[current]->env, 1);
}
/**/
int n = 3;
static void schedule(void)
{
    initial_tasklist();
    setjmp(schedule_buf);

    if (n != 0) {
        if (n == 3) {
            n--;
            task1();
        } else if (n == 2) {
            n--;
            task2();
        } else {
            n--;
            keyboard();
        }
    }

    if (end_attr)
        return;

    file_listen();
    current++;
    switch_task();
}

int main(int argc, char *argv[])
{
    if (!status_check())
        exit(1);

    raw_mode_enable();
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);


    // fd_set readset;
    device_fd = open(XO_DEVICE_FILE, O_RDONLY);
    max_fd = device_fd > STDIN_FILENO ? device_fd : STDIN_FILENO;
    read_attr = true;
    end_attr = false;

    schedule();

    for (int i = 0; i < 2; i++)
        free(tasklist[i]);

    raw_mode_disable();
    fcntl(STDIN_FILENO, F_SETFL, flags);

    close(device_fd);

    return 0;
}
