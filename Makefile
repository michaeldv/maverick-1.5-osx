# Makefile to build Maverick 1.5 on OS X.

CC = gcc -O3 -Wall

APP = maverick-1.5-osx-64
SRC = $(wildcard *.cpp)
OBJ = $(SRC:.cpp=.o)

$(APP): $(OBJ)
	$(CC) -pthread -c $(SRC)
	$(CC) -o $(APP) $(OBJ)

clean:
	rm -f *.o
	rm -f $(APP)
