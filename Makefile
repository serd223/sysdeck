# No --std=c99 because we make use of non standard POSIX functions
C_FLAGS = -Wextra -Wall -pedantic
LIB_ARGS = 

# Generate .clangd file based on compiler flags
comma := ,
empty :=
space := $(empty) $(empty)
FLAGS_LIST = $(subst $(space),$(comma),$(C_FLAGS))
a := $(file > .clangd, CompileFlags:)
b := $(file >> .clangd, 	Add: [$(FLAGS_LIST)])

main: main.c
	cc $(C_FLAGS) main.c -o main $(LIB_ARGS)
