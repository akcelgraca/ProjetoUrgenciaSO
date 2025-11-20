/* admission.c
 Minimal but functional Admission process.
 - Creates FIFO input_pipe
 - Creates POSIX message queue /MSQ
 - Creates shared memory /shm_dei with basic stats + mutex
 - Maps DEI_Emergency.log as an MMF (simple append by offset)
 - Starts triage threads and forks doctor processes (exec ./doctor)
 - Reads lines from FIFO and enqueues to triage queue
 - SIGUSR1 prints stats; SIGINT triggers shutdown
 Note: this is a starter implementation. Extend per spec as needed.
*/

#include <unistd.h> // sleep, write, read, fork, pipe
#include <stdarg.h>
#include <stdint.h> // uint64_t
#include <stdio.h> // printf, perror
#include <stdlib.h> // malloc, exit
#include <string.h> // strings
#include <fcntl.h> // open
#include <mqueue.h> // POSIX message queue
#include <pthread.h> // threads e mutexes
#include <sys/mman.h> // shared memory, memory-mapped files
#include <sys/types.h> // pid_t
#include <sys/wait.h> // esperar pelo processo filho
#include <errno.h> // mensagens de erro
#include <signal.h> // SIGINT, SIGUSR1
#include <time.h> // gettimeofday
#include <sys/time.h>
#include <sys/stat.h> // mkfifo, permisos
#include <sys/file.h> // flock
#include <semaphore.h>

#define SEM_MSQ_ITEMS "/sem_msq_items"   // indica quantos pacientes disponíveis na MSQ
#define SEM_MSQ_SPACE "/sem_msq_space"   // indica espaço livre na MSQ
sem_t *sem_msq_items;
sem_t *sem_msq_space;
#define FIFO_NAME "input_pipe"
#define MQ_NAME "/MSQ"
#define SHM_NAME "/SHM"
#define LOG_FILE "DEI_Emergency.log"
#define LOG_MMF_SIZE (1<<20) // 1MB
#define MAX_MSG_SIZE 256
#define MAX_NAME 64

volatile sig_atomic_t keep_running = 1; // controla a execução do main loop.
volatile sig_atomic_t print_stats_flag = 0; // indica quando imprimir estatísticas.

// Estrutura de dados na memória partilhada para estatísticas globais
typedef struct {
    pthread_mutex_t lock; // protects the stats below
    uint64_t total_triaged;
    uint64_t total_attended;
    uint64_t sum_wait_before_triage_ms;
    uint64_t sum_wait_between_triage_and_att_ms;
    uint64_t sum_total_time_ms;
} stats_shm_t;

// Estrutura de dados para pacientes
typedef struct patient {
    int arrival_no;
    char name[MAX_NAME];
    int triage_time_ms;
    int attend_time_ms;
    unsigned int priority;
    struct timeval t_arrival;
    struct timeval t_triage_start;
    struct timeval t_triage_end;
    struct timeval t_att_start;   // início do atendimento pelo doctor
    struct timeval t_att_end;     // fim do atendimento pelo doctor
} patient_t;

// Fila de triagem (bounded buffer) para pacientes
typedef struct {
    patient_t *buf;
    int capacity;
    int head, tail; // head: next pop, tail: next push
    int cnt;
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} triage_queue_t;

triage_queue_t triage_q;
stats_shm_t *gstats = NULL;
mqd_t mqdes = (mqd_t)-1;
char *log_map = NULL;
size_t log_map_off = 0;
int fifo_fd = -1;
int TRIAGE;
int TRIAGE_QUEUE_MAX;
int DOCTORS;
int SHIFT_LENGTH;
int MSQ_WAIT_MAX;

// Signal handlers
void sigint_handler(int signo){ (void)signo; keep_running = 0; }
void sigusr1_handler(int signo){ (void)signo; print_stats_flag = 1; }

