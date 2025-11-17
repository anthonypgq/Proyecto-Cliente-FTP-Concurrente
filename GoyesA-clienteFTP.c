#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#define FTP_PORT 21
#define BUFFER_SIZE 1024

void leer_respuesta(int sock) {
    char buffer[BUFFER_SIZE];
    int len = recv(sock, buffer, BUFFER_SIZE - 1, 0);
    if (len > 0) {
        buffer[len] = '\0';
        printf("Servidor: %s", buffer);
    }
}

// Función para abrir PASV y devolver socket de datos
int abrir_pasv(int sock) {
    char buffer[BUFFER_SIZE];
    int data_sock;
    struct sockaddr_in data_addr;
    int len, p1, p2, i1, i2, i3, i4;
    char ip_str[64];

    send(sock, "PASV\r\n", 6, 0);
    len = recv(sock, buffer, BUFFER_SIZE - 1, 0);
    buffer[len] = '\0';
    printf("Servidor: %s", buffer);

    sscanf(buffer, "227 Entering Passive Mode (%d,%d,%d,%d,%d,%d)",
           &i1, &i2, &i3, &i4, &p1, &p2);

    sprintf(ip_str, "%d.%d.%d.%d", i1, i2, i3, i4);
    int data_port = p1 * 256 + p2;
    printf("📡 Canal de datos → %s:%d\n", ip_str, data_port);

    data_sock = socket(AF_INET, SOCK_STREAM, 0);
    data_addr.sin_family = AF_INET;
    data_addr.sin_port = htons(data_port);
    inet_pton(AF_INET, ip_str, &data_addr.sin_addr);

    if (connect(data_sock, (struct sockaddr*)&data_addr, sizeof(data_addr)) < 0) {
        perror("❌ No se pudo conectar al canal de datos");
        return -1;
    }

    return data_sock;
}

int main() {
    int sock, data_sock, len;
    struct sockaddr_in server_addr;
    char buffer[BUFFER_SIZE];
    char comando[BUFFER_SIZE];
    char argumento[256];

    // Conexión principal
    sock = socket(AF_INET, SOCK_STREAM, 0);
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(FTP_PORT);
    inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);

    connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr));
    leer_respuesta(sock);

    // ==== LOGIN MANUAL ====
    printf("Inicia sesión:\nEjemplo:\nUSER ftpuser\nPASS 12345\n\n");
    while (1) {
        printf("ftp> ");
        fgets(comando, sizeof(comando), stdin);
        send(sock, comando, strlen(comando), 0);
        leer_respuesta(sock);

        if (strncmp(comando, "PASS", 4) == 0)
            break;
    }
    // ======================

    while (1) {
        printf("\nComandos disponibles:\n");
        printf("LIST - Listar archivos\n");
        printf("CWD <dir> - Cambiar directorio\n");
        printf("RETR <archivo> - Descargar archivo\n");
        printf("STOR <archivo> - Subir archivo\n");
        printf("DELE <archivo> - Eliminar archivo\n");
        printf("QUIT - Salir\n\n");

        printf("ftp> ");
        fgets(comando, sizeof(comando), stdin);

        // QUIT
        if (strncmp(comando, "QUIT", 4) == 0) {
            send(sock, "QUIT\r\n", 6, 0);
            leer_respuesta(sock);
            break;
        }

        // LIST
        if (strncmp(comando, "LIST", 4) == 0) {
            data_sock = abrir_pasv(sock);
            send(sock, "LIST\r\n", 6, 0);
            leer_respuesta(sock);
            while ((len = recv(data_sock, buffer, BUFFER_SIZE - 1, 0)) > 0) {
                buffer[len] = '\0';
                printf("%s", buffer);
            }
            close(data_sock);
            leer_respuesta(sock);
        }

        // CWD
        else if (sscanf(comando, "CWD %s", argumento) == 1) {
            sprintf(buffer, "CWD %s\r\n", argumento);
            send(sock, buffer, strlen(buffer), 0);
            leer_respuesta(sock);
        }

        // RETR (descargar)
        else if (sscanf(comando, "RETR %s", argumento) == 1) {
            FILE *f = fopen(argumento, "wb");
            data_sock = abrir_pasv(sock);
            sprintf(buffer, "RETR %s\r\n", argumento);
            send(sock, buffer, strlen(buffer), 0);
            leer_respuesta(sock);

            while ((len = recv(data_sock, buffer, BUFFER_SIZE, 0)) > 0)
                fwrite(buffer, 1, len, f);

            fclose(f);
            close(data_sock);
            leer_respuesta(sock);
            printf("\nExcelente!!! Archivo descargado.\n");
        }

        // STOR (subir)
        else if (sscanf(comando, "STOR %s", argumento) == 1) {
            FILE *f = fopen(argumento, "rb");
            if (!f) { printf("\nX No se pudo abrir el archivo local.\n"); continue; }

            data_sock = abrir_pasv(sock);
            sprintf(buffer, "STOR %s\r\n", argumento);
            send(sock, buffer, strlen(buffer), 0);
            leer_respuesta(sock);

            while ((len = fread(buffer, 1, BUFFER_SIZE, f)) > 0)
                send(data_sock, buffer, len, 0);

            fclose(f);
            close(data_sock);
            leer_respuesta(sock);
            printf("Excelente!!! Archivo subido.\n");
        }

        // DELE (eliminar)
        else if (sscanf(comando, "DELE %s", argumento) == 1) {
            sprintf(buffer, "DELE %s\r\n", argumento);
            send(sock, buffer, strlen(buffer), 0);
            leer_respuesta(sock);
        }

        else {
            printf("⚠ Comando no reconocido.\n");
        }
    }

    close(sock);
    return 0;
}
