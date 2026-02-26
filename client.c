#define _POSIX_C_SOURCE 200112L
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <strings.h>


#define LINE_MAXLEN 4096
#define PROMPT "mytftp> "
#define CHUNK 65536



static ssize_t sendEverything(int fd, const void *buf, size_t len) 
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

static ssize_t recvEverything(int fd, void *buf, size_t len) 
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
        if (i + 1 < outcap)
            out[i++] = c;
    }
}

static int sendNumber(int fd, uint64_t v) 
{
    return sendEverything(fd, &v, sizeof(v)) == (ssize_t)sizeof(v) ? 0 : -1;
}

static int recvNumber(int fd, uint64_t *out) 
{
    return recvEverything(fd, out, sizeof(*out)) == (ssize_t)sizeof(*out) ? 0 : -1;
}

static int connectServer(const char *host, const char *port_str) 
{
    struct addrinfo hints = {0}, *res;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port_str, &hints, &res) != 0)
        return -1;

    int fd = -1;
    for (struct addrinfo *p = res; p; p = p->ai_next) 
    {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

static const char *basename_simple(const char *path) 
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void textReciever(int fd) 
{
    uint64_t len;
    if (recvNumber(fd, &len) != 0) 
    {
        fprintf(stderr, "Failed to read output length.\n");
        return;
    }

    uint8_t buf[8192];
    while (len > 0) 
    {
        size_t want = len > sizeof(buf) ? sizeof(buf) : (size_t)len;
        ssize_t n = recv(fd, buf, want, 0);
        if (n <= 0) break;
        fwrite(buf, 1, (size_t)n, stdout);
        len -= (uint64_t)n;
    }
    fflush(stdout);
}

static void getReceiver(int fd, const char *remote_filename) 
{
    uint64_t file_size;
    if (recvNumber(fd, &file_size) != 0) 
    {
        fprintf(stderr, "Failed to read file size.\n");
        return;
    }

    const char *local = basename_simple(remote_filename);
    FILE *fp = fopen(local, "wb");
    if (!fp) 
    {
        perror("fopen");
        return;
    }

    uint8_t buf[CHUNK];
    while (file_size > 0) 
    {
        size_t want = file_size > CHUNK ? CHUNK : (size_t)file_size;
        ssize_t n = recv(fd, buf, want, 0);
        if (n <= 0) break;
        fwrite(buf, 1, (size_t)n, fp);
        file_size -= (uint64_t)n;
    }

    fclose(fp);
    printf("Downloaded %s\n", local);
}

static void putReceiver(int fd, const char *local_filename) 
{
    FILE *fp = fopen(local_filename, "rb");
    if (!fp) {
        perror("fopen");
        sendNumber(fd, 0);
        return;
    }

    fseek(fp, 0, SEEK_END);
    uint64_t file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    sendNumber(fd, file_size);

    uint8_t buf[CHUNK];
    while (file_size > 0) 
    {
        size_t n = fread(buf, 1, CHUNK, fp);
        if (n == 0) break;
        sendEverything(fd, buf, n);
        file_size -= n;
    }

    fclose(fp);

    char status[LINE_MAXLEN];
    if (recvLine(fd, status, sizeof(status)) > 0) 
    {
        if (strcmp(status, "OK") == 0)
            printf("Upload complete.\n");
        else
            fprintf(stderr, "%s\n", status);
    }
}

int main(int argc, char **argv) 
{
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <server_host> <server_port>\n", argv[0]);
        return 1;
    }

    int fd = connectServer(argv[1], argv[2]);
    if (fd < 0) 
    {
        fprintf(stderr, "Connection failed.\n");
        return 1;
    }

    char input[LINE_MAXLEN];

    while (1) 
    {
        printf(PROMPT);
        fflush(stdout);

        if (!fgets(input, sizeof(input), stdin))
            strcpy(input, "quit\n");

        input[strcspn(input, "\r\n")] = '\0';
        if (strlen(input) == 0) continue;

        char sendbuf[LINE_MAXLEN + 2];
        snprintf(sendbuf, sizeof(sendbuf), "%s\n", input);
        sendEverything(fd, sendbuf, strlen(sendbuf));

        char status[LINE_MAXLEN];
        if (recvLine(fd, status, sizeof(status)) <= 0)
            break;

        char cmd[64];
        sscanf(input, "%63s", cmd);

        if (strcmp(status, "OK") == 0) 
        {
            if (strcasecmp(cmd, "quit") == 0) break;
            else if (strcasecmp(cmd, "get") == 0)
                getReceiver(fd, strchr(input, ' ') + 1);
            else if (strcasecmp(cmd, "put") == 0)
                putReceiver(fd, strchr(input, ' ') + 1);
            else
                textReciever(fd);
        } else 
        {
            fprintf(stderr, "%s\n", status);
            if (strcasecmp(cmd, "quit") == 0) break;
        }
    }

    close(fd);
    return 0;
}

