#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/select.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

jmp_buf env;
char* err_prefix;

// Based on code from https://github.com/nelhage/reptyr

int check(char* prefix, int n) {
  if (n < 0) {
    err_prefix = prefix;
    longjmp(env, errno);
  } else {
    return n;
  }
}

int writeall(int fd, const void *buf, ssize_t count) {
  ssize_t n;
  while (count > 0) {
    n = write(fd, buf, count);
    if (n < 0) {
      if (errno == EINTR) continue;
      return n;
    }
    count -= n;
    buf += n;
  }
  return 0;
}

void proxy(int in, int out, int err) {
  char buf[PATH_MAX];
  ssize_t count;
  fd_set set;
  int nfds = (out > err ? out : err) + 1;

  while (1) {
    FD_ZERO(&set);
    FD_SET(0, &set);
    FD_SET(out, &set);
    if (err != out) FD_SET(err, &set);

    if (select(nfds, &set, NULL, NULL, NULL) < 0) {
      if (errno == EINTR) continue;
      fprintf(stderr, "Error %d on select\n", errno);
      return;
    }

    if (FD_ISSET(0, &set)) {
      count = check("read in", read(0, buf, sizeof buf));
      if (count == 0) return;
      writeall(in, buf, count);
    }
    if (FD_ISSET(out, &set)) {
      count = check("read out", read(out, buf, sizeof buf));
      if (count == 0) return;
      writeall(1, buf, count);
    }
    if (err != out && FD_ISSET(err, &set)) {
      count = check("read err", read(err, buf, sizeof buf));
      writeall(2, buf, count);
    }
  }
}

int open_pty() {
  int fd = check("posix_openpt", posix_openpt(O_RDWR | O_NOCTTY));

  check("grantpt",  grantpt(fd));
  check("unlockpt", unlockpt(fd));

  return fd;
}

char* path(char* dir, char* base) {
  static char path[PATH_MAX];
  snprintf(path, PATH_MAX, "%s/%s", dir, base);
  return path;
}

int open_fifo(int oflag, char* dir, char* base) {
  char* filename = path(dir, base);
  check("mkfifo", mkfifo(filename, 0666));
  return check("open", open(filename, oflag));
}

void write_tty_name(char* tty_name, char* dir) {
  FILE* tty = fopen(path(dir, "tty"), "w");
  if (tty_name) fprintf(tty, "%s", tty_name);
  fclose(tty);
}

int streql(char* s1, char* s2) {
  return (s1 && s2 && strcmp(s1, s2) == 0);
}

int main(int argc, char** argv) {
  if (argc != 2) {
    fprintf(stderr, "Usage: drip_proxy dir\n");
    exit(1);
  }
  char* jvm_dir = argv[1];

  char* tty_name = ttyname(0);
  struct termios prev;
  int exit_code = 0;

  int in  = 0;
  int out = 0;
  int err = 0;

  if (setjmp(env) == 0) { // try
    if (tty_name) {
      check("tcgetcattr", tcgetattr(0, &prev));
      struct termios raw = prev;
      cfmakeraw(&raw);
      check("tcsetattr raw", tcsetattr(0, TCSANOW, &raw));

      int pty = open_pty();
      char* pty_name = ptsname(pty);

      in = pty;
      check("symlink in", symlink(pty_name, path(jvm_dir, "in")));

      if (streql(tty_name, ttyname(1))) {
        out = pty;
        check("symlink out", symlink(pty_name, path(jvm_dir, "out")));
      }

      if (streql(tty_name, ttyname(2))) {
        err = pty;
        check("symlink err", symlink(pty_name, path(jvm_dir, "err")));
      }
    }
    write_tty_name(tty_name, jvm_dir);

    if (!in)  in  = open_fifo(O_WRONLY, jvm_dir, "in");
    if (!out) out = open_fifo(O_RDONLY, jvm_dir, "out");
    if (!err) err = open_fifo(O_RDONLY, jvm_dir, "err");

    proxy(in, out, err);
  } else { // catch
    exit_code = 1;
  }

  if (tty_name) {
    check("tcsetattr prev", tcsetattr(0, TCSANOW, &prev));
  }

  if (exit_code) perror(err_prefix);

  return exit_code;
}
