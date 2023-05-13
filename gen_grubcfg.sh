GRUB_CONF_NAME=$1
OS_NAME=$2

rm -f $GRUB_CONF_NAME
touch $GRUB_CONF_NAME
cat > $GRUB_CONF_NAME << EOF 
menuentry "$OS_NAME" { 
    multiboot /boot/$OS_NAME.bin
} 
    EOF