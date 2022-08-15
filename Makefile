LDLIBS=-ldl -lbcm_host -lm -lasound -ludev -lpthread
LDFLAGS=-L/opt/vc/lib/
CFLAGS=-Wall -O3 -D_GNU_SOURCE -I/opt/vc/include -g
all: main mainrun

main: main.o lz4.o

mainrun: mainrun.o maininput.o main_udev.o