#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>

#define APP_NAME "sntc"
#define VERSION "0.1.0"
#define PORT "51631"
#define MSG_HDR_SIZE 5
#define MAX_CLIENTS 64

enum msg_type {
  MSG_JOIN = 1,
  MSG_CHAT,
  MSG_LEAVE,
};

struct msg {
  uint8_t type;
  uint32_t len;
  char payload[4096];
};

struct user {
  int fd;
  char name[32];
};

static void msg_set(struct msg *m, uint8_t type, const char *fmt, ...) {
  m->type = type;
  va_list ap;
  va_start(ap, fmt);
  m->len = vsnprintf(m->payload, sizeof m->payload, fmt, ap);
  va_end(ap);
}

static int msg_send(int fd, const struct msg *m) {
  uint8_t buf[4096 + MSG_HDR_SIZE];
  uint32_t net_len = htonl(m->len);

  buf[0] = m->type;
  memcpy(buf + 1, &net_len, 4);
  memcpy(buf + MSG_HDR_SIZE, m->payload, m->len);

  size_t total = MSG_HDR_SIZE + m->len;
  size_t sent = 0;
  while (sent < total) {
    ssize_t n = send(fd, buf + sent, total - sent, 0);
    if (n <= 0)
      return -1;
    sent += n;
  }
  return 0;
}

static int msg_recv(int fd, struct msg *m) {
  uint8_t hdr[MSG_HDR_SIZE];
  size_t got = 0;
  while (got < MSG_HDR_SIZE) {
    ssize_t n = recv(fd, hdr + got, MSG_HDR_SIZE - got, 0);
    if (n <= 0)
      return -1;
    got += n;
  }

  m->type = hdr[0];
  uint32_t net_len;
  memcpy(&net_len, hdr + 1, 4);
  m->len = ntohl(net_len);

  if (m->len > sizeof m->payload - 1)
    return -1;

  got = 0;
  while (got < m->len) {
    ssize_t n = recv(fd, m->payload + got, m->len - got, 0);
    if (n <= 0)
      return -1;
    got += n;
  }
  m->payload[m->len] = '\0';
  return 0;
}

// socket setup

typedef int (*socket_fn)(int fd, const struct addrinfo *ai);

static int do_bind(int fd, const struct addrinfo *ai) {
  int yes = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
  return bind(fd, ai->ai_addr, ai->ai_addrlen);
}

static int do_connect(int fd, const struct addrinfo *ai) {
  return connect(fd, ai->ai_addr, ai->ai_addrlen);
}

static int open_socket(const char *host, int flags, socket_fn setup) {
  struct addrinfo hints = {
      .ai_family = AF_UNSPEC,
      .ai_socktype = SOCK_STREAM,
      .ai_flags = flags,
  };
  struct addrinfo *res;

  int status = getaddrinfo(host, PORT, &hints, &res);
  if (status != 0) {
    fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(status));
    return -1;
  }

  int fd = -1;
  for (struct addrinfo *p = res; p; p = p->ai_next) {
    fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (fd == -1)
      continue;
    if (setup(fd, p) == 0)
      break;
    close(fd);
    fd = -1;
  }
  freeaddrinfo(res);
  return fd;
}

// server

static struct user clients[MAX_CLIENTS];
static int num_clients;

static void broadcast(const struct msg *m, int exclude_fd) {
  for (int i = 0; i < num_clients; i++) {
    if (clients[i].fd != exclude_fd)
      msg_send(clients[i].fd, m);
  }
}

static void remove_client(int index) {
  struct msg m;
  msg_set(&m, MSG_LEAVE, "%s left", clients[index].name);
  printf("%s disconnected\n", clients[index].name);
  close(clients[index].fd);
  clients[index] = clients[--num_clients];
  broadcast(&m, -1);
}

static void gen_password(char *buf, size_t len) {
  static const char charset[] = "abcdefghijkmnpqrstuvwxyz23456789";
  FILE *f = fopen("/dev/urandom", "rb");
  for (size_t i = 0; i < len; i++) {
    unsigned char b;
    if (!f || fread(&b, 1, 1, f) != 1)
      b = (unsigned char)rand();
    buf[i] = charset[b % (sizeof charset - 1)];
  }
  if (f)
    fclose(f);
  buf[len] = '\0';
}

static void print_join_commands(const char *password) {
  struct ifaddrs *ifaddr, *ifa;
  if (getifaddrs(&ifaddr) == -1)
    return;

  printf("\nshare this command to join:\n");
  for (ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
    if (!ifa->ifa_addr)
      continue;
    int family = ifa->ifa_addr->sa_family;
    if (family != AF_INET && family != AF_INET6)
      continue;
    char host[NI_MAXHOST];
    socklen_t sa_len = (family == AF_INET) ? sizeof(struct sockaddr_in)
                                           : sizeof(struct sockaddr_in6);
    getnameinfo(ifa->ifa_addr, sa_len, host, sizeof host, NULL, 0,
                NI_NUMERICHOST);
    if (strcmp(host, "127.0.0.1") == 0 || strcmp(host, "::1") == 0)
      continue;
    if (family == AF_INET6 && strncmp(host, "fe80:", 5) == 0)
      continue;
    const char *fmt = (family == AF_INET6) ? "  " APP_NAME " join [%s] pass=%s\n"
                                           : "  " APP_NAME " join %s pass=%s\n";
    printf(fmt, host, password);
  }
  printf("\n");
  freeifaddrs(ifaddr);
}

