##
# TEST
#
# @file
# @version 0.1

BELUGA_OUTPUT=bin

BUILD_DIR=build

BUILD_FLAGS=-Wall -Wextra -pedantic

build: main.c src/*
	if [ ! -d $(BELUGA_OUTPUT) ]; then mkdir $(BELUGA_OUTPUT); fi
	if [ ! -d doc/ ]; then mkdir doc; fi
	$(CC) main.c -o $(BELUGA_OUTPUT)/beluga src/*
	doxygen

DEBUG_FLAGS=-Wall -Wextra -pedantic -Werror -fsanitize=address,undefined -g

debug:  main.c src/*
	if [ ! -d $(BELUGA_OUTPUT) ]; then mkdir $(BELUGA_OUTPUT); fi
	$(CC) main.c -o $(BELUGA_OUTPUT)/beluga src/* $(DEBUG_FLAGS)

clean:
	rm -r $(BELUGA_OUTPUT)/*
	rm -rf doc/

# end