// Função para escrever logs com timestamp na MMF e stdout
void write_log(const char *fmt, ...) {
    va_list ap;
    char buf[256];
    struct timeval tv;
    gettimeofday(&tv, NULL);
    time_t t = tv.tv_sec;
    struct tm tm;
    localtime_r(&t, &tm);
    int n = strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    int off = snprintf(buf + n, sizeof(buf) - n, ".%03ld ", tv.tv_usec/1000);
    va_start(ap, fmt);
    vsnprintf(buf + n + off, sizeof(buf) - n - off, fmt, ap);
    va_end(ap);
    strcat(buf, "\n");
    // print to stdout
    printf("%s", buf);
    fflush(stdout);
    // write to mmf (simple append)
    size_t len = strlen(buf);
    if (log_map && (log_map_off + len) < LOG_MMF_SIZE) {
        memcpy(log_map + log_map_off, buf, len);
        log_map_off += len;
    } else {
        // fallback: append to file normally
        int fd = open(LOG_FILE, O_WRONLY | O_APPEND);
        if (fd >= 0) {
            write(fd, buf, len);
            close(fd);
        }
    }
}

/* Triange queue functions */
void triage_queue_init(triage_queue_t *q, int capacity) {
    q->buf = calloc(capacity, sizeof(patient_t));
    q->capacity = capacity;
    q->head = q->tail = q->cnt = 0;
    pthread_mutexattr_t ma;
    pthread_mutexattr_init(&ma);
    pthread_mutex_init(&q->lock, &ma);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
}
void triage_queue_destroy(triage_queue_t *q) {
    free(q->buf);
    pthread_mutex_destroy(&q->lock);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
}
int triage_queue_push(triage_queue_t *q, const patient_t *p) {
    pthread_mutex_lock(&q->lock);
    while (q->cnt == q->capacity && keep_running) {
        pthread_cond_wait(&q->not_full, &q->lock);
    }
    if (!keep_running) { pthread_mutex_unlock(&q->lock); return -1; }
    q->buf[q->tail] = *p;
    q->tail = (q->tail + 1) % q->capacity;
    q->cnt++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->lock);
    return 0;
}
int triage_queue_pop(triage_queue_t *q, patient_t *out) {
    pthread_mutex_lock(&q->lock);
    while (q->cnt == 0 && keep_running) {
        pthread_cond_wait(&q->not_empty, &q->lock);
    }
    if (q->cnt == 0 && !keep_running) { pthread_mutex_unlock(&q->lock); return -1; }
    *out = q->buf[q->head];
    q->head = (q->head + 1) % q->capacity;
    q->cnt--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->lock);
    return 0;
}

/* O paciente sai de Admission → Entra na fila
 (triage_queue) → Uma thread triagem pega
  nele → Faz triagem → Envia para MSQ*/
