# Target library
lib 	:= libfs.a
objs    := fs.o disk.o

CC      := gcc
CFLAGS  := -Wall -Wextra -Werror -MMD
CFLAGS  += -g

ifneq ($(V),1)
Q = @
endif

all: $(lib)

deps := $(patsubst %.o,%.d,$(objs))
-include $(deps)

%.a: $(objs)
	ar rcs $@ $^

%.o: %.c
	@echo "CC $@"
	$(Q)$(CC) $(CFLAGS) -c -o $@ $<


clean:
	@echo "clean"
	$(Q)rm -f $(targets) $(objs) $(deps)
