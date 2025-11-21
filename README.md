# 📘 Proyecto: Cliente FTP Concurrente 

## 📌 Descripción General

Este proyecto implementa un **cliente FTP concurrente**, capaz de comunicarse con un servidor FTP real siguiendo el estándar definido por el **RFC 959**.
El cliente permite:

* Iniciar sesión en un servidor FTP.
* Transferir archivos en **ambos sentidos** (RETR y STOR).
* Ejecutar varias descargas y subidas **de forma concurrente**, sin bloquear la conexión de control.
* Soportar ambos modos de transferencia de datos:

  * **PASV (Passive Mode)**
  * **PORT (Active Mode)**
* Reanudar descargas interrumpidas usando el comando **REST**.
* Ejecutar comandos adicionales del protocolo FTP:

  * `MKD`, `RMD`, `PWD`, `DELE`

La concurrencia se implementa utilizando **procesos hijos (`fork()`)**, garantizando que la conexión de control del proceso padre permanezca activa mientras se realizan trasferencias simultáneas.

Este cliente cumple **todos los requerimientos obligatorios** y añade funcionalidades adicionales para mayor robustez y flexibilidad.

---

## 🧱 Arquitectura del Programa

El cliente está dividido en varios componentes:

### 🔸 1. Conexión de Control

El proceso padre mantiene un socket activo hacia el servidor FTP.
Desde ahí se envían los comandos:

* `USER`
* `PASS`
* `PWD`
* `CWD`
* `MKD`
* `RMD`
* `DELE`
* `MODE PASV` y `MODE PORT` (comandos internos del cliente)

### 🔸 2. Conexión de Datos

Dependiendo del modo elegido:

#### ♦ Modo PASV (passive)

* El servidor indica IP y puerto.
* El cliente se conecta a ese socket.

#### ♦ Modo PORT (active)

* El cliente abre un puerto local.
* Envia `PORT h1,h2,h3,h4,p1,p2`.
* El servidor se conecta **al cliente**.

### 🔸 3. Concurrencia con fork()

Cada operación `RETR` o `STOR` genera un proceso hijo:

* **El padre**:

  * Mantiene la conexión de control.
  * Acepta nuevos comandos.
  * Recolecta procesos hijos (evita zombies).

* **El hijo**:

  * Abre una **nueva conexión de control** usando la misma IP/USER/PASS.
  * Fuerza `TYPE I` (modo binario).
  * Abre su propio canal de datos (PASV o PORT).
  * Ejecuta `RETR` o `STOR`.
  * Usa `REST offset` si corresponde.
  * Cierra sesión (`QUIT`) y termina.

---

## 🔁 Funcionalidad de Reanudación de Descargas (REST)

Cuando una descarga `RETR` se interrumpe:

1. El cliente revisa si el archivo parcial existe.
2. Obtiene su tamaño con `stat()`.
3. Si existe y tiene >0 bytes:

   * Envía `REST <tamaño_actual>`.
   * Si el servidor acepta (código 350):

     * Se reanuda desde ese punto.
   * Si no:

     * Se descarga desde cero.

Esta funcionalidad solo es posible porque todo el cliente usa **TYPE I (binario)**, que garantiza offsets exactos en bytes.

---

## 📜 Comandos Implementados

### ✔ Comandos obligatorios del RFC 959

| Comando | Descripción              |
| ------- | ------------------------ |
| USER    | Enviar nombre de usuario |
| PASS    | Enviar contraseña        |
| STOR    | Subir un archivo         |
| RETR    | Descargar un archivo     |
| PORT    | Activar modo activo      |
| PASV    | Activar modo pasivo      |

### ✔ Extensiones implementadas (EXTRA CRÉDITO)

| Comando  | Descripción                      |
| -------- | -------------------------------- |
| LIST     | Listar archivos del servidor     |
| PWD      | Mostrar directorio de trabajo    |
| CWD      | Cambiar directorio               |
| MKD      | Crear directorio                 |
| RMD      | Eliminar directorio              |
| DELE     | Eliminar archivo                 |
| **REST** | Reanudar descargas interrumpidas |

---

## 🧠 Requerimientos Técnicos Cumplidos

Este proyecto cumple completamente con lo solicitado:

### ✔ Uso obligatorio de:

* `connectsock.c`
* `connectTCP.c`
* `errexit.c`

### ✔ Implementación completa del protocolo FTP básico

Incluye autenticación, transferencia binaria, modos de datos, comandos básicos y reanudación.

### ✔ Concurrencia real

Implementada con **fork()**, ejemplo:

```
RETR archivo1 archivo2 archivo3
```

→ Se crean 3 hijos simultáneos descargando en paralelo.

### ✔ Conexión de control NO se cierra durante transferencias

