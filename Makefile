##
# TEST
#
# @file
# @version 0.1

BELUGA_OUTPUT=bin

BUILD_FLAGS=-Wall -Wextra -pedantic

build: main.c src/*
	if [ ! -d $(BELUGA_OUTPUT) ]; then mkdir $(BELUGA_OUTPUT); fi
	$(CC) main.c -o $(BELUGA_OUTPUT)/beluga src/* $(BUILD_FLAGS)

DEBUG_FLAGS=-Wall -Wextra -pedantic -Werror -fsanitize=address -g

debug:  main.c src/*
	if [ ! -d $(BELUGA_OUTPUT) ]; then mkdir $(BELUGA_OUTPUT); fi
	$(CC) main.c -o $(BELUGA_OUTPUT)/beluga src/* $(DEBUG_FLAGS)

doc:
	if [ ! -d doc/ ]; then mkdir doc; fi
	doxygen

clean:
	rm -r $(BELUGA_OUTPUT)
	rm -rf doc/
	rm -rf tmp/

all: build doc

# end
