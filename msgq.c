/* $Id$ */
/* Simple message queue implementation using UNIX domain socket.
 * Copyright (C) 2010  Seong-Kook Shin <cinsky@gmail.com>
 */

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <error.h>

#include <pthread.h>

#include "msgq.h"
#include "elist.h"


#ifndef UNIX_PATH_MAX
#define UNIX_PATH_MAX   (sizeof(struct sockaddr_un) - sizeof(sa_family_t))
#endif

#define sizeof_packet(packet)   (sizeof(*(packet)) + (packet)->size)

/*
 * All received message is packaged in struct msgq_node.  This struct
 * provides 'link' so that each struct can be wired in a doubly linked list
 * Users cannot see this struct -- users can see only the 'packet' member,
 * which is struct msgq_packet instance.
 *
 * Both struct msgq_node instance and its packet member are
 * dynamically allocated.  Once the users got the message (e.g. using
 * msgq_recv()), We do not keep a pointer to struct msgq_node
 * instance.  Instead, the 'container' member of the struct
 * msgq_packet('packet') will point the enclosing struct msgq_node.
 * See the source of msgq_pkt_delete() for more.
 */
struct msgq_node {
  struct elist link;            /* for the doubly linked list */
  char sender[UNIX_PATH_MAX];   /* sender address for 'packet */
  struct msgq_packet *packet;   /* the actual message */
};


/*
 * Currently, 'recv_mutex' is the only mutex that struct msgq_ provided.
 * Since we do not have any other queue except the 'recvq' member,  one
 * mutex is sufficient for now.
 *
 * 'recv_cond' is using together with 'recv_mutex' if needed.
 * 'recv_cond' wakes any caller that is waiting for more message,
 * specifically, msgq_recv_wait().
 */
struct msgq_ {
  int fd;
  char address[UNIX_PATH_MAX];

  unsigned char *pkbuf;         /* internal buffer to receive a message */

  int broadcast;                /* use ptheread_cond_broatcast() if nonzero */

  struct elist recvq;           /* queue for received messages */
  size_t recvs;                 /* number of packets in recvq */

  pthread_cond_t recv_cond;     /* condition for waiting for incoming msg */
  pthread_mutex_t recv_mutex;
  pthread_t receiver;           /* thread for receiving messages */
};


#define RECVQ_LOCK(msgq)        pthread_mutex_lock(&(msgq)->recv_mutex)
#define RECVQ_UNLOCK(msgq)      pthread_mutex_unlock(&(msgq)->recv_mutex)



static int validate_packet(struct msgq_packet *packet, ssize_t len);
static int msgq_start_receiver(MSGQ *msgq);
static void *msgq_receiver(void *arg);
static int msgq_get_listener(MSGQ *msgq, const char *address);

static int bind_anonymous(int fd, char address[]);
static struct msgq_packet *msgq_copy_packet(const struct msgq_packet *packet);
static struct msgq_node *msgq_node_create(const char *sender,
                                          const struct msgq_packet *packet);

static void verror_(const char *kind, int status, int errnum,
                    const char *fmt, va_list ap);
static void warning_(int errnum, const char *fmt, ...)
  __attribute__((format(printf, 2, 3)));
static void debug_(int errnum, const char *fmt, ...)
  __attribute__((format(printf, 2, 3)));

#ifndef NDEBUG
# define DEBUG(err, ...)        debug_(err, __VA_ARGS__)
# define WARN(err, ...)         warning_(err, __VA_ARGS__)
#else
# define DEBUG(err, ...)        ((void)0)
# define WARN(err, ...)         ((void)0)
#endif  /* NDEBUG */

/*
 * If nonzero, block all signals (using sigfillset()) before creating
 * the receiver thread.  The reason is that I do not know that the
 * exact meaning of 'all signals' in sigfillset context.  Is sigfillset()
 * fill *really* all signals?   I remembered that pthread uses some real-time
 * signals internally (don't remember the name of the signal though).
 *
 * Frankly, I don't think that calling sigfillset() for blocking
 * signals will cause trouble later.  Anyway, if 'block_all_signals'
 * is zero, the receiver thread will block only certain signals.  See
 * msgq_start_receiver() for more.
 */
static int block_all_signals = 1;

int
msgq_send_string(MSGQ *msgq, const char *receiver, const char *fmt, ...)
{
  va_list ap;
  int ret;
  struct msgq_packet *p;

  va_start(ap, fmt);
  ret = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  p = malloc(sizeof(*p) + ret + 1);
  if (!p)
    return -1;

  va_start(ap, fmt);
  vsnprintf(p->data, ret + 1, fmt, ap);
  va_end(ap);
  p->size = ret + 1;
  p->container = NULL;

  ret = msgq_send(msgq, receiver, p);
  free(p);
  return ret;
}