El usuario puede:

* Cambiar directorio
* Listar
* Borrar archivos
* Programar nuevas transferencias

Mientras hay hijos activos.

---

## 💻 Compilación

Ejecutar en la terminal:

```bash
make
```

Esto generará el ejecutable:

```
ftp_client
```

---

## ▶ Uso

Ejecutar:

```bash
./ftp_client
```

El programa solicitará:

```
Servidor FTP (IP o nombre):
USER:
PASS:
```

Luego mostrará los comandos disponibles:

```
LIST
CWD <dir>
PWD
MKD <dir>
RMD <dir>
DELE <archivo>
MODE PASV | MODE PORT
RETR <f1> [f2 ...]
STOR <f1> [f2 ...]
QUIT
```















Aquí tienes la **sección completa de ejemplos** en **Markdown**, lista para pegar directamente en tu README:

---

## ▶ Ejemplos de Uso (todos los comandos)

A continuación se muestran ejemplos concretos del uso de cada comando soportado por el cliente FTP.

---

### 🔹 **1. LIST — Listar archivos del servidor**

Muestra todos los archivos del directorio actual del servidor.

```txt
LIST
```

Salida típica:

```txt
-rw-r--r-- archivo1.txt
drwxr-xr-x carpeta1
```

---

### 🔹 **2. PWD — Mostrar el directorio actual del servidor**

```txt
PWD
```

Salida:

```txt
257 "/home/usuario/ftp" is current directory
```

---

### 🔹 **3. CWD <dir> — Cambiar de directorio**

Ejemplo:

```txt
CWD Documentos
```

Cambia al directorio remoto `Documentos`.

---

### 🔹 **4. MKD <dir> — Crear un directorio en el servidor**

```txt
MKD NuevaCarpeta
```

Crea `NuevaCarpeta` en el servidor.

---

### 🔹 **5. RMD <dir> — Eliminar un directorio**

```txt
RMD CarpetaVacia
```

Solo funciona si el directorio está vacío.

---

### 🔹 **6. DELE <archivo> — Eliminar un archivo del servidor**

```txt
DELE archivoObsoleto.txt
```

Elimina un archivo del directorio remoto.

---

### 🔹 **7. MODE PASV — Activar modo pasivo**

```txt
MODE PASV
```

El servidor devuelve un puerto y el cliente se conecta a él.

---

### 🔹 **8. MODE PORT — Activar modo activo**

```txt
MODE PORT
```

El cliente abre un puerto local, envía `PORT h1,h2,h3,h4,p1,p2` y el servidor se conecta al cliente.

---

### 🔹 **9. RETR <archivo> — Descargar un archivo**

```txt
RETR video.mp4
```

Crea un proceso hijo que descarga el archivo sin bloquear la sesión principal.

#### Descargar varios archivos **concurrentemente**

```txt
RETR foto1.png foto2.png foto3.png
```

Cada archivo se procesa en **un proceso hijo independiente**.

---

### 🔹 **10. STOR <archivo> — Subir un archivo**

```txt
STOR documento.pdf
```

Sube un archivo al servidor.

#### Subir varios archivos concurrentemente:

```txt
STOR a.pdf b.pdf c.pdf
```

---

### 🔹 **11. Reanudación automática (REST) — Solo para RETR**

El usuario **no escribe REST manualmente**.
El cliente lo aplica automáticamente si detecta un archivo parcial.

Ejemplo:

```txt
RETR archivoGrande.bin
```

Si el archivo existe localmente:

```txt
archivoGrande.bin (71655424 bytes) ya existe → enviando REST 71655424
```

Si el servidor acepta:

```txt
350 Restart position accepted
```

La descarga continúa desde donde se quedó.

---

### 🔹 **12. QUIT — Cerrar sesión**

```txt
QUIT
```

Finaliza la conexión de control.

---


















## 📂 Estructura del Proyecto

```
ProyectoFTP/
│
├── Makefile
├── ftp_client (ejecutable)
├── GoyesA-clienteFTP.c  ← código principal del cliente
├── connectsock.c
├── connectTCP.c
├── errexit.c
└── otros archivos .o generados por compilación
```
## 🔧 Nota sobre la configuración del servidor FTP (vsftpd)

Este cliente se ha probado utilizando un servidor vsftpd en configuración estándar.
Para asegurar el correcto funcionamiento de los comandos STOR, RETR, PORT, PASV, MKD, RMD y DELE, deben estar activadas las siguientes opciones en /etc/vsftpd.conf:

```
anonymous_enable=NO
local_enable=YES
write_enable=YES
connect_from_port_20=YES
```
No es necesario modificar ninguna otra opción.
El cliente funciona tanto en modo PASV como modo PORT usando la configuración por defecto del servidor.
