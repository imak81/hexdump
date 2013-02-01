CFLAGS := -std=c99 -g -O2 -Wall -Wextra -Werror -Wno-unused-variable -Wno-unused-parameter

hexdump: hexdump.c hexdump.h
	$(CC) $(CFLAGS) -o $@ $< $(CPPFLAGS)

.PHONY: clean clean~

clean:
	rm -f hexdump
	rm -fr *.dSYM

clean~: clean
	rm -f *~

