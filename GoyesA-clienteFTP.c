#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   // strcasecmp
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/wait.h>
#include <errno.h>
#include <sys/stat.h>   // <<< REST: para stat()

#define FTP_PORT_STR "21"
#define BUFFER_SIZE 1024
#define MAX_ARGS    16

/* ==== Prototipos ==== */
int connectsock(const char *host, const char *service, const char *transport);
int connectTCP(const char *host, const char *service);
int errexit(const char *format, ...);

/* ==== Estructura de sesión ==== */
typedef struct {
    char server_ip[64];
    char user[64];
    char pass[64];
    int  use_pasv;   // 1 = PASV, 0 = PORT
} SessionInfo;

/* ----------------- Utilidades generales ----------------- */
void leer_respuesta(int sock) {
    char buffer[BUFFER_SIZE];
    int len = recv(sock, buffer, BUFFER_SIZE - 1, 0);
    if (len > 0) {
        buffer[len] = '\0';
        printf("Servidor: %s", buffer);
    }
}

/* Lee respuesta y devuelve el código numérico (220, 331, 150, etc.) */
int leer_codigo(int sock, char *linea, size_t tam) {
    int len = recv(sock, linea, tam - 1, 0);
    if (len <= 0) return -1;
    linea[len] = '\0';
    printf("Servidor: %s", linea);
    return atoi(linea);
}

/* ----------------- PASV ----------------- */
int abrir_pasv(int sock_ctrl) {
    char buffer[BUFFER_SIZE];
    int data_sock;
    struct sockaddr_in data_addr;
    int len, p1, p2, i1, i2, i3, i4;
    char ip_str[64];

    send(sock_ctrl, "PASV\r\n", 6, 0);
    len = recv(sock_ctrl, buffer, BUFFER_SIZE - 1, 0);
    if (len <= 0) {
        perror("Error recibiendo respuesta PASV");
        return -1;
    }
    buffer[len] = '\0';
    printf("Servidor: %s", buffer);

    if (sscanf(buffer, "227 Entering Passive Mode (%d,%d,%d,%d,%d,%d)",
               &i1, &i2, &i3, &i4, &p1, &p2) != 6) {
        fprintf(stderr, "Respuesta PASV no reconocida.\n");
        return -1;
    }

    sprintf(ip_str, "%d.%d.%d.%d", i1, i2, i3, i4);
    int data_port = p1 * 256 + p2;
    printf("📡 Canal de datos PASV → %s:%d\n", ip_str, data_port);

    data_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (data_sock < 0) {
        perror("socket datos");
        return -1;
    }

    memset(&data_addr, 0, sizeof(data_addr));
    data_addr.sin_family = AF_INET;
    data_addr.sin_port = htons(data_port);
    inet_pton(AF_INET, ip_str, &data_addr.sin_addr);

    if (connect(data_sock, (struct sockaddr*)&data_addr, sizeof(data_addr)) < 0) {
        perror("❌ No se pudo conectar al canal de datos (PASV)");
        close(data_sock);
        return -1;
    }

    return data_sock;
}