static int run_server(void) {
  int server_fd = open_socket(NULL, AI_PASSIVE, do_bind);
  if (server_fd == -1) {
    fprintf(stderr, "failed to bind\n");
    return 1;
  }

  if (listen(server_fd, 10) == -1) {
    perror("listen");
    return 1;
  }

  char password[17];
  gen_password(password, 16);

  printf("server listening on port %s\n", PORT);
  print_join_commands(password);

  while (1) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    FD_SET(server_fd, &fds);
    int maxfd = server_fd;

    for (int i = 0; i < num_clients; i++) {
      FD_SET(clients[i].fd, &fds);
      if (clients[i].fd > maxfd)
        maxfd = clients[i].fd;
    }

    if (select(maxfd + 1, &fds, NULL, NULL, NULL) == -1) {
      perror("select");
      break;
    }

    if (FD_ISSET(server_fd, &fds)) {
      int fd = accept(server_fd, NULL, NULL);
      if (fd != -1) {
        if (num_clients >= MAX_CLIENTS) {
          close(fd);
        } else {
          clients[num_clients].fd = fd;
          clients[num_clients].name[0] = '\0';
          num_clients++;
        }
      }
    }

    if (FD_ISSET(STDIN_FILENO, &fds)) {
      char line[4096];
      if (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\n")] = '\0';
        struct msg m;
        msg_set(&m, MSG_CHAT, "host: %s", line);
        broadcast(&m, -1);
      }
    }

    for (int i = 0; i < num_clients; i++) {
      if (!FD_ISSET(clients[i].fd, &fds))
        continue;

      struct msg m;
      if (msg_recv(clients[i].fd, &m) != 0) {
        remove_client(i--);
        continue;
      }

      switch (m.type) {
      case MSG_JOIN: {
        char *sep = memchr(m.payload, '\n', m.len);
        if (sep)
          *sep = '\0';
        if (!sep || strcmp(m.payload, password) != 0) {
          printf("rejected connection (bad password)\n");
          close(clients[i].fd);
          clients[i] = clients[--num_clients];
          i--;
          continue;
        }
        char *client_name = sep + 1;
        strncpy(clients[i].name, client_name, sizeof clients[i].name - 1);
        clients[i].name[sizeof clients[i].name - 1] = '\0';
        printf("%s joined\n", clients[i].name);

        struct msg welcome;
        msg_set(&welcome, MSG_JOIN, "welcome, %s", clients[i].name);
        msg_send(clients[i].fd, &welcome);

        struct msg notify;
        msg_set(&notify, MSG_JOIN, "%s joined", clients[i].name);
        broadcast(&notify, clients[i].fd);
        break;
      }
      case MSG_CHAT: {
        printf("%s: %s\n", clients[i].name, m.payload);
        struct msg out;
        msg_set(&out, MSG_CHAT, "%s: %s", clients[i].name, m.payload);
        broadcast(&out, clients[i].fd);
        break;
      }
      default:
        break;
      }
    }
  }

  close(server_fd);
  return 0;
}

// client

static int run_client(const char *host, const char *password) {
  int sock_fd = open_socket(host, 0, do_connect);
  if (sock_fd == -1) {
    fprintf(stderr, "failed to connect\n");
    return 1;
  }

  printf("connected to %s:%s\n", host, PORT);

  char name[32];
  printf("name: ");
  fflush(stdout);
  if (!fgets(name, sizeof name, stdin))
    return 1;
  name[strcspn(name, "\n")] = '\0';

  struct msg join;
  msg_set(&join, MSG_JOIN, "%s\n%s", password, name);
  if (msg_send(sock_fd, &join) != 0) {
    close(sock_fd);
    return 1;
  }

  while (1) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    FD_SET(sock_fd, &fds);
    int maxfd = sock_fd > STDIN_FILENO ? sock_fd : STDIN_FILENO;

    if (select(maxfd + 1, &fds, NULL, NULL, NULL) == -1) {
      perror("select");
      break;
    }

    if (FD_ISSET(STDIN_FILENO, &fds)) {
      char line[4096];
      if (!fgets(line, sizeof line, stdin))
        break;
      line[strcspn(line, "\n")] = '\0';

      struct msg m;
      msg_set(&m, MSG_CHAT, "%s", line);
      if (msg_send(sock_fd, &m) != 0)
        break;
    }

    if (FD_ISSET(sock_fd, &fds)) {
      struct msg m;
      if (msg_recv(sock_fd, &m) != 0) {
        printf("disconnected\n");
        break;
      }

      switch (m.type) {
      case MSG_JOIN:
      case MSG_LEAVE:
        printf("* %s\n", m.payload);
        break;
      case MSG_CHAT:
        printf("%s\n", m.payload);
        break;
      default:
        printf("unknown message type %d\n", m.type);
        break;
      }
    }
  }

  close(sock_fd);
  return 0;
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s init | join [host] pass=<password>\n", argv[0]);
    return 1;
  }

  if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0) {
    printf(APP_NAME " %s\n", VERSION);
    return 0;
  }

  if (strcmp(argv[1], "init") == 0)
    return run_server();

  if (strcmp(argv[1], "join") == 0) {
    const char *host = "127.0.0.1";
    const char *password = NULL;
    static char hostbuf[NI_MAXHOST];
    for (int i = 2; i < argc; i++) {
      if (strncmp(argv[i], "pass=", 5) == 0) {
        password = argv[i] + 5;
      } else {
        host = argv[i];
        if (host[0] == '[') {
          strncpy(hostbuf, host + 1, sizeof hostbuf - 1);
          hostbuf[sizeof hostbuf - 1] = '\0';
          char *bracket = strchr(hostbuf, ']');
          if (bracket)
            *bracket = '\0';
          host = hostbuf;
        }
      }
    }
    if (!password) {
      fprintf(stderr, "missing password (pass=...)\n");
      return 1;
    }
    return run_client(host, password);
  }

  fprintf(stderr, "unknown command\n");
  return 1;
}
