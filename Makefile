CFLAGS = -Wall -Wextra -Wno-write-strings -DSWAP_BYTES \
         -fdiagnostics-show-option -I/usr/include/mysql \
         -L/usr/lib64/mysql -lmysqlclient

CC = g++

CHOPPER_SOURCES = chopper.cpp PZdabFile.cxx PZdabWriter.cxx \
                  MD5Checksum.cxx 

all: chopper 

chopper: $(CHOPPER_SOURCES)
	$(CC) -o chopper $(CHOPPER_SOURCES) $(CFLAGS) 

clean:
	rm -f chopper
