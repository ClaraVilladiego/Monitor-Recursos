#include <ncurses.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysinfo.h>
#include <math.h>
#include <dirent.h>
#include <sys/types.h>
#include <ctype.h>
#include <pthread.h>
#include <libnotify/notify.h>
#include <glib-object.h>  // Added for g_object_unref

#define REFRESH_RATE 1000000 // Actualizar cada 0.5 segundos
#define MAX_PROCESSES 1024
#define MAX_CYCLES 100 // Número máximo de ciclos antes de cerrar
#define CPU_THRESHOLD 70.0 // Umbral de uso de CPU
#define MEMORY_THRESHOLD 500 // Umbral de uso de memoria en MB


// Mutexes para sincronización
pthread_mutex_t cpu_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t memory_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t process_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    double user;
    double system;
    double idle;
    double iowait;
} CPUMetrics;

typedef struct {
    size_t total;
    size_t used;
    size_t free;
    size_t cached;
    size_t swap_used;
} MemoryMetrics;

typedef struct {
    int pid;
    double cpu_usage;
    size_t memory_usage;
    int priority;
    char state[16];
} ProcessMetrics;

// Estructura global para compartir datos entre hilos
typedef struct {
    CPUMetrics cpu;
    MemoryMetrics memory;
    ProcessMetrics processes[MAX_PROCESSES];
    int num_processes;
    volatile int should_run;
} SystemMetrics;

SystemMetrics system_metrics = {0};

void notify(const char *title, const char *message, int pid, double cpu_usage);

// Funciones para obtener las métricas
void get_cpu_metrics(CPUMetrics *cpu_metrics) {
    static unsigned long long last_user = 0, last_nice = 0, last_system = 0, last_idle = 0, last_iowait = 0;
    static unsigned long long last_total = 0;

    FILE *fp;
    char line[256];
    unsigned long long user, nice, system, idle, iowait, irq, softirq, steal, guest, guest_nice;

    fp = fopen("/proc/stat", "r");
    if (fp == NULL) {
        perror("Error abriendo /proc/stat");
        return;
    }

    // Leer la línea de CPU
    if (fgets(line, sizeof(line), fp) == NULL) {
        fclose(fp);
        return;
    }

    fclose(fp);

    // Usar sscanf para capturar todos los valores
    if (sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
               &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal, &guest, &guest_nice) != 10) {
        return;
    }

    // Calcular los totales actuales
    unsigned long long total = user + nice + system + idle + iowait + irq + softirq + steal + guest + guest_nice;
    unsigned long long active = (user + nice + system + irq + softirq + steal);

    // Calcular los incrementos desde la última muestra
    unsigned long long total_diff = total - last_total;
    unsigned long long active_diff = active - (last_user + last_nice + last_system + last_iowait);

    // Calcular los porcentajes
    if (total_diff > 0) {
        cpu_metrics->user = (double)(user - last_user) / total_diff * 100.0;
        cpu_metrics->system = (double)(system - last_system) / total_diff * 100.0;
        cpu_metrics->idle = (double)(idle - last_idle) / total_diff * 100.0;
        cpu_metrics->iowait = (double)(iowait - last_iowait) / total_diff * 100.0;
    }

    // Actualizar los valores anteriores
    last_user = user;
    last_nice = nice;
    last_system = system;
    last_idle = idle;
    last_iowait = iowait;
    last_total = total;
}

void get_memory_metrics(MemoryMetrics *memory_metrics) {
    struct sysinfo info;
    if (sysinfo(&info) != 0) {
        perror("Error obteniendo información del sistema");
        return;
    }

    memory_metrics->total = info.totalram;
    memory_metrics->used = info.totalram - info.freeram - info.bufferram;
    memory_metrics->free = info.freeram;
    memory_metrics->cached = info.bufferram;
    memory_metrics->swap_used = info.totalswap - info.freeswap;
}

