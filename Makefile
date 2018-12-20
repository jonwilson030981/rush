PWD:=$(shell pwd)/
SRC:=$(PWD)src/
OBJ:=$(PWD)obj/

.PHONY: all clean run

all: $(OBJ)rush

clean:
	rm -rf $(OBJ)

run: $(OBJ)rush
	$(OBJ)rush 127.0.0.1 2020

$(OBJ)rush: $(OBJ)rush.o
	$(CC) -o $@ $<

$(OBJ)rush.o: $(SRC)rush.c | $(OBJ)
	$(CC) -o $@ -c $<

$(OBJ):
	mkdir -p $(OBJ)
