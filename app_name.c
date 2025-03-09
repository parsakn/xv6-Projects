#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"

int isBalanced(char *str) {
    int balance = 0;
    for (int i = 0; str[i] != '\0'; i++) {
        if (str[i] == '{') {
            balance++;
        } else if (str[i] == '}') {
            balance--;
            if (balance < 0) {
                return 0; // Unbalanced closing brace
            }
        }
    }
    return balance == 0; // Balanced if balance is 0
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf(2, "Usage: app_name <string>\n");
        exit();
    }

    char *input = argv[1];
    int isValid = isBalanced(input);

    // Open or create Result.txt
    int fd = open("Result.txt", O_CREATE | O_WRONLY);
    if (fd < 0) {
        printf(2, "Error: Could not create Result.txt\n");
        exit();
    }

    // Write "Right" or "Wrong" to the file
    if (isValid) {
        write(fd, "Right\n", 6);
    } else {
        write(fd, "Wrong\n", 6);
    }

    close(fd);
    exit();
}