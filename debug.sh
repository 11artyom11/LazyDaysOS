gnome-terminal --window --title="GDB Server"  --command="bash -c 'sh bootit.sh -d; $SHELL'"
gnome-terminal --window --title="GDB Client"  --command="bash -c 'gdb --command=gdbconf; $SHELL'"