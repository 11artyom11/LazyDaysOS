#!/bin/sh
GRUB_CONF_NAME=$1
OS_NAME=$2
BIN_LOCATION=$3

rm -f $GRUB_CONF_NAME
touch $GRUB_CONF_NAME
cat > $GRUB_CONF_NAME << EOF2
set timeout=0
set default=0

menuentry "$OS_NAME" { 
    multiboot $BIN_LOCATION
} 
EOF2
