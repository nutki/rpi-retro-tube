LDLIBS=-ldl -ldrm -lm -lasound -ludev -lpthread
LDFLAGS=-L/opt/vc/lib/
CFLAGS=-Wall -O3 -D_GNU_SOURCE -I/opt/vc/include -g
CFLAGS += $(shell pkg-config --cflags libdrm)
all: main mainrun

main: main.o lz4.o mainlog.o

mainrun: mainrun.o maininput.o main_udev.o mainlog.o maindispmanx.o vcrfont.o