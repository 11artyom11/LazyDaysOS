GRUB_CONF_NAME=$1
OS_NAME=$2

rm -f $GRUB_CONF_NAME
touch $GRUB_CONF_NAME
echo "menuentry \"$OS_NAME\" {\n" >> $GRUB_CONF_NAME
echo "	multiboot /boot/$OS_NAME.bin\n" >> $GRUB_CONF_NAME
echo "	}" >> $GRUB_CONF_NAME