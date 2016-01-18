CC = gcc
MACRO = -DPIC -DDEBUG -DFINE_GRAIN_DEBUG
LIBS = -lasound -lm
TARGET = libasound_module_pcm_fading.so
OUT = -o $(TARGET)

all: fade

fade: fade.o
	$(CC) -Wall $(OUT) fade.o -shared -fpic $(LIBS)

fade.o: fade.c
	$(CC) -Wall -c fade.c $(MACRO) -fpic

install:
	cp $(TARGET) /usr/lib64/alsa-lib/

clean:
	rm *.so *.o
