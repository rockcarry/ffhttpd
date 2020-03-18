#!/bin/sh

gcc ffhttpd.c -Wall -lws2_32 -o ffhttpd.exe
strip ffhttpd.exe

gcc cgitest.c -Wall -shared -o cgitest.cgi
strip cgitest.cgi