void get_process_metrics(ProcessMetrics *process_metrics, int *num_processes) {
    static unsigned long long last_total_time = 0;
    static ProcessMetrics last_process_metrics[MAX_PROCESSES] = {0};
    unsigned long long current_total_time = 0;

    DIR *dir;
    struct dirent *entry;
    int process_count = 0;

    dir = opendir("/proc");
    if (dir == NULL) {
        perror("Error abriendo /proc");
        return;
    }

    // Read total CPU time
    FILE *stat_file = fopen("/proc/stat", "r");
    if (stat_file) {
        char line[256];
        unsigned long long user, nice, system, idle, iowait, irq, softirq, steal, guest, guest_nice;
        if (fgets(line, sizeof(line), stat_file) != NULL) {
            sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu", 
                   &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal, &guest, &guest_nice);
            current_total_time = user + nice + system + idle + iowait + irq + softirq + steal + guest + guest_nice;
        }
        fclose(stat_file);
    }

    long clock_ticks_per_second = sysconf(_SC_CLK_TCK);
    long page_size = sysconf(_SC_PAGESIZE);

    while ((entry = readdir(dir)) != NULL && process_count < MAX_PROCESSES) {
        if (entry->d_type == DT_DIR && isdigit(entry->d_name[0])) {
            int pid = atoi(entry->d_name);
            char stat_path[64];
            char statm_path[64];
            snprintf(stat_path, sizeof(stat_path), "/proc/%d/stat", pid);
            snprintf(statm_path, sizeof(statm_path), "/proc/%d/statm", pid);

            FILE *fp_stat = fopen(stat_path, "r");
            FILE *fp_statm = fopen(statm_path, "r");
            
            // Try to read memory from /proc/[pid]/status as an alternative
            if (fp_stat != NULL && (fp_statm != NULL || (fp_statm = fopen(statm_path, "r")) != NULL)) {
                unsigned long long utime, stime;
                char comm[256], state;
                int priority;
                
                unsigned long vm_size = 0, rss = 0, shared = 0, text = 0, lib = 0, data = 0, dt = 0;
                
                // If statm fails, try reading from status file
                if (fp_statm == NULL) {
                    char status_path[64];
                    snprintf(status_path, sizeof(status_path), "/proc/%d/status", pid);
                    FILE *fp_status = fopen(status_path, "r");
                    if (fp_status) {
                        char line[256];
                        while (fgets(line, sizeof(line), fp_status)) {
                            if (strncmp(line, "VmRSS:", 6) == 0) {
                                sscanf(line + 6, "%lu", &rss);
                                break;
                            }
                        }
                        fclose(fp_status);
                    }
                } else {
                    // Read from statm as before
                    fscanf(fp_statm, "%lu %lu %lu %lu %lu %lu %lu", 
                           &vm_size, &rss, &shared, &text, &lib, &data, &dt);
                }
                
                if (fscanf(fp_stat, "%d %s %c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu %*d %*d %d", 
                           &process_metrics[process_count].pid, comm, &state, &utime, &stime, &priority) == 6) {
                    
                    // Find the previous process entry
                    ProcessMetrics *last_process = NULL;
                    for (int i = 0; i < *num_processes; i++) {
                        if (last_process_metrics[i].pid == pid) {
                            last_process = &last_process_metrics[i];
                            break;
                        }
                    }

                    // Calculate CPU usage
                    if (last_process && last_total_time > 0) {
                        unsigned long long process_total_time = utime + stime;
                        unsigned long long last_process_total_time = 
                            last_process->cpu_usage * clock_ticks_per_second;
                        
                        unsigned long long process_time_diff = process_total_time - last_process_total_time;
                        unsigned long long total_time_diff = current_total_time - last_total_time;

                        // Calculate CPU usage percentage
                        process_metrics[process_count].cpu_usage = 
                            (process_time_diff * 100.0) / (total_time_diff * clock_ticks_per_second);
                    } else {
                        process_metrics[process_count].cpu_usage = 0.0;
                    }

                    // Set memory usage 
                    process_metrics[process_count].memory_usage = rss * 1024; // Convert from kB to bytes

                    process_metrics[process_count].priority = priority;
                    snprintf(process_metrics[process_count].state, sizeof(process_metrics[process_count].state), "%c", state);

                    process_count++;
                }
                
                if (fp_stat) fclose(fp_stat);
                if (fp_statm) fclose(fp_statm);
            }
        }
    }

    // Save metrics for next iteration
    memcpy(last_process_metrics, process_metrics, sizeof(ProcessMetrics) * process_count);
    last_total_time = current_total_time;
    *num_processes = process_count;

    closedir(dir);
}
// Función para mostrar las métricas
void display_metrics(const SystemMetrics *metrics) {
    clear();
    attron(COLOR_PAIR(1));
    mvprintw(0, 0, "Monitor de Sistema (Multihilo)");
    attroff(COLOR_PAIR(1));

    // CPU Metrics
    pthread_mutex_lock(&cpu_mutex);
    mvprintw(2, 0, "CPU:");
    mvprintw(3, 2, "Usuario: %.2f%%", metrics->cpu.user);
    mvprintw(4, 2, "Sistema: %.2f%%", metrics->cpu.system);
    mvprintw(5, 2, "Inactivo: %.2f%%", metrics->cpu.idle);
    mvprintw(6, 2, "I/O Wait: %.2f%%", metrics->cpu.iowait);
    pthread_mutex_unlock(&cpu_mutex);

    // Memory Metrics
    pthread_mutex_lock(&memory_mutex);
    mvprintw(8, 0, "Memoria:");
    mvprintw(9, 2, "Total: %lu MB", metrics->memory.total / 1024 / 1024);
    mvprintw(10, 2, "Usada: %lu MB", metrics->memory.used / 1024 / 1024);
    mvprintw(11, 2, "Libre: %lu MB", metrics->memory.free / 1024 / 1024);
    mvprintw(12, 2, "Caché: %lu MB", metrics->memory.cached / 1024 / 1024);
    mvprintw(13, 2, "Swap Usada: %lu MB", metrics->memory.swap_used / 1024 / 1024);
    pthread_mutex_unlock(&memory_mutex);

    // Process Metrics
    pthread_mutex_lock(&process_mutex);
    mvprintw(15, 0, "Procesos (%d):", metrics->num_processes);
    int y = 16;
    for (int i = 0; i < metrics->num_processes && y < LINES - 1; i++) {
        if (metrics->processes[i].cpu_usage > CPU_THRESHOLD) {
            attron(COLOR_PAIR(3));
            notify("Alerta", "Uso de CPU alto", metrics->processes[i].pid, metrics->processes[i].cpu_usage);
        }
         else if (metrics->processes[i].cpu_usage > 50.0) {
            attron(COLOR_PAIR(2));
        }

        // Mostrar los detalles de cada proceso
        mvprintw(y++, 2, "PID: %d, CPU: %.2f%%, Memoria: %.2f MB, Prioridad: %d, Estado: %s",
                 metrics->processes[i].pid, 
                 metrics->processes[i].cpu_usage,
                 (double)metrics->processes[i].memory_usage / (1024 * 1024), // Convertir bytes a MB
                 metrics->processes[i].priority,
                 metrics->processes[i].state);

        attroff(COLOR_PAIR(2));
        attroff(COLOR_PAIR(3));
    }
    pthread_mutex_unlock(&process_mutex);

    refresh();
}

