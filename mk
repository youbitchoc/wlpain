#!/bin/sh
# what's a makefile?
cc -o "example" "example.c" xdg-shell-protocol.c -lwayland-client -lrt