void *triage_thread_fn(void *arg) {
    int id = (intptr_t)arg;
    write_log("Triage thread %d started", id);

    while (keep_running) {

        patient_t p;
        if (triage_queue_pop(&triage_q, &p) != 0)
            break;

        // marcar início real da triagem
        gettimeofday(&p.t_triage_start, NULL);

        write_log("Triage %d: start triage patient %s (arr#%d)",
                  id, p.name, p.arrival_no);

        // calcular tempo de espera até triagem
        long wait_ms =
            (p.t_triage_start.tv_sec - p.t_arrival.tv_sec) * 1000 +
            (p.t_triage_start.tv_usec - p.t_arrival.tv_usec) / 1000;
        if (wait_ms < 0) wait_ms = 0;

        // simular duração da triagem
        usleep(p.triage_time_ms * 500000);

        // fim da triagem
        gettimeofday(&p.t_triage_end, NULL);

        // atualizar estatísticas globais
        pthread_mutex_lock(&gstats->lock);
        gstats->total_triaged++;
        gstats->sum_wait_before_triage_ms += wait_ms;
        pthread_mutex_unlock(&gstats->lock);

        // enviar paciente para a MSQ
        char buf[MAX_MSG_SIZE];
        memset(buf, 0, sizeof(buf));
        snprintf(buf, sizeof(buf),
            "%d|%s|%d|%d|%u|%ld|%ld|%ld|%ld",
            p.arrival_no, p.name, p.triage_time_ms, p.attend_time_ms, p.priority,
            (long)p.t_arrival.tv_sec, (long)p.t_arrival.tv_usec,
            (long)p.t_triage_end.tv_sec, (long)p.t_triage_end.tv_usec
        );

    // espera por espaço disponível
    sem_wait(sem_msq_space);

    // envia paciente
    if (mq_send(mqdes, buf, strlen(buf) + 1, p.priority) != 0) {
        write_log("Triage %d: failed to mq_send patient %s: %s",
                id, p.name, strerror(errno));
        sem_post(sem_msq_space); // reverte se falhou
    } else {
        write_log("Triage %d: patient %s enqueued to MSQ with priority %u",
                id, p.name, p.priority);
        sem_post(sem_msq_items); // incrementa número de pacientes disponíveis
    }
    }

    write_log("Triage thread %d exiting", id);
    return NULL;
}


/* Read config file simple parser */
void read_config(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        perror("Erro a abrir config.txt");
        exit(EXIT_FAILURE);
    }

    char line[256];
    int found_all = 0;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "TRIAGE_QUEUE_MAX=%d", &TRIAGE_QUEUE_MAX) == 1) { found_all++; continue; }
        if (sscanf(line, "TRIAGE=%d", &TRIAGE) == 1) { found_all++; continue; }
        if (sscanf(line, "DOCTORS=%d", &DOCTORS) == 1) { found_all++; continue; }
        if (sscanf(line, "SHIFT_LENGTH=%d", &SHIFT_LENGTH) == 1) { found_all++; continue; }
        if (sscanf(line, "MSQ_WAIT_MAX=%d", &MSQ_WAIT_MAX) == 1) { found_all++; continue; }
    }
    fclose(f);

    if (found_all < 5) {
        fprintf(stderr, "config.txt incompleto ou mal formatado\n");
        exit(EXIT_FAILURE);
    }
}