void notify(const char *title, const char *message, int pid, double cpu_usage) {
    // Asegurarse de que la inicialización fue exitosa
    if (!notify_is_initted() && !notify_init("Monitoreo de Sistema")) {
        fprintf(stderr, "Error: No se pudo inicializar el sistema de notificaciones\n");
        return;
    }

    // Crear un mensaje más detallado
    char detailed_message[256];
    snprintf(detailed_message, sizeof(detailed_message), 
             "Proceso PID %d está usando demasiada CPU!\n"
             "Uso actual: %.2f%%\n"
             "Threshold: %.2f%%", 
             pid, cpu_usage, CPU_THRESHOLD);

    // Crear la notificación con un ícono
    NotifyNotification *notification = notify_notification_new(
        "⚠️ ALERTA DE CPU",
        detailed_message,
        "dialog-warning"
    );

    // Configurar la urgencia como crítica
    notify_notification_set_urgency(notification, NOTIFY_URGENCY_CRITICAL);
    
    // Configurar el tiempo de expiración (10 segundos)
    notify_notification_set_timeout(notification, 10000);

    // Intentar mostrar la notificación
    GError *error = NULL;
    if (!notify_notification_show(notification, &error)) {
        fprintf(stderr, "Error mostrando la notificación: %s\n", 
                error ? error->message : "desconocido");
        if (error) g_error_free(error);
    }

    g_object_unref(G_OBJECT(notification));
}

// Funciones para hilos
void *cpu_thread(void *arg) {
    while (system_metrics.should_run) {
        pthread_mutex_lock(&cpu_mutex);
        get_cpu_metrics(&system_metrics.cpu);
        pthread_mutex_unlock(&cpu_mutex);
        usleep(REFRESH_RATE);
    }
    return NULL;
}

void *memory_thread(void *arg) {
    while (system_metrics.should_run) {
        pthread_mutex_lock(&memory_mutex);
        get_memory_metrics(&system_metrics.memory);
        pthread_mutex_unlock(&memory_mutex);
        usleep(REFRESH_RATE);
    }
    return NULL;
}

void *process_thread(void *arg) {
    while (system_metrics.should_run) {
        pthread_mutex_lock(&process_mutex);
        get_process_metrics(system_metrics.processes, &system_metrics.num_processes);
        pthread_mutex_unlock(&process_mutex);
        usleep(REFRESH_RATE);
    }
    return NULL;
}

int main() {
    initscr();
    start_color();
    init_pair(1, COLOR_YELLOW, COLOR_BLACK); // Título
    init_pair(2, COLOR_GREEN, COLOR_BLACK);  // Proceso activo
    init_pair(3, COLOR_RED, COLOR_BLACK);    // Proceso con alerta

    system_metrics.should_run = 1;

    pthread_t cpu_tid, memory_tid, process_tid;
    pthread_create(&cpu_tid, NULL, cpu_thread, NULL);
    pthread_create(&memory_tid, NULL, memory_thread, NULL);
    pthread_create(&process_tid, NULL, process_thread, NULL);

    while (system_metrics.should_run) {
        display_metrics(&system_metrics);
        usleep(REFRESH_RATE);
    }

    system_metrics.should_run = 0;
    pthread_join(cpu_tid, NULL);
    pthread_join(memory_tid, NULL);
    pthread_join(process_tid, NULL);

    endwin();
    return 0;
}