int
msgq_message_count(MSGQ *msgq)
{
  int ret;

  RECVQ_LOCK(msgq);
  ret = msgq->recvs;
  RECVQ_UNLOCK(msgq);

  return ret;
}


const char *
msgq_pkt_sender(struct msgq_packet *packet)
{
  struct msgq_node *np;
  if (!packet->container)
    return NULL;
  np = (struct msgq_node *)packet->container;
  return np->sender;
}


int
msgq_pkt_delete(struct msgq_packet *packet)
{
  struct msgq_node *np;
  if (!packet->container)
    return -1;
  np = (struct msgq_node *)packet->container;

  assert(ELIST_NEXT(np->link) == 0);
  assert(ELIST_PREV(np->link) == 0);

  free(packet);
  free(np);
  return 0;
}


struct msgq_packet *
msgq_recv_wait(MSGQ *msgq)
{
  struct elist *p;
  struct msgq_node *np;

  RECVQ_LOCK(msgq);
 again:
  p = edque_pop_front(&msgq->recvq);
  if (!p) {
    DEBUG(0, "msgq_recv_wait: waiting...");
    pthread_cond_wait(&msgq->recv_cond, &msgq->recv_mutex);
    DEBUG(0, "msgq_recv_wait: awaken!");
    goto again;
  }
  msgq->recvs--;
  RECVQ_UNLOCK(msgq);

  np = ELIST_ENTRY(p, struct msgq_node, link);
  return np->packet;
}


struct msgq_packet *
msgq_recv(MSGQ *msgq)
{
  struct elist *p;
  struct msgq_node *np;

  RECVQ_LOCK(msgq);
  p = edque_pop_front(&msgq->recvq);
  if (!p) {
    RECVQ_UNLOCK(msgq);
    return NULL;
  }
  msgq->recvs--;
  RECVQ_UNLOCK(msgq);

  np = ELIST_ENTRY(p, struct msgq_node, link);
  return np->packet;
}


int
msgq_send(MSGQ *msgq, const char *receiver, const struct msgq_packet *packet)
{
  struct sockaddr_un addr;
  ssize_t ret;

  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, receiver, sizeof(addr.sun_path) - 1);

  /* TODO: lock?? */
  ret = sendto(msgq->fd, packet, sizeof_packet(packet), MSG_NOSIGNAL,
               (struct sockaddr *)&addr, sizeof(addr));
  /* TODO: unlock?? */

  if (ret < 0) {
    WARN(errno, "sendto(2) failed");
    return -1;
  }

  return 0;
}


MSGQ *
msgq_open(const char *address)
{
  MSGQ *p;
  pthread_mutexattr_t attr;

  p = malloc(sizeof(*p));
  if (!p)
    return NULL;

  memset(p, 0, sizeof(*p));

  p->pkbuf = malloc(MSGQ_MSG_MAX);
  if (!p->pkbuf) {
    free(p);
    return NULL;
  }

  p->fd = -1;
  ELIST_INIT(p->recvq);
  p->recvs = 0;
  p->broadcast = 0;

  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
  if (pthread_mutex_init(&p->recv_mutex, &attr) != 0) {
    WARN(errno, "pthread_mutex_init(3) failed");
    goto err_free;
  }
  pthread_mutexattr_destroy(&attr);

  if (pthread_cond_init(&p->recv_cond, NULL) != 0) {
    WARN(errno, "pthread_cond_init(3) failed");
    goto err_cond;
  }

  RECVQ_LOCK(p);
  if (msgq_get_listener(p, address) < 0)
    goto err;
  if (msgq_start_receiver(p) < 0)
    goto err;
  RECVQ_UNLOCK(p);

  return p;

 err:
  pthread_cond_destroy(&p->recv_cond);
 err_cond:
  RECVQ_UNLOCK(p);
  if (p->fd >= 0)
    close(p->fd);
  pthread_mutex_destroy(&p->recv_mutex);
 err_free:
  if (p->pkbuf)
    free(p->pkbuf);
  if (p)
    free(p);
  return NULL;
}


void
msgq_close(MSGQ *msgq)
{
  void *retval;
  struct elist *p;
  struct msgq_node *np;

  pthread_cancel(msgq->receiver);
  pthread_join(msgq->receiver, &retval);

  RECVQ_LOCK(msgq);

  if (msgq->fd >= 0) {
    close(msgq->fd);
    msgq->fd = -1;
  }

  if (msgq->pkbuf) {
    free(msgq->pkbuf);
    msgq->pkbuf = NULL;
  }

  /* TODO: delete all remaining packets??? */
  DEBUG(0, "%u packet(s) will be destroyed", msgq->recvs);

  while ((p = edque_pop_front(&msgq->recvq)) != NULL) {
    np = ELIST_ENTRY(p, struct msgq_node, link);
    msgq->recvs--;

    DEBUG(0, "\tdestroying packet from %s...", np->sender);
    free(np->packet);
    free(np);
  }

  RECVQ_UNLOCK(msgq);

  /* TODO: possible race condition? */
  pthread_mutex_destroy(&msgq->recv_mutex);
  free(msgq);
}


