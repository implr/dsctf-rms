rms-fixed: main.c
	gcc -o $@  -Wall -Wextra -std=gnu11 -pthread -fstack-protector-strong -fPIE -pie -Wl,-z,now $^
