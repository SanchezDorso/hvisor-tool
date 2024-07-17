make all ARCH=riscv KDIR=~/fdisk/linux-aia-rlk
./trans_file.sh ./tools/hvisor 
./trans_file_ko.sh ./driver/hvisor.ko 