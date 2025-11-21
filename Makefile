# Makefile para el Cliente FTP Concurrente

CC = gcc
CFLAGS = -Wall -Wextra -pedantic -std=c11 -D h_addr=h_addr_list[0]

OBJS = cliConcurrente.o connectsock.o connectTCP.o errexit.o
TARGET = ftp_client

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS)

cliConcurrente.o: cliConcurrente.c
	$(CC) $(CFLAGS) -c cliConcurrente.c

connectsock.o: connectsock.c
	$(CC) $(CFLAGS) -c connectsock.c

connectTCP.o: connectTCP.c
	$(CC) $(CFLAGS) -c connectTCP.c

errexit.o: errexit.c
	$(CC) $(CFLAGS) -c errexit.c

clean:
	rm -f *.o $(TARGET)

