##
# TEST
#
# @file
# @version 0.1

BELUGA_OUTPUT=beluga-output

BUILD_DIR=build

BUILD_FLAGS=-Wall -Wextra -pedantic

build: main.c main.h
	if [ ! -d beluga-output ]; then mkdir beluga-output; fi
	if [ ! -d beluga-output/build ]; then mkdir beluga-output/build; fi
	$(CC) main.c -o $(BELUGA_OUTPUT)/$(BUILD_DIR)/beluga $(BUILD_FLAGS)

DEBUG_DIR=debug

DEBUG_FLAGS=-Wall -Wextra -pedantic -Werror -fsanitize=address,undefined -g

debug:  main.c main.h
	if [ ! -d beluga-output ]; then mkdir beluga-output; fi
	if [ ! -d beluga-output/debug ]; then mkdir beluga-output/debug; fi
	$(CC) main.c -o $(BELUGA_OUTPUT)/$(DEBUG_DIR)/beluga $(DEBUG_FLAGS)

clean:
	rm -rf $(BELUGA-OUTPUT)

# end
