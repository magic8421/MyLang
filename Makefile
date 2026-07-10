CC       = gcc
CFLAGS   = -Wall -Wextra -std=c99 -g
SRCDIR   = src
OBJS     = $(SRCDIR)/token.o $(SRCDIR)/ast.o $(SRCDIR)/lexer.o $(SRCDIR)/symtab.o $(SRCDIR)/parser.o $(SRCDIR)/codegen.o $(SRCDIR)/main.o
TARGET   = mylang.exe

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

$(SRCDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

test: $(TARGET)
	./$(TARGET) test/sample.my test/out.c
	gcc -Wall -o test/sample.exe test/out.c
	./test/sample.exe

clean:
	rm -f $(SRCDIR)/*.o $(TARGET) test/out.c test/sample.exe
