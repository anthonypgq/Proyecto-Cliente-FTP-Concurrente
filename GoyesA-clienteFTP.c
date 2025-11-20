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

#define FTP_PORT_STR "21"
#define BUFFER_SIZE 1024
#define MAX_ARGS    16

/* ==== Prototipos de las funciones proporcionadas ==== */
int connectsock(const char *host, const char *service, const char *transport);
int connectTCP(const char *host, const char *service);
int errexit(const char *format, ...);

/* ==== Estructura para compartir info de sesión con los hijos ==== */
typedef struct {
    char server_ip[64];
    char user[64];
    char pass[64];
    int  use_pasv;   // 1 = PASV, 0 = PORT
} SessionInfo;

/* ----------------- Utilidades generales ----------------- */

/* Lee una respuesta (una línea) y la imprime */
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

/* ----------------- Modo PASV ----------------- */
/* Abre canal de datos en modo PASV y devuelve socket de datos ya conectado */
int abrir_pasv(int sock_ctrl) {
    char buffer[BUFFER_SIZE];
    int data_sock;
    struct sockaddr_in data_addr;
    int len, p1, p2, i1, i2, i3, i4;
    char ip_str[64];

    /* Enviar comando PASV */
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

/* ----------------- Modo PORT (activo) ----------------- */
/* Prepara socket de escucha local y envía comando PORT; 
   deja el socket escuchando en *listen_sock */
int abrir_port(int sock_ctrl, int *listen_sock) {
    int lsock;
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    char buffer[BUFFER_SIZE];

    lsock = socket(AF_INET, SOCK_STREAM, 0);
    if (lsock < 0) {
        perror("socket PORT");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);   // cualquier IP local
    addr.sin_port        = 0;                   // puerto efímero

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

    // Descubrimos IP y puerto local
    if (getsockname(lsock, (struct sockaddr*)&addr, &addrlen) < 0) {
        perror("getsockname");
        close(lsock);
        return -1;
    }

    unsigned int ip = ntohl(addr.sin_addr.s_addr);
    int p = ntohs(addr.sin_port);
    int p1 = p / 256;
    int p2 = p % 256;

    int h1 = (ip >> 24) & 0xFF;
    int h2 = (ip >> 16) & 0xFF;
    int h3 = (ip >> 8) & 0xFF;
    int h4 =  ip        & 0xFF;

    // Comando PORT h1,h2,h3,h4,p1,p2
    snprintf(buffer, sizeof(buffer), "PORT %d,%d,%d,%d,%d,%d\r\n",
             h1, h2, h3, h4, p1, p2);
    printf("📡 Enviando: %s", buffer);
    send(sock_ctrl, buffer, strlen(buffer), 0);

    leer_respuesta(sock_ctrl);  // normalmente 200 Command okay

    *listen_sock = lsock;
    printf("📡 Canal de datos PORT escuchando en puerto local %d\n", p);
    return 0;
}

/* Acepta la conexión del servidor en modo PORT y devuelve el socket de datos */
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

/* ----------------- Login y conexión de control ----------------- */

/* Realiza una sesión FTP simple: connectTCP + banner + USER/PASS */
int ftp_connect_and_login(const SessionInfo *info) {
    int ctrl = connectTCP(info->server_ip, FTP_PORT_STR);
    if (ctrl < 0) {
        perror("connectTCP");
        return -1;
    }
    // leer banner 220
    leer_respuesta(ctrl);

    char buffer[BUFFER_SIZE];

    snprintf(buffer, sizeof(buffer), "USER %s\r\n", info->user);
    send(ctrl, buffer, strlen(buffer), 0);
    leer_respuesta(ctrl);

    snprintf(buffer, sizeof(buffer), "PASS %s\r\n", info->pass);
    send(ctrl, buffer, strlen(buffer), 0);
    leer_respuesta(ctrl);

    return ctrl;
}

/* ----------------- Transferencias en un proceso hijo ----------------- */

/* Transferencia de un archivo (RETR o STOR) en un proceso hijo, usando PASV o PORT */
void hijo_transferencia(const SessionInfo *info, const char *cmd, const char *filename) {
    int ctrl = ftp_connect_and_login(info);
    if (ctrl < 0) exit(1);

    char buffer[BUFFER_SIZE];
    int data_sock = -1;

    if (info->use_pasv) {
        data_sock = abrir_pasv(ctrl);
        if (data_sock < 0) {
            close(ctrl);
            exit(1);
        }
    } else {
        int lsock;
        if (abrir_port(ctrl, &lsock) < 0) {
            close(ctrl);
            exit(1);
        }
        data_sock = aceptar_port(lsock);
        if (data_sock < 0) {
            close(ctrl);
            exit(1);
        }
    }

    /* RETR: descargar desde servidor al archivo local */
    if (strcasecmp(cmd, "RETR") == 0) {
        FILE *f = fopen(filename, "wb");
        if (!f) {
            perror("fopen local RETR");
            close(data_sock);
            close(ctrl);
            exit(1);
        }

        snprintf(buffer, sizeof(buffer), "RETR %s\r\n", filename);
        send(ctrl, buffer, strlen(buffer), 0);
        leer_respuesta(ctrl); // 150 Opening data connection...

        int len;
        while ((len = recv(data_sock, buffer, BUFFER_SIZE, 0)) > 0) {
            fwrite(buffer, 1, len, f);
        }

        fclose(f);
        close(data_sock);
        leer_respuesta(ctrl); // 226 Transfer complete
        printf("✅ [Hijo %d] Archivo descargado: %s\n", getpid(), filename);
    }
    /* STOR: subir archivo local al servidor */
    else if (strcasecmp(cmd, "STOR") == 0) {
        FILE *f = fopen(filename, "rb");
        if (!f) {
            perror("fopen local STOR");
            close(data_sock);
            close(ctrl);
            exit(1);
        }

        snprintf(buffer, sizeof(buffer), "STOR %s\r\n", filename);
        send(ctrl, buffer, strlen(buffer), 0);
        leer_respuesta(ctrl); // 150 Opening data connection...

        int len;
        while ((len = fread(buffer, 1, BUFFER_SIZE, f)) > 0) {
            send(data_sock, buffer, len, 0);
        }

        fclose(f);
        close(data_sock);
        leer_respuesta(ctrl); // 226 Transfer complete
        printf("✅ [Hijo %d] Archivo subido: %s\n", getpid(), filename);
    }

    send(ctrl, "QUIT\r\n", 6, 0);
    leer_respuesta(ctrl);
    close(ctrl);
    exit(0);
}

/* Pequeña función para evitar zombies (recolección no bloqueante) */
void reap_children(void) {
    int status;
    while (waitpid(-1, &status, WNOHANG) > 0) {
        // recogidos
    }
}

/* ----------------- MAIN ----------------- */

int main() {
    int sock;  // conexión de control principal
    char buffer[BUFFER_SIZE];
    char linea[BUFFER_SIZE];
    SessionInfo info;
    memset(&info, 0, sizeof(info));

    /* ====== Pedimos IP del servidor y credenciales ====== */
    printf("Servidor FTP (IP o nombre): ");
    if (!fgets(info.server_ip, sizeof(info.server_ip), stdin)) exit(1);
    info.server_ip[strcspn(info.server_ip, "\r\n")] = '\0';

    printf("USER: ");
    if (!fgets(info.user, sizeof(info.user), stdin)) exit(1);
    info.user[strcspn(info.user, "\r\n")] = '\0';

    printf("PASS: ");
    if (!fgets(info.pass, sizeof(info.pass), stdin)) exit(1);
    info.pass[strcspn(info.pass, "\r\n")] = '\0';

    info.use_pasv = 1; // por defecto trabajamos en PASV

    /* ====== Conexión de control principal usando connectTCP ====== */
    sock = connectTCP(info.server_ip, FTP_PORT_STR);
    if (sock < 0) errexit("No se pudo conectar al servidor FTP\n");

    // Banner 220
    leer_respuesta(sock);

    // Enviamos USER / PASS (implementación de esos comandos)
    snprintf(buffer, sizeof(buffer), "USER %s\r\n", info.user);
    send(sock, buffer, strlen(buffer), 0);
    leer_respuesta(sock);

    snprintf(buffer, sizeof(buffer), "PASS %s\r\n", info.pass);
    send(sock, buffer, strlen(buffer), 0);
    leer_respuesta(sock);

    printf("\nSesión FTP iniciada. Modo de datos por defecto: PASV.\n");
    printf("Puedes cambiar a PORT con el comando: MODE PORT\n");
    printf("Volver a PASV con: MODE PASV\n\n");

    /* ====== Bucle principal de comandos ====== */
    while (1) {
        reap_children(); // limpiar hijos terminados

        printf("\nComandos disponibles:\n");
        printf(" LIST                  - Listar archivos\n");
        printf(" CWD <dir>             - Cambiar directorio\n");
        printf(" PWD                   - Directorio actual\n");
        printf(" RETR <f1> [f2 ...]    - Descargar uno o varios archivos (concurrente)\n");
        printf(" STOR <f1> [f2 ...]    - Subir uno o varios archivos (concurrente)\n");
        printf(" DELE <archivo>        - Eliminar archivo en servidor\n");
        printf(" MODE PASV | MODE PORT - Cambiar modo de transferencia (usa PASV/PORT)\n");
        printf(" QUIT                  - Salir\n\n");

        printf("ftp> ");
        if (!fgets(linea, sizeof(linea), stdin)) break;

        // Parseo básico por tokens
        char *argv[MAX_ARGS];
        int argc = 0;
        char *tok = strtok(linea, " \t\r\n");
        while (tok && argc < MAX_ARGS) {
            argv[argc++] = tok;
            tok = strtok(NULL, " \t\r\n");
        }
        if (argc == 0) continue;

        /* ---- QUIT ---- */
        if (strcasecmp(argv[0], "QUIT") == 0) {
            send(sock, "QUIT\r\n", 6, 0);
            leer_respuesta(sock);
            break;
        }

        /* ---- MODE PASV / MODE PORT ---- */
        else if (strcasecmp(argv[0], "MODE") == 0 && argc >= 2) {
            if (strcasecmp(argv[1], "PASV") == 0) {
                info.use_pasv = 1;
                printf("✅ Modo de datos cambiado a PASV.\n");
            } else if (strcasecmp(argv[1], "PORT") == 0) {
                info.use_pasv = 0;
                printf("✅ Modo de datos cambiado a PORT.\n");
            } else {
                printf("Uso: MODE PASV | MODE PORT\n");
            }
        }

        /* ---- LIST ---- */
        else if (strcasecmp(argv[0], "LIST") == 0) {
            int data_sock;
            int lsock;

            if (info.use_pasv) {
                data_sock = abrir_pasv(sock);
                if (data_sock < 0) continue;
            } else {
                if (abrir_port(sock, &lsock) < 0) continue;
                data_sock = aceptar_port(lsock);
                if (data_sock < 0) continue;
            }

            snprintf(buffer, sizeof(buffer), "LIST\r\n");
            send(sock, buffer, strlen(buffer), 0);
            leer_respuesta(sock); // 150

            int len;
            while ((len = recv(data_sock, buffer, BUFFER_SIZE - 1, 0)) > 0) {
                buffer[len] = '\0';
                printf("%s", buffer);
            }
            close(data_sock);
            leer_respuesta(sock); // 226
        }

        /* ---- CWD dir ---- */
        else if (strcasecmp(argv[0], "CWD") == 0 && argc >= 2) {
            snprintf(buffer, sizeof(buffer), "CWD %s\r\n", argv[1]);
            send(sock, buffer, strlen(buffer), 0);
            leer_respuesta(sock);
        }

        /* ---- PWD ---- */
        else if (strcasecmp(argv[0], "PWD") == 0) {
            send(sock, "PWD\r\n", 5, 0);
            leer_respuesta(sock);
        }

        /* ---- DELE archivo ---- */
        else if (strcasecmp(argv[0], "DELE") == 0 && argc >= 2) {
            snprintf(buffer, sizeof(buffer), "DELE %s\r\n", argv[1]);
            send(sock, buffer, strlen(buffer), 0);
            leer_respuesta(sock);
        }

        /* ---- RETR f1 [f2 ...] → concurrente ---- */
        else if (strcasecmp(argv[0], "RETR") == 0 && argc >= 2) {
            for (int i = 1; i < argc; i++) {
                pid_t pid = fork();
                if (pid == 0) {
                    // Proceso hijo: hace una sola transferencia RETR
                    hijo_transferencia(&info, "RETR", argv[i]);
                } else if (pid < 0) {
                    perror("fork RETR");
                } else {
                    printf("▶ Lanzado hijo %d para RETR %s\n", pid, argv[i]);
                }
            }
        }

        /* ---- STOR f1 [f2 ...] → concurrente ---- */
        else if (strcasecmp(argv[0], "STOR") == 0 && argc >= 2) {
            for (int i = 1; i < argc; i++) {
                pid_t pid = fork();
                if (pid == 0) {
                    // Proceso hijo: hace una sola transferencia STOR
                    hijo_transferencia(&info, "STOR", argv[i]);
                } else if (pid < 0) {
                    perror("fork STOR");
                } else {
                    printf("▶ Lanzado hijo %d para STOR %s\n", pid, argv[i]);
                }
            }
        }

        else {
            printf("⚠ Comando no reconocido o argumentos insuficientes.\n");
        }
    }

    close(sock);
    return 0;
}
