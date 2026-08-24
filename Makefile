# ============================================================================
# STORMFISH ENGINE - HIGH-PERFORMANCE MAKEFILE (CON STOCKFISH_NNUE_PROBE)
# ============================================================================

CXX      := g++
STD      := -std=c++17

# Añade -Wa,-I. y -Wa,-Istockfish_nnue_probe para resolver las rutas de .incbin con LTO
OPTFLAGS := -O3 -march=native -mavx2 -mfma -flto=auto -funroll-loops -finline-functions -fomit-frame-pointer -Wa,-I. -Wa,-Istockfish_nnue_probe

# Directorios de inclusión (Añadido stockfish_nnue_probe)
INCLUDES := -Iinclude -Isrc -Istockfish_nnue_probe

CXXFLAGS := $(STD) $(OPTFLAGS) -DNDEBUG -Wall -Wextra $(INCLUDES)

LDFLAGS  := $(OPTFLAGS) -s -Wl,--gc-sections -pthread -Wa,-I. -Wa,-Istockfish_nnue_probe

# Busca automáticamente las fuentes en src/ y en la librería clonada
SRC_ENGINE := $(wildcard src/*.cpp)
# Filtra el main.cpp de la librería y los archivos JNI
# Busca archivos .cpp hasta en carpetas subanidadas (como nnue/features/)
SRC_PROBE_ALL := $(wildcard stockfish_nnue_probe/*.cpp) \
                 $(wildcard stockfish_nnue_probe/*/*.cpp) \
                 $(wildcard stockfish_nnue_probe/*/*/*.cpp) \
                 $(wildcard stockfish_nnue_probe/*/*/*/*.cpp)

SRC_PROBE     := $(filter-out stockfish_nnue_probe/main.cpp stockfish_nnue_probe/NNUEBridge%, $(SRC_PROBE_ALL))
SRC        := $(SRC_ENGINE) $(SRC_PROBE)

# Convierte las rutas .cpp a .o
OBJ        := $(SRC:.cpp=.o)

ifeq ($(OS),Windows_NT)
    TARGET  := stormfish.exe
    RUN_CMD := .\$(TARGET)
    RM      := del /f /q
    RM_DIR  := rmdir /s /q
else
    TARGET  := stormfish
    RUN_CMD := ./$(TARGET)
    RM      := rm -f
    RM_DIR  := rm -rf
endif

all: $(TARGET)

$(TARGET): $(OBJ)
	@echo "Enlazando $(TARGET) a máxima velocidad..."
	$(CXX) $(CXXFLAGS) $(OBJ) -o $(TARGET) $(LDFLAGS)
	@echo "¡Compilación completada con éxito!"

%.o: %.cpp
	@echo "Compilando $<..."
	$(CXX) $(CXXFLAGS) -c $< -o $@

run: $(TARGET)
	$(RUN_CMD)

clean:
	@echo "Limpiando archivos objeto y ejecutables..."
	@$(RM) $(TARGET)
	@$(RM) src\*.o 2>nul || $(RM) src/*.o 2>/dev/null || true
	@$(RM) stockfish_nnue_probe\*.o 2>nul || $(RM) stockfish_nnue_probe/*.o 2>/dev/null || true

.PHONY: all clean run