static int
msgq_get_listener(MSGQ *msgq, const char *address)
{
  struct stat sbuf;
  struct sockaddr_un addr;
  int fd = -1;

  assert(msgq->fd == -1);

  fd = socket(AF_LOCAL, SOCK_DGRAM, 0);
  if (fd < 0) {
    WARN(errno, "socket() failed");
    goto err;
  }

  if (!address) {
    if (bind_anonymous(fd, msgq->address) < 0) {
      goto err;
    }
  }
  else {
    if (stat(address, &sbuf) == 0) {
      if (!S_ISSOCK(sbuf.st_mode)) {
        WARN(0, "file (%s) already exists", address);
        goto err;
      }
      unlink(address);
    }

    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, address, UNIX_PATH_MAX - 1);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
      WARN(errno, "bind(2) failed");
      goto err;
    }
    strncpy(msgq->address, address, UNIX_PATH_MAX - 1);
  }
  msgq->fd = fd;
  return 0;

 err:
  if (fd >= 0)
    close(fd);
  return -1;
}


static int
bind_anonymous(int fd, char address[])
{
  struct sockaddr_un addr;
  char tmpname[sizeof(MSGQ_TMP_TEMPLATE)];
  int tfd;

  assert(fd >= 0);

  while (1) {
    strcpy(tmpname, MSGQ_TMP_TEMPLATE);
    tfd = mkstemp(tmpname);
    if (tfd < 0) {
      WARN(errno, "mkstemp() failed");
      break;
    }
    close(tfd);

    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, tmpname);

    unlink(tmpname);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
      if (errno != EADDRINUSE) {
        WARN(errno, "bind(2) failed");
        break;
      }
    }
    else {
      strncpy(address, tmpname, UNIX_PATH_MAX);
      return 0;
    }
  }
  return -1;
}


static int
msgq_start_receiver(MSGQ *msgq)
{
  sigset_t cur, old;
  int ret = 0;
  static const int blocksigs[] = {
    SIGINT,
    SIGQUIT,
    SIGPIPE,
    SIGALRM,
    SIGTERM,
    SIGUSR1,
    SIGUSR2,
    SIGCHLD,
    SIGSTOP,
    SIGTSTP,
  };
  int i;

  pthread_sigmask(SIG_SETMASK, NULL, &old);

  sigemptyset(&cur);

  if (!block_all_signals)
    for (i = 0; i < sizeof(blocksigs) / sizeof(int); i++) {
      sigaddset(&cur, blocksigs[i]);
    }
  else
    sigfillset(&cur);

  pthread_sigmask(SIG_SETMASK, &cur, NULL);

  if (pthread_create(&msgq->receiver, NULL,
                     msgq_receiver, (void *)msgq) != 0) {
    WARN(errno, "pthread_create() failed");
    ret = -1;
  }
  pthread_sigmask(SIG_SETMASK, &old, NULL);

  return ret;
}


static void
msgq_receiver_cleaner(void *arg)
{
  MSGQ *msgq = (MSGQ *)arg;

  DEBUG(0, "receiver: cleanup handler started");
  pthread_mutex_unlock(&msgq->recv_mutex);
}


static void *
msgq_receiver(void *arg)
{
  int fd;
  struct sockaddr_un addr;
  socklen_t addrlen;
  ssize_t len;
  struct msgq_packet *packet;
  struct msgq_node *np;
  MSGQ *msgq = (MSGQ *)arg;

  DEBUG(0, "receiver: thread started");
  pthread_cleanup_push(msgq_receiver_cleaner, arg);

  RECVQ_LOCK(msgq);
  fd = msgq->fd;
  RECVQ_UNLOCK(msgq);

  while (1) {
    pthread_testcancel();

    DEBUG(0, "receiver: waiting for incoming packet from fd(%d)", fd);
    addrlen = sizeof(addr);
    len = recvfrom(fd, (void *)msgq->pkbuf, MSGQ_MSG_MAX, MSG_WAITALL,
                   (struct sockaddr *)&addr, &addrlen);
    if (len < 0) {
      WARN(errno, "recvfrom() failed");
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        continue;
      break;
    }

    packet = (struct msgq_packet *)msgq->pkbuf;
    if (validate_packet(packet, len) < 0) {
      DEBUG(0, "receiver: ignoring invalid(too short) packet from %s",
            addr.sun_path);
      continue;
    }

    np = msgq_node_create(addr.sun_path, packet);
    if (!np) {
      /* TODO: failed to create msgq_node struct, out of memory? */
      continue;
    }

    RECVQ_LOCK(msgq);
    edque_push_back(&msgq->recvq, &np->link);
    msgq->recvs++;
    DEBUG(0, "receiver: accepting a packet.");

    if (msgq->broadcast) {
      DEBUG(0, "receiver: broadcast!");
      pthread_cond_broadcast(&msgq->recv_cond);
    }
    else {
      DEBUG(0, "receiver: signal!");
      pthread_cond_signal(&msgq->recv_cond);
    }

    RECVQ_UNLOCK(msgq);
  }

  pthread_cleanup_pop(1);
  return NULL;
}


