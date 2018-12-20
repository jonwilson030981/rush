PWD:=$(shell pwd)/
SRC:=$(PWD)src/
OBJ:=$(PWD)obj/

.PHONY: all clean

all: $(OBJ)rush

clean:
	rm -rf $(OBJ)

$(OBJ)rush: $(OBJ)rush.o
	$(CC) -o $@ $<

$(OBJ)rush.o: $(SRC)rush.c | $(OBJ)
	$(CC) -o $@ -c $<

$(OBJ):
	mkdir -p $(OBJ)
