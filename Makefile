###############################################################################
## On macOS Ventura: brew install ncurses
###############################################################################
LDFLAGS="-L/opt/homebrew/opt/ncurses/lib"
INC="-I/opt/homebrew/opt/ncurses/include"
CC=clang
CFLAGS=-Wall -std=c99 -lmenu -lncurses

all: browse

clean:
	@rm -f browse

browse: browse.c
	$(CC) $(INC) -o $@ $(CFLAGS) $(LDFLAGS) $<
