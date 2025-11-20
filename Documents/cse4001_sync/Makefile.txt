CC = g++
CFLAGS = -Wall -pthread -std=c++11
TARGET = cse4001_sync
SRC = cse4001_sync.cpp

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET)

clean:
	rm -f $(TARGET)
