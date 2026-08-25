# ============================================================================
# FLASHBIT / STORMFISH ENGINE - HIGH-PERFORMANCE MAKEFILE
# ============================================================================

CXX      := g++
STD      := -std=c++17

# Opciones de optimización e inclusión para submódulos NNUE
OPTFLAGS := -O3 -march=x86-64-v3 -mavx2 -mfma -flto=auto -funroll-loops -finline-functions -fomit-frame-pointer -Wa,-I. -Wa,-Istockfish_nnue_probe
INCLUDES := -Iinclude -Isrc -Istockfish_nnue_probe

CXXFLAGS := $(STD) $(OPTFLAGS) -DNDEBUG -Wall -Wextra -MMD -MP $(INCLUDES)
LDFLAGS  := $(OPTFLAGS) -s -Wl,--gc-sections -pthread -Wa,-I. -Wa,-Istockfish_nnue_probe

# Búsqueda automática de fuentes
SRC_ENGINE    := $(wildcard src/*.cpp)
SRC_PROBE_ALL := $(wildcard stockfish_nnue_probe/*.cpp) \
                 $(wildcard stockfish_nnue_probe/*/*.cpp) \
                 $(wildcard stockfish_nnue_probe/*/*/*.cpp) \
                 $(wildcard stockfish_nnue_probe/*/*/*/*.cpp)

SRC_PROBE     := $(filter-out stockfish_nnue_probe/main.cpp stockfish_nnue_probe/NNUEBridge%, $(SRC_PROBE_ALL))
SRC           := $(SRC_ENGINE) $(SRC_PROBE)

# Objetos y dependencias
OBJ           := $(SRC:.cpp=.o)
DEP           := $(OBJ:.o=.d)

# Detección de Sistema Operativo y comandos de limpieza
ifeq ($(OS),Windows_NT)
    TARGET   := FlashBit.exe
    RUN_CMD  := .\$(TARGET)
    FIXPATH   = $(subst /,\,$1)
    RM_FILES  = -del /f /q $(call FIXPATH,$1) 2>nul
else
    TARGET   := FlashBit
    RUN_CMD  := ./$(TARGET)
    FIXPATH   = $1
    RM_FILES  = -rm -f $1 2>/dev/null
endif

all: $(TARGET)

-include $(DEP)

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
	@$(call RM_FILES,$(TARGET))
	@$(call RM_FILES,$(OBJ))
	@$(call RM_FILES,$(DEP))

.PHONY: all clean run