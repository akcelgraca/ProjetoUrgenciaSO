/*
 * Doctor process with semaphores
 * Usage: ./doctor <SHIFT_LENGTH_seconds>
 * - Opens MQ and SHM, waits for patients using semaphores, simulates attend_time_ms,
 *   updates SHM stats.
 * - Ends when SHIFT_LENGTH expires.
 */

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mqueue.h>
#include <stdint.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <time.h>
#include <pthread.h>
#include <sys/time.h>
#include <errno.h>
#include <semaphore.h>
#include <bits/time.h>

#define MQ_NAME "/MSQ"
#define SHM_NAME "/SHM"
#define SEM_MSQ_ITEMS "/sem_msq_items"
#define SEM_MSQ_SPACE "/sem_msq_space"
#define MAX_MSG_SIZE 256

typedef struct {
    pthread_mutex_t lock;
    uint64_t total_triaged;
    uint64_t total_attended;
    uint64_t sum_wait_before_triage_ms;
    uint64_t sum_wait_between_triage_and_att_ms;
    uint64_t sum_total_time_ms;
} stats_shm_t;

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s SHIFT_LENGTH\n", argv[0]);
        return 1;
    }

    int shift = atoi(argv[1]);

    // abrir semáforos (criadas pelo Admission)
    sem_t *sem_msq_items = sem_open(SEM_MSQ_ITEMS, 0);
    sem_t *sem_msq_space = sem_open(SEM_MSQ_SPACE, 0);
    if (sem_msq_items == SEM_FAILED || sem_msq_space == SEM_FAILED) {
        perror("sem_open Doctor");
        return 1;
    }

    // abrir fila de mensagens
    mqd_t mq = mq_open(MQ_NAME, O_RDONLY);
    if (mq == (mqd_t)-1) { perror("mq_open doctor"); return 1; }

    // abrir memória partilhada
    int shm_fd = shm_open(SHM_NAME, O_RDWR, 0);
    if (shm_fd < 0) { perror("shm_open doctor"); return 1; }
    stats_shm_t *gstats = mmap(NULL, sizeof(stats_shm_t),
                               PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (gstats == MAP_FAILED) { perror("mmap doctor"); return 1; }

    char buf[MAX_MSG_SIZE];
    unsigned int prio;

    struct timespec ts_end;
    clock_gettime(CLOCK_REALTIME, &ts_end);
    ts_end.tv_sec += shift;

    printf("Doctor started. Shift %d seconds. PID %d\n", shift, getpid());

    while (1) {
        // verifica fim do turno
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        if (now.tv_sec >= ts_end.tv_sec) break;

        // espera paciente disponível
        if (sem_wait(sem_msq_items) < 0) {
            if (errno == EINTR) continue; // ignorar sinais
            perror("sem_wait sem_msq_items");
            break;
        }

        // recebe paciente da MSQ
        ssize_t n = mq_receive(mq, buf, sizeof(buf), &prio);
        if (n < 0) {
            perror("mq_receive doctor");
            sem_post(sem_msq_items); // reverte se falhou
            continue;
        }

        // há espaço livre na MSQ
        sem_post(sem_msq_space);

        buf[n] = 0;

        // parse da mensagem do paciente
        int arrival_no = 0;
        char name[64];
        int triage_ms = 0, attend_ms = 0;
        unsigned int priority = 0;
        long arr_sec, arr_usec, tri_end_sec, tri_end_usec;

        sscanf(buf, "%d|%63[^|]|%d|%d|%u|%ld|%ld|%ld|%ld",
               &arrival_no, name, &triage_ms, &attend_ms, &priority,
               &arr_sec, &arr_usec, &tri_end_sec, &tri_end_usec);

        struct timeval t_arrival     = { arr_sec, arr_usec };
        struct timeval t_triage_end  = { tri_end_sec, tri_end_usec };
        struct timeval t_att_start, t_att_end;

        printf("Doctor %d: attending patient %s (arr#%d) for %d ms (prio %u)\n",
               getpid(), name, arrival_no, attend_ms, priority);

        // início atendimento
        gettimeofday(&t_att_start, NULL);
        usleep(attend_ms * 1000); // simula atendimento
        gettimeofday(&t_att_end, NULL);

        // calcula tempo entre triagem e atendimento
        long wait_triage_to_att_ms =
            (t_att_start.tv_sec - t_triage_end.tv_sec) * 1000 +
            (t_att_start.tv_usec - t_triage_end.tv_usec) / 1000;
        if (wait_triage_to_att_ms < 0) wait_triage_to_att_ms = 0;

        // calcula tempo total do paciente
        long total_time_ms =
            (t_att_end.tv_sec - t_arrival.tv_sec) * 1000 +
            (t_att_end.tv_usec - t_arrival.tv_usec) / 1000;
        if (total_time_ms < 0) total_time_ms = 0;

        // atualiza estatísticas na SHM
        pthread_mutex_lock(&gstats->lock);
        gstats->total_attended++;
        gstats->sum_wait_between_triage_and_att_ms += wait_triage_to_att_ms;
        gstats->sum_total_time_ms += total_time_ms;
        pthread_mutex_unlock(&gstats->lock);

        printf("Doctor %d: finished patient %s\n", getpid(), name);
    }

    printf("Doctor PID %d ending shift.\n", getpid());

    mq_close(mq);
    munmap(gstats, sizeof(stats_shm_t));
    close(shm_fd);
    sem_close(sem_msq_items);
    sem_close(sem_msq_space);

    return 0;
}
