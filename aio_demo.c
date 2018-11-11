#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <syscall.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <linux/aio_abi.h>

#define MAX_BUFF_SIZE 1024

typedef struct __file_info {
    int fd;
    int efd;
    struct stat f_stat;
    aio_context_t ctx;
} file_info;

static int io_setup(unsigned nr_events, aio_context_t *ctx_idp)
{
    return (int)syscall(SYS_io_setup, nr_events, ctx_idp);
}

static  int io_destroy(aio_context_t ctx_id)
{
    return (int)syscall(SYS_io_setup, ctx_id);
}

static int io_getevents(aio_context_t ctx_id, long min_nr, long nr,
                        struct io_event *events, struct timespec *timeout)
{
    return (int)syscall(SYS_io_getevents, ctx_id, min_nr, nr, events, timeout);
}

static int io_cancel(aio_context_t ctx_id, struct iocb *iocb,
                     struct io_event *result)
{
    return (int)syscall(SYS_io_cancel, ctx_id, iocb, result);
}

static int io_submit(aio_context_t ctx_id, long nr, struct iocb **iocbpp)
{
    return (int)syscall(SYS_io_submit, ctx_id, nr, iocbpp);
}


static int eventfd(unsigned int initval, int flags)
{
    return  (int)syscall(SYS_eventfd, initval, flags);
}


static void* listen_file_notify(void* data) {
    uint64_t ready = 0;
    struct iocb* scb;
    file_info *info = data;
    struct timespec val;
    struct io_event iov[10];
    int ndfs = 0;
    int evfd = -1;
    int n = 0;
    int events = 0;

    struct epoll_event ev, evs[12];

    if (info == NULL) {
        perror("invalid param");
        pthread_exit(NULL);
    }

    evfd = epoll_create1(0);
    if (evfd < 0) {
        perror("create epoll handle failed");
        pthread_exit(NULL);
    }

    ev.data.fd = info->efd;
    ev.events = EPOLLIN;

    if (epoll_ctl(evfd, EPOLL_CTL_ADD, info->efd, &ev) == -1) {
        printf("evfd : %d\r\n", evfd);
        pthread_exit(NULL);
    }
    while (1) {
        ndfs = epoll_wait(evfd, evs, 12, -1);
        if (ndfs < 0) {
            perror("epoll wait failed");
            pthread_exit(NULL);
        }

        if (ndfs == 0) {
            printf("wait zero events\r\n");
            continue;
        }

        for (int i = 0; i < ndfs; i++) {
            if (evs[i].data.fd == info->efd && (evs[i].events & EPOLLIN== EPOLLIN)) {
                n = read(info->efd, &ready, sizeof(ready));
                if (n != 8) {
                    printf("read dirty data\r\n");
                    continue;
                }
                while (ready > 0) {
                    val.tv_nsec = 0;
                    val.tv_sec = 0;
                    events = io_getevents(info->ctx, 1, 10, iov, &val);
                    ready -= (uint64_t) events;
                    for (int j = 0; j < events; j++) {
                        scb =(struct iocb *)iov[j].obj;
                        printf("buf: %s\r\n", (char *)scb->aio_buf);
                        printf("size: %d\r\n", scb->aio_nbytes);
                        printf("res = %lld\r\n", iov[i].res);
                        printf("res2 = %lld\r\n", iov[i].res2);
                        printf("errno: %d\r\n", errno);
                    }
                }
            }
        }
    }
    return NULL;
}

pthread_t create_work_thread(void* ptr)
{
    pthread_t tid;
    int ret = -1;

    ret = pthread_create(&tid, NULL, listen_file_notify, ptr);
    if (ret != 0) {
        perror("create thread failed");
        return 0;
    }

    return tid;
}

int main(int argc, char *argv[])
{
    int efd = -1;
    int ret = -1;
    struct iocb* scb[1];
    aio_context_t *ctx = NULL;
    pthread_t tid;
    file_info info;
    char* buf = NULL;

    if (argc != 2) {
        printf("Usage: %s <file>", argv[0]);
        exit(-1);
    }

    // create async ctx
    ctx = calloc(sizeof(aio_context_t), 1);
    ret = io_setup(1, ctx);
    if (ret != 0) {
        perror("io_setup failed :");
        goto failed_3;
    }
    info.ctx = *ctx;

    // create event fd
    efd = eventfd(0, 0);;
    if (efd == -1) {
        perror("eventfd failed");
        goto failed_3;
    }
    info.efd = efd;

    tid = create_work_thread(&info);
    if (tid == 0) {
        goto failed_2;
    }

    // directly access file
    info.fd = open(argv[1], O_RDONLY | O_DIRECT);
    if (info.fd < 0) {
        perror("open failed :");
        goto failed_2;
    }

    ret = fstat(info.fd, &info.f_stat);
    if (ret != 0) {
        perror("get file perporty failed :");
        goto failed_1;
    }
    printf ("file size: %ld Bytes\r\n", info.f_stat.st_size);

    buf = aligned_alloc(512, MAX_BUFF_SIZE);
    memset(buf, 0, MAX_BUFF_SIZE);

    scb[0] = calloc(sizeof(struct iocb), 1);

    scb[0]->aio_lio_opcode = IOCB_CMD_PREADV;
    scb[0]->aio_rw_flags = RWF_SYNC;
    scb[0]->aio_fildes = (uint32_t)info.fd;
    scb[0]->aio_buf = (uint64_t) buf;
    scb[0]->aio_nbytes = MAX_BUFF_SIZE;
    scb[0]->aio_offset = 0;
    scb[0]->aio_flags = IOCB_FLAG_RESFD;
    scb[0]->aio_resfd = (unsigned int)efd;

    do {
        ret = io_submit(*ctx, 1, scb);
        if (ret >= 0) {
            break;
        }

        if (ret == EAGAIN) {
            usleep(1000);
            continue;
        }

        if (ret == ENOSYS) {
            perror("No system invoke");
            goto failed_1;
        }

        pthread_cancel(tid);
        perror("io_submit error");
        break;
    } while (1);

    ret = pthread_join(tid, NULL);

    io_destroy(info.ctx);
failed_1:
    close(info.fd);
failed_2:
    close(efd);
failed_3:
    free(scb[0]);
    free(ctx);
    exit(ret);
}