/*
 * Validate given PACKET.   The PACKET points the message buffer that
 * contains the data just received from the remote.
 *
 * LEN contains the total bytes received from the remote.
 */
static int
validate_packet(struct msgq_packet *packet, ssize_t len)
{
  ssize_t estimated;

  estimated = len - offsetof(struct msgq_packet, data);

  if (estimated < 0) {          /* wrong packet? */
    /* The received bytes is too small even for struct msgq_packet itself. */
    return -1;
  }

  if (estimated < packet->size) /* correct wrong sized packet */
    packet->size = estimated;

  packet->container = 0;
  return 0;
}


/*
 * Copy(duplicate) the given PACKET.
 */
static struct msgq_packet *
msgq_copy_packet(const struct msgq_packet *packet)
{
  struct msgq_packet *p;
  size_t size, newsize;

  size = sizeof(*packet) + packet->size;
  newsize = size;

  if (packet->data[packet->size - 1] != '\0') {
    /* To make easy/safe debugging, If the last byte in the packet
     * is not '\0',  Add '\0' to the copied packet.  Since 'size' member
     * of the copied will not be changed,  it is okay for the sensitive
     * receiver. */
    newsize = size + 1;
  }
  p = malloc(newsize);
  if (!p)
    return NULL;
  memcpy(p, packet, size);

  if (newsize > size) {
    p->data[p->size] = '\0';
  }

  return p;
}


/*
 * Create new struct msgq_node instance using given PACKET's duplicate.
 */
static struct msgq_node *
msgq_node_create(const char *sender, const struct msgq_packet *packet)
{
  struct msgq_node *p;

  p = malloc(sizeof(*p));
  if (!p)
    return NULL;

  p->packet = msgq_copy_packet(packet);
  if (!p->packet) {
    free(p);
    return NULL;
  }
  p->packet->container = p;
  strncpy(p->sender, sender, UNIX_PATH_MAX - 1);
  ELIST_INIT(p->link);

  return p;
}


static void
verror_(const char *kind, int status, int errnum, const char *fmt, va_list ap)
{
  fprintf(stderr, "%s: ", kind);

  vfprintf(stderr, fmt, ap);

  if (errnum > 0)
    fprintf(stderr, ": %s", strerror(errnum));

  fputc('\n', stderr);

  if (status)
    exit(status);
}


static void
warning_(int errnum, const char *fmt, ...)
{
  va_list ap;

  va_start(ap, fmt);
  verror_("warning", 0, errnum, fmt, ap);
  va_end(ap);
}

static void
debug_(int errnum, const char *fmt, ...)
{
  va_list ap;

  va_start(ap, fmt);
  verror_("debug", 0, errnum, fmt, ap);
  va_end(ap);
}


#ifdef TEST_MSGQ
/*
 * This is the server side sample.
 *
 * You can send a message to this server using socat(1).
 * Each line by socat(1) will be a packet to the server.
 *
 * Since the first 8 bytes are used for maintaing packet header,
 * you may pass any byte for the first 8 bytes.  The server will
 * ignore that data.
 *
 * If the line(packet) is shorter than 8, the server will ignore that
 * packet.
 *
 * $ socat -U UNIX-SENDTO:/tmp/msgq,bind=/tmpmsgq-cli STDIO
 * 00000000hello, world
 */
int
main(void)
{
  MSGQ *msgq;
  struct msgq_packet *packet;
  int cond = 1;

  msgq = msgq_open("/tmp/msgq");

  while (cond) {
    packet = msgq_recv_wait(msgq);

    {
      int len = strlen(packet->data);
      if (packet->data[len - 1] == '\n')
        packet->data[len - 1] = '\0';

      printf("packet(%s): |%s|\n",
             msgq_pkt_sender(packet), packet->data);

      if (strcmp(packet->data, "quit") == 0)
        cond = 0;
    }
    msgq_pkt_delete(packet);
  }

  msgq_close(msgq);
  return 0;
}
#endif  /* TEST_MSGQ */