/* Create shared memory and initialize stats and mutex (process-shared) */
void create_shm() {
    int fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (fd < 0) { perror("shm_open"); exit(1); }
    ftruncate(fd, sizeof(stats_shm_t));
    void *p = mmap(NULL, sizeof(stats_shm_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { perror("mmap shm"); exit(1); }
    close(fd);
    gstats = (stats_shm_t*)p;
    // Initialize mutex for process-shared use only if first created (crudely)
    pthread_mutexattr_t ma;
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_setpshared(&ma, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&gstats->lock, &ma);
    // zero stats
    pthread_mutex_lock(&gstats->lock);
    gstats->total_triaged = 0;
    gstats->total_attended = 0;
    gstats->sum_wait_before_triage_ms = 0;
    gstats->sum_wait_between_triage_and_att_ms = 0;
    gstats->sum_total_time_ms = 0;
    pthread_mutex_unlock(&gstats->lock);
}

/* Create MMF for log */
void create_log_mmf() {
    int fd = open(LOG_FILE, O_RDWR | O_CREAT, 0666);
    if (fd < 0) { perror("open log"); return; }
    ftruncate(fd, LOG_MMF_SIZE);
    log_map = mmap(NULL, LOG_MMF_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (log_map == MAP_FAILED) {
        perror("mmap log");
        log_map = NULL;
    }
    close(fd);
}

/* Spawn doctors */
void spawn_doctors(int n) {
    for (int i=0;i<n;i++) {
        pid_t pid = fork();
        if (pid == 0) {
            // child => exec doctor
            char arg[16];
            snprintf(arg,sizeof(arg), "%d", SHIFT_LENGTH);
            execl("./doctor", "./doctor", arg, (char*)NULL);
            perror("execl doctor");
            _exit(1);
        } else if (pid > 0) {
            write_log("Doctor PID %d", pid);
        } else {
            perror("fork");
        }
    }
}

/* Admission main loop: read FIFO lines like:
   João 10 50 1
   or
   8 10 65 3
*/
int main(int argc, char **argv) {
    (void)argc; (void)argv;
    read_config("config.txt");
    printf("Config lida: TRIAGE_QUEUE_MAX=%d, TRIAGE=%d, DOCTORS=%d, SHIFT_LENGTH=%d, MSQ_WAIT_MAX=%d\n",
       TRIAGE_QUEUE_MAX, TRIAGE, DOCTORS, SHIFT_LENGTH, MSQ_WAIT_MAX);

    // remove semáforos antigos se existirem
    sem_unlink(SEM_MSQ_ITEMS);
    sem_unlink(SEM_MSQ_SPACE);

    // inicializa semáforos
    // inicializa semáforos POSIX globais
    sem_msq_items = sem_open(SEM_MSQ_ITEMS, O_CREAT | O_EXCL, 0666, 0);  // inicialmente 0 pacientes
    if (sem_msq_items == SEM_FAILED) {
        if (errno == EEXIST) {
            sem_unlink(SEM_MSQ_ITEMS);
            sem_msq_items = sem_open(SEM_MSQ_ITEMS, O_CREAT | O_EXCL, 0666, 0);
        }
    }
    sem_msq_space = sem_open(SEM_MSQ_SPACE, O_CREAT | O_EXCL, 0666, MSQ_WAIT_MAX);
    if (sem_msq_space == SEM_FAILED) {
        if (errno == EEXIST) {
            sem_unlink(SEM_MSQ_SPACE);
            sem_msq_space = sem_open(SEM_MSQ_SPACE, O_CREAT | O_EXCL, 0666, MSQ_WAIT_MAX);
        }
    }

    if (sem_msq_items == SEM_FAILED || sem_msq_space == SEM_FAILED) {
        perror("sem_open Admission");
        exit(1);
    }
    // create message queue
    struct mq_attr attr;
    mq_getattr(mqdes, &attr);
    if (attr.mq_curmsgs >= 0.8 * MSQ_WAIT_MAX) {
        // fork/exec de um Doctor temporário
        pid_t pid = fork();
        if (pid == 0) {
            char arg[16];
            snprintf(arg, sizeof(arg), "%d", SHIFT_LENGTH);
            execl("./doctor", "./doctor", arg, (char*)NULL);
            _exit(1);
        }
    }
    memset(&attr, 0, sizeof(attr));   // ← ESSENCIAL!!!

    attr.mq_flags = 0;
    attr.mq_maxmsg = 10;              // permitido pelo teu sistema
    attr.mq_msgsize = MAX_MSG_SIZE;   // 256 bytes → OK
    attr.mq_curmsgs = 0;

    mq_unlink(MQ_NAME);

    mqdes = mq_open(MQ_NAME, O_CREAT | O_RDWR, 0666, &attr);
    if (mqdes == (mqd_t)-1) {
        perror("mq_open ERROR");
        exit(1);
    }
    signal(SIGINT, sigint_handler);
    signal(SIGUSR1, sigusr1_handler);

    // create FIFO
    unlink(FIFO_NAME);
    if (mkfifo(FIFO_NAME, 0666) < 0) {
        if (errno != EEXIST) { perror("mkfifo"); exit(1); }
    }
    // open FIFO non-blocking read
    fifo_fd = open(FIFO_NAME, O_RDONLY | O_NONBLOCK);
    if (fifo_fd < 0) { perror("open fifo"); exit(1); }


    // create shared memory
    create_shm();

    // create mmf log
    create_log_mmf();

    // triage queue
    triage_queue_init(&triage_q, TRIAGE_QUEUE_MAX);

    // create triage threads
    pthread_t *tids = calloc(TRIAGE, sizeof(pthread_t));
    for (int i=0;i<TRIAGE;i++) {
        pthread_create(&tids[i], NULL, triage_thread_fn, (void*)(intptr_t)i);
    }

    // cria os processos doctor
    spawn_doctors(DOCTORS);

    write_log("Admission started. FIFO %s opened. Waiting for patients...", FIFO_NAME);

    // Main loop reading FIFO
    char buf[256];
    int arrival_no = 0;
    while (keep_running) {
        // check for signals
        if (print_stats_flag) {
            pthread_mutex_lock(&gstats->lock);
            uint64_t ttri = gstats->total_triaged;
            uint64_t tatt = gstats->total_attended;
            uint64_t s1 = gstats->sum_wait_before_triage_ms;
            uint64_t s2 = gstats->sum_wait_between_triage_and_att_ms;
            uint64_t s3 = gstats->sum_total_time_ms;
            pthread_mutex_unlock(&gstats->lock);
            write_log("STATISTICS: triaged=%lu attended=%lu avg_wait_before_triage_ms=%.2f avg_wait_between_triage_and_att_ms=%.2f avg_total_time_ms=%.2f",
                ttri, tatt,
                ttri? (double)s1/ttri : 0.0,
                tatt? (double)s2/tatt : 0.0,
                tatt? (double)s3/tatt : 0.0);
            print_stats_flag = 0;
        }

        ssize_t r = read(fifo_fd, buf, sizeof(buf)-1);
        if (r > 0) {
            buf[r] = '\0';
            // support multiple lines if present
            char *line = strtok(buf, "\n");
            while (line) {
                // parse: name triage_ms attend_ms priority
                patient_t p;
                memset(&p,0,sizeof(p));
                arrival_no++; p.arrival_no = arrival_no;
                gettimeofday(&p.t_arrival, NULL);
                int parsed = 0;
                if (sscanf(line, "%s %d %d %u", p.name, &p.triage_time_ms, &p.attend_time_ms, &p.priority) == 4) {
                    parsed = 1;
                } else if (sscanf(line, "%d %d %d %u", &arrival_no, &p.triage_time_ms, &p.attend_time_ms, &p.priority) == 4) {
                    // group, but ignoring group expansion in starter
                    parsed = 1;
                }
                if (parsed) {
                    write_log("Admission: received patient %s (triage %d ms, attend %d ms, prio %u)", p.name, p.triage_time_ms, p.attend_time_ms, p.priority);
                    // push to triage queue
                    if (triage_queue_push(&triage_q, &p) != 0) {
                        write_log("Admission: triage queue full, dropping patient %s", p.name);
                    }
                } else {
                    write_log("Admission: invalid input line: %s", line);
                }
                line = strtok(NULL, "\n");
            }
        } else {
            // no data: sleep a bit (non-busy wait)
            usleep(100000);
        }
    }

    write_log("Admission: shutdown signal received. Cleaning up...");

    // unblock triage threads
    pthread_mutex_lock(&triage_q.lock);
    pthread_cond_broadcast(&triage_q.not_empty);
    pthread_cond_broadcast(&triage_q.not_full);
    pthread_mutex_unlock(&triage_q.lock);

    // join triage threads
    for (int i=0;i<TRIAGE;i++) pthread_join(tids[i], NULL);
    free(tids);

    // close mq
    mq_close(mqdes);
    mq_unlink(MQ_NAME);
    // unmap shm
    munmap(gstats, sizeof(stats_shm_t));
    shm_unlink(SHM_NAME);

    // unmap log
    if (log_map) munmap(log_map, LOG_MMF_SIZE);

    // close fifo
    close(fifo_fd);
    unlink(FIFO_NAME);

    triage_queue_destroy(&triage_q);
    

    write_log("Admission terminated cleanly.");
    return 0;
}
