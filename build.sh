#!/usr/bin/env bash

rm -f *.exe

_ARGS="-std=gnu99 -pthread"

if [[ $OSTYPE == darwin* ]]; then
    ESC="\x1B"
else
    ESC="\e"
fi

echo ""

# client
echo -e "$ESC[1m[1/2]$ESC[0m $ESC[94mbuilding client$ESC[0m"
if gcc $_ARGS src/client.c -o client.exe; then
    echo -e " :::  $ESC[32mclient success$ESC[0m"
else
    echo -e "\n - $ESC[1m$ESC[91mclient failed$ESC[0m"
    echo ""
    exit
fi

echo ""

# server
echo -e "$ESC[1m[2/2]$ESC[0m $ESC[94mbuilding server$ESC[0m"
if gcc $_ARGS src/server.c -o server.exe; then
    echo -e " :::  $ESC[32mserver success$ESC[0m"
else
    echo -e "\n - $ESC[1m$ESC[91mserver failed$ESC[0m"
    echo ""
    exit
fi

echo ""
