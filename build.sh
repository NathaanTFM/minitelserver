#!/bin/sh
gcc -g -Wall -Wextra -Wno-unused-parameter miniteldemo.c minitelserver.c -lspandsp -lpjsua -fPIC -lpjmedia -lpj -I/usr/include/python3.11 -lpython3.11 -shared -o minitelserver.so
