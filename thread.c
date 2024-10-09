
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>

// Define the client structure
typedef struct s_client {
    int fd;
    int id;
    char *msg;
    struct s_client *next;
    struct s_server *server; // Reference to server structure for sending messages
} t_client;

// Define the server structure
typedef struct s_server {
    int sockfd;
    int port;
    int counter;
    struct sockaddr_in addr;
    t_client *head;
    pthread_mutex_t lock;  // Mutex for thread-safe operations
} t_server;

// Function prototypes
void fatalError(t_server *s);
void deleteAll(t_server *s);
void freeClient(t_client *cli);
void deregisterClient(t_server *s, int fd, int cli_id);
void sendNotification(t_server *s, int fd, char *msg);
char *str_join(char *buf, char *add);
void *clientHandler(void *arg);
t_client *findClient(t_server *s, int fd);
void registerClient(t_server *s, int fd);
void handleCon(t_server *s);
void bindAndListen(t_server *s);
void configAddr(t_server *s);
void createSock(t_server *s);
t_server *initServer(int port);
void removeClient(t_server *s, int fd);
int extract_message(char **buf, char **msg);

// Handle fatal errors and free resources
void fatalError(t_server *s) {
    deleteAll(s);
    write(2, "Fatal error\n", 12);
    exit(1);
}

// Extract messages separated by '\n' from the buffer
int extract_message(char **buf, char **msg) {
    char *newbuf;
    int i;

    *msg = 0;
    if (*buf == 0)
        return (0);
    i = 0;
    while ((*buf)[i]) {
        if ((*buf)[i] == '\n') {
            newbuf = calloc(1, sizeof(*newbuf) * (strlen(*buf + i + 1) + 1));
            if (newbuf == 0)
                return (-1);
            strcpy(newbuf, *buf + i + 1);
            *msg = *buf;
            (*msg)[i + 1] = 0;
            *buf = newbuf;
            return (1);
        }
        i++;
    }
    return (0);
}

// Concatenate two strings, freeing the first one
char *str_join(char *buf, char *add) {
    char *newbuf;
    int len;

    if (buf == 0)
        len = 0;
    else
        len = strlen(buf);
    newbuf = malloc(sizeof(*newbuf) * (len + strlen(add) + 1));
    if (newbuf == 0)
        return (0);
    newbuf[0] = 0;
    if (buf != 0)
        strcat(newbuf, buf);
    free(buf);
    strcat(newbuf, add);
    return (newbuf);
}

// Free the memory allocated for a client
void freeClient(t_client *cli) {
    if (cli) {
        if (cli->msg) free(cli->msg);
        if (cli->fd > 0) close(cli->fd);
        free(cli);
    }
}

// Add a client to the server's client list
t_client *addClient(t_server *s, int fd) {
    t_client *cli = (t_client *)malloc(sizeof(t_client));
    if (!cli) fatalError(s);
    bzero(cli, sizeof(t_client));
    cli->fd = fd;
    cli->id = s->counter++;
    cli->msg = NULL;
    cli->server = s;  // Assign server reference to client
    cli->next = s->head;
    s->head = cli;
    return cli;
}

// Find a client by file descriptor
t_client *findClient(t_server *s, int fd) {
    t_client *tmp = s->head;
    while (tmp && tmp->fd != fd)
        tmp = tmp->next;
    return tmp;
}

// Remove a client from the server's client list
void removeClient(t_server *s, int fd) {
    t_client *tmp = s->head;
    t_client *prev = NULL;

    pthread_mutex_lock(&s->lock);  // Lock the mutex before modifying the list
    while (tmp && tmp->fd != fd) {
        prev = tmp;
        tmp = tmp->next;
    }
    if (tmp) {
        if (prev)
            prev->next = tmp->next;
        else
            s->head = tmp->next;
        freeClient(tmp);
    }
    pthread_mutex_unlock(&s->lock);  // Unlock the mutex after modifying the list
}

// Delete all clients and server resources
void deleteAll(t_server *s) {
    pthread_mutex_lock(&s->lock);
    t_client *tmp = s->head;

    while (tmp) {
        t_client *cache = tmp;
        tmp = tmp->next;
        freeClient(cache);
    }
    if (s->sockfd > 0) {
        close(s->sockfd);
        s->sockfd = -1;
    }
    pthread_mutex_unlock(&s->lock);
    pthread_mutex_destroy(&s->lock);
    free(s);
}

