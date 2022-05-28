LDLIBS=-ldl -lbcm_host -lm -lasound
LDFLAGS=-L/opt/vc/lib/
CFLAGS=-Wall -O3 -D_GNU_SOURCE -I/opt/vc/include -g
main: main.o lz4.o