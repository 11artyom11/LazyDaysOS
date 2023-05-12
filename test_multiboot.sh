RED="\e[31m"
GREEN="\e[32m"
ENDCOLOR="\e[0m"
echo
echo "[INFO] Running multiboot test" 

grub-file --is-x86-multiboot  $1
status=$?
if [ $status -eq 1 ] 
then
    echo "${RED}[ERROR]${ENDCOLOR} $1 is invalid multiboot image"
else 
    echo "${GREEN}[SUCCE]${ENDCOLOR} $1 is valid multiboot image"
fi