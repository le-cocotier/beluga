##
# TEST
#
# @file
# @version 0.1

app: main.c main.h
	$(CC) main.c -o main -Wall -Wextra -pedantic

# end
