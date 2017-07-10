#ifndef MACHINARIUM_PRIVATE_H_
#define MACHINARIUM_PRIVATE_H_

/*
 * machinarium.
 *
 * cooperative multitasking engine.
*/

#define _GNU_SOURCE 1

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <time.h>
#include <assert.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>

#include <openssl/ssl.h>
#include <openssl/crypto.h>
#include <openssl/engine.h>
#include <openssl/conf.h>
#include <openssl/bio.h>
#include <openssl/err.h>

#include "build.h"
#include "macro.h"
#include "list.h"
#include "buf.h"

#include "fd.h"
#include "poll.h"
#include "timer.h"
#include "clock.h"
#include "idle.h"
#include "loop.h"
#include "epoll.h"
#include "socket.h"

#include "context_stack.h"
#include "context.h"
#include "coroutine.h"
#include "scheduler.h"
#include "call.h"

#include "thread.h"
#include "signal_mgr.h"

#include "event.h"
#include "event_mgr.h"

#include "msg.h"
#include "msg_cache.h"
#include "channel.h"
#include "queue.h"

#include "task.h"
#include "task_mgr.h"

#include "machine.h"
#include "machine_mgr.h"
#include "mm.h"

#include "tls_api.h"
#include "tls_io.h"
#include "io.h"
#include "read.h"
#include "write.h"

#endif
