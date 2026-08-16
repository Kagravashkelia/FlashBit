CXX      = g++
CXXFLAGS = -O3 -std=c++17 -Wall -Iinclude -flto -march=native
SRC      = src/main.cpp

# Detección automática del Sistema Operativo
ifeq ($(OS),Windows_NT)
    TARGET  = stormfish.exe
    RM      = del -f
    RUN_CMD = .\$(TARGET)
else
    TARGET  = stormfish
    RM      = rm -f
    RUN_CMD = ./$(TARGET)
endif

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $(SRC) -o $(TARGET)

run: $(TARGET)
	$(RUN_CMD)

clean:
	$(RM) $(TARGET)

.PHONY: all clean run