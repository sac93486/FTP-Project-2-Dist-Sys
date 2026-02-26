#define _POSIX_C_SOURCE 200112L
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LINE_MAXLEN 4096
#define BUF_CHUNK   8192




static ssize_t sendAll(int fd, const void *buf, size_t len) 
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t sent = 0;
    while (sent < len) 
    {
        ssize_t n = send(fd, p + sent, len - sent, 0);
        if (n < 0) 
        {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) break;
        sent += (size_t)n;
    }
    return (ssize_t)sent;
}

static ssize_t recvAll(int fd, void *buf, size_t len) 
{
    uint8_t *p = (uint8_t *)buf;
    size_t recvd = 0;
    while (recvd < len) 
    {
        ssize_t n = recv(fd, p + recvd, len - recvd, 0);
        if (n < 0) 
        {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return 0; 
        recvd += (size_t)n;
    }
    return (ssize_t)recvd;
}


static int recvLine(int fd, char *out, size_t outcap) 
{
    size_t i = 0;
    while (1) 
    {
        char c;
        ssize_t n = recv(fd, &c, 1, 0);
        if (n < 0) 
        {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return 0;
        if (c == '\n') 
        {
            out[i] = '\0';
            return 1;
        }
        if (i + 1 < outcap) out[i++] = c;

    }
}

static int sendLine(int fd, const char *line) 
{
    size_t len = strlen(line);
    return (sendAll(fd, line, len) == (ssize_t)len) ? 0 : -1;
}


static int sendNumber(int fd, uint64_t v) 
{
    return (sendAll(fd, &v, sizeof(v)) == (ssize_t)sizeof(v)) ? 0 : -1;
}
static int recvNumber(int fd, uint64_t *out) 
{
    return (recvAll(fd, out, sizeof(*out)) == (ssize_t)sizeof(*out)) ? 0 : -1;
}

static void sendErrorMessage(int client_fd, const char *msg) 
{
    char line[LINE_MAXLEN];
    snprintf(line, sizeof(line), "ERR %s\n", msg);
    (void)sendLine(client_fd, line);
}
static void checkOk(int client_fd) 
{
    (void)sendLine(client_fd, "OK\n");
}


static void textReceiver(int client_fd, const char *text) 
{
    uint64_t len = (uint64_t)strlen(text);
    (void)sendNumber(client_fd, len);
    if (len > 0) (void)sendAll(client_fd, text, (size_t)len);
}


static void pwdFunction(int client_fd) 
{
    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd))) 
    {
        sendErrorMessage(client_fd, "Failed to get current directory.");
        return;
    }
    checkOk(client_fd);


    char out[8192];
    snprintf(out, sizeof(out), "%s\n", cwd);
    textReceiver(client_fd, out);
}

static void cdFunction(int client_fd, const char *dir) 
{
    if (chdir(dir) != 0) 
    {
        sendErrorMessage(client_fd, "Failed to change directory.");
        return;
    }
    checkOk(client_fd);
    textReceiver(client_fd, "Directory changed successfully.\n");
}

static void mkdirFunction(int client_fd, const char *dir) 
{
    if (mkdir(dir, 0777) != 0) 
    {
        sendErrorMessage(client_fd, "Failed to create directory.");
        return;
    }
    checkOk(client_fd);
    textReceiver(client_fd, "Directory created successfully.\n");
}

static void deleteFunction(int client_fd, const char *filename) 
{
    if (remove(filename) != 0) 
    {
        sendErrorMessage(client_fd, "Failed to delete file.");
        return;
    }
    checkOk(client_fd);

    char out[LINE_MAXLEN];
    snprintf(out, sizeof(out), "File '%s' deleted successfully.\n", filename);
    textReceiver(client_fd, out);
}


static void lsFunction(int client_fd) 
{
    DIR *d = opendir(".");
    if (!d) 
    {
        sendErrorMessage(client_fd, "Could not open current directory.");
        return;
    }


    size_t cap = 4096, len = 0;
    char *out = (char *)malloc(cap);
    if (!out) 
    {
        closedir(d);
        sendErrorMessage(client_fd, "Out of memory.");
        return;
    }
    out[0] = '\0';

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) 
    {
        const char *name = ent->d_name;

        
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

        size_t need = strlen(name) + 1; 
        if (len + need + 1 > cap) 
        {
            cap *= 2;
            char *tmp = (char *)realloc(out, cap);
            if (!tmp) 
            {
                free(out);
                closedir(d);
                sendErrorMessage(client_fd, "Out of memory.");
                return;
            }
            out = tmp;
        }
        memcpy(out + len, name, strlen(name));
        len += strlen(name);
        out[len++] = '\n';
        out[len] = '\0';
    }
    closedir(d);

    checkOk(client_fd);
    textReceiver(client_fd, out);
    free(out);
}

static void getFunction(int client_fd, const char *filename) 
{
    FILE *fp = fopen(filename, "rb");
    if (!fp) 
    {
        sendErrorMessage(client_fd, "File not found.");
        return;
    }

    if (fseek(fp, 0, SEEK_END) != 0) 
    {
        fclose(fp);
        sendErrorMessage(client_fd, "Failed to seek file.");
        return;
    }
    long sz = ftell(fp);
    if (sz < 0) 
    {
        fclose(fp);
        sendErrorMessage(client_fd, "Failed to get file size.");
        return;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) 
    {
        fclose(fp);
        sendErrorMessage(client_fd, "Failed to seek file.");
        return;
    }

    uint64_t file_size = (uint64_t)sz;

    checkOk(client_fd);
    if (sendNumber(client_fd, file_size) != 0) 
    {
        fclose(fp);
        return;
    }

    uint8_t buf[BUF_CHUNK];
    uint64_t sent = 0;
    while (sent < file_size) 
    {
        size_t want = (file_size - sent) > sizeof(buf) ? sizeof(buf) : (size_t)(file_size - sent);
        size_t nread = fread(buf, 1, want, fp);
        if (nread == 0) break;
        if (sendAll(client_fd, buf, nread) != (ssize_t)nread) break;
        sent += (uint64_t)nread;
    }

    fclose(fp);
}

static void putFunction(int client_fd, const char *filename) 
{

    checkOk(client_fd);

    uint64_t file_size = 0;
    if (recvNumber(client_fd, &file_size) != 0) 
    {
        sendErrorMessage(client_fd, "Failed to read upload size.");
        return;
    }

    FILE *fp = fopen(filename, "wb");
    if (!fp) 
    {

        uint64_t remaining = file_size;
        uint8_t drain[BUF_CHUNK];
        while (remaining > 0) 
        {
            size_t want = remaining > sizeof(drain) ? sizeof(drain) : (size_t)remaining;
            ssize_t n = recv(client_fd, drain, want, 0);
            if (n <= 0) break;
            remaining -= (uint64_t)n;
        }
        sendErrorMessage(client_fd, "Failed to create local file on server.");
        return;
    }

    uint64_t remaining = file_size;
    uint8_t buf[BUF_CHUNK];
    while (remaining > 0) 
    {
        size_t want = remaining > sizeof(buf) ? sizeof(buf) : (size_t)remaining;
        ssize_t n = recv(client_fd, buf, want, 0);
        if (n <= 0) 
        {
            fclose(fp);
            sendErrorMessage(client_fd, "Connection closed during upload.");
            return;
        }
        size_t w = fwrite(buf, 1, (size_t)n, fp);
        if (w != (size_t)n) 
        {
            fclose(fp);
            sendErrorMessage(client_fd, "Disk write error on server.");
            return;
        }
        remaining -= (uint64_t)n;
    }

    fclose(fp);


    checkOk(client_fd);
}


static void clientServe(int client_fd) 
{
    char line[LINE_MAXLEN];

    while (1) 
    {
        int r = recvLine(client_fd, line, sizeof(line));
        if (r <= 0) break; 


        size_t n = strlen(line);
        if (n > 0 && line[n - 1] == '\r') line[n - 1] = '\0';


        char cmd[32] = {0};
        sscanf(line, "%31s", cmd);

        if (strcmp(cmd, "quit") == 0) 
        {

            checkOk(client_fd);
            break;
        } else if (strcmp(cmd, "pwd") == 0) 
        {
            pwdFunction(client_fd);
        } else if (strcmp(cmd, "ls") == 0) 
        {
            lsFunction(client_fd);
        } else if (strcmp(cmd, "cd") == 0) 
        {

            const char *arg = strchr(line, ' ');
            cdFunction(client_fd, arg ? arg + 1 : "");
        } else if (strcmp(cmd, "mkdir") == 0) 
        {
            const char *arg = strchr(line, ' ');
            mkdirFunction(client_fd, arg ? arg + 1 : "");
        } else if (strcmp(cmd, "delete") == 0) 
        {
            const char *arg = strchr(line, ' ');
            deleteFunction(client_fd, arg ? arg + 1 : "");
        } else if (strcmp(cmd, "get") == 0) 
        {
            const char *arg = strchr(line, ' ');
            getFunction(client_fd, arg ? arg + 1 : "");
        } else if (strcmp(cmd, "put") == 0) 
        {
            const char *arg = strchr(line, ' ');
            putFunction(client_fd, arg ? arg + 1 : "");
        } else 
        {

            sendErrorMessage(client_fd, "Unknown command.");
        }
    }
}

int main(int argc, char **argv) 
{
    if (argc != 2) 
    {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return 2;
    }

    int port = atoi(argv[1]);
    if (port <= 0 || port > 65535) 
    {
        fprintf(stderr, "Invalid port.\n");
        return 2;
    }

    
    char server_start_dir[4096];
    if (!getcwd(server_start_dir, sizeof(server_start_dir))) 
    {
        perror("getcwd");
        return 1;
    }

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) 
    {
        perror("socket");
        return 1;
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) 
    {
        perror("setsockopt");
        close(server_fd);
        return 1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) 
    {
        perror("bind");
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, 10) < 0) 
    {
        perror("listen");
        close(server_fd);
        return 1;
    }

    printf("myftpserver listening on port %d...\n", port);

    while (1) 
    {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) 
        {
            perror("accept");
            continue;
        }


        if (chdir(server_start_dir) != 0) 
        {

            perror("chdir(server_start_dir)");
        }

        clientServe(client_fd);
        printf("Client disconnected.\n");
        close(client_fd);
        printf("Waiting for next client\n");
    }

    close(server_fd);
    return 0;
}

