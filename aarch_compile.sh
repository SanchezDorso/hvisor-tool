make all ARCH=arm64 KDIR=/home/lgw/study/hypervisor/linux
./trans_file.sh ./tools/hvisor 
./trans_file_ko.sh ./driver/hvisor.ko 