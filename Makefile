all: protocols client

client:
	${CC} -g -o client client.c xdg-shell-protocol.c -lwayland-client -lrt

protocols:
	wayland-scanner private-code \
	  < /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml \
	  > xdg-shell-protocol.c
	wayland-scanner client-header \
	  < /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml \
	  > xdg-shell-client-protocol.h

run:
	./client

clean:
	-rm client *.o *protocol.c *protocol.h

.PHONY: all clean
