#include <debug.h>
#include <errno.h>
#include <nuttx/config.h>
#include <nuttx/drivers/drivers.h>
#include <nuttx/fs/fs.h>
#include <poll.h>
#include <stdbool.h>
#include <string.h>
#include <sys/types.h>

static int my_open(FAR struct file* filep);
static int my_close(FAR struct file* filep);
static ssize_t my_read(FAR struct file* filep, FAR char* buffer, size_t buflen);
static ssize_t my_write(FAR struct file* filep, FAR const char* buffer,
                        size_t buflen);
static int my_ioctl(FAR struct file* filep, int cmd, unsigned long arg);
static int my_poll(FAR struct file* filep, FAR struct pollfd* fds, bool setup);

static const struct file_operations g_my_driver_fops = {
  my_open,  /* open */
  my_close, /* close */
  my_read,  /* read */
  my_write, /* write */
  NULL,     /* seek */
  my_ioctl, /* ioctl */
  NULL,     /* mmap */
  NULL,     /* truncate */
  my_poll   /* poll */
};

static struct {
  int read_pos;
} g_priv;

static int my_open(FAR struct file* filep) {
  filep->f_inode->i_private;
  _err("My driver Opened!\n");
  return OK;
}

static int my_close(FAR struct file* filep) {
  _err("My driver Closed!\n");
  return OK;
}

static ssize_t my_read(FAR struct file* filep, FAR char* buffer,
                       size_t buflen) {
  const char* text = "Reading result";
  _err("My driver Reading for %d bytes!\n Returning %s\n", buflen, text);
  return strlen(text);
}

static ssize_t my_write(FAR struct file* filep, FAR const char* buffer,
                        size_t buflen) {
  _err("My driver Writing for %d bytes: %.*s\n", buflen, buflen, buffer);
  return buflen;
}

static int my_ioctl(FAR struct file* filep, int cmd, unsigned long arg) {
  _err("My driver IOCTL with cmd=%d and arg=%ld\n", cmd, arg);
  return OK;
}

static int my_poll(FAR struct file* filep, FAR struct pollfd* fds, bool setup) {
  return EINVAL;
}

void my_driver_register(void) {
  g_priv.read_pos = 0;
  register_driver("/dev/mydev", &g_my_driver_fops, 0666, NULL);
}
