CXX ?= g++
CXXFLAGS = -O3 -std=c++17 -Wall -Iinclude -flto

# Búsqueda automática de TODOS los archivos .cpp en la raíz y en subcarpetas (como src/)
SRC = $(wildcard *.cpp) $(wildcard src/*.cpp) $(wildcard src/**/*.cpp)

# Generar la lista de archivos objeto (.o) automáticamente a partir de los .cpp
OBJ = $(SRC:.cpp=.o)

# Detección automática del Sistema Operativo
ifeq ($(OS),Windows_NT)
    TARGET  = stormfish.exe
    RM      = del /f /q
    RUN_CMD = .\$(TARGET)
else
    TARGET  = stormfish
    RM      = rm -f
    RUN_CMD = ./$(TARGET)
endif

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CXX) $(CXXFLAGS) $^ -o $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

run: $(TARGET)
	$(RUN_CMD)

clean:
	$(RM) $(TARGET) $(OBJ)

.PHONY: all clean run
