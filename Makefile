all:
	wayland-scanner private-code \
	  < /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml \
	  > xdg-shell-protocol.c
	wayland-scanner client-header \
	  < /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml \
	  > xdg-shell-client-protocol.h

all: client

client:
	${CC} -o client client.c xdg-shell-protocol.c -lwayland-client -lrt
