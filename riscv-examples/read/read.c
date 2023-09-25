#include <unistd.h>
#include <string.h>
int main()
{
    char prompt [] = "Please enter text to be echoed: \n";
    write(STDOUT_FILENO, prompt, strlen(prompt));
    char buffer[256];
    int n = read(STDIN_FILENO, buffer, sizeof(buffer)-1);
    buffer[n] = '\n';
    write(STDOUT_FILENO, buffer, n);
    return 0;
}