// Send a notification message to all clients except the sender
void sendNotification(t_server *s, int fd, char *msg) {
    pthread_mutex_lock(&s->lock);
    t_client *cli = s->head;
    while (cli) {
        if (cli->fd != fd)
            if (send(cli->fd, msg, strlen(msg), 0) < 0) fatalError(s);
        cli = cli->next;
    }
    pthread_mutex_unlock(&s->lock);
}

// Send a message from one client to others
void sendMessage(t_server *s, t_client *cli) {
    char buf[127];
    char *msg;
    while (extract_message(&cli->msg, &msg)) {
        sprintf(buf, "client %d: ", cli->id);
        sendNotification(s, cli->fd, buf);
        sendNotification(s, cli->fd, msg);
        free(msg);
    }
}

// Remove a client and notify others
void deregisterClient(t_server *s, int fd, int cli_id) {
    char buf[127];
    sprintf(buf, "server: client %d just left\n", cli_id);
    sendNotification(s, fd, buf);
    removeClient(s, fd);
}

// Handle messages for a specific client in a separate thread
void *clientHandler(void *arg) {
    t_client *cli = (t_client *)arg;
    t_server *s = cli->server;
    char buf[4096];
    int read_bytes;

    while ((read_bytes = recv(cli->fd, buf, sizeof(buf) - 1, 0)) > 0) {
        buf[read_bytes] = '\0';
        cli->msg = str_join(cli->msg, buf);
        sendMessage(s, cli);
    }

    if (read_bytes <= 0) {
        printf("Client %d disconnected\n", cli->id);
        deregisterClient(s, cli->fd, cli->id);
        close(cli->fd);
    }
    pthread_exit(NULL);
}

// Register a new client and create a thread to handle it
void registerClient(t_server *s, int fd) {
    t_client *cli = addClient(s, fd);
    pthread_t thread_id;

    if (pthread_create(&thread_id, NULL, clientHandler, (void *)cli) != 0) {
        perror("Failed to create thread");
        freeClient(cli);
    }
    pthread_detach(thread_id);  // Automatically free resources on thread exit
    printf("Client %d connected and handler thread created\n", cli->id);
}

// Handle incoming connections in the main thread
void handleCon(t_server *s) {
    while (1) {
        struct sockaddr_in cli_addr;
        socklen_t cli_len = sizeof(cli_addr);
        int client_fd = accept(s->sockfd, (struct sockaddr *)&cli_addr, &cli_len);

        if (client_fd < 0) {
            perror("Failed to accept client connection");
            continue;
        }
        registerClient(s, client_fd);
    }
}

// Bind and listen on the specified socket
void bindAndListen(t_server *s) {
    if (bind(s->sockfd, (const struct sockaddr *)&s->addr, sizeof(s->addr)) < 0) fatalError(s);
    if (listen(s->sockfd, SOMAXCONN) < 0) fatalError(s);
}

// Configure the server address
void configAddr(t_server *s) {
    bzero(&s->addr, sizeof(s->addr));
    s->addr.sin_family = AF_INET;
    s->addr.sin_addr.s_addr = htonl(2130706433); // 127.0.0.1 in network byte order
    s->addr.sin_port = htons(s->port);
}

// Create a socket for the server
void createSock(t_server *s) {
    s->sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (s->sockfd < 0) {
        perror("Failed to create socket");
        fatalError(s);
    }
}

// Initialize the server structure
t_server *initServer(int port) {
    t_server *s = (t_server *)malloc(sizeof(t_server));
    if (!s) fatalError(NULL);
    bzero(s, sizeof(t_server));
    s->port = port;
    s->head = NULL;
    s->counter = 1;
    pthread_mutex_init(&s->lock, NULL);  // Initialize the mutex lock
    return s;
}

// Main function to initialize and run the server
int main(int ac, char **av) {
    if (ac != 2) {
        write(2, "Wrong number of arguments\n", 26);
        exit(1);
    }

    int port = atoi(av[1]);
    if (port <= 0 || port > 65535) {
        write(2, "Invalid port number\n", 20);
        exit(1);
    }

    // Initialize the server
    t_server *serv = initServer(port);
    if (serv) {
        createSock(serv);      // Create the server socket
        configAddr(serv);      // Configure the server address
        bindAndListen(serv);   // Bind and start listening for connections
        handleCon(serv);       // Handle incoming connections
        deleteAll(serv);       // Clean up resources
    }
    return 0;
}
