
TESTROOT = ../..
include ${TESTROOT}/include/common.makefile


#
# <rdar://problem/5807857> there should be some way to temporarily turn off -pie
#
# run a PIE four times and verify its load address was the same every time
#

all-check: all check

check:
	export DYLD_NO_PIE=1 && ./main > main.out
	export DYLD_NO_PIE=1 && ./main >> main.out
	export DYLD_NO_PIE=1 && ./main >> main.out
	export DYLD_NO_PIE=1 && ./main >> main.out
	if [ `sort main.out -u | wc -l` == 1 ]; \
	then \
		echo "PASS pie-DYLD_NO_PIE"; \
	else \
		echo "FAIL pie-DYLD_NO_PIE"; \
	fi; \
	 
all: main

main : main.c
	${CC} ${CCFLAGS} -I${TESTROOT}/include -Wl,-pie -o main main.c


clean:
	${RM} ${RMFLAGS} *~ main main.out