/* ----------------- PORT ----------------- */
int abrir_port(int sock_ctrl, int *listen_sock) {
    int lsock;
    struct sockaddr_in addr, local_ctrl_addr;
    socklen_t addrlen = sizeof(addr);
    char buffer[BUFFER_SIZE];

    socklen_t ctrl_len = sizeof(local_ctrl_addr);
    if (getsockname(sock_ctrl, (struct sockaddr*)&local_ctrl_addr, &ctrl_len) < 0) {
        perror("getsockname (control)");
        return -1;
    }

    lsock = socket(AF_INET, SOCK_STREAM, 0);
    if (lsock < 0) {
        perror("socket PORT");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr = local_ctrl_addr.sin_addr;
    addr.sin_port = 0;

    if (bind(lsock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind PORT");
        close(lsock);
        return -1;
    }

    if (listen(lsock, 1) < 0) {
        perror("listen PORT");
        close(lsock);
        return -1;
    }

    if (getsockname(lsock, (struct sockaddr*)&addr, &addrlen) < 0) {
        perror("getsockname (listen)");
        close(lsock);
        return -1;
    }

    unsigned char *ip = (unsigned char*)&addr.sin_addr.s_addr;
    int p = ntohs(addr.sin_port);
    int p1 = p / 256;
    int p2 = p % 256;

    snprintf(buffer, sizeof(buffer),
             "PORT %d,%d,%d,%d,%d,%d\r\n",
             ip[0], ip[1], ip[2], ip[3], p1, p2);

    printf("📡 Enviando: %s", buffer);
    send(sock_ctrl, buffer, strlen(buffer), 0);
    leer_respuesta(sock_ctrl);

    printf("📡 Canal de datos PORT escuchando en puerto local %d\n", p);

    *listen_sock = lsock;
    return 0;
}

int aceptar_port(int listen_sock) {
    struct sockaddr_in cliaddr;
    socklen_t len = sizeof(cliaddr);
    int data_sock = accept(listen_sock, (struct sockaddr*)&cliaddr, &len);
    if (data_sock < 0) {
        perror("accept PORT");
        return -1;
    }
    close(listen_sock);
    return data_sock;
}

/* ----------------- Login ----------------- */
int ftp_connect_and_login(const SessionInfo *info) {
    int ctrl = connectTCP(info->server_ip, FTP_PORT_STR);
    if (ctrl < 0) {
        perror("connectTCP");
        return -1;
    }

    leer_respuesta(ctrl);

    char buffer[BUFFER_SIZE];
    snprintf(buffer, sizeof(buffer), "USER %s\r\n", info->user);
    send(ctrl, buffer, strlen(buffer), 0);
    leer_respuesta(ctrl);

    snprintf(buffer, sizeof(buffer), "PASS %s\r\n", info->pass);
    send(ctrl, buffer, strlen(buffer), 0);
    leer_respuesta(ctrl);

    /* <<< FORZAR MODO BINARIO >>> */
    send(ctrl, "TYPE I\r\n", 8, 0);
    leer_respuesta(ctrl);

    return ctrl;
}

/* ----------------- Transferencia hijo ----------------- */
void hijo_transferencia(const SessionInfo *info, const char *cmd, const char *filename) {

    int ctrl = ftp_connect_and_login(info);
    if (ctrl < 0) exit(1);

    char buffer[BUFFER_SIZE];
    int data_sock = -1;
    int lsock = -1;

    /* PORT: preparar escucha y enviar PORT */
    if (!info->use_pasv) {
        if (abrir_port(ctrl, &lsock) < 0) {
            close(ctrl);
            exit(1);
        }
    }

    /* ================= RETR con REST (reanudación) ================= */
    if (strcasecmp(cmd, "RETR") == 0) {

        struct stat st;
        off_t offset = 0;
        int resume = 0;

        /* ¿Existe ya el archivo local? */
        if (stat(filename, &st) == 0 && st.st_size > 0) {
            offset = st.st_size;
            printf("🔍 Archivo local '%s' ya existe (%lld bytes). Intentando REST...\n",
                   filename, (long long)offset);

            /* Enviar REST <offset> */
            snprintf(buffer, sizeof(buffer), "REST %lld\r\n", (long long)offset);
            send(ctrl, buffer, strlen(buffer), 0);

            int code = leer_codigo(ctrl, buffer, sizeof(buffer));
            if (code >= 300 && code < 400) {
                resume = 1;
                printf("🔁 REST aceptado. Reanudando descarga desde el byte %lld.\n",
                       (long long)offset);
            } else {
                printf("⚠ REST no aceptado (código %d). Descargando desde el inicio.\n", code);
                offset = 0;
            }
        }

        /* Abrir archivo local */
        FILE *f;
        if (resume) {
            f = fopen(filename, "ab");   // continuar al final
        } else {
            f = fopen(filename, "wb");   // truncar o crear nuevo
        }

        if (!f) {
            perror("fopen RETR");
            close(ctrl);
            if (lsock != -1) close(lsock);
            exit(1);
        }

        /* Preparar canal de datos */
        if (info->use_pasv) {
            data_sock = abrir_pasv(ctrl);
            if (data_sock < 0) { fclose(f); close(ctrl); exit(1); }
        }

        /* Enviar RETR */
        snprintf(buffer, sizeof(buffer), "RETR %s\r\n", filename);
        send(ctrl, buffer, strlen(buffer), 0);
        leer_respuesta(ctrl);   // 150...

        if (!info->use_pasv) {
            data_sock = aceptar_port(lsock);
            if (data_sock < 0) { fclose(f); close(ctrl); exit(1); }
        }

        int len;
        while ((len = recv(data_sock, buffer, BUFFER_SIZE, 0)) > 0) {
            fwrite(buffer, 1, len, f);
        }

        fclose(f);
        close(data_sock);
        leer_respuesta(ctrl);   // 226
        printf("✅ [Hijo %d] Archivo descargado: %s\n", getpid(), filename);
    }

    /* ================= STOR (SIN REST) ================= */
    else if (strcasecmp(cmd, "STOR") == 0) {
        FILE *f = fopen(filename, "rb");
        if (!f) { perror("fopen STOR"); close(ctrl); if(lsock!=-1) close(lsock); exit(1); }

        if (info->use_pasv) {
            data_sock = abrir_pasv(ctrl);
            if (data_sock < 0) { fclose(f); close(ctrl); exit(1); }
        }

        snprintf(buffer, sizeof(buffer), "STOR %s\r\n", filename);
        send(ctrl, buffer, strlen(buffer), 0);
        leer_respuesta(ctrl);

        if (!info->use_pasv) {
            data_sock = aceptar_port(lsock);
            if (data_sock < 0) { fclose(f); close(ctrl); exit(1); }
        }

        int len;
        while ((len = fread(buffer, 1, BUFFER_SIZE, f)) > 0) {
            send(data_sock, buffer, len, 0);
        }

        fclose(f);
        close(data_sock);
        leer_respuesta(ctrl);
        printf("✅ [Hijo %d] Archivo subido: %s\n", getpid(), filename);
    }

    send(ctrl, "QUIT\r\n", 6, 0);
    leer_respuesta(ctrl);
    close(ctrl);
    exit(0);
}

/* Evitar zombies */
void reap_children(void) {
    int status;
    while (waitpid(-1, &status, WNOHANG) > 0) {}
}

/* ----------------- MAIN ----------------- */
int main() {
    int sock;
    char buffer[BUFFER_SIZE];
    char linea[BUFFER_SIZE];
    SessionInfo info;
    memset(&info, 0, sizeof(info));

    printf("Servidor FTP (IP o nombre): ");
    fgets(info.server_ip, sizeof(info.server_ip), stdin);
    info.server_ip[strcspn(info.server_ip, "\r\n")] = 0;

    printf("USER: ");
    fgets(info.user, sizeof(info.user), stdin);
    info.user[strcspn(info.user, "\r\n")] = 0;

    printf("PASS: ");
    fgets(info.pass, sizeof(info.pass), stdin);
    info.pass[strcspn(info.pass, "\r\n")] = 0;

    info.use_pasv = 1;

    sock = connectTCP(info.server_ip, FTP_PORT_STR);
    if (sock < 0) errexit("No se pudo conectar.\n");

    leer_respuesta(sock);

    snprintf(buffer, sizeof(buffer), "USER %s\r\n", info.user);
    send(sock, buffer, strlen(buffer), 0);
    leer_respuesta(sock);

    snprintf(buffer, sizeof(buffer), "PASS %s\r\n", info.pass);
    send(sock, buffer, strlen(buffer), 0);
    leer_respuesta(sock);

    
    send(sock, "TYPE I\r\n", 8, 0);
    leer_respuesta(sock);

    printf("\nSesión FTP iniciada. Modo de datos por defecto: PASV.\n");
    printf("Puedes cambiar a PORT con el comando: MODE PORT\n");
    printf("Volver a PASV con: MODE PASV\n\n");

    while (1) {

        reap_children();

        printf("\nComandos disponibles:\n");
        printf(" LIST                  - Listar archivos\n");
        printf(" CWD <dir>             - Cambiar directorio\n");
        printf(" PWD                   - Directorio actual\n");
        printf(" MKD <dir>             - Crear directorio\n");
        printf(" RMD <dir>             - Eliminar directorio\n");
        printf(" RETR <f1> [f2 ...]    - Descargar archivos (servidor → cliente), concurrente, con REST\n");
        printf(" STOR <f1> [f2 ...]    - Subir archivos (cliente → servidor), concurrente\n");
        printf(" DELE <archivo>        - Eliminar archivo\n");
        printf(" MODE PASV | MODE PORT - Cambiar modo de datos\n");
        printf(" QUIT                  - Salir\n\n");

        printf("ftp> ");
        if (!fgets(linea, sizeof(linea), stdin)) break;

        char *argv[MAX_ARGS];
        int argc = 0;
        char *tok = strtok(linea, " \t\r\n");
        while (tok && argc < MAX_ARGS) {
            argv[argc++] = tok;
            tok = strtok(NULL, " \t\r\n");
        }
        if (argc == 0) continue;

        /* QUIT */
        if (strcasecmp(argv[0], "QUIT") == 0) {
            send(sock, "QUIT\r\n", 6, 0);
            leer_respuesta(sock);
            break;
        }

        /* MODE */
        else if (strcasecmp(argv[0], "MODE") == 0 && argc >= 2) {
            if (strcasecmp(argv[1], "PASV") == 0) {
                info.use_pasv = 1;
                printf("Modo PASV.\n");
            } else if (strcasecmp(argv[1], "PORT") == 0) {
                info.use_pasv = 0;
                printf("Modo PORT.\n");
            }
        }

        /* LIST */
        else if (strcasecmp(argv[0], "LIST") == 0) {
            int data_sock, lsock;

            if (info.use_pasv) {
                data_sock = abrir_pasv(sock);
                if (data_sock < 0) continue;
            } else {
                if (abrir_port(sock, &lsock) < 0) continue;
                data_sock = aceptar_port(lsock);
                if (data_sock < 0) continue;
            }

            send(sock, "LIST\r\n", 6, 0);
            leer_respuesta(sock);

            int len;
            while ((len = recv(data_sock, buffer, BUFFER_SIZE - 1, 0)) > 0) {
                buffer[len] = 0;
                printf("%s", buffer);
            }
            close(data_sock);
            leer_respuesta(sock);
        }

        /* CWD */
        else if (strcasecmp(argv[0], "CWD") == 0 && argc >= 2) {
            snprintf(buffer, sizeof(buffer), "CWD %s\r\n", argv[1]);
            send(sock, buffer, strlen(buffer), 0);
            leer_respuesta(sock);
        }

        /* PWD */
        else if (strcasecmp(argv[0], "PWD") == 0) {
            send(sock, "PWD\r\n", 5, 0);
            leer_respuesta(sock);
        }

        /* MKD */
        else if (strcasecmp(argv[0], "MKD") == 0 && argc >= 2) {
            snprintf(buffer, sizeof(buffer), "MKD %s\r\n", argv[1]);
            send(sock, buffer, strlen(buffer), 0);
            leer_respuesta(sock);
        }

        /* RMD */
        else if (strcasecmp(argv[0], "RMD") == 0 && argc >= 2) {
            snprintf(buffer, sizeof(buffer), "RMD %s\r\n", argv[1]);
            send(sock, buffer, strlen(buffer), 0);
            leer_respuesta(sock);
        }

        /* DELE */
        else if (strcasecmp(argv[0], "DELE") == 0 && argc >= 2) {
            snprintf(buffer, sizeof(buffer), "DELE %s\r\n", argv[1]);
            send(sock, buffer, strlen(buffer), 0);
            leer_respuesta(sock);
        }

        /* ---------------- RETR (concurrente, robusto) ---------------- */
        else if (strcasecmp(argv[0], "RETR") == 0 && argc >= 2) {
            for (int i = 1; i < argc; i++) {

                pid_t pid = fork();

                if (pid == 0) {  
                    /* HIJO */
                    hijo_transferencia(&info, "RETR", argv[i]);
                }
                else if (pid < 0) {  
                    /* ERROR */
                    perror("❌ fork RETR");
                }
                else {  
                    /* PADRE */
                    printf("▶ Lanzado hijo %d para RETR %s\n", pid, argv[i]);
                }
            }
        }

        /* ---------------- STOR (concurrente, robusto) ---------------- */
        else if (strcasecmp(argv[0], "STOR") == 0 && argc >= 2) {
            for (int i = 1; i < argc; i++) {

                pid_t pid = fork();

                if (pid == 0) {  
                    /* HIJO */
                    hijo_transferencia(&info, "STOR", argv[i]);
                }
                else if (pid < 0) {  
                    /* ERROR */
                    perror("❌ fork STOR");
                }
                else {  
                    /* PADRE */
                    printf("▶ Lanzado hijo %d para STOR %s\n", pid, argv[i]);
                }
            }
        }

        else {
            printf("⚠ Comando no reconocido.\n");
        }
    }

    close(sock);
    return 0;
}