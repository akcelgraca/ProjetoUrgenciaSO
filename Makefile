CC = gcc
CFLAGS = -Wall -Wextra -pthread -lrt
LDFLAGS = -pthread -lrt

all: admission doctor

admission: admission.c
	$(CC) $(CFLAGS) -o admission admission.c $(LDFLAGS)

doctor: doctor.c
	$(CC) $(CFLAGS) -o doctor doctor.c $(LDFLAGS)

clean:
	rm -f admission doctor DEI_Emergency.log input_pipe

.PHONY: all clean
