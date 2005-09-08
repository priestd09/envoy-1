OCAMLC=ocamlc
CC=gcc
# -D_FILE_OFFSET_BITS=64 comes from `getconf LFS_CFLAGS`
CCOPTS=-g -Wall -D_FILE_OFFSET_BITS=64
LINKOPTS=-lgc

SRC=9pstatic.c 9p.c list.c util.c config.c state.c transport.c fs.c dispatch.c envoy.c map.c
OBJ=$(SRC:.c=.o)

all:	envoy

9p.h 9p.c: 9p.msg gen
	./gen 9p.msg

gen:	gen.ml
	$(OCAMLC) -o gen gen.ml
	rm -f gen.cmo gen.cmi

%.o:	%.c
	$(CC) $(CCOPTS) -c $<

envoy: $(OBJ)
	$(CC) $(CCOPTS) $(LINKOPTS) -o envoy $(OBJ)

clean:
	rm -f $(OBJ) gen.{cmo,cmi} envoy

cleanall: clean
	rm -f 9p.{c,h} gen depend

depend: gen 9p.h
	$(CC) $(CCOPTS) -M $(SRC) > depend

include